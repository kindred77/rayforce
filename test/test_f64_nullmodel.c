/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* test/test_f64_nullmodel.c — single-null float model gates.
 *
 * Model: the F64 value domain is {finite} ∪ {0Nf}.  ANY non-finite F64 result
 * — NaN OR ±Inf — canonicalizes to NULL_F64 (= __builtin_nan("")) at the
 * produce site.  Inf is NOT a value.  Null detection is `x != x`.  HAS_NULLS
 * must be set on a column whenever any element is canonicalized to 0Nf.
 *
 * Two gates:
 *   A. VM≡fallback differential — for every F64 elementwise op, the fused VM
 *      kernel (expr.c) and the per-op fallback kernel (exec_elementwise_*,
 *      forced via ray_expr_disable) must produce BIT-IDENTICAL results over
 *      the same edge inputs: every non-finite case is exactly NULL_F64 on
 *      BOTH, with HAS_NULLS set on both outputs.
 *   B. Property "no column holds a non-finite-non-0Nf value" — for every float
 *      op PLUS the aggregates, feed edge inputs and scan the ENTIRE output
 *      column asserting each element is isfinite(x) OR exactly 0Nf (x != x);
 *      assert HAS_NULLS set whenever a null was produced and that nil?/count
 *      treat 0Nf as null.  A failing element = a missed produce site.
 */

#include "test.h"
#include <rayforce.h>
#include "ops/ops.h"
#include "ops/internal.h"
#include "lang/eval.h"
#include "lang/internal.h"   /* ray_sqrt_fn / ray_log_fn / ... / ray_pearson_corr_fn */
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <math.h>
#include <float.h>

/* Edge inputs that exercise every non-finite producer:
 *   0 / +0.0 / -0.0, DBL_MAX / -DBL_MAX (overflow on +,*,exp), tiny,
 *   negatives (sqrt/log → NaN), 1.0 (log→0). */
static const double EDGE[] = {
    0.0, -0.0, 1.0, -1.0, 2.5, -3.5,
    DBL_MAX, -DBL_MAX, DBL_MIN, 1e-300, 1e300, -1e300,
};
enum { EDGE_N = (int)(sizeof(EDGE) / sizeof(EDGE[0])) };

/* ── shared helpers ───────────────────────────────────────────────────── */

static ray_t* edge_col(void) {
    return ray_vec_from_raw(RAY_F64, (void*)EDGE, EDGE_N);
}

/* Build a 1-column F64 table named "f" from the edge inputs. */
static ray_t* edge_table(void) {
    ray_t* col = edge_col();
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);
    return tbl;
}

/* Build a 2-column F64 table "f","g" for binary ops; g spans values that
 * force 0-divisor, overflow products, and equal/constant columns. */
static ray_t* edge_table2(void) {
    static const double G[EDGE_N] = {
        0.0, 1.0, 0.0, 2.0, -0.5, 1e308,
        1e308, 1.0, 0.0, 1e-300, 1e300, 3.0,
    };
    ray_t* f = edge_col();
    ray_t* g = ray_vec_from_raw(RAY_F64, (void*)G, EDGE_N);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), f);
    tbl = ray_table_add_col(tbl, ray_sym_intern("g", 1), g);
    ray_release(f); ray_release(g);
    return tbl;
}

/* THE invariant: every F64 element is finite OR exactly 0Nf (x != x). */
static test_result_t assert_finite_or_null(ray_t* v, const char* what) {
    TEST_ASSERT_FMT(v && !RAY_IS_ERR(v), "%s: valid result", what);
    TEST_ASSERT_FMT(v->type == RAY_F64, "%s: F64 output (got type %d)", what, v->type);
    const double* d = (const double*)ray_data(v);
    bool any_null = false;
    for (int64_t i = 0; i < v->len; i++) {
        double x = d[i];
        bool is_nan = (x != x);
        /* Forbid Inf and any non-canonical non-finite: only finite or 0Nf. */
        TEST_ASSERT_FMT(isfinite(x) || is_nan,
                        "%s: element %lld is non-finite-non-0Nf (%g)",
                        what, (long long)i, x);
        if (is_nan) {
            any_null = true;
            /* 0Nf must read back as a null and as the canonical sentinel. */
            TEST_ASSERT_FMT(ray_vec_is_null(v, i),
                            "%s: 0Nf at %lld not seen as null", what, (long long)i);
        }
    }
    if (any_null)
        TEST_ASSERT_FMT(v->attrs & RAY_ATTR_HAS_NULLS,
                        "%s: HAS_NULLS must be set when a 0Nf was produced", what);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════════
 * Gate A — VM (fused) ≡ fallback (per-op) differential
 * ════════════════════════════════════════════════════════════════════════ */

typedef ray_op_t* (*expr_builder_t)(ray_graph_t* g);

/* Run builder() fused (default) and forced-fallback (ray_expr_disable), assert
 * the two F64 outputs are BIT-identical, every non-finite is exactly 0Nf on
 * both, and HAS_NULLS agrees.  Also asserts the model invariant on each. */
static test_result_t diff_f64(ray_t* tbl, expr_builder_t builder, const char* what) {
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* fused = ray_execute(g1, builder(g1));

    ray_expr_disable = true;
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* fall = ray_execute(g2, builder(g2));
    ray_expr_disable = false;

    test_result_t r;
    r = assert_finite_or_null(fused, what); if (r.status != TEST_PASS) goto done;
    r = assert_finite_or_null(fall,  what); if (r.status != TEST_PASS) goto done;

    TEST_ASSERT_FMT(fused->len == fall->len, "%s: len mismatch", what);
    /* HAS_NULLS must agree between the two implementations. */
    {
        bool hn_f = (fused->attrs & RAY_ATTR_HAS_NULLS) != 0;
        bool hn_b = (fall->attrs  & RAY_ATTR_HAS_NULLS) != 0;
        if (hn_f != hn_b) {
            static char msg[128];
            snprintf(msg, sizeof(msg),
                     "%s: HAS_NULLS disagrees fused=%d fallback=%d", what, hn_f, hn_b);
            r = (test_result_t){ TEST_FAIL, msg };
            goto done;
        }
    }
    const double* a = (const double*)ray_data(fused);
    const double* b = (const double*)ray_data(fall);
    for (int64_t i = 0; i < fused->len; i++) {
        int na = (a[i] != a[i]), nb = (b[i] != b[i]);
        /* Every non-finite must be 0Nf on BOTH (na==nb), and finite lanes
         * must be bit-identical between fused and fallback. */
        if (na || nb) {
            TEST_ASSERT_FMT(na && nb,
                "%s: null/value divergence at %lld: fused=%g fallback=%g",
                what, (long long)i, a[i], b[i]);
        } else {
            uint64_t ba, bb; memcpy(&ba, &a[i], 8); memcpy(&bb, &b[i], 8);
            TEST_ASSERT_FMT(ba == bb,
                "%s: value mismatch at %lld: fused=%g fallback=%g",
                what, (long long)i, a[i], b[i]);
        }
    }
done:
    ray_release(fused); ray_release(fall);
    ray_graph_free(g1); ray_graph_free(g2);
    return r;
}

static ray_op_t* b_add (ray_graph_t* g){ return ray_add (g, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_sub (ray_graph_t* g){ return ray_sub (g, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_mul (ray_graph_t* g){ return ray_mul (g, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_div (ray_graph_t* g){ return ray_div (g, ray_scan(g,"f"), ray_scan(g,"g")); }
/* ray_idiv() hard-codes I64 output; use ray_binop(OP_IDIV) so promote(F64,F64)
 * keeps the result F64 (the floor-divide path that can produce 0Nf). */
static ray_op_t* b_idiv(ray_graph_t* g){ return ray_binop(g, OP_IDIV, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_mod (ray_graph_t* g){ return ray_mod (g, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_min (ray_graph_t* g){ return ray_min2(g, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_max (ray_graph_t* g){ return ray_max2(g, ray_scan(g,"f"), ray_scan(g,"g")); }
static ray_op_t* b_neg (ray_graph_t* g){ return ray_neg (g, ray_scan(g,"f")); }
static ray_op_t* b_abs (ray_graph_t* g){ return ray_abs (g, ray_scan(g,"f")); }
static ray_op_t* b_sqrt(ray_graph_t* g){ return ray_sqrt_op(g, ray_scan(g,"f")); }
static ray_op_t* b_log (ray_graph_t* g){ return ray_log_op (g, ray_scan(g,"f")); }
static ray_op_t* b_exp (ray_graph_t* g){ return ray_exp_op (g, ray_scan(g,"f")); }

static test_result_t test_diff_all_f64_ops(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* t1 = edge_table();
    ray_t* t2 = edge_table2();
    test_result_t r = { TEST_PASS, NULL };
    struct { ray_t* tbl; expr_builder_t b; const char* name; } cases[] = {
        { t2, b_add,  "add"  }, { t2, b_sub,  "sub"  }, { t2, b_mul,  "mul"  },
        { t2, b_div,  "div"  }, { t2, b_idiv, "idiv" }, { t2, b_mod,  "mod"  },
        { t2, b_min,  "min2" }, { t2, b_max,  "max2" },
        { t1, b_neg,  "neg"  }, { t1, b_abs,  "abs"  }, { t1, b_sqrt, "sqrt" },
        { t1, b_log,  "log"  }, { t1, b_exp,  "exp"  },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        r = diff_f64(cases[i].tbl, cases[i].b, cases[i].name);
        if (r.status != TEST_PASS) break;
    }
    ray_release(t1); ray_release(t2);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ════════════════════════════════════════════════════════════════════════
 * Gate B — property: no column holds a non-finite-non-0Nf value
 * ════════════════════════════════════════════════════════════════════════ */

/* Elementwise ops through the graph executor (fused path). */
static test_result_t test_prop_elementwise(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* t1 = edge_table();
    ray_t* t2 = edge_table2();
    test_result_t r = { TEST_PASS, NULL };
    struct { ray_t* tbl; expr_builder_t b; const char* name; } cases[] = {
        { t2, b_add,  "add"  }, { t2, b_sub,  "sub"  }, { t2, b_mul,  "mul"  },
        { t2, b_div,  "div"  }, { t2, b_idiv, "idiv" }, { t2, b_mod,  "mod"  },
        { t2, b_min,  "min2" }, { t2, b_max,  "max2" },
        { t1, b_neg,  "neg"  }, { t1, b_abs,  "abs"  }, { t1, b_sqrt, "sqrt" },
        { t1, b_log,  "log"  }, { t1, b_exp,  "exp"  },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ray_graph_t* g = ray_graph_new(cases[i].tbl);
        ray_t* out = ray_execute(g, cases[i].b(g));
        r = assert_finite_or_null(out, cases[i].name);
        ray_release(out); ray_graph_free(g);
        if (r.status != TEST_PASS) break;
    }
    ray_release(t1); ray_release(t2);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Assert a scalar F64 atom is finite or exactly 0Nf (never raw non-finite). */
static test_result_t check_scalar_atom(ray_t* a, const char* what) {
    TEST_ASSERT_FMT(a && !RAY_IS_ERR(a), "%s: valid result", what);
    TEST_ASSERT_FMT(a->type == -RAY_F64 || a->type == RAY_F64,
                    "%s: F64 atom (got %d)", what, a->type);
    double x = a->f64;
    TEST_ASSERT_FMT(isfinite(x) || (x != x),
                    "%s: non-finite-non-0Nf scalar (%g)", what, x);
    if (x != x)
        TEST_ASSERT_FMT(ray_atom_is_null_fn(a), "%s: 0Nf scalar not null", what);
    PASS();
}

/* pow is a scalar (atom) builtin via make_f64 — exercise overflow/NaN edges. */
static test_result_t test_prop_pow_and_unary(void) {
    ray_heap_init(); (void)ray_sym_init();
    test_result_t r = { TEST_PASS, NULL };

    /* pow over scalar edges: pow(DBL_MAX, 2)=Inf, pow(-1,0.5)=NaN, pow(1e300,2)=Inf
     * — every non-finite result must canonicalize to 0Nf. */
    struct { double base, exp; const char* name; } pw[] = {
        { DBL_MAX, 2.0,  "pow(DBL_MAX,2)" },
        { -1.0,    0.5,  "pow(-1,0.5)"    },
        { 1e300,   2.0,  "pow(1e300,2)"   },
        { 2.0,     3.0,  "pow(2,3)"       },   /* finite stays finite */
        { 0.0,     0.0,  "pow(0,0)"       },
    };
    for (size_t i = 0; i < sizeof(pw)/sizeof(pw[0]); i++) {
        ray_t* b = ray_f64(pw[i].base);
        ray_t* e = ray_f64(pw[i].exp);
        ray_t* p = ray_pow_fn(b, e);
        r = check_scalar_atom(p, pw[i].name);
        ray_release(p); ray_release(b); ray_release(e);
        if (r.status != TEST_PASS) goto done;
    }
    /* pow(2,3) must remain the finite value 8. */
    {
        ray_t* b = ray_f64(2.0), *e = ray_f64(3.0);
        ray_t* p = ray_pow_fn(b, e);
        TEST_ASSERT(p && !RAY_IS_ERR(p) && p->f64 == 8.0, "pow(2,3)==8 preserved");
        ray_release(p); ray_release(b); ray_release(e);
    }
done:
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* An aggregate cell (atom) must be finite or exactly 0Nf — never a raw
 * non-finite value.  Returns PASS/FAIL. */
static test_result_t check_agg_atom(ray_t* a, const char* what) {
    TEST_ASSERT_FMT(a && !RAY_IS_ERR(a), "%s: valid agg result", what);
    /* Aggregate scalars are F64 atoms (or typed nulls). */
    if (a->type == -RAY_F64 || a->type == RAY_F64) {
        double x = a->f64;
        TEST_ASSERT_FMT(isfinite(x) || (x != x),
                        "%s: agg produced non-finite-non-0Nf (%g)", what, x);
        /* A NaN payload must be recognized as null by the model. */
        if (x != x)
            TEST_ASSERT_FMT(ray_atom_is_null_fn(a), "%s: 0Nf agg not seen as null", what);
    }
    PASS();
}

/* Aggregates over edge inputs: sum/avg/min/max/var/stddev + single-row var,
 * all-null group, zero-variance pearson, overflow. */
static test_result_t test_prop_aggregates(void) {
    ray_heap_init(); (void)ray_sym_init();
    test_result_t r = { TEST_PASS, NULL };

    /* Overflow-prone column: summing/squaring DBL_MAX overflows. */
    ray_t* col = edge_col();

    struct { ray_t* (*fn)(ray_t*); const char* name; } aggs[] = {
        { ray_sum_fn, "sum" }, { ray_avg_fn, "avg" },
        { ray_min_fn, "min" }, { ray_max_fn, "max" },
        { ray_var_fn, "var" }, { ray_var_pop_fn, "var_pop" },
        { ray_stddev_fn, "stddev" }, { ray_stddev_pop_fn, "stddev_pop" },
    };
    for (size_t i = 0; i < sizeof(aggs)/sizeof(aggs[0]); i++) {
        ray_t* res = ray_lazy_materialize(aggs[i].fn(col));
        r = check_agg_atom(res, aggs[i].name);
        ray_release(res);
        if (r.status != TEST_PASS) goto done;
    }

    /* Single-row variance/stddev → insufficient data → typed null. */
    {
        double one[1] = { 3.0 };
        ray_t* c1 = ray_vec_from_raw(RAY_F64, one, 1);
        ray_t* v  = ray_lazy_materialize(ray_var_fn(c1));
        r = check_agg_atom(v, "var_single_row");
        if (r.status == TEST_PASS)
            TEST_ASSERT(ray_atom_is_null_fn(v), "single-row var is null");
        ray_release(v); ray_release(c1);
        if (r.status != TEST_PASS) goto done;
    }

    /* All-null group: every element 0Nf → sum skips all → null-or-finite,
     * never a summed NaN value. */
    {
        double nans[4] = { NULL_F64, NULL_F64, NULL_F64, NULL_F64 };
        ray_t* cn = ray_vec_from_raw(RAY_F64, nans, 4);
        for (int i = 0; i < 4; i++) ray_vec_set_null(cn, i, true);
        ray_t* s = ray_lazy_materialize(ray_sum_fn(cn));
        r = check_agg_atom(s, "sum_all_null");
        ray_release(s); ray_release(cn);
        if (r.status != TEST_PASS) goto done;
    }

    /* Zero-variance pearson: y constant → denominator 0 → undefined → 0Nf. */
    {
        double xs[5] = { 1, 2, 3, 4, 5 };
        double ys[5] = { 7, 7, 7, 7, 7 };
        ray_t* cx = ray_vec_from_raw(RAY_F64, xs, 5);
        ray_t* cy = ray_vec_from_raw(RAY_F64, ys, 5);
        ray_t* pe = ray_lazy_materialize(ray_pearson_corr_fn(cx, cy));
        r = check_agg_atom(pe, "pearson_zero_var");
        if (r.status == TEST_PASS)
            TEST_ASSERT(ray_atom_is_null_fn(pe), "zero-variance pearson is 0Nf/null");
        ray_release(pe); ray_release(cx); ray_release(cy);
        if (r.status != TEST_PASS) goto done;
    }
done:
    ray_release(col);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* VM-in-select: a fused F64 expression used as a projected column over edge
 * inputs (the (/ f g) path) must scan finite-or-0Nf and carry HAS_NULLS. */
static test_result_t test_prop_vm_in_select(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = edge_table2();

    /* ((f / g) * f) + f — a chain that produces 0Nf at g==0 lanes and on
     * overflow; exercises HAS_NULLS propagation through fused arithmetic. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* d = ray_div(g, ray_scan(g, "f"), ray_scan(g, "g"));
    ray_op_t* m = ray_mul(g, d, ray_scan(g, "f"));
    ray_op_t* a = ray_add(g, m, ray_scan(g, "f"));
    ray_t* out = ray_execute(g, a);

    test_result_t r = assert_finite_or_null(out, "vm_in_select chain");

    ray_release(out); ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

const test_entry_t f64_nullmodel_entries[] = {
    { "f64_nullmodel/diff_vm_eq_fallback", test_diff_all_f64_ops,  NULL, NULL },
    { "f64_nullmodel/prop_elementwise",   test_prop_elementwise,   NULL, NULL },
    { "f64_nullmodel/prop_pow_unary",     test_prop_pow_and_unary, NULL, NULL },
    { "f64_nullmodel/prop_aggregates",    test_prop_aggregates,    NULL, NULL },
    { "f64_nullmodel/prop_vm_in_select",  test_prop_vm_in_select,  NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

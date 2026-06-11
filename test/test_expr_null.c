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

/* test/test_expr_null.c — null-aware fused expression tests */
#include "test.h"
#include <rayforce.h>
#include "ops/ops.h"
#include "ops/internal.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <math.h>

/* Build an I64 column with nulls at given indices. */
static ray_t* vec_i64_with_nulls(const int64_t* vals, int64_t n,
                                 const int64_t* null_idx, int64_t n_nulls) {
    ray_t* v = ray_vec_from_raw(RAY_I64, (void*)vals, n);
    for (int64_t i = 0; i < n_nulls; i++)
        ray_vec_set_null(v, null_idx[i], true);
    return v;
}

/* Compare two vectors element-wise: same length, same null positions,
 * same non-null values (F64 compared bitwise-as-null + 1e-12 tolerance).
 * Convention: a=fused, b=fallback (matches the diff_run call site). */
static test_result_t vec_expect_equal(ray_t* a, ray_t* b) {
    TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "valid results");
    TEST_ASSERT_FMT(a->type == b->type, "type %d != %d", a->type, b->type);
    TEST_ASSERT_FMT(a->len == b->len, "len %lld != %lld",
                    (long long)a->len, (long long)b->len);
    for (int64_t i = 0; i < a->len; i++) {
        bool na = ray_vec_is_null(a, i), nb = ray_vec_is_null(b, i);
        TEST_ASSERT_FMT(na == nb, "null mismatch at %lld: fused=%d fallback=%d",
                        (long long)i, na, nb);
        if (na) continue;
        if (a->type == RAY_F64) {
            double xa = ((double*)ray_data(a))[i];
            double xb = ((double*)ray_data(b))[i];
            /* Both-NaN is an acceptable match: F64 ops (e.g. DIV by zero)
             * legitimately produce non-null NaN in both fused and fallback
             * paths; the harness contract is fused≡fallback, not absolute
             * correctness. Null-position equivalence is checked above. */
            TEST_ASSERT_FMT(fabs(xa - xb) < 1e-12 || (xa != xa && xb != xb),
                            "f64 mismatch at %lld: fused=%g fallback=%g",
                            (long long)i, xa, xb);
        } else {
            /* Hoist elem size: type equality asserted above, so a->type == b->type. */
            uint8_t esz = ray_elem_size(a->type);
            int64_t xa = 0, xb = 0;
            memcpy(&xa, (char*)ray_data(a) + i * esz, esz);
            memcpy(&xb, (char*)ray_data(b) + i * esz, esz);
            /* Sign-extend narrow values so diagnostic messages are readable
             * (e.g. I32 -1 prints as -1, not 4294967295). */
            if      (esz == 4) { xa = (int32_t)xa; xb = (int32_t)xb; }
            else if (esz == 2) { xa = (int16_t)xa; xb = (int16_t)xb; }
            else if (esz == 1) { xa = (int8_t)xa;  xb = (int8_t)xb;  }
            TEST_ASSERT_FMT(xa == xb, "mismatch at %lld: fused=%lld fallback=%lld",
                            (long long)i, (long long)xa, (long long)xb);
        }
    }
    PASS();
}

/* Sum all bail counters across all bail reasons. */
static uint64_t expr_bails_total(void) {
    uint64_t t = 0;
    for (int i = 0; i < EXPR_BAIL__N; i++) t += ray_expr_bail_counts[i];
    return t;
}

/* Execute builder() twice — fused-eligible and forced-fallback — and
 * compare. expect_fused: assert the fused path actually compiled.
 *
 * Root-compile detection uses BOTH ok and bails snapshots.  Using ok alone
 * is a false-positive: when the root bails, exec.c recursively evaluates
 * children, and an all-non-null child subtree can compile (incrementing ok)
 * even though root fusion never happened.  A genuine root-level compile
 * produces ok_delta >= 1 with bail_delta == 0; a root bail produces
 * bail_delta >= 1 (the root contributes at least one bail count). */
typedef ray_op_t* (*expr_builder_t)(ray_graph_t* g);

static test_result_t diff_run(ray_t* tbl, expr_builder_t builder,
                              bool expect_fused) {
    uint64_t ok_before    = ray_expr_compile_ok;
    uint64_t bails_before = expr_bails_total();

    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* fused = ray_execute(g1, builder(g1));

    /* Capture bails AFTER the first execute but BEFORE disabling fusion,
     * so the disabled second run's (uncounted) path doesn't skew the delta. */
    bool compiled = (ray_expr_compile_ok > ok_before) &&
                    (expr_bails_total() == bails_before);

    ray_expr_disable = true;
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* fall = ray_execute(g2, builder(g2));
    ray_expr_disable = false;

    test_result_t res = vec_expect_equal(fused, fall);
    if (res.status == TEST_PASS && expect_fused && !compiled)
        res = (test_result_t){ TEST_FAIL,
            "expected fused compile but expr_compile bailed" };

    ray_release(fused); ray_release(fall);
    ray_graph_free(g1); ray_graph_free(g2);
    return res;
}

static test_result_t test_expr_bail_counter_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    int64_t nidx[] = {2, 5};
    ray_t* col = vec_i64_with_nulls(vals, 8, nidx, 2);
    TEST_ASSERT(col->attrs & RAY_ATTR_HAS_NULLS, "attr set");

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    /* Task 7: nullable i64 arithmetic now has a null-aware kernel variant.
     * The fused path compiles and executes — no EXPR_BAIL_NULL_SHAPE fires. */
    uint64_t ok_before   = ray_expr_compile_ok;
    uint64_t bail_before = 0;
    for (int i = 0; i < EXPR_BAIL__N; i++) bail_before += ray_expr_bail_counts[i];
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* e = ray_add(g, x, ray_const_i64(g, 1));
    ray_t* r = ray_execute(g, e);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Fused compile should succeed: ok increments, bails do not. */
    TEST_ASSERT(ray_expr_compile_ok > ok_before,
                "nullable i64+const now compiles in fused path (Task 7)");
    uint64_t bail_after = 0;
    for (int i = 0; i < EXPR_BAIL__N; i++) bail_after += ray_expr_bail_counts[i];
    TEST_ASSERT(bail_after == bail_before,
                "no bail for nullable i64+const (kernel variant live)");

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_nullfree_stream_unchanged(void) {
    ray_heap_init(); (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 8);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* e = ray_mul(g, ray_add(g, ray_scan(g, "x"),
                                     ray_const_i64(g, 1)),
                          ray_const_i64(g, 2));
    ray_expr_t ex;
    TEST_ASSERT(expr_compile(g, tbl, e, &ex), "null-free compiles");
    for (uint8_t i = 0; i < ex.n_ins; i++)
        TEST_ASSERT(ex.ins[i].null_aware == 0, "no null_aware on null-free");
    for (uint8_t r2 = 0; r2 < ex.n_regs; r2++)
        TEST_ASSERT(!ex.regs[r2].nullable, "no nullable regs on null-free");

    ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static ray_op_t* build_add_const(ray_graph_t* g) {
    return ray_add(g, ray_scan(g, "x"), ray_const_i64(g, 7));
}

static test_result_t test_diff_i64_add_nullable_prelanding(void) {
    ray_heap_init(); (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t nidx[] = {0, 4, 9};
    ray_t* col = vec_i64_with_nulls(vals, 10, nidx, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    /* Task 7: null-aware i64 arithmetic kernel landed; expect_fused=true. */
    test_result_t r = diff_run(tbl, build_add_const, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Step 1: new diff tests for nullable F64 fused execution ---- */

/* (f * 2.0 + 1.0) > 5.0 — arith chain into null-aware f64 compare */
static ray_op_t* build_f64_chain(ray_graph_t* g) {
    ray_op_t* f = ray_scan(g, "f");
    ray_op_t* m = ray_mul(g, f, ray_const_f64(g, 2.0));
    ray_op_t* a = ray_add(g, m, ray_const_f64(g, 1.0));
    return ray_gt(g, a, ray_const_f64(g, 5.0));
}

static test_result_t test_diff_f64_chain_nullable(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 10;
    double raw[10];
    for (int64_t i = 0; i < n; i++) raw[i] = 0.5 + (double)i;
    ray_t* col = ray_vec_from_raw(RAY_F64, raw, n);
    ray_vec_set_null(col, 1, true);
    ray_vec_set_null(col, 6, true);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);

    test_result_t r = diff_run(tbl, build_f64_chain, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* x / 2.0 — nullable i64 promoted to f64 via inserted CAST */
static ray_op_t* build_i64_promoted(ray_graph_t* g) {
    return ray_div(g, ray_scan(g, "x"), ray_const_f64(g, 2.0));
}

static test_result_t test_diff_i64_promoted_nullable(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t vals[] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20};
    int64_t nidx[] = {0, 5, 9};
    ray_t* col = vec_i64_with_nulls(vals, 10, nidx, 3);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    test_result_t r = diff_run(tbl, build_i64_promoted, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_f64_chain_parallel(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* col = ray_vec_new(RAY_F64, n);
    col->len = n;
    double* d = (double*)ray_data(col);
    for (int64_t i = 0; i < n; i++) d[i] = 0.5 + (double)(i % 10);
    /* null every 1000th row */
    for (int64_t i = 0; i < n; i += 1000)
        ray_vec_set_null(col, i, true);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);

    test_result_t r = diff_run(tbl, build_f64_chain, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Step 6: null-free promotion case ---- */

/* x + f — forces expr_ensure_type cast from i64 to f64 */
static ray_op_t* build_i64_plus_f64(ray_graph_t* g) {
    return ray_add(g, ray_scan(g, "x"), ray_scan(g, "f"));
}

static test_result_t test_nullfree_promotion_invariance(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t xvals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    double  fvals[] = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5};
    ray_t* xcol = ray_vec_from_raw(RAY_I64, xvals, 8);
    ray_t* fcol = ray_vec_from_raw(RAY_F64, fvals, 8);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), fcol);
    ray_release(xcol); ray_release(fcol);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_expr_t ex;
    TEST_ASSERT(expr_compile(g, tbl, build_i64_plus_f64(g), &ex),
                "null-free promotion compiles");
    for (uint8_t i = 0; i < ex.n_ins; i++)
        TEST_ASSERT(ex.ins[i].null_aware == 0,
                    "no null_aware on null-free promotion");
    for (uint8_t r2 = 0; r2 < ex.n_regs; r2++)
        TEST_ASSERT(!ex.regs[r2].nullable,
                    "no nullable regs on null-free promotion");

    ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* (f * 2.0) + 1.0 — pure arithmetic output, stays F64, no comparison.
 * Verifies: fused == fallback for numeric output; AND that the result
 * vec has RAY_ATTR_HAS_NULLS set with nulls exactly at the input null
 * positions {2,7} (numeric-output attr propagation). */
static ray_op_t* build_f64_arith_output(ray_graph_t* g) {
    ray_op_t* f = ray_scan(g, "f");
    ray_op_t* m = ray_mul(g, f, ray_const_f64(g, 2.0));
    return ray_add(g, m, ray_const_f64(g, 1.0));
}

static test_result_t test_diff_f64_arith_output(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 10;
    double raw[10];
    for (int64_t i = 0; i < n; i++) raw[i] = 0.5 + (double)i;
    ray_t* col = ray_vec_from_raw(RAY_F64, raw, n);
    ray_vec_set_null(col, 2, true);
    ray_vec_set_null(col, 7, true);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);

    /* diff_run: fused == fallback */
    test_result_t r = diff_run(tbl, build_f64_arith_output, true);
    if (r.status != TEST_PASS) {
        ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
        return r;
    }

    /* Re-execute normally and check attr propagation + null positions */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* res = ray_execute(g, build_f64_arith_output(g));
    TEST_ASSERT(res && !RAY_IS_ERR(res), "execute succeeded");
    TEST_ASSERT(res->attrs & RAY_ATTR_HAS_NULLS, "HAS_NULLS attr set on numeric output");
    TEST_ASSERT(ray_vec_is_null(res, 2), "null at index 2");
    TEST_ASSERT(ray_vec_is_null(res, 7), "null at index 7");
    for (int64_t i = 0; i < n; i++) {
        if (i == 2 || i == 7) continue;
        TEST_ASSERT_FMT(!ray_vec_is_null(res, i),
                        "unexpected null at index %lld", (long long)i);
    }
    ray_release(res); ray_graph_free(g);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ---- Task 6: null-aware i64 comparisons, AND/OR, ISNULL ---- */

/* Fixture: 10-row table with two nullable i64 columns.
 *   x: vals 1..10, nulls at {0,4,9}
 *   y: vals 10..1 (decreasing), nulls at {0,3,8} */
static ray_t* make_task6_table(void) {
    int64_t xv[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int64_t yv[] = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i64_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

/* x > 5 */
static ray_op_t* build_x_gt_const(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 5));
}
/* x == 3 */
static ray_op_t* build_x_eq_const(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "x"), ray_const_i64(g, 3));
}
/* x != 3 */
static ray_op_t* build_x_ne_const(ray_graph_t* g) {
    return ray_ne(g, ray_scan(g, "x"), ray_const_i64(g, 3));
}
/* x <= y */
static ray_op_t* build_x_le_y(ray_graph_t* g) {
    return ray_le(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
/* (x > 1) and (y < 5) */
static ray_op_t* build_and_expr(ray_graph_t* g) {
    return ray_and(g,
        ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 1)),
        ray_lt(g, ray_scan(g, "y"), ray_const_i64(g, 5)));
}
/* (x > 1) or (y < 5) */
static ray_op_t* build_or_expr(ray_graph_t* g) {
    return ray_or(g,
        ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 1)),
        ray_lt(g, ray_scan(g, "y"), ray_const_i64(g, 5)));
}
/* isnull(x) */
static ray_op_t* build_isnull_x(ray_graph_t* g) {
    return ray_isnull(g, ray_scan(g, "x"));
}

static test_result_t test_diff_i64_gt_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_gt_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_eq_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_eq_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_ne_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_ne_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_le_y(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_le_y, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_and(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_and_expr, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_or(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_or_expr, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_isnull_x(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_isnull_x, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* isnull over a NULL-FREE i64 column: result must be all-false BOOL vec.
 * Pre-change: ISNULL falls through ot=RAY_I64 in expr_compile so it bails
 * (BOOL output type is wrong → sentinel path is absent) — the fused path
 * would produce garbage if it didn't bail, but the capability choke fires
 * EXPR_BAIL_NULL_SHAPE and the fallback runs instead.  This test pins the
 * correct all-false result regardless of whether fusion fires. */
static test_result_t test_isnull_nonnullable_fused(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 10);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* res = ray_execute(g, ray_isnull(g, ray_scan(g, "x")));
    TEST_ASSERT(res && !RAY_IS_ERR(res), "execute succeeded");
    TEST_ASSERT(res->type == RAY_BOOL, "result is BOOL");
    TEST_ASSERT_FMT(res->len == 10, "len %lld", (long long)res->len);
    uint8_t* d = (uint8_t*)ray_data(res);
    for (int64_t i = 0; i < 10; i++)
        TEST_ASSERT_FMT(d[i] == 0, "expected false at %lld, got %d",
                        (long long)i, (int)d[i]);
    ray_release(res); ray_graph_free(g);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ---- Raw nullable i64 AND/OR: both inputs are nullable i64 columns ----
 * Unlike the Task 6 tests (which feed AND/OR from comparison outputs that
 * are non-nullable BOOLs), these feed raw nullable i64 values directly so
 * the null_aware I64 BOOL kernel cells are exercised.
 *
 * Fixture:
 *   x: vals {1,0,3,4,5,6,7,8,9,10}  nulls {0,4,9}
 *   y: vals {1,2,0,4,5,6,7,8,9,10}  nulls {0,3,8}
 *
 * Interesting rows:
 *   row 0: x null,  y null   → both null
 *   row 2: x=3,     y=0      → y is zero (falsy non-null)
 *   row 3: x=4,     y null   → y null, x truthy
 *   row 4: x null,  y=5      → x null, y truthy
 *   row 8: x=9,     y null   → y null, x truthy
 *   row 9: x null,  y=10     → x null, y truthy
 *   remaining rows: both non-null, varying truthiness
 *
 * Expected fallback semantics (any null → 0):
 *   AND: null-on-either → 0; else (a && b) ? 1 : 0
 *   OR:  null-on-either → 0; else (a || b) ? 1 : 0
 */
static ray_t* make_raw_andor_table(void) {
    int64_t xv[] = {1, 0, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int64_t yv[] = {1, 2, 0, 4, 5, 6, 7, 8, 9, 10};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i64_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

static ray_op_t* build_and_raw(ray_graph_t* g) {
    return ray_and(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

static ray_op_t* build_or_raw(ray_graph_t* g) {
    return ray_or(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

/* x < 5 — LT with a constant, exercises the null-aware LT comparison cell */
static ray_op_t* build_x_lt_const(ray_graph_t* g) {
    return ray_lt(g, ray_scan(g, "x"), ray_const_i64(g, 5));
}

/* x >= y — GE between two nullable i64 columns */
static ray_op_t* build_x_ge_y(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

static test_result_t test_diff_i64_lt_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_lt_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_ge_y(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_ge_y, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_and_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table();
    test_result_t r = diff_run(tbl, build_and_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_or_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table();
    test_result_t r = diff_run(tbl, build_or_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Task 7: null-aware i64 arithmetic + f64 min2/max2 ---- */

/* Two-column i64 fixture:
 *   x: {1,2,3,4,5,6,7,8,9,10}  nulls {0,4,9}
 *   y: {0,3,0,7,6,5,4,3,2,1}   nulls {0,3,8}
 *
 * y has zero values at non-null rows 2 and 6 (for div/mod zero-divisor tests).
 * Null rows 0 and 3 have value 0 in raw storage but are null-flagged. */
static ray_t* make_task7_table(void) {
    int64_t xv[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int64_t yv[] = {0, 3, 0, 7, 6, 5, 4, 3, 2, 1};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i64_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

static ray_op_t* build_i64_sub(ray_graph_t* g) {
    return ray_sub(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_mul(ray_graph_t* g) {
    return ray_mul(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_div(ray_graph_t* g) {
    /* y has zero at rows 2,6 (non-null) → zero-divisor null; y null at 0,3,8 → propagate null */
    return ray_div(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_mod(ray_graph_t* g) {
    return ray_mod(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_min2(ray_graph_t* g) {
    return ray_min2(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_max2(ray_graph_t* g) {
    return ray_max2(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

static test_result_t test_diff_i64_sub(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_sub, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_mul(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_mul, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_div(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_div, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_mod(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_mod, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_min2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_min2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_max2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_max2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Unary NEG/ABS: include INT64_MIN+1 and large-magnitude values.
 * Fixture:
 *   x: {1, -1, INT64_MIN+1, INT64_MAX, 0, 42, -42, 100, -100, 7}
 *   nulls at {0, 5}
 *
 * INT64_MIN+1 negated = INT64_MAX (no overflow).
 * INT64_MAX negated = INT64_MIN+1 via unsigned wrap (no overflow but result is negative large).
 * Note: INT64_MIN itself would overflow via unsigned wrap → NULL_I64; not placed at a non-null
 * index to keep the fixture simple (overflow sentinel is exercised by mark_i64_overflow_as_null). */
static ray_t* make_neg_abs_table(void) {
    int64_t xv[] = {1, -1, INT64_MIN+1, INT64_MAX, 0, 42, -42, 100, -100, 7};
    int64_t xi[] = {0, 5};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    ray_release(xcol);
    return tbl;
}

static ray_op_t* build_i64_neg(ray_graph_t* g) {
    return ray_neg(g, ray_scan(g, "x"));
}
static ray_op_t* build_i64_abs(ray_graph_t* g) {
    return ray_abs(g, ray_scan(g, "x"));
}

static test_result_t test_diff_i64_neg(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_neg_abs_table();
    test_result_t r = diff_run(tbl, build_i64_neg, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_abs(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_neg_abs_table();
    test_result_t r = diff_run(tbl, build_i64_abs, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Deep chain: ((x + 1) * y - 3) % 7
 * Exercises scratch-register sentinel flow through a multi-op chain.
 * Uses make_task7_table (y has zero divisors for % but 7 is the right divisor here — no zero). */
static ray_op_t* build_i64_deep_chain(ray_graph_t* g) {
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* y = ray_scan(g, "y");
    ray_op_t* xp1 = ray_add(g, x, ray_const_i64(g, 1));
    ray_op_t* mul = ray_mul(g, xp1, y);
    ray_op_t* sub = ray_sub(g, mul, ray_const_i64(g, 3));
    return ray_mod(g, sub, ray_const_i64(g, 7));
}

static test_result_t test_diff_i64_deep_chain(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_deep_chain, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* F64 min2/max2 over two nullable F64 columns (no zero-div shapes).
 * Fixture:
 *   f: {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}  nulls {1, 6}
 *   g: {10.0, 9.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0}  nulls {2, 7} */
static ray_t* make_f64_minmax_table(void) {
    double fv[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
    double gv[] = {10.0, 9.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0};
    ray_t* fcol = ray_vec_from_raw(RAY_F64, fv, 10);
    ray_vec_set_null(fcol, 1, true);
    ray_vec_set_null(fcol, 6, true);
    ray_t* gcol = ray_vec_from_raw(RAY_F64, gv, 10);
    ray_vec_set_null(gcol, 2, true);
    ray_vec_set_null(gcol, 7, true);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), fcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("g", 1), gcol);
    ray_release(fcol); ray_release(gcol);
    return tbl;
}

static ray_op_t* build_f64_min2(ray_graph_t* g) {
    return ray_min2(g, ray_scan(g, "f"), ray_scan(g, "g"));
}
static ray_op_t* build_f64_max2(ray_graph_t* g) {
    return ray_max2(g, ray_scan(g, "f"), ray_scan(g, "g"));
}

static test_result_t test_diff_f64_min2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_f64_minmax_table();
    test_result_t r = diff_run(tbl, build_f64_min2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_f64_max2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_f64_minmax_table();
    test_result_t r = diff_run(tbl, build_f64_max2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Task 7 guard: non-parted len-1 scalar-broadcast column bails fused compile ----
 *
 * ray_table_add_col does NOT validate column length — a len-1 column can be
 * appended to a 5-row table.  ray_table_nrows() uses the FIRST column's length,
 * so nrows=5 but b->len=1.  The guard in expr_compile (SCAN branch) detects
 * col->len != nrows for non-parted columns and fires EXPR_BAIL_OTHER, preventing
 * the fused evaluator from overreading b.  The fallback exec_elementwise_binary
 * handles the len-1 column correctly as a scalar broadcast (r_scalar=true path).
 *
 * Table:
 *   x: {10, 20, 30, 40, 50}          — 5 rows, no nulls
 *   b: {7}                            — 1 row (scalar-broadcast, no nulls)
 *
 * expr_compile called directly on x+b: must return false, EXPR_BAIL_OTHER++
 * ray_execute (fused bails → fallback): result == {17, 27, 37, 47, 57}
 * diff_run (expect_fused=false): fused-via-fallback == forced-fallback */
static test_result_t test_scalar_broadcast_col_bails(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* x: 5-row column — drives ray_table_nrows() == 5 */
    int64_t xv[] = {10, 20, 30, 40, 50};
    ray_t* xcol = ray_vec_from_raw(RAY_I64, xv, 5);
    /* b: len-1 column — legitimate scalar-broadcast, but not an atom (type > 0) */
    int64_t bv[] = {7};
    ray_t* bcol = ray_vec_from_raw(RAY_I64, bv, 1);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("b", 1), bcol);
    ray_release(xcol);
    ray_release(bcol);

    /* --- Part 1: expr_compile directly must bail with EXPR_BAIL_OTHER --- */
    ray_graph_t* gc = ray_graph_new(tbl);
    ray_op_t* sx = ray_scan(gc, "x");
    ray_op_t* sb = ray_scan(gc, "b");
    ray_op_t* add = ray_add(gc, sx, sb);

    uint64_t bail_other_before = ray_expr_bail_counts[EXPR_BAIL_OTHER];
    ray_expr_t ex;
    bool compiled = expr_compile(gc, tbl, add, &ex);
    TEST_ASSERT_FALSE(compiled);
    TEST_ASSERT_FMT(ray_expr_bail_counts[EXPR_BAIL_OTHER] > bail_other_before,
                    "EXPR_BAIL_OTHER must increment (was %llu, now %llu)",
                    (unsigned long long)bail_other_before,
                    (unsigned long long)ray_expr_bail_counts[EXPR_BAIL_OTHER]);
    ray_graph_free(gc);

    /* --- Part 2: ray_execute bails fused and falls back to scalar-broadcast path ---
     * The fallback exec_elementwise_binary treats len-1 as r_scalar=true;
     * result should be {17, 27, 37, 47, 57}. */
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* res = ray_execute(g1, ray_add(g1, ray_scan(g1, "x"), ray_scan(g1, "b")));
    TEST_ASSERT(res && !RAY_IS_ERR(res), "ray_execute fallback succeeded");
    TEST_ASSERT_FMT(res->type == RAY_I64, "result type I64, got %d", (int)res->type);
    TEST_ASSERT_FMT(res->len == 5, "result len 5, got %lld", (long long)res->len);
    int64_t* rd = (int64_t*)ray_data(res);
    for (int64_t i = 0; i < 5; i++) {
        int64_t expected = xv[i] + bv[0];
        TEST_ASSERT_FMT(rd[i] == expected,
                        "row %lld: expected %lld got %lld",
                        (long long)i, (long long)expected, (long long)rd[i]);
    }
    ray_release(res);
    ray_graph_free(g1);

    /* --- Part 3: diff_run — fused-via-fallback == forced-fallback (expect_fused=false) --- */
    {
        /* reuse tbl — build fresh graph inside diff_run via builder */
        ray_graph_t* gf = ray_graph_new(tbl);
        ray_t* fused_res = ray_execute(gf, ray_add(gf, ray_scan(gf, "x"), ray_scan(gf, "b")));

        ray_expr_disable = true;
        ray_graph_t* gfall = ray_graph_new(tbl);
        ray_t* fall_res = ray_execute(gfall, ray_add(gfall, ray_scan(gfall, "x"), ray_scan(gfall, "b")));
        ray_expr_disable = false;

        test_result_t cmp = vec_expect_equal(fused_res, fall_res);
        ray_release(fused_res); ray_release(fall_res);
        ray_graph_free(gf); ray_graph_free(gfall);
        if (cmp.status != TEST_PASS) {
            ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
            return cmp;
        }
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t expr_null_entries[] = {
    { "expr_null/bail_counter",            test_expr_bail_counter_nulls,          NULL, NULL },
    { "expr_null/nullfree_invariance",     test_nullfree_stream_unchanged,        NULL, NULL },
    { "expr_null/diff_i64_add",            test_diff_i64_add_nullable_prelanding, NULL, NULL },
    { "expr_null/diff_f64_chain",          test_diff_f64_chain_nullable,          NULL, NULL },
    { "expr_null/diff_i64_promoted",       test_diff_i64_promoted_nullable,       NULL, NULL },
    { "expr_null/diff_f64_chain_parallel", test_diff_f64_chain_parallel,          NULL, NULL },
    { "expr_null/nullfree_promotion",      test_nullfree_promotion_invariance,    NULL, NULL },
    { "expr_null/diff_f64_arith_output",   test_diff_f64_arith_output,            NULL, NULL },
    /* Task 6: null-aware i64 comparisons, AND/OR, ISNULL */
    { "expr_null/diff_i64_gt_const",       test_diff_i64_gt_const,                NULL, NULL },
    { "expr_null/diff_i64_eq_const",       test_diff_i64_eq_const,                NULL, NULL },
    { "expr_null/diff_i64_ne_const",       test_diff_i64_ne_const,                NULL, NULL },
    { "expr_null/diff_i64_le_y",           test_diff_i64_le_y,                    NULL, NULL },
    { "expr_null/diff_i64_and",            test_diff_i64_and,                     NULL, NULL },
    { "expr_null/diff_i64_or",             test_diff_i64_or,                      NULL, NULL },
    { "expr_null/diff_isnull_x",           test_diff_isnull_x,                    NULL, NULL },
    { "expr_null/isnull_nonnullable",      test_isnull_nonnullable_fused,          NULL, NULL },
    /* LT/GE: additional null-aware comparison cells */
    { "expr_null/diff_i64_lt_const",       test_diff_i64_lt_const,                NULL, NULL },
    { "expr_null/diff_i64_ge_y",           test_diff_i64_ge_y,                    NULL, NULL },
    /* Raw nullable i64 AND/OR: exercises the null_aware I64 BOOL kernel directly */
    { "expr_null/diff_i64_and_raw",        test_diff_i64_and_raw,                 NULL, NULL },
    { "expr_null/diff_i64_or_raw",         test_diff_i64_or_raw,                  NULL, NULL },
    /* Task 7: null-aware i64 arithmetic */
    { "expr_null/diff_i64_sub",            test_diff_i64_sub,                     NULL, NULL },
    { "expr_null/diff_i64_mul",            test_diff_i64_mul,                     NULL, NULL },
    { "expr_null/diff_i64_div",            test_diff_i64_div,                     NULL, NULL },
    { "expr_null/diff_i64_mod",            test_diff_i64_mod,                     NULL, NULL },
    { "expr_null/diff_i64_min2",           test_diff_i64_min2,                    NULL, NULL },
    { "expr_null/diff_i64_max2",           test_diff_i64_max2,                    NULL, NULL },
    { "expr_null/diff_i64_neg",            test_diff_i64_neg,                     NULL, NULL },
    { "expr_null/diff_i64_abs",            test_diff_i64_abs,                     NULL, NULL },
    { "expr_null/diff_i64_deep_chain",     test_diff_i64_deep_chain,              NULL, NULL },
    /* Task 7: null-aware f64 min2/max2 */
    { "expr_null/diff_f64_min2",           test_diff_f64_min2,                    NULL, NULL },
    { "expr_null/diff_f64_max2",           test_diff_f64_max2,                    NULL, NULL },
    /* Task 7 guard: non-parted len-1 scalar-broadcast column bails fused compile */
    { "expr_null/scalar_broadcast_bails",  test_scalar_broadcast_col_bails,       NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

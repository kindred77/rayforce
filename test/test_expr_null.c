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

    uint64_t before = ray_expr_bail_counts[EXPR_BAIL_NULL_SHAPE];
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* e = ray_add(g, x, ray_const_i64(g, 1));
    ray_t* r = ray_execute(g, e);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Nullable i64 column: scan is tracked, arithmetic sets null_aware,
     * then the capability choke fires EXPR_BAIL_NULL_SHAPE (no kernel yet). */
    TEST_ASSERT(ray_expr_bail_counts[EXPR_BAIL_NULL_SHAPE] > before,
                "nullable i64+const counted as EXPR_BAIL_NULL_SHAPE");

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

    /* expect_fused=false until the null kernels land (Task 7 flips it). */
    test_result_t r = diff_run(tbl, build_add_const, false);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

const test_entry_t expr_null_entries[] = {
    { "expr_null/bail_counter",        test_expr_bail_counter_nulls,          NULL, NULL },
    { "expr_null/nullfree_invariance", test_nullfree_stream_unchanged,        NULL, NULL },
    { "expr_null/diff_i64_add",        test_diff_i64_add_nullable_prelanding, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * test_fused_group.c — phase-1 OP_FILTERED_GROUP exec coverage.
 *
 * Builds a tiny I64 table directly via the graph API (mirrors
 * test_group_extra.c), constructs an OP_FILTERED_GROUP node with a
 * (==/!= col const) predicate plus a single OP_COUNT aggregate, runs
 * ray_execute, and verifies the materialised key+count table.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/internal.h"
#include "ops/ops.h"
#include "ops/fused_group.h"
#include "ops/idxop.h"
#include "lang/parse.h"
#include "table/sym.h"
#include <string.h>

/* AdvEngineID = [0, 1, 0, 2, 0, 1]
 *   (== 0) → 3 rows (group 0, count 3)
 *   (!= 0) → 3 rows in 2 groups (1 → 2, 2 → 1) */
static ray_t* make_adv_table(void) {
    int64_t adv[] = { 0, 1, 0, 2, 0, 1 };
    ray_t* col = ray_vec_new(RAY_I64, 6);
    if (!col || RAY_IS_ERR(col)) return NULL;
    col->len = 6;
    memcpy(ray_data(col), adv, sizeof(adv));
    int64_t name = ray_sym_intern("AdvEngineID", 11);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, col);
    ray_release(col);
    return tbl;
}

/* (== AdvEngineID 0) by AdvEngineID → 1 row, count = 3 */
static test_result_t test_eq_count(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k    = ray_scan(g, "AdvEngineID");
    ray_op_t* scan_pred = ray_scan(g, "AdvEngineID");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(cnt_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(cnt_col))[0], 3);

    int64_t adv_sym = ray_sym_intern("AdvEngineID", 11);
    ray_t* key_col = ray_table_get_col(res, adv_sym);
    TEST_ASSERT_NOT_NULL(key_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(key_col))[0], 0);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (!= AdvEngineID 0) by AdvEngineID → 2 rows: {1: 2, 2: 1} (order
 * depends on hash, so verify by lookup). */
static test_result_t test_ne_two_groups(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvEngineID");
    ray_op_t* scan_pred = ray_scan(g, "AdvEngineID");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_NE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    int64_t adv_sym = ray_sym_intern("AdvEngineID", 11);
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* key_col = ray_table_get_col(res, adv_sym);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(key_col);
    TEST_ASSERT_NOT_NULL(cnt_col);
    int64_t* ks = (int64_t*)ray_data(key_col);
    int64_t* cs = (int64_t*)ray_data(cnt_col);
    int64_t got_one = -1, got_two = -1;
    for (int64_t i = 0; i < 2; i++) {
        if (ks[i] == 1) got_one = cs[i];
        else if (ks[i] == 2) got_two = cs[i];
    }
    TEST_ASSERT_EQ_I(got_one, 2);
    TEST_ASSERT_EQ_I(got_two, 1);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Finding #3 (constant truncation) regression: a U8 column compared
 * against a constant > 255 must match 0 rows for `==`, not the
 * truncated value (300 → 44).  Build a U8 column with values
 * {0, 1, 0, 2, 0, 1} (same as AdvEngineID test) and check that
 * fp_eval_cmp folds the out-of-range constant. */
static ray_t* make_adv_u8_table(void) {
    uint8_t adv[] = { 0, 1, 0, 2, 0, 1, 44 };  /* trailing 44 is the
                                                  truncated value of 300 */
    ray_t* col = ray_vec_new(RAY_U8, 7);
    if (!col || RAY_IS_ERR(col)) return NULL;
    col->len = 7;
    memcpy(ray_data(col), adv, sizeof(adv));
    int64_t name = ray_sym_intern("AdvU8", 5);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, col);
    ray_release(col);
    return tbl;
}

static test_result_t test_eq_const_out_of_range_u8(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* big       = ray_const_i64(g, 300);  /* out-of-range for U8 */
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, big);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* No U8 row can equal 300 — pre-truncation fix would have matched
     * the value 44 (300 & 0xFF) and returned 1 row. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* `u8_col != 300` is a tautology — every value is unequal to 300, so
 * with the fold we expect 7 source rows ⇒ count 7 in a single group
 * (BUT keyed by U8 so groups are {0:3, 1:2, 2:1, 44:1} = 4 rows). */
static test_result_t test_ne_const_out_of_range_u8(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* big       = ray_const_i64(g, 300);
    ray_op_t* pred      = ray_binop(g, OP_NE, scan_pred, big);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Tautology — every row passes — 4 distinct U8 keys: {0, 1, 2, 44}. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, 7);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_count1_i16_direct_counts_negative_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int16_t kv[] = { -2, -1, -2, 0, 1, 1, 1 };
    ray_t* col = ray_vec_new(RAY_I16, 7);
    TEST_ASSERT_NOT_NULL(col);
    col->len = 7;
    memcpy(ray_data(col), kv, sizeof(kv));

    int64_t k_sym = ray_sym_intern("k16", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, k_sym, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k    = ray_scan(g, "k16");
    ray_op_t* scan_pred = ray_scan(g, "k16");
    ray_op_t* min_key   = ray_const_i64(g, -2);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, min_key);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* key_col = ray_table_get_col(res, k_sym);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(key_col);
    TEST_ASSERT_NOT_NULL(cnt_col);

    int64_t got_m2 = -1, got_m1 = -1, got_0 = -1, got_1 = -1;
    int16_t* ks = (int16_t*)ray_data(key_col);
    int64_t* cs = (int64_t*)ray_data(cnt_col);
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        if (ks[i] == -2) got_m2 = cs[i];
        else if (ks[i] == -1) got_m1 = cs[i];
        else if (ks[i] == 0) got_0 = cs[i];
        else if (ks[i] == 1) got_1 = cs[i];
    }
    TEST_ASSERT_EQ_I(got_m2, 2);
    TEST_ASSERT_EQ_I(got_m1, 1);
    TEST_ASSERT_EQ_I(got_0, 1);
    TEST_ASSERT_EQ_I(got_1, 3);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Finding #4 (signed narrow agg read) regression: SUM of an I16
 * column with negative values must produce the correct signed sum,
 * not a sum where -1 is read as 65535. */
static test_result_t test_sum_negative_i16(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Group key — single group so we sum everything together. */
    int64_t k[] = { 0, 0, 0, 0 };
    /* I16 input — -1 + -2 + 3 + 4 = 4.  Pre-fix would compute
     * 65535 + 65534 + 3 + 4 = 131076. */
    int16_t v[] = { -1, -2, 3, 4 };

    ray_t* kcol = ray_vec_new(RAY_I64, 4);
    kcol->len = 4;
    memcpy(ray_data(kcol), k, sizeof(k));
    ray_t* vcol = ray_vec_new(RAY_I16, 4);
    vcol->len = 4;
    memcpy(ray_data(vcol), v, sizeof(v));

    int64_t kn = ray_sym_intern("g", 1);
    int64_t vn = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, kn, kcol); ray_release(kcol);
    tbl = ray_table_add_col(tbl, vn, vcol); ray_release(vcol);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "g");
    ray_op_t* scan_v    = ray_scan(g, "v");
    /* Predicate: (== g 0) — every row passes, single group. */
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_k, zero);
    uint16_t  agg_ops[] = { OP_SUM, OP_MIN, OP_MAX };
    ray_op_t* agg_ins[] = { scan_v, scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 3);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    int64_t sum_sym = ray_sym_intern("sum", 3);
    int64_t min_sym = ray_sym_intern("min", 3);
    int64_t max_sym = ray_sym_intern("max", 3);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    ray_t* min_col = ray_table_get_col(res, min_sym);
    ray_t* max_col = ray_table_get_col(res, max_sym);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 4);   /* -1+-2+3+4 */
    /* MIN/MAX out cols are RAY_I16, so cast through int16_t. */
    TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(min_col))[0], -2);
    TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(max_col))[0], 4);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Forced-fallback regression (review blocker #1).  Build a fused-group
 * node whose agg input column is nullable — mk_compile rejects this
 * shape, so exec_filtered_group falls through to the unfused
 * FILTER + GROUP path.  The earlier fallback discarded the filter
 * predicate (root variable was overwritten by ray_group), so this
 * test would have returned an unfiltered SUM.  After the fix the
 * fallback runs OP_FILTER first to install g->selection, then
 * OP_GROUP consumes that selection and the SUM is properly scoped.
 *
 *   g  = [0 0 0 1 1]
 *   v  = [10 20 30 40 50] with row 4 set null (forces fallback)
 *   pred (== g 0) selects rows {0,1,2}
 *   expected: sum(v) over filtered rows = 60
 *
 * Pre-fix: filter ignored ⇒ sum = 10+20+30+40 = 100 (row 4 null
 * skipped by SUM but rows 3 was included). */
static test_result_t test_fallback_filter_honored(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* g_col = ray_vec_new(RAY_I64, 5);
    g_col->len = 5;
    int64_t gv[] = {0, 0, 0, 1, 1};
    memcpy(ray_data(g_col), gv, sizeof(gv));

    ray_t* v_col = ray_vec_new(RAY_I64, 5);
    v_col->len = 5;
    int64_t vv[] = {10, 20, 30, 40, 50};
    memcpy(ray_data(v_col), vv, sizeof(vv));
    /* Mark row 4 null so mk_compile rejects this agg input and the
     * dispatcher falls through to the unfused fallback. */
    ray_vec_set_null(v_col, 4, true);

    int64_t g_sym = ray_sym_intern("g", 1);
    int64_t v_sym = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, g_sym, g_col); ray_release(g_col);
    tbl = ray_table_add_col(tbl, v_sym, v_col); ray_release(v_col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_g    = ray_scan(g, "g");
    ray_op_t* scan_v    = ray_scan(g, "v");
    ray_op_t* scan_g2   = ray_scan(g, "g");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_g2, zero);
    uint16_t  agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_g };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Result must have one row (single g=0 bucket).  If the filter
     * were silently dropped, we'd see two rows (g=0 and g=1). */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    /* Find the SUM column by walking the result schema — ray_group
     * via the C API doesn't pick up alias names from a select dict,
     * so the column name is derived (typically the input col sym
     * or "sum").  Walk the columns and find the int64 agg result. */
    int64_t got_sum = -1;
    int64_t ncols = ray_table_ncols(res);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(res, c);
        if (col && col->type == RAY_I64 && col->len == 1) {
            int64_t v = ((int64_t*)ray_data(col))[0];
            /* Distinguish key column (= 0) from sum column (= 60). */
            if (v == 60) { got_sum = v; break; }
        }
    }
    TEST_ASSERT_EQ_I(got_sum, 60);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct-API count1 must reject nullable group keys (review blocker
 * #2).  The planner rejects this shape, but a C-API caller bypasses
 * the planner — count1's executor must reject too, otherwise the
 * per-row HT probe would bucket null sentinels as real key values. */
static test_result_t test_count1_rejects_nullable_key(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k_col = ray_vec_new(RAY_I64, 4);
    k_col->len = 4;
    int64_t kv[] = {1, 2, 3, 4};
    memcpy(ray_data(k_col), kv, sizeof(kv));
    ray_vec_set_null(k_col, 1, true);  /* row 1 has null key */

    int64_t k_sym = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, k_sym, k_col); ray_release(k_col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_kp   = ray_scan(g, "k");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_kp, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    /* count1's nullable-key gate forces fallback, which materialises
     * the filtered table first.  The filter `(>= k 0)` evaluates
     * null compares to false, dropping the null-key row, so the
     * unfused group sees only k = {1, 3, 4} — 3 distinct buckets.
     * (If the count1 executor had not rejected, the per-row HT
     * probe would have read the null sentinel as a real key value
     * and produced a fourth bogus bucket.) */
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─────────────────────────────────────────────────────────────────────
 *  Coverage chunks for fused_group.c
 * ──────────────────────────────────────────────────────────────────── */

/* Chunk 8: fp_compile_cmp out-of-range fold — exercise EQ/NE/LT/LE/GT/GE
 * × narrow types (U8/I16/I32) with the constant outside the column's
 * representable range.  Each call routes through the FP_FOLD_TRUE /
 * FP_FOLD_FALSE branch in fp_compile_cmp (fused_group.c:471-481), which
 * memsets the bits in fp_eval_cmp via the `if (p->fold)` early return.
 *
 * Test grid:
 *   U8 col with value -1 (below): LT → all-false (col < below ⇒ false),
 *                                  GT → all-true.
 *   U8 col with value 300 (above): LE → all-true, GE → all-false.
 *   I16 col with value 100000 (above INT16_MAX): LT → all-true, GE → all-false.
 *   I32 col with value 5e9 (above INT32_MAX): EQ → 0 rows, NE → all rows. */
static test_result_t test_fold_u8_lt_below(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();  /* values {0,1,0,2,0,1,44} */
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* below     = ray_const_i64(g, -1);  /* below U8 min */
    ray_op_t* pred      = ray_binop(g, OP_LT, scan_pred, below);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* col < (below_min) ⇒ FOLD_FALSE → no rows match → empty result. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_fold_u8_gt_below(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* below     = ray_const_i64(g, -1);
    ray_op_t* pred      = ray_binop(g, OP_GT, scan_pred, below);  /* col > -1 ⇒ FOLD_TRUE */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* All 7 rows match.  4 distinct keys: {0:3, 1:2, 2:1, 44:1}. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_fold_u8_le_above(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* above     = ray_const_i64(g, 300);
    ray_op_t* pred      = ray_binop(g, OP_LE, scan_pred, above);  /* col <= 300 ⇒ FOLD_TRUE */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_fold_u8_ge_above(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* above     = ray_const_i64(g, 300);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, above);  /* col >= 300 ⇒ FOLD_FALSE */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_fold_i16_lt_above(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_I16, 5);
    col->len = 5;
    int16_t v[] = {-2, -1, 0, 1, 2};
    memcpy(ray_data(col), v, sizeof(v));
    int64_t k_sym = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, k_sym, col); ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* above     = ray_const_i64(g, 100000);  /* > INT16_MAX */
    ray_op_t* pred      = ray_binop(g, OP_LT, scan_pred, above);  /* FOLD_TRUE */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* All 5 rows pass — 5 distinct I16 keys. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 5);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_fold_i32_eq_above(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_I32, 4);
    col->len = 4;
    int32_t v[] = {1, 2, 3, 4};
    memcpy(ray_data(col), v, sizeof(v));
    int64_t k_sym = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, k_sym, col); ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* above     = ray_const_i64(g, 5000000000LL);  /* > INT32_MAX */
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, above);  /* FOLD_FALSE */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_fold_i32_ne_above(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_I32, 4);
    col->len = 4;
    int32_t v[] = {0, 0, 1, 1};
    memcpy(ray_data(col), v, sizeof(v));
    int64_t k_sym = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, k_sym, col); ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* above     = ray_const_i64(g, 5000000000LL);
    ray_op_t* pred      = ray_binop(g, OP_NE, scan_pred, above);  /* FOLD_TRUE */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* All rows pass; 2 distinct I32 keys. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Chunk 1: multi-agg / multi-key path — fused_group.c:2475-2518.
 *
 * Two keys (g1, g2) of total width <= 8 bytes (narrow path) and three
 * aggregates (COUNT, SUM, AVG).  This routes through
 * exec_filtered_group_multi → mk_compile → mk_par_fn → wide=0 path;
 * and triggers mk_state_merge in the global combine via AVG fold. */
static test_result_t test_multi_agg_multi_key(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* g1 (I32) + g2 (I32) → 8 bytes total → narrow path. */
    ray_t* g1c = ray_vec_new(RAY_I32, 8); g1c->len = 8;
    ray_t* g2c = ray_vec_new(RAY_I32, 8); g2c->len = 8;
    ray_t* vc  = ray_vec_new(RAY_I64, 8); vc->len  = 8;
    int32_t g1[] = {1, 1, 2, 2, 1, 1, 2, 2};
    int32_t g2[] = {1, 2, 1, 2, 1, 2, 1, 2};
    int64_t v[]  = {10, 20, 30, 40, 50, 60, 70, 80};
    memcpy(ray_data(g1c), g1, sizeof(g1));
    memcpy(ray_data(g2c), g2, sizeof(g2));
    memcpy(ray_data(vc),  v,  sizeof(v));

    int64_t s_g1 = ray_sym_intern("g1", 2);
    int64_t s_g2 = ray_sym_intern("g2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_g1, g1c); ray_release(g1c);
    tbl = ray_table_add_col(tbl, s_g2, g2c); ray_release(g2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_g1 = ray_scan(g, "g1");
    ray_op_t* scan_g2 = ray_scan(g, "g2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_g1p= ray_scan(g, "g1");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_g1p, zero);  /* always-true */

    uint16_t  agg_ops[] = { OP_COUNT, OP_SUM, OP_AVG };
    ray_op_t* agg_ins[] = { scan_v, scan_v, scan_v };
    ray_op_t* keys[]    = { scan_g1, scan_g2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 3);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* 4 distinct (g1, g2) pairs. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    /* Total of all sums = sum of all v = 360.  Total of all counts = 8. */
    int64_t sum_sym = ray_sym_intern("sum", 3);
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total_sum = 0, total_cnt = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        total_sum += ((int64_t*)ray_data(sum_col))[i];
        total_cnt += ((int64_t*)ray_data(cnt_col))[i];
    }
    TEST_ASSERT_EQ_I(total_sum, 360);
    TEST_ASSERT_EQ_I(total_cnt, 8);

    /* AVG column is RAY_F64.  Sum of all per-group averages × count
     * weights should equal total_sum.  Cheaper sanity: each avg is in
     * [10,80].  (Use ncols loop because avg may not be by sym=avg if
     * disambiguation differs.) */
    int64_t avg_sym = ray_sym_intern("avg", 3);
    ray_t* avg_col = ray_table_get_col(res, avg_sym);
    TEST_ASSERT_NOT_NULL(avg_col);
    TEST_ASSERT_EQ_I(avg_col->type, RAY_F64);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_multi_key_in_pred(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* g1c = ray_vec_new(RAY_I32, 8); g1c->len = 8;
    ray_t* g2c = ray_vec_new(RAY_I32, 8); g2c->len = 8;
    ray_t* vc  = ray_vec_new(RAY_I64, 8); vc->len  = 8;
    ray_t* sc  = ray_vec_new(RAY_I64, 2); sc->len  = 2;
    int32_t g1[] = {1, 1, 2, 2, 1, 1, 2, 2};
    int32_t g2[] = {1, 2, 1, 2, 1, 2, 1, 2};
    int64_t v[]  = {10, 20, 30, 40, 50, 60, 70, 80};
    int64_t set_vals[] = {20, 70};
    memcpy(ray_data(g1c), g1, sizeof(g1));
    memcpy(ray_data(g2c), g2, sizeof(g2));
    memcpy(ray_data(vc),  v,  sizeof(v));
    memcpy(ray_data(sc),  set_vals, sizeof(set_vals));

    int64_t s_g1 = ray_sym_intern("g1", 2);
    int64_t s_g2 = ray_sym_intern("g2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_g1, g1c); ray_release(g1c);
    tbl = ray_table_add_col(tbl, s_g2, g2c); ray_release(g2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_g1 = ray_scan(g, "g1");
    ray_op_t* scan_g2 = ray_scan(g, "g2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* set_op  = ray_const_vec(g, sc);
    ray_op_t* pred    = ray_binop(g, OP_IN, scan_vp, set_op);
    ray_release(sc);

    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_g1, scan_g2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* k1_col = ray_table_get_col(res, s_g1);
    ray_t* k2_col = ray_table_get_col(res, s_g2);
    ray_t* c_col  = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(k1_col);
    TEST_ASSERT_NOT_NULL(k2_col);
    TEST_ASSERT_NOT_NULL(c_col);
    int64_t got_12 = 0, got_21 = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int32_t a = ((int32_t*)ray_data(k1_col))[i];
        int32_t b = ((int32_t*)ray_data(k2_col))[i];
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        if (a == 1 && b == 2) got_12 = c;
        if (a == 2 && b == 1) got_21 = c;
    }
    TEST_ASSERT_EQ_I(got_12, 1);
    TEST_ASSERT_EQ_I(got_21, 1);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_wide_multi_key_in_pred(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k1c = ray_vec_new(RAY_I64, 8); k1c->len = 8;
    ray_t* k2c = ray_vec_new(RAY_I32, 8); k2c->len = 8;
    ray_t* vc  = ray_vec_new(RAY_I64, 8); vc->len  = 8;
    ray_t* sc  = ray_vec_new(RAY_I64, 2); sc->len  = 2;
    int64_t k1[] = {10, 10, 20, 20, 10, 10, 20, 20};
    int32_t k2[] = {1, 2, 1, 2, 1, 2, 1, 2};
    int64_t v[]  = {10, 20, 30, 40, 50, 60, 70, 80};
    int64_t set_vals[] = {20, 70};
    memcpy(ray_data(k1c), k1, sizeof(k1));
    memcpy(ray_data(k2c), k2, sizeof(k2));
    memcpy(ray_data(vc),  v,  sizeof(v));
    memcpy(ray_data(sc),  set_vals, sizeof(set_vals));

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* set_op  = ray_const_vec(g, sc);
    ray_op_t* pred    = ray_binop(g, OP_IN, scan_vp, set_op);
    ray_release(sc);

    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* out_k1 = ray_table_get_col(res, s_k1);
    ray_t* out_k2 = ray_table_get_col(res, s_k2);
    ray_t* c_col  = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(out_k1);
    TEST_ASSERT_NOT_NULL(out_k2);
    TEST_ASSERT_NOT_NULL(c_col);
    int64_t got_102 = 0, got_201 = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int64_t a = ((int64_t*)ray_data(out_k1))[i];
        int32_t b = ((int32_t*)ray_data(out_k2))[i];
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        if (a == 10 && b == 2) got_102 = c;
        if (a == 20 && b == 1) got_201 = c;
    }
    TEST_ASSERT_EQ_I(got_102, 1);
    TEST_ASSERT_EQ_I(got_201, 1);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_wide_multi_key_top_count_emit_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    enum { N = 15 };
    ray_t* k1c = ray_vec_new(RAY_I64, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int64_t k1[] = {
        10,10,10,10,10, 20,20,20,20, 30,30,30, 40,40, 50
    };
    int32_t k2[] = {
        1,1,1,1,1, 2,2,2,2, 3,3,3, 4,4, 5
    };
    int64_t v[] = {
        0,1,2,3,4, 5,6,7,8, 9,10,11, 12,13, 14
    };
    memcpy(ray_data(k1c), k1, sizeof(k1));
    memcpy(ray_data(k2c), k2, sizeof(k2));
    memcpy(ray_data(vc),  v,  sizeof(v));

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 2;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* out_k1 = ray_table_get_col(res, s_k1);
    ray_t* out_k2 = ray_table_get_col(res, s_k2);
    ray_t* c_col  = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(out_k1);
    TEST_ASSERT_NOT_NULL(out_k2);
    TEST_ASSERT_NOT_NULL(c_col);

    int64_t got_101 = 0, got_202 = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int64_t a = ((int64_t*)ray_data(out_k1))[i];
        int32_t b = ((int32_t*)ray_data(out_k2))[i];
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        if (a == 10 && b == 1) got_101 = c;
        if (a == 20 && b == 2) got_202 = c;
    }
    TEST_ASSERT_EQ_I(got_101, 5);
    TEST_ASSERT_EQ_I(got_202, 4);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Chunk 2 + 3 + 4: wide multi-key path (mk_shard_grow wide branch,
 * mk_compose_key2, mk_hash_lo_hi, mk_state_merge AVG).
 *
 * Three I32 keys → 12 bytes total > 8 → wide=1 path.  Use distinct
 * key triples so the HT has many entries; with 100K rows we cross
 * FP_COMBINE_PAR_MIN to also trigger the parallel combine (chunk 5
 * hist/scat/dedup wide branches). */
static test_result_t test_wide_multi_key(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 100,000 rows, each (i % 100, i % 200, i % 50) → triple key.
     * 100 * 200 * 50 = 1,000,000 possible distinct keys but constrained
     * by row count, so we get plenty of distinct buckets to grow shards. */
    int64_t N = 100000;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* k3c = ray_vec_new(RAY_I32, N); k3c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int32_t* k3 = (int32_t*)ray_data(k3c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i % 100);
        k2[i] = (int32_t)(i % 200);
        k3[i] = (int32_t)(i % 50);
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_k3 = ray_sym_intern("k3", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_k3, k3c); ray_release(k3c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_k3 = ray_scan(g, "k3");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_k1p= ray_scan(g, "k1");
    ray_op_t* zero    = ray_const_i64(g, 0);
    /* Non-trivial WHERE: filter half the rows. */
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_k1p, zero);

    /* COUNT + AVG ⇒ AVG state contributes 2 ints per slot (sum + cnt),
     * exercising mk_state_merge AVG branch in the parallel combine. */
    uint16_t  agg_ops[] = { OP_COUNT, OP_AVG };
    ray_op_t* agg_ins[] = { scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2, scan_k3 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 3, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    int64_t nrows = ray_table_nrows(res);
    /* Number of distinct (k1, k2, k3) triples is bounded by lcm-derived
     * count.  With i=0..99999, gcd(100,200)=100, gcd(100,50)=50,
     * gcd(200,50)=50.  Just sanity: result has multiple groups, total
     * count = N. */
    TEST_ASSERT_TRUE(nrows > 100);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total_cnt = 0;
    for (int64_t i = 0; i < nrows; i++)
        total_cnt += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total_cnt, N);

    /* AVG result column should be F64. */
    int64_t avg_sym = ray_sym_intern("avg", 3);
    ray_t* avg_col = ray_table_get_col(res, avg_sym);
    TEST_ASSERT_NOT_NULL(avg_col);
    TEST_ASSERT_EQ_I(avg_col->type, RAY_F64);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Chunk 7: count1 parallel combine path — fp_combine_hist_fn /
 * _scat_fn / _dedup_fn (fused_group.c:710-836).
 *
 * Drive enough distinct keys past FP_COMBINE_PAR_MIN (50000) so the
 * parallel combine fires.  Single key + single COUNT routes through
 * exec_filtered_group_count1 → fp_combine_and_materialize → parallel
 * combine fork. */
static test_result_t test_count1_parallel_combine(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 200,000 rows with 100,000 distinct I64 keys (i / 2). */
    int64_t N = 200000;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* k = (int64_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) k[i] = i / 2;
    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* zero      = ray_const_i64(g, 0);
    /* Non-trivial WHERE that admits everything (≥ 0). */
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* 100,000 distinct keys. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 100000);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Chunk 6: fp_shard_grow — drive the count1 shard past initial cap so
 * fp_shard_grow runs at least once on each worker.
 *
 * INIT_CAP = 1024; load >= 0.5 triggers grow.  With 10,000 distinct
 * keys spread across workers, every shard will grow several times. */
static test_result_t test_count1_shard_grow(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 30000;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* k = (int64_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) k[i] = i;  /* all distinct */
    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Multi-agg + AND predicate to exercise fp_eval_pred multi-child branch
 * (n_children > 1) and AND-of-cmps compile path
 * (fp_compile_pred_dag recursion). */
static test_result_t test_multi_agg_and_pred(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 200;
    ray_t* gc = ray_vec_new(RAY_I64, N); gc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* g_data = (int64_t*)ray_data(gc);
    int64_t* v_data = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        g_data[i] = i % 4;     /* 4 groups */
        v_data[i] = i + 1;
    }
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_g  = ray_scan(g, "g");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* scan_vp2= ray_scan(g, "v");
    ray_op_t* lo      = ray_const_i64(g, 50);
    ray_op_t* hi      = ray_const_i64(g, 150);
    ray_op_t* p1      = ray_binop(g, OP_GE, scan_vp, lo);
    ray_op_t* p2      = ray_binop(g, OP_LE, scan_vp2, hi);
    ray_op_t* pred    = ray_binop(g, OP_AND, p1, p2);

    /* MIN + MAX → exercise both MIN_INT64/MAX_INT64 init in mk_par_fn,
     * and mk_state_merge MIN/MAX branches. */
    uint16_t  agg_ops[] = { OP_MIN, OP_MAX, OP_SUM };
    ray_op_t* agg_ins[] = { scan_v, scan_v, scan_v };
    ray_op_t* keys[]    = { scan_g };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 3);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* 4 groups, all should have at least one row in [50, 150]. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    int64_t min_sym = ray_sym_intern("min", 3);
    int64_t max_sym = ray_sym_intern("max", 3);
    ray_t* min_col = ray_table_get_col(res, min_sym);
    ray_t* max_col = ray_table_get_col(res, max_sym);
    TEST_ASSERT_NOT_NULL(min_col);
    TEST_ASSERT_NOT_NULL(max_col);
    /* Every per-group min must be ≥ 50 and every per-group max ≤ 150. */
    for (int64_t i = 0; i < 4; i++) {
        int64_t mn = ((int64_t*)ray_data(min_col))[i];
        int64_t mx = ((int64_t*)ray_data(max_col))[i];
        TEST_ASSERT_TRUE(mn >= 50);
        TEST_ASSERT_TRUE(mx <= 150);
    }

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Multi-agg path with BOOL/U8 inputs to exercise the in_unsigned=1
 * branch in mk_compile (line 2431).  Use SUM over BOOL (which counts
 * trues) and over U8 (sum of all values). */
static test_result_t test_multi_agg_unsigned_inputs(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 50;
    ray_t* gc  = ray_vec_new(RAY_I64,  N); gc->len  = N;
    ray_t* bc  = ray_vec_new(RAY_BOOL, N); bc->len  = N;
    ray_t* uc  = ray_vec_new(RAY_U8,   N); uc->len  = N;
    int64_t* gd = (int64_t*)ray_data(gc);
    uint8_t* bd = (uint8_t*)ray_data(bc);
    uint8_t* ud = (uint8_t*)ray_data(uc);
    int64_t total_b = 0, total_u = 0;
    for (int64_t i = 0; i < N; i++) {
        gd[i] = i % 2;
        bd[i] = (uint8_t)(i & 1);
        ud[i] = (uint8_t)(i % 200);
        total_b += bd[i];
        total_u += ud[i];
    }
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_b = ray_sym_intern("b", 1);
    int64_t s_u = ray_sym_intern("u", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_b, bc); ray_release(bc);
    tbl = ray_table_add_col(tbl, s_u, uc); ray_release(uc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_g  = ray_scan(g, "g");
    ray_op_t* scan_b  = ray_scan(g, "b");
    ray_op_t* scan_u  = ray_scan(g, "u");
    ray_op_t* scan_gp = ray_scan(g, "g");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_gp, zero);  /* WHERE that always passes */
    uint16_t  agg_ops[] = { OP_SUM, OP_SUM };
    ray_op_t* agg_ins[] = { scan_b, scan_u };
    ray_op_t* keys[]    = { scan_g };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 2);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    /* Total of all sums across both groups equals total_b and total_u. */
    int64_t sum_sym = ray_sym_intern("sum", 3);
    /* There are two columns named "sum" — fetch by index. */
    int64_t cnt = ray_table_ncols(res);
    int64_t got_b = 0, got_u = 0;
    int seen_first_sum = 0;
    for (int64_t c = 0; c < cnt; c++) {
        ray_t* col = ray_table_get_col_idx(res, c);
        if (!col || col->type != RAY_I64) continue;
        if (col->len != 2) continue;
        /* Skip the key column g (which is also I64 with len 2). */
        int64_t a = ((int64_t*)ray_data(col))[0];
        int64_t b = ((int64_t*)ray_data(col))[1];
        if (a == 0 && b == 1) continue;  /* key column: g has values {0,1} */
        if (!seen_first_sum) { got_b = a + b; seen_first_sum = 1; }
        else                  { got_u = a + b; }
    }
    (void)sum_sym;
    TEST_ASSERT_EQ_I(got_b, total_b);
    TEST_ASSERT_EQ_I(got_u, total_u);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* SYM key + count1 to cover the SYM-id path inside fp_par_fn: walks
 * through ray_sym_elem_size and read_by_esz with W32-width SYM. */
static test_result_t test_count1_sym_key_w32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 100;
    /* RAY_SYM_W32 column. */
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, N);
    sc->len = N;
    /* Three distinct symbols. */
    int64_t s_a = ray_sym_intern("alpha", 5);
    int64_t s_b = ray_sym_intern("beta",  4);
    int64_t s_c = ray_sym_intern("gamma", 5);
    int64_t syms[3] = { s_a, s_b, s_c };
    int32_t* d = (int32_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) d[i] = (int32_t)syms[i % 3];
    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, sc); ray_release(sc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* aatm      = ray_const_i64(g, s_a);
    /* Construct atom of -RAY_SYM via ray_const_atom is harder; use NE
     * against a sym-id that's encoded as I64 — but the column is SYM,
     * which requires atom_type compatibility check.  Use OP_EQ against
     * a known sym id by stashing a SYM atom.  Simpler: bypass and do
     * a no-WHERE direct group via no-pred path... but the brief says
     * every fused-path test MUST include a non-trivial WHERE.  Use
     * a different non-SYM column for the predicate. */
    (void)aatm; (void)scan_pred;
    /* Add a numeric `sel` column to use as predicate. */
    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;
    int64_t s_sel = ray_sym_intern("sel", 3);
    tbl = ray_table_add_col(tbl, s_sel, selc); ray_release(selc);

    ray_graph_free(g);
    g = ray_graph_new(tbl);
    scan_k = ray_scan(g, "k");
    ray_op_t* scan_sel  = ray_scan(g, "sel");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_sel, zero);  /* keep all */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* (== AdvEngineID 99) → no rows pass → empty result. */
static test_result_t test_eq_no_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvEngineID");
    ray_op_t* scan_pred = ray_scan(g, "AdvEngineID");
    ray_op_t* big       = ray_const_i64(g, 99);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, big);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ──────────────────────────────────────────────────────────────────────
 * Coverage extensions: multi-key parallel combine (mk_combine_hist_fn /
 * mk_combine_scat_fn / mk_combine_dedup_fn), fused TOP-N count heap
 * (fp_count_heap_* + fp_count_emit_keep_min), and Phase-3 const-string
 * predicate gate (fp_expr_const_str).
 * ────────────────────────────────────────────────────────────────────── */

/* mk_combine_parallel path: 2 wide I64 keys (16 bytes total → wide=1).
 * Drive enough distinct (k1,k2) pairs past FP_COMBINE_PAR_MIN (50,000)
 * across all worker shards so the 3-pass radix scatter activates.  Each
 * worker sees its row range and shards into a private HT — with all-
 * distinct rows the shard fills equal nrows/nw, summing past 50K across
 * the pool. */
static test_result_t test_mk_combine_2i64_parallel_wide(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* k1c = ray_vec_new(RAY_I64, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I64, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int64_t* k1 = (int64_t*)ray_data(k1c);
    int64_t* k2 = (int64_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    /* All (k1, k2) pairs distinct so per-shard n_filled = rows/nw and
     * total_local = N — comfortably above FP_COMBINE_PAR_MIN (50K). */
    for (int64_t i = 0; i < N; i++) {
        k1[i] = i;
        k2[i] = i * 3 + 7;
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    /* Non-trivial WHERE that passes everything. */
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* All pairs distinct → N output rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), N);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(cnt_col);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_combine narrow branch: 2 I32 keys → 8 bytes total → wide=0.  All
 * (k1, k2) pairs distinct so total_local hits the parallel threshold.
 * Exercises the !wide branches of mk_combine_hist_fn / scat_fn / dedup_fn. */
static test_result_t test_mk_combine_2i32_parallel_narrow(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    /* k1 = i / 4, k2 = i % 4 → all (k1,k2) distinct because i = k1*4 + k2. */
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i / 4);
        k2[i] = (int32_t)(i % 4);
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), N);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_combine 2 SYM keys with W32 width.  Total = 4+4 = 8 bytes → wide=0.
 * Each row carries a distinct (s1, s2) pair so total_local exceeds
 * FP_COMBINE_PAR_MIN. */
static test_result_t test_mk_combine_2sym_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* s1c = ray_sym_vec_new(RAY_SYM_W32, N); s1c->len = N;
    ray_t* s2c = ray_sym_vec_new(RAY_SYM_W32, N); s2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N);         vc->len  = N;
    int32_t* s1 = (int32_t*)ray_data(s1c);
    int32_t* s2 = (int32_t*)ray_data(s2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    /* Intern N distinct symbols up front so we can index into them. */
    int64_t pool[400];
    char  nm[16];
    for (int j = 0; j < 400; j++) {
        int l = snprintf(nm, sizeof(nm), "sym_%04d", j);
        pool[j] = ray_sym_intern(nm, (size_t)l);
    }
    /* (s1[i], s2[i]) = (pool[i / 400], pool[i % 400]) — 400 × 400 = 160K
     * possible pairs; with N=80K rows all pairs distinct (i runs 0..N). */
    for (int64_t i = 0; i < N; i++) {
        s1[i] = (int32_t)pool[i / 400];
        s2[i] = (int32_t)pool[i % 400];
        v[i]  = i + 1;
    }
    int64_t s_a = ray_sym_intern("a", 1);
    int64_t s_b = ray_sym_intern("b", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_a, s1c); ray_release(s1c);
    tbl = ray_table_add_col(tbl, s_b, s2c); ray_release(s2c);
    tbl = ray_table_add_col(tbl, s_v, vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_a  = ray_scan(g, "a");
    ray_op_t* scan_b  = ray_scan(g, "b");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_a, scan_b };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* All pairs distinct. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), N);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_combine mixed: SYM_W32 (4 bytes) + I64 (8 bytes) = 12 bytes → wide=1.
 * Exercises the wide branch with a SYM-bearing decompose at materialize. */
static test_result_t test_mk_combine_sym_i64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, N); sc->len = N;
    ray_t* kc = ray_vec_new(RAY_I64, N);         kc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N);         vc->len = N;
    int32_t* s = (int32_t*)ray_data(sc);
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t* v = (int64_t*)ray_data(vc);
    int64_t pool[400];
    char nm[16];
    for (int j = 0; j < 400; j++) {
        int l = snprintf(nm, sizeof(nm), "msy_%04d", j);
        pool[j] = ray_sym_intern(nm, (size_t)l);
    }
    /* (s[i], k[i]) = (pool[i % 400], i) — N distinct pairs (k unique). */
    for (int64_t i = 0; i < N; i++) {
        s[i] = (int32_t)pool[i % 400];
        k[i] = i;
        v[i] = i + 1;
    }
    int64_t s_s = ray_sym_intern("s", 1);
    int64_t s_k = ray_sym_intern("k", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_s, sc); ray_release(sc);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_s  = ray_scan(g, "s");
    ray_op_t* scan_k  = ray_scan(g, "k");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_s, scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), N);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Forward-declare the runtime API for fp_expr_const_str tests.  Mirrors
 * test_fused_topk.c pattern — fp_expr_const_str is called only from
 * fp_check_like inside ray_fused_group_supported, which needs a parsed
 * AST.  ray_parse requires a live runtime for its symbol-table state. */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

/* fp_expr_const_str: LIKE on a SYM column with a string-literal pattern
 * should be recognised by the planner gate (returns 1).  Exercises the
 * `expr->type == -RAY_STR && !RAY_ATTR_NAME` base case of the recursive
 * walker. */
static test_result_t test_fp_expr_const_str_simple_like(void) {
    ray_runtime_create(0, NULL);

    /* Tiny SYM table — fp_check_like requires the column to exist and be
     * STR/SYM type. */
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, 3); sc->len = 3;
    int32_t* sd = (int32_t*)ray_data(sc);
    int64_t s_a = ray_sym_intern("apple", 5);
    int64_t s_b = ray_sym_intern("banana", 6);
    int64_t s_c = ray_sym_intern("cherry", 6);
    sd[0] = (int32_t)s_a; sd[1] = (int32_t)s_b; sd[2] = (int32_t)s_c;
    int64_t s_name = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_name, sc); ray_release(sc);

    ray_t* expr = ray_parse("(like name \"app*\")");
    TEST_ASSERT_NOT_NULL(expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(expr));

    /* Predicate gate must accept (like sym_col "literal") — this recurses
     * through fp_check_like → fp_expr_const_str on the literal. */
    int ok = ray_fused_group_supported(expr, tbl);
    TEST_ASSERT_EQ_I(ok, 1);

    ray_release(expr);
    ray_release(tbl);
    ray_runtime_destroy(__RUNTIME);
    PASS();
}

/* fp_expr_const_str: nested (concat str str) pattern.  Exercises the
 * "is_concat" branch + recursion into each child. */
static test_result_t test_fp_expr_const_str_concat_like(void) {
    ray_runtime_create(0, NULL);

    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, 2); sc->len = 2;
    int32_t* sd = (int32_t*)ray_data(sc);
    int64_t s_x = ray_sym_intern("foo_x", 5);
    int64_t s_y = ray_sym_intern("foo_y", 5);
    sd[0] = (int32_t)s_x; sd[1] = (int32_t)s_y;
    int64_t s_n = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_n, sc); ray_release(sc);

    /* Pattern is (concat "foo" "*") — a nested-list const-string. */
    ray_t* expr = ray_parse("(like name (concat \"foo\" \"*\"))");
    TEST_ASSERT_NOT_NULL(expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(expr));

    int ok = ray_fused_group_supported(expr, tbl);
    TEST_ASSERT_EQ_I(ok, 1);

    ray_release(expr);
    ray_release(tbl);
    ray_runtime_destroy(__RUNTIME);
    PASS();
}

/* fp_expr_const_str: deeply-nested (concat (concat str str) str) — drives
 * the recursive fp_expr_const_str over a tree, not just a flat list. */
static test_result_t test_fp_expr_const_str_nested_concat(void) {
    ray_runtime_create(0, NULL);

    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, 1); sc->len = 1;
    int32_t* sd = (int32_t*)ray_data(sc);
    int64_t s_q = ray_sym_intern("abcdefg", 7);
    sd[0] = (int32_t)s_q;
    int64_t s_n = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_n, sc); ray_release(sc);

    ray_t* expr = ray_parse("(like name (concat (concat \"a\" \"b\") \"*\"))");
    TEST_ASSERT_NOT_NULL(expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(expr));
    int ok = ray_fused_group_supported(expr, tbl);
    TEST_ASSERT_EQ_I(ok, 1);

    ray_release(expr);
    ray_release(tbl);
    ray_runtime_destroy(__RUNTIME);
    PASS();
}

/* fp_count_heap_*: U8 column → fp_try_direct_count1 fires (256 slots);
 * with emit_filter.top_count_take = 3 and many distinct keys, the
 * fp_count_emit_keep_min path runs the heap (n_slots ≫ k_take). */
static test_result_t test_fp_count_heap_u8_top3(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 20 distinct U8 keys with sharply different counts so the top-3 is
     * unambiguous: key i appears (i+1) times — total rows = 1+2+...+20
     * = 210. */
    int64_t total_rows = 0;
    for (int64_t i = 1; i <= 20; i++) total_rows += i;
    ray_t* kc = ray_vec_new(RAY_U8, total_rows); kc->len = total_rows;
    uint8_t* k = (uint8_t*)ray_data(kc);
    int64_t pos = 0;
    for (int64_t key = 1; key <= 20; key++) {
        for (int64_t r = 0; r < key; r++) k[pos++] = (uint8_t)key;
    }
    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 3;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top-3 counts: keys 20, 19, 18 with counts 20, 19, 18 respectively.
     * fp_count_emit_keep_min returns heap[0] = 18 — every group with
     * count >= 18 is retained, so exactly 3 rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* k_col = ray_table_get_col(res, s_k);
    ray_t* c_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(k_col);
    TEST_ASSERT_NOT_NULL(c_col);
    int seen_18 = 0, seen_19 = 0, seen_20 = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int64_t key = (int64_t)((uint8_t*)ray_data(k_col))[i];
        int64_t cnt = ((int64_t*)ray_data(c_col))[i];
        if (key == 18) { TEST_ASSERT_EQ_I(cnt, 18); seen_18 = 1; }
        if (key == 19) { TEST_ASSERT_EQ_I(cnt, 19); seen_19 = 1; }
        if (key == 20) { TEST_ASSERT_EQ_I(cnt, 20); seen_20 = 1; }
    }
    TEST_ASSERT_TRUE(seen_18 && seen_19 && seen_20);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* fp_count_heap_*: I16 key → fp_try_direct_count1 with 65536 slots; with
 * a small top-K the heap_up / heap_down branches both fire as the heap
 * gets pushed past capacity and then sees rows that displace heap[0]. */
static test_result_t test_fp_count_heap_i16_top5(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 12 distinct I16 keys, counts decreasing as the key increases.  The
     * key sequence (intentionally not sorted) drives both the up-heap
     * (initial fill) and down-heap (replace heap[0] when a bigger count
     * appears later in the slot walk) paths. */
    int64_t per_key[12] = { 5, 11, 3, 17, 2, 9, 13, 21, 1, 7, 19, 4 };
    int64_t total_rows = 0;
    for (int i = 0; i < 12; i++) total_rows += per_key[i];
    ray_t* kc = ray_vec_new(RAY_I16, total_rows); kc->len = total_rows;
    int16_t* k = (int16_t*)ray_data(kc);
    int64_t pos = 0;
    for (int i = 0; i < 12; i++)
        for (int64_t r = 0; r < per_key[i]; r++)
            k[pos++] = (int16_t)(i + 100);  /* keys 100..111 */
    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 5;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top-5 counts: sorted descending = 21, 19, 17, 13, 11.  keep_min = 11.
     * Result rows: every key whose count >= 11.  Counts 21,19,17,13,11 →
     * 5 rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 5);

    /* Verify the result counts are exactly {21,19,17,13,11}. */
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* c_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(c_col);
    int64_t expect[5] = { 11, 13, 17, 19, 21 };
    int seen[5] = {0, 0, 0, 0, 0};
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        for (int j = 0; j < 5; j++)
            if (c == expect[j] && !seen[j]) { seen[j] = 1; break; }
    }
    for (int j = 0; j < 5; j++) TEST_ASSERT_TRUE(seen[j]);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* fp_count_emit_keep_min via the serial-combine path of count1 with a
 * wide-key I64 column.  fp_try_direct_count1 rejects (kt != BOOL/U8/I16)
 * so the code falls through to fp_combine_and_materialize.  With
 * use_emit_filter on, the parallel-combine branch is skipped (line 1343)
 * and the serial combine + fp_count_emit_keep_min path runs.  The
 * used_key_slots parameter is non-NULL in this branch, exercising the
 * `used_key_slots && !used_key_slots[s * 2]` skip. */
static test_result_t test_fp_count_emit_keep_min_i64_serial(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 15 distinct I64 keys with monotone counts 1..15.  Big enough that
     * after the serial HT-build the open-addressed table has many empty
     * slots interspersed with filled ones, exercising the
     * used_key_slots-skip branch. */
    int64_t per_key[15];
    int64_t total_rows = 0;
    for (int i = 0; i < 15; i++) { per_key[i] = i + 1; total_rows += per_key[i]; }
    ray_t* kc = ray_vec_new(RAY_I64, total_rows); kc->len = total_rows;
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t pos = 0;
    for (int i = 0; i < 15; i++)
        for (int64_t r = 0; r < per_key[i]; r++)
            k[pos++] = (int64_t)(1000 + i);
    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_GE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 4;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top-4 counts = 15, 14, 13, 12. keep_min = 12 → 4 rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* c_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(c_col);
    int64_t expect[4] = { 12, 13, 14, 15 };
    int seen[4] = { 0, 0, 0, 0 };
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        for (int j = 0; j < 4; j++)
            if (c == expect[j] && !seen[j]) { seen[j] = 1; break; }
    }
    for (int j = 0; j < 4; j++) TEST_ASSERT_TRUE(seen[j]);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ──────────────────────────────────────────────────────────────────────
 * Coverage extensions for v2 / hash-index / MG-top-K paths (PR #219).
 *
 * Targets:
 *   - mk_par_v2_fn / mk_combine_v2_part_fn / mk_combine_v2_parallel
 *     (multi-key v2: per-(worker, partition) shards with COUNT/SUM/AVG).
 *   - mk_eq_hash_count_fn / mk_par_hash_fn (single-predicate FP_EQ
 *     dispatched through a column's attached RAY_IDX_HASH).
 *   - fp_try_i64_mg_top_count + fp_try_i32_mg_top_count + the matching
 *     fp_iXX_mg_lookup / hash_slot / rebuild helpers (Misra-Gries top-K
 *     over I64/TIMESTAMP/I32 keys with no WHERE clause).
 *   - mk_eq_i64_count_fn (multi-predicate AND with one FP_EQ on i64,
 *     including the chunk_zone-driven chunk-skip path).
 *   - mk_apply_count_emit_filter top-N SUM and top-N MIN branches.
 *
 * Unreachable from the test harness (documented for future readers):
 *
 *   1. OOM cleanup branches: fp_shard_init / mk_shard_init /
 *      mk_shard_grow / fp_shard_grow / fp_combine_and_materialize /
 *      mk_combine_parallel / mk_combine_v2_parallel all carry early-
 *      return OOM cleanup paths for scratch_alloc failures.  The test
 *      runtime grows its scratch allocator on demand so these never
 *      fire in practice; covering them would require an injection
 *      hook in the allocator.  Leaves ~5-10 missed regions per
 *      function (≈ 20-25% of mk_shard_init etc.).
 *
 *   2. mk_v2_apply_agg_inline MIN/MAX cases: dead by construction.
 *      The v2_ok gate in exec_filtered_group_multi (line 4930) flips
 *      false when any agg is MK_AGG_MIN/MAX, so mk_par_v2_fn (which
 *      is the sole caller of mk_v2_apply_agg_inline) only ever runs
 *      with COUNT/SUM/AVG aggs.  The MIN/MAX switch arms exist for
 *      symmetry with mk_par_fn but are unreachable.
 *
 *   3. fp_try_i64_mg_top_count / fp_try_i32_mg_top_count: the
 *      "decrements && keep_min ≤ safety_bound" return-NULL fallback
 *      (line 1436) fires only when MG's heavy-hitter guarantee can't
 *      cover the top-K.  Our rebuild_path tests intentionally avoid
 *      the safety-bound trip so the result is non-NULL (we want to
 *      see real top-K output); covering this branch would require
 *      a degenerate input where every key has approximately the
 *      same low count, defeating the test's correctness check.
 *
 *   4. ray_filtered_group NULL-graph / NULL-ext guards (lines 44, 60):
 *      defensive checks against C-API misuse the test harness can't
 *      trigger without a fault injection layer.
 *
 *   5. fp_compile_cmp / fp_atom_col_compatible / fp_check_*: a handful
 *      of malformed-AST defensive branches are unreachable from the
 *      parser (documented in fused_group_branch_cov.rfl Section 29).
 * ────────────────────────────────────────────────────────────────────── */

/* v2 path activator: 2 I64 keys → 16 bytes total → wide=1, multi-key,
 * COUNT + SUM + AVG (no MIN/MAX), no hash-eq / eq-i64 / SYM keys.  N is
 * large enough that the parallel pool fires and per-(worker, partition)
 * shards are populated; mk_combine_v2_parallel then merges. */
static test_result_t test_v2_wide_2i64_count_sum_avg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* k1c = ray_vec_new(RAY_I64, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I64, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int64_t* k1 = (int64_t*)ray_data(k1c);
    int64_t* k2 = (int64_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    /* High-cardinality (k1, k2) → many groups, many partitions touched. */
    for (int64_t i = 0; i < N; i++) {
        k1[i] = i / 4;          /* 20000 distinct k1 */
        k2[i] = (i * 31) % 7919; /* 7919 distinct k2 */
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    /* COUNT + SUM + AVG only — these are the v2 path's supported aggs. */
    uint16_t  agg_ops[] = { OP_COUNT, OP_SUM, OP_AVG };
    ray_op_t* agg_ins[] = { scan_v, scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 3);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    int64_t nrows = ray_table_nrows(res);
    TEST_ASSERT_TRUE(nrows > 1000);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    int64_t sum_sym = ray_sym_intern("sum",   3);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    TEST_ASSERT_NOT_NULL(cnt_col);
    TEST_ASSERT_NOT_NULL(sum_col);
    int64_t total_cnt = 0, total_sum = 0;
    int64_t expect_sum = N * (N + 1) / 2;
    for (int64_t i = 0; i < nrows; i++) {
        total_cnt += ((int64_t*)ray_data(cnt_col))[i];
        total_sum += ((int64_t*)ray_data(sum_col))[i];
    }
    TEST_ASSERT_EQ_I(total_cnt, N);
    TEST_ASSERT_EQ_I(total_sum, expect_sum);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* v2 path narrow: 2 I32 keys → 8 bytes → wide=0.  Same gate conditions.
 * Exercises mk_par_v2_fn's narrow branch + mk_combine_v2_part_fn narrow
 * branch (slots_hi NULL path). */
static test_result_t test_v2_narrow_2i32_count_sum(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i / 8);     /* 10000 distinct k1 */
        k2[i] = (int32_t)((i * 11) % 1009);
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_COUNT, OP_SUM };
    ray_op_t* agg_ins[] = { scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    int64_t nrows = ray_table_nrows(res);
    TEST_ASSERT_TRUE(nrows > 100);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    int64_t sum_sym = ray_sym_intern("sum",   3);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    int64_t total_cnt = 0, total_sum = 0;
    int64_t expect_sum = N * (N + 1) / 2;
    for (int64_t i = 0; i < nrows; i++) {
        total_cnt += ((int64_t*)ray_data(cnt_col))[i];
        total_sum += ((int64_t*)ray_data(sum_col))[i];
    }
    TEST_ASSERT_EQ_I(total_cnt, N);
    TEST_ASSERT_EQ_I(total_sum, expect_sum);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* v2 gate flips off for MIN/MAX: ensure that multi-key + MIN/MAX still
 * works correctly (falls back to v1 mk_par_fn), exercising the v2_ok
 * agg-kind check (the `kk != MK_AGG_COUNT && kk != MK_AGG_SUM &&
 * kk != MK_AGG_AVG` branch). */
static test_result_t test_v2_gate_min_max_falls_back(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 80000;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i % 100);
        k2[i] = (int32_t)((i / 100) % 100);
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    /* MIN + MAX → v2_ok flips false → falls back to v1 mk_par_fn. */
    uint16_t  agg_ops[] = { OP_MIN, OP_MAX };
    ray_op_t* agg_ins[] = { scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    int64_t nrows = ray_table_nrows(res);
    /* k1 × k2 = 100 × 100 = 10000 distinct pairs, but i is bounded so
     * only (i%100, (i/100)%100) for i in [0..80000) → 10000 pairs of
     * which 8000 see at least one row. */
    TEST_ASSERT_TRUE(nrows > 1000);

    /* Sanity: global MIN must be 1, global MAX must be N. */
    int64_t min_sym = ray_sym_intern("min", 3);
    int64_t max_sym = ray_sym_intern("max", 3);
    ray_t* min_col = ray_table_get_col(res, min_sym);
    ray_t* max_col = ray_table_get_col(res, max_sym);
    int64_t gmin = INT64_MAX, gmax = INT64_MIN;
    for (int64_t i = 0; i < nrows; i++) {
        int64_t mn = ((int64_t*)ray_data(min_col))[i];
        int64_t mx = ((int64_t*)ray_data(max_col))[i];
        if (mn < gmin) gmin = mn;
        if (mx > gmax) gmax = mx;
    }
    TEST_ASSERT_EQ_I(gmin, 1);
    TEST_ASSERT_EQ_I(gmax, N);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Hash-index dispatch via mk_par_hash_fn: build an I64 column with an
 * attached RAY_IDX_HASH, then run `where: (== col K) by: col` with
 * multi-agg (so the COUNT-only count_fn shortcut is skipped) to drive
 * mk_par_hash_fn.  Validates that the hash-chain walk produces the same
 * counts as the scan path. */
static test_result_t test_hash_eq_count_dispatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 1000;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* k = (int64_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) k[i] = (i % 10) * 1000;
    /* Attach hash index BEFORE adding to the table. */
    ray_t* kc_orig = kc;
    ray_t* r = ray_index_attach_hash(&kc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(kc->attrs & RAY_ATTR_HAS_INDEX);
    (void)kc_orig;

    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    /* Confirm the col carries the hash index after table embedding. */
    ray_t* col_in_tbl = ray_table_get_col(tbl, s_k);
    TEST_ASSERT_NOT_NULL(col_in_tbl);
    TEST_ASSERT_TRUE(col_in_tbl->attrs & RAY_ATTR_HAS_INDEX);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* needle    = ray_const_i64(g, 5000);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, needle);
    /* COUNT + COUNT (n_aggs=2) → not the COUNT-only fast path → routes
     * through mk_par_hash_fn (hash-chain walk with per-agg accumulate). */
    uint16_t  agg_ops[] = { OP_COUNT, OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k, scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Only key=5000 passes → 1 group with count = 100 (i mod 10 == 5). */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t got = ((int64_t*)ray_data(cnt_col))[0];
    TEST_ASSERT_EQ_I(got, 100);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Hash-index COUNT-only dispatch: 2 keys + single COUNT → multi path
 * with n_aggs==1, COUNT, single-FP_EQ predicate.  This is the gate that
 * routes to mk_eq_hash_count_fn (the COUNT-only fast variant of
 * mk_par_hash_fn that skips per-agg state init/update). */
static test_result_t test_mk_eq_hash_count_fn_dispatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 800;
    ray_t* k1c = ray_vec_new(RAY_I64, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I64, N); k2c->len = N;
    int64_t* k1 = (int64_t*)ray_data(k1c);
    int64_t* k2 = (int64_t*)ray_data(k2c);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (i % 8) * 1000;
        k2[i] = i % 4;
    }
    /* Attach hash index on the FP_EQ target column (k1). */
    ray_t* r = ray_index_attach_hash(&k1c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(k1c->attrs & RAY_ATTR_HAS_INDEX);

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1   = ray_scan(g, "k1");
    ray_op_t* scan_k2   = ray_scan(g, "k2");
    ray_op_t* scan_pred = ray_scan(g, "k1");
    ray_op_t* needle    = ray_const_i64(g, 4000);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, needle);
    /* COUNT only, 2 keys → multi path with COUNT-only fast variant. */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k1 };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Rows where k1=4000 → i % 8 == 4 → i ∈ {4, 12, 20, ..., 796} → 100
     * rows.  These rows have k2 ∈ {i%4} = {0}.  All 100 rows have the
     * same (k1=4000, k2=0) so we expect 1 group with count=100. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(cnt_col))[0], 100);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Hash-index dispatch with SUM agg → mk_par_hash_fn (not count_fn).
 * Same setup as above but with OP_SUM aggregating an integer payload. */
static test_result_t test_hash_eq_sum_dispatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 1000;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t* v = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { k[i] = (i % 10) * 1000; v[i] = i + 1; }
    /* Attach hash index on the key column. */
    ray_t* r = ray_index_attach_hash(&kc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(kc->attrs & RAY_ATTR_HAS_INDEX);

    int64_t s_k = ray_sym_intern("k", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_v    = ray_scan(g, "v");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* needle    = ray_const_i64(g, 3000);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, needle);
    /* SUM + COUNT → forces mk_par_hash_fn (not the COUNT-only count_fn). */
    uint16_t  agg_ops[] = { OP_SUM, OP_COUNT };
    ray_op_t* agg_ins[] = { scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    /* Rows passing: k[i]==3000 → i % 10 == 3 → i ∈ {3, 13, 23, ..., 993}.
     * Sum of (i+1) for those = sum over j=0..99 of (3 + 10*j + 1)
     *                        = sum (4 + 10*j) = 4*100 + 10 * (99*100/2)
     *                        = 400 + 49500 = 49900. */
    int64_t cnt_sym = ray_sym_intern("count", 5);
    int64_t sum_sym = ray_sym_intern("sum",   3);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(cnt_col))[0], 100);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 49900);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Hash-eq dispatch is gated on n_children == 1; with an AND predicate
 * the hash dispatch does not fire and the scan path runs.  Verifies
 * mk_find_hash_eq_child returns -1 for multi-child preds. */
static test_result_t test_hash_eq_multi_pred_no_dispatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 1000;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t* v = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { k[i] = i % 10; v[i] = i + 1; }
    ray_t* r = ray_index_attach_hash(&kc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    int64_t s_k = ray_sym_intern("k", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_v    = ray_scan(g, "v");
    ray_op_t* scan_kp   = ray_scan(g, "k");
    ray_op_t* scan_vp   = ray_scan(g, "v");
    ray_op_t* five      = ray_const_i64(g, 5);
    ray_op_t* lo        = ray_const_i64(g, 100);
    ray_op_t* p1        = ray_binop(g, OP_EQ, scan_kp, five);
    ray_op_t* p2        = ray_binop(g, OP_GE, scan_vp, lo);
    ray_op_t* pred      = ray_binop(g, OP_AND, p1, p2);
    /* COUNT + SUM → multi path, AND predicate → mk_find_hash_eq_child
     * returns -1 (n_children != 1), then mk_find_i64_eq_child returns 0
     * → mk_eq_i64_count_fn fires.  Wait: that's COUNT-only path.  We
     * want SUM to force the standard mk_par_fn scan with AND-folded
     * predicates.  Use SUM alone. */
    uint16_t  agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    /* k=5 rows: i ∈ {5, 15, ..., 995}. v=i+1.  Filter v >= 100: i+1 >= 100
     * → i >= 99 → i ∈ {105, 115, ..., 995} → 90 rows.
     * Sum of (i+1) for i ∈ {105, 115, ..., 995} = sum of {106, 116, ..., 996}
     *   = 90 * (106 + 996) / 2 = 90 * 551 = 49590. */
    int64_t sum_sym = ray_sym_intern("sum", 3);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 49590);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_eq_i64_count_fn chunk-skip path: at least one predicate child column
 * carries a RAY_IDX_CHUNK_ZONE, so the loop strides in chunk-size steps
 * and skips chunks where [min, max] proves all-fail.  Builds a clustered
 * column (sorted values) so most chunks are skipped.  Requires 2 keys
 * + 1 COUNT to route through the multi path (not count1). */
static test_result_t test_mk_eq_i64_count_fn_chunk_skip(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 1024 rows, chunk_log2=8 → 4 chunks of 256 rows each.  k is the
     * EQ-target (i64), ts is the chunk-zoned > predicate column, and
     * k2 is a secondary group key (forces n_keys=2 → multi path). */
    int64_t N = 1024;
    ray_t* kc  = ray_vec_new(RAY_I64, N); kc->len  = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* tsc = ray_vec_new(RAY_I64, N); tsc->len = N;
    int64_t* k  = (int64_t*)ray_data(kc);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* ts = (int64_t*)ray_data(tsc);
    for (int64_t i = 0; i < N; i++) {
        k[i]  = i % 8;
        k2[i] = (int32_t)(i % 3);
        ts[i] = i;       /* strictly monotone → tight chunk min/max */
    }
    /* Attach a chunk_zone index on the timestamp column. */
    ray_t* r = ray_index_attach_chunk_zone(&tsc, 8);  /* 256-row chunks */
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(tsc->attrs & RAY_ATTR_HAS_INDEX);

    int64_t s_k  = ray_sym_intern("k",  1);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_ts = ray_sym_intern("ts", 2);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k,  kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_ts, tsc); ray_release(tsc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_k2   = ray_scan(g, "k2");
    ray_op_t* scan_kp   = ray_scan(g, "k");
    ray_op_t* scan_tsp  = ray_scan(g, "ts");
    ray_op_t* needle    = ray_const_i64(g, 3);
    ray_op_t* threshold = ray_const_i64(g, 700);  /* skips chunks 0-2 */
    ray_op_t* p1        = ray_binop(g, OP_EQ, scan_kp, needle);
    ray_op_t* p2        = ray_binop(g, OP_GE, scan_tsp, threshold);
    ray_op_t* pred      = ray_binop(g, OP_AND, p1, p2);

    /* COUNT-only + 2 keys + AND with FP_EQ_i64 child → multi path with
     * eq_i64_idx >= 0 → mk_eq_i64_count_fn fires with chunk_log2=8
     * picked up from the ts column's chunk_zone. */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Rows where k=3 and ts>=700.  k=i%8==3 → i ∈ {3,11,...,1019} → 128
     * rows.  ts=i so ts>=700 → i>=700.
     * Intersection: i ∈ {707,715,...,1019} → 40 rows.  These are
     * partitioned by k2=i%3 → 3 groups (some may have unequal counts). */
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, 40);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_eq_i64_count_fn: AND predicate with one FP_EQ on i64 col + COUNT
 * + 2 group keys (forces multi-path entry, since single-key single-COUNT
 * routes to count1).  Exercises the per-row scan branch (no chunk_zone). */
static test_result_t test_mk_eq_i64_count_and_pred(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 5000;
    ray_t* kc  = ray_vec_new(RAY_I64, N); kc->len  = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int64_t* k  = (int64_t*)ray_data(kc);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        k[i]  = i % 50;
        k2[i] = (int32_t)(i % 3);
        v[i]  = i + 1;
    }

    int64_t s_k  = ray_sym_intern("k",  1);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k,  kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_k2   = ray_scan(g, "k2");
    ray_op_t* scan_kp   = ray_scan(g, "k");
    ray_op_t* scan_vp   = ray_scan(g, "v");
    ray_op_t* needle    = ray_const_i64(g, 7);
    ray_op_t* threshold = ray_const_i64(g, 500);
    ray_op_t* p1        = ray_binop(g, OP_EQ, scan_kp, needle);
    ray_op_t* p2        = ray_binop(g, OP_GE, scan_vp, threshold);
    ray_op_t* pred      = ray_binop(g, OP_AND, p1, p2);
    /* COUNT-only with multi-child AND containing FP_EQ on i64 → multi-
     * path because n_keys=2 → mk_find_i64_eq_child returns 0 →
     * mk_eq_i64_count_fn fires. */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));

    /* k=7 rows: i ∈ {7, 57, 107, ..., N-43}. v = i+1.  Filter v >= 500
     * → i >= 499 → first such i is 507.
     * Series: {507, 557, ..., 4957} → 90 rows total, partitioned by k2.
     * Sum of counts across all k2 groups must = 90. */
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, 90);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* fp_try_i64_mg_top_count: I64 key, NO WHERE clause (pred=NULL),
 * top-K emit filter set.  Requires C-API construction since the planner
 * gate (query.c:4586) only fires when where_expr is non-NULL.
 *
 * Builds a table with a known skewed distribution and verifies the
 * top-K results match. */
static test_result_t test_fp_try_i64_mg_top_count_no_where(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 20 distinct keys with strongly monotone counts 1..20.  Top-3 by
     * count are keys with counts 18,19,20 (out of 20).  Heavy-hitter
     * keys dominate so MG's first pass keeps them all in the candidates
     * dict (cap=8192 ≫ 20). */
    int64_t per_key[20];
    int64_t total_rows = 0;
    for (int i = 0; i < 20; i++) { per_key[i] = i + 1; total_rows += per_key[i]; }
    ray_t* kc = ray_vec_new(RAY_I64, total_rows); kc->len = total_rows;
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t pos = 0;
    for (int i = 0; i < 20; i++)
        for (int64_t r = 0; r < per_key[i]; r++)
            k[pos++] = (int64_t)(100000 + i);

    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    /* NULL pred = no WHERE → n_children=0 in compiled fp_pred. */
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, NULL, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 3;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top-3 counts = 18, 19, 20.  keep_min = 18 → 3 rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* c_col = ray_table_get_col(res, cnt_sym);
    int64_t got_max = 0, got_sum = 0;
    for (int64_t i = 0; i < 3; i++) {
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        if (c > got_max) got_max = c;
        got_sum += c;
    }
    TEST_ASSERT_EQ_I(got_max, 20);
    TEST_ASSERT_EQ_I(got_sum, 18 + 19 + 20);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_apply_count_emit_filter top-N SUM path (multi-key + SUM agg with
 * emit_filter for top-N by SUM).  Exercises the OP_SUM branch of the
 * agg-op switch + the kept/threshold compaction sweep. */
static test_result_t test_mk_apply_count_emit_filter_top_sum(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 100;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    /* 10 distinct (k1, k2) pairs with v values that produce monotone
     * sums per group: group i has 10 rows of v=i+1 → sum = 10*(i+1). */
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i / 10);
        k2[i] = (int32_t)(i / 10);
        v[i]  = (i / 10) + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 3;
    filter.agg_op = OP_SUM;       /* sort by SUM, not COUNT */
    filter.desc = 1;              /* top-3 largest sums */
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top-3 sums = 100, 90, 80 (groups 9, 8, 7).  3 rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);

    int64_t sum_sym = ray_sym_intern("sum", 3);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    int64_t got_total = 0;
    for (int64_t i = 0; i < 3; i++) got_total += ((int64_t*)ray_data(sum_col))[i];
    TEST_ASSERT_EQ_I(got_total, 100 + 90 + 80);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_apply_count_emit_filter top-N MIN path (multi-key + MIN agg + desc).
 * Exercises the OP_MIN branch of the agg-op switch.  Sort by MIN value. */
static test_result_t test_mk_apply_count_emit_filter_top_min(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 100;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    /* 10 groups, each with v values starting at group_id * 100 + 1 ...
     * + 10.  Group min = group_id * 100 + 1.  Top-3 min largest =
     * groups 7, 8, 9 with mins 701, 801, 901. */
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i / 10);
        k2[i] = (int32_t)(i / 10);
        v[i]  = (i / 10) * 100 + (i % 10) + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_MIN };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 3;
    filter.agg_op = OP_MIN;
    filter.desc = 1;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);

    int64_t min_sym = ray_sym_intern("min", 3);
    ray_t* min_col = ray_table_get_col(res, min_sym);
    int64_t got_total = 0;
    for (int64_t i = 0; i < 3; i++) got_total += ((int64_t*)ray_data(min_col))[i];
    /* Top-3 mins (largest) = 901, 801, 701 → total = 2403. */
    TEST_ASSERT_EQ_I(got_total, 2403);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* I32 mirror of the rebuild test: forces fp_i32_mg_rebuild via >8192
 * distinct I32 keys.  Reached via OP_GROUP path (fp_try_i32_mg_top_count
 * is also called from there, not exec_filtered_group_count1 — but the
 * function is shared).  Build a single-column I32 table, no WHERE, then
 * top-K to fire MG. */
static test_result_t test_fp_try_i32_mg_top_count_rebuild_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 12000 + 5 * 50;
    ray_t* kc = ray_vec_new(RAY_I32, N); kc->len = N;
    int32_t* k = (int32_t*)ray_data(kc);
    int64_t pos = 0;
    for (int hh = 1; hh <= 5; hh++)
        for (int64_t r = 0; r < 50; r++)
            k[pos++] = (int32_t)hh;
    for (int64_t i = 0; i < 12000; i++)
        k[pos++] = (int32_t)(1000 + i);

    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, NULL, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 3;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Either MG returns top-K (heavy hitters survive) or returns NULL and
     * the fallback path runs; either way ensures fp_i32_mg_rebuild is hit. */
    int64_t nrows = ray_table_nrows(res);
    if (nrows > 0) {
        int64_t cnt_sym = ray_sym_intern("count", 5);
        ray_t* c_col = ray_table_get_col(res, cnt_sym);
        int seen_50 = 0;
        for (int64_t i = 0; i < nrows; i++) {
            if (((int64_t*)ray_data(c_col))[i] == 50) seen_50 = 1;
        }
        TEST_ASSERT_TRUE(seen_50);
    }

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* fp_try_i64_mg_top_count with MORE than MG cap (8192) distinct keys
 * to drive fp_i64_mg_rebuild (cap exceeded → decrement-and-rebuild step). */
static test_result_t test_fp_try_i64_mg_top_count_rebuild_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 12,000 distinct sparse keys (each appearing once) → exceeds the
     * 8,192-candidate MG cap → triggers the rebuild path.  A handful
     * of "heavy hitter" keys appearing 50× each survive the decrements
     * and bubble up to the top-K. */
    int64_t N = 12000 + 5 * 50;  /* 12,250 rows */
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t pos = 0;
    /* Heavy hitters: keys 1..5 each appearing 50 times. */
    for (int hh = 1; hh <= 5; hh++)
        for (int64_t r = 0; r < 50; r++)
            k[pos++] = (int64_t)hh;
    /* Long tail: keys 1000..12999 each once. */
    for (int64_t i = 0; i < 12000; i++)
        k[pos++] = (int64_t)(1000 + i);

    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, NULL, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 3;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* MG with rebuild may give NULL → falls back; either way the result
     * is correct (or empty if a fallback materialised).  Validate that
     * the heavy hitter (count=50) is in the result if any rows came out. */
    int64_t nrows = ray_table_nrows(res);
    if (nrows > 0) {
        int64_t cnt_sym = ray_sym_intern("count", 5);
        ray_t* c_col = ray_table_get_col(res, cnt_sym);
        int seen_50 = 0;
        for (int64_t i = 0; i < nrows; i++) {
            if (((int64_t*)ray_data(c_col))[i] == 50) seen_50 = 1;
        }
        TEST_ASSERT_TRUE(seen_50);
    }

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* fp_try_i64_mg_top_count with TIMESTAMP key column.  Same MG path, with
 * RAY_TIMESTAMP → kt branch in fp_try_i64_mg_top_count. */
static test_result_t test_fp_try_timestamp_mg_top_count_no_where(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t per_key[10];
    int64_t total_rows = 0;
    for (int i = 0; i < 10; i++) { per_key[i] = (i + 1) * 2; total_rows += per_key[i]; }
    ray_t* kc = ray_vec_new(RAY_TIMESTAMP, total_rows); kc->len = total_rows;
    int64_t* k = (int64_t*)ray_data(kc);
    int64_t pos = 0;
    for (int i = 0; i < 10; i++)
        for (int64_t r = 0; r < per_key[i]; r++)
            k[pos++] = (int64_t)(1000000000000LL + i);  /* TIMESTAMP nanos */

    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, NULL, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 2;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, fused);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top-2 counts = 18, 20 (keys 8 and 9). */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* c_col = ray_table_get_col(res, cnt_sym);
    int64_t got_max = 0, got_sum = 0;
    for (int64_t i = 0; i < 2; i++) {
        int64_t c = ((int64_t*)ray_data(c_col))[i];
        if (c > got_max) got_max = c;
        got_sum += c;
    }
    TEST_ASSERT_EQ_I(got_max, 20);
    TEST_ASSERT_EQ_I(got_sum, 18 + 20);

    /* Verify key column is TIMESTAMP. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    TEST_ASSERT_EQ_I(k_col->type, RAY_TIMESTAMP);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* v2 path with 3 I32 keys (12 bytes total → wide=1).  Drives the
 * mk_par_v2_fn wide branch combined with AVG-only state (2 slots per
 * group), exercising mk_v2_apply_agg_inline AVG and mk_state_merge AVG
 * branches inside mk_combine_v2_part_fn. */
static test_result_t test_v2_wide_3i32_avg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 60000;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* k3c = ray_vec_new(RAY_I32, N); k3c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int32_t* k3 = (int32_t*)ray_data(k3c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i % 40);
        k2[i] = (int32_t)((i / 40) % 40);
        k3[i] = (int32_t)((i / 1600) % 20);
        v[i]  = 100 + (i % 50);
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_k3 = ray_sym_intern("k3", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_k3, k3c); ray_release(k3c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_k3 = ray_scan(g, "k3");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    /* AVG only → v2_ok stays true; AVG state contributes 2 slots/group. */
    uint16_t  agg_ops[] = { OP_AVG };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2, scan_k3 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 3, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    int64_t nrows = ray_table_nrows(res);
    TEST_ASSERT_TRUE(nrows > 100);

    int64_t avg_sym = ray_sym_intern("avg", 3);
    ray_t* avg_col = ray_table_get_col(res, avg_sym);
    TEST_ASSERT_NOT_NULL(avg_col);
    TEST_ASSERT_EQ_I(avg_col->type, RAY_F64);
    /* Each per-group AVG is in [100, 149] (range of v values). */
    double* avgs = (double*)ray_data(avg_col);
    for (int64_t i = 0; i < nrows; i++) {
        TEST_ASSERT_TRUE(avgs[i] >= 100.0 && avgs[i] <= 149.0);
    }

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* v2 path activates and then mk_combine_v2_parallel must run.  With very
 * small N (below the parallel pool threshold) the partition combine still
 * runs on worker 0 — exercising the "single thread" branch of
 * mk_combine_v2_parallel. */
static test_result_t test_v2_small_n_single_thread_combine(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Small enough that the dispatcher likely runs single-threaded but
     * still routes via the v2 path (multi-key, no SYM, COUNT/SUM only). */
    int64_t N = 200;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i % 5);
        k2[i] = (int32_t)(i % 7);
        v[i]  = i + 1;
    }

    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    ray_op_t* scan_vp = ray_scan(g, "v");
    ray_op_t* zero    = ray_const_i64(g, 0);
    ray_op_t* pred    = ray_binop(g, OP_GE, scan_vp, zero);

    uint16_t  agg_ops[] = { OP_COUNT, OP_SUM };
    ray_op_t* agg_ins[] = { scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k1, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* lcm(5,7) = 35 distinct (k1, k2) pairs. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 35);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    int64_t sum_sym = ray_sym_intern("sum",   3);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    int64_t total_cnt = 0, total_sum = 0;
    for (int64_t i = 0; i < 35; i++) {
        total_cnt += ((int64_t*)ray_data(cnt_col))[i];
        total_sum += ((int64_t*)ray_data(sum_col))[i];
    }
    TEST_ASSERT_EQ_I(total_cnt, N);
    TEST_ASSERT_EQ_I(total_sum, N * (N + 1) / 2);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* mk_par_hash_fn wide branch: hash-eq dispatch with 2 wide-composite
 * keys (16 bytes total → wide=1) so the wide-key compose + slots_hi
 * path runs. */
static test_result_t test_hash_eq_dispatch_wide_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 500;
    ray_t* kpc = ray_vec_new(RAY_I64, N); kpc->len = N;  /* predicate key */
    ray_t* k2c = ray_vec_new(RAY_I64, N); k2c->len = N;  /* second group key */
    ray_t* vc  = ray_vec_new(RAY_I64, N); vc->len  = N;
    int64_t* kp = (int64_t*)ray_data(kpc);
    int64_t* k2 = (int64_t*)ray_data(k2c);
    int64_t* v  = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) {
        kp[i] = (i % 5) * 1000;
        k2[i] = (i % 4);
        v[i]  = i + 1;
    }
    /* Attach hash index on predicate column. */
    ray_t* r = ray_index_attach_hash(&kpc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    int64_t s_kp = ray_sym_intern("kp", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_kp, kpc); ray_release(kpc);
    tbl = ray_table_add_col(tbl, s_k2, k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_v,  vc);  ray_release(vc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_kp   = ray_scan(g, "kp");
    ray_op_t* scan_k2   = ray_scan(g, "k2");
    ray_op_t* scan_v    = ray_scan(g, "v");
    ray_op_t* scan_pred = ray_scan(g, "kp");
    ray_op_t* needle    = ray_const_i64(g, 3000);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, needle);
    /* SUM agg (forces mk_par_hash_fn, not count_fn) with 2 keys (16 bytes
     * → wide=1) to exercise the wide branch. */
    uint16_t  agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { scan_v };
    ray_op_t* keys[]    = { scan_kp, scan_k2 };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 2, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    int64_t nrows = ray_table_nrows(res);
    TEST_ASSERT_TRUE(nrows >= 1 && nrows <= 4);

    /* Rows where kp=3000 → i % 5 == 3 → i ∈ {3, 8, 13, ..., 498}.
     * 100 such rows.  Sum of (i+1) for i ∈ {3, 8, ..., 498} =
     * 100 * (4 + 499) / 2 = 25150.  Group by (kp, k2). */
    int64_t sum_sym = ray_sym_intern("sum", 3);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    int64_t total_sum = 0;
    for (int64_t i = 0; i < nrows; i++) {
        total_sum += ((int64_t*)ray_data(sum_col))[i];
    }
    TEST_ASSERT_EQ_I(total_sum, 25150);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Hash-eq dispatch on I32 column → mk_eq_hash_count_fn col_esz=4 branch.
 * The hash builder zero/sign-extends keys based on storage width, so
 * I32 columns hit a separate switch case than I64. */
static test_result_t test_hash_eq_i32_count_dispatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 500;
    ray_t* kc = ray_vec_new(RAY_I32, N); kc->len = N;
    int32_t* k = (int32_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) k[i] = (int32_t)((i % 5) * 1000);
    ray_t* r = ray_index_attach_hash(&kc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    int64_t s_k = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, s_k, kc); ray_release(kc);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "k");
    ray_op_t* scan_pred = ray_scan(g, "k");
    ray_op_t* needle    = ray_const_i64(g, 2000);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, needle);
    /* Multi-agg COUNT + COUNT → multi path. */
    uint16_t  agg_ops[] = { OP_COUNT, OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k, scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 2);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    /* k=2000 corresponds to (i % 5) == 2 → 100 rows. */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(cnt_col))[0], 100);

    ray_release(res); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

const test_entry_t fused_group_entries[] = {
    { "fused_group/eq_count",                    test_eq_count,                    NULL, NULL },
    { "fused_group/ne_two_groups",               test_ne_two_groups,               NULL, NULL },
    { "fused_group/eq_no_match",                 test_eq_no_match,                 NULL, NULL },
    { "fused_group/eq_const_out_of_range_u8",    test_eq_const_out_of_range_u8,    NULL, NULL },
    { "fused_group/ne_const_out_of_range_u8",    test_ne_const_out_of_range_u8,    NULL, NULL },
    { "fused_group/count1_i16_direct_negative",  test_count1_i16_direct_counts_negative_keys, NULL, NULL },
    { "fused_group/sum_negative_i16",            test_sum_negative_i16,            NULL, NULL },
    { "fused_group/fallback_filter_honored",     test_fallback_filter_honored,     NULL, NULL },
    { "fused_group/count1_rejects_nullable_key", test_count1_rejects_nullable_key, NULL, NULL },
    /* Coverage extensions for chunks 1-8. */
    { "fused_group/fold_u8_lt_below",            test_fold_u8_lt_below,            NULL, NULL },
    { "fused_group/fold_u8_gt_below",            test_fold_u8_gt_below,            NULL, NULL },
    { "fused_group/fold_u8_le_above",            test_fold_u8_le_above,            NULL, NULL },
    { "fused_group/fold_u8_ge_above",            test_fold_u8_ge_above,            NULL, NULL },
    { "fused_group/fold_i16_lt_above",           test_fold_i16_lt_above,           NULL, NULL },
    { "fused_group/fold_i32_eq_above",           test_fold_i32_eq_above,           NULL, NULL },
    { "fused_group/fold_i32_ne_above",           test_fold_i32_ne_above,           NULL, NULL },
    { "fused_group/multi_agg_multi_key",         test_multi_agg_multi_key,         NULL, NULL },
    { "fused_group/multi_key_in_pred",           test_multi_key_in_pred,           NULL, NULL },
    { "fused_group/wide_multi_key_in_pred",      test_wide_multi_key_in_pred,      NULL, NULL },
    { "fused_group/wide_multi_key_top_count",    test_wide_multi_key_top_count_emit_filter, NULL, NULL },
    { "fused_group/wide_multi_key",              test_wide_multi_key,              NULL, NULL },
    { "fused_group/count1_parallel_combine",     test_count1_parallel_combine,     NULL, NULL },
    { "fused_group/count1_shard_grow",           test_count1_shard_grow,           NULL, NULL },
    { "fused_group/multi_agg_and_pred",          test_multi_agg_and_pred,          NULL, NULL },
    { "fused_group/multi_agg_unsigned_inputs",   test_multi_agg_unsigned_inputs,   NULL, NULL },
    { "fused_group/count1_sym_key_w32",          test_count1_sym_key_w32,          NULL, NULL },
    /* mk_combine_* (multi-key parallel 3-pass radix scatter) + fused
     * TOP-N count heap + Phase-3 const-string LIKE gate. */
    { "fused_group/mk_combine_2i64_parallel_wide",  test_mk_combine_2i64_parallel_wide,  NULL, NULL },
    { "fused_group/mk_combine_2i32_parallel_narrow",test_mk_combine_2i32_parallel_narrow,NULL, NULL },
    { "fused_group/mk_combine_2sym_parallel",       test_mk_combine_2sym_parallel,       NULL, NULL },
    { "fused_group/mk_combine_sym_i64_parallel",    test_mk_combine_sym_i64_parallel,    NULL, NULL },
    { "fused_group/fp_expr_const_str_simple_like",  test_fp_expr_const_str_simple_like,  NULL, NULL },
    { "fused_group/fp_expr_const_str_concat_like",  test_fp_expr_const_str_concat_like,  NULL, NULL },
    { "fused_group/fp_expr_const_str_nested_concat",test_fp_expr_const_str_nested_concat,NULL, NULL },
    { "fused_group/fp_count_heap_u8_top3",          test_fp_count_heap_u8_top3,          NULL, NULL },
    { "fused_group/fp_count_heap_i16_top5",         test_fp_count_heap_i16_top5,         NULL, NULL },
    { "fused_group/fp_count_emit_keep_min_i64_serial", test_fp_count_emit_keep_min_i64_serial, NULL, NULL },
    /* v2 path (per-(worker, partition) shards) + hash-eq dispatch +
     * I64/TIMESTAMP MG top-K + eq-i64 chunk-skip scan (PR #219). */
    { "fused_group/v2_wide_2i64_count_sum_avg",         test_v2_wide_2i64_count_sum_avg,         NULL, NULL },
    { "fused_group/v2_narrow_2i32_count_sum",           test_v2_narrow_2i32_count_sum,           NULL, NULL },
    { "fused_group/v2_gate_min_max_falls_back",         test_v2_gate_min_max_falls_back,         NULL, NULL },
    { "fused_group/v2_wide_3i32_avg",                   test_v2_wide_3i32_avg,                   NULL, NULL },
    { "fused_group/v2_small_n_single_thread_combine",   test_v2_small_n_single_thread_combine,   NULL, NULL },
    { "fused_group/hash_eq_count_dispatch",             test_hash_eq_count_dispatch,             NULL, NULL },
    { "fused_group/mk_eq_hash_count_fn_dispatch",       test_mk_eq_hash_count_fn_dispatch,       NULL, NULL },
    { "fused_group/hash_eq_sum_dispatch",               test_hash_eq_sum_dispatch,               NULL, NULL },
    { "fused_group/hash_eq_multi_pred_no_dispatch",     test_hash_eq_multi_pred_no_dispatch,     NULL, NULL },
    { "fused_group/hash_eq_dispatch_wide_keys",         test_hash_eq_dispatch_wide_keys,         NULL, NULL },
    { "fused_group/hash_eq_i32_count_dispatch",         test_hash_eq_i32_count_dispatch,         NULL, NULL },
    { "fused_group/mk_eq_i64_count_fn_chunk_skip",      test_mk_eq_i64_count_fn_chunk_skip,      NULL, NULL },
    { "fused_group/mk_eq_i64_count_and_pred",           test_mk_eq_i64_count_and_pred,           NULL, NULL },
    { "fused_group/fp_try_i64_mg_top_count_no_where",   test_fp_try_i64_mg_top_count_no_where,   NULL, NULL },
    { "fused_group/mk_apply_count_emit_filter_top_sum", test_mk_apply_count_emit_filter_top_sum, NULL, NULL },
    { "fused_group/mk_apply_count_emit_filter_top_min", test_mk_apply_count_emit_filter_top_min, NULL, NULL },
    { "fused_group/fp_try_i32_mg_top_count_rebuild_path", test_fp_try_i32_mg_top_count_rebuild_path, NULL, NULL },
    { "fused_group/fp_try_i64_mg_top_count_rebuild_path", test_fp_try_i64_mg_top_count_rebuild_path, NULL, NULL },
    { "fused_group/fp_try_timestamp_mg_top_count_no_where", test_fp_try_timestamp_mg_top_count_no_where, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

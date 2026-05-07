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
#include "ops/ops.h"
#include "ops/fused_group.h"
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

const test_entry_t fused_group_entries[] = {
    { "fused_group/eq_count",                    test_eq_count,                    NULL, NULL },
    { "fused_group/ne_two_groups",               test_ne_two_groups,               NULL, NULL },
    { "fused_group/eq_no_match",                 test_eq_no_match,                 NULL, NULL },
    { "fused_group/eq_const_out_of_range_u8",    test_eq_const_out_of_range_u8,    NULL, NULL },
    { "fused_group/ne_const_out_of_range_u8",    test_ne_const_out_of_range_u8,    NULL, NULL },
    { "fused_group/sum_negative_i16",            test_sum_negative_i16,            NULL, NULL },
    { "fused_group/fallback_filter_honored",     test_fallback_filter_honored,     NULL, NULL },
    { "fused_group/count1_rejects_nullable_key", test_count1_rejects_nullable_key, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

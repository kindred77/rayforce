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

/* test/test_group_pushdown.c — GROUP predicate pushdown (HAVING-on-key) */
#include "test.h"
#include "rayforce.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <math.h>

/* Fixture: table k(I64 group key), v(I64 values), 12 rows, 4 key values.
 * k: 1 1 1 2 2 2 3 3 3 4 4 4
 * v: 1 2 3 4 5 6 7 8 9 10 11 12   (sum per group: 6, 15, 24, 33) */
static ray_t* make_gp_table(void) {
    (void)ray_sym_init();
    int64_t kd[] = {1,1,1,2,2,2,3,3,3,4,4,4};
    int64_t vd[] = {1,2,3,4,5,6,7,8,9,10,11,12};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 12);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 12);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv); ray_release(vv);
    return tbl;
}

typedef ray_op_t* (*gp_pred_builder_t)(ray_graph_t* g);

/* Build FILTER(pred, GROUP(sum v by k)), run ray_optimize, return root. */
static ray_op_t* build_having_plan(ray_graph_t* g, gp_pred_builder_t pb,
                                   ray_op_t** out_grp) {
    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* v = ray_scan(g, "v");
    ray_op_t* keys[] = {k};
    uint16_t aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp = ray_group(g, keys, 1, aops, ains, 1);
    ray_op_t* filt = ray_filter(g, grp, pb(g));
    if (out_grp) *out_grp = grp;
    return ray_optimize(g, filt);
}

/* Plan-shape probe: pushed = root is GROUP with an OP_FILTER inputs[0]. */
static bool plan_pushed(ray_graph_t* g, ray_op_t* root) {
    (void)g;
    return root && root->opcode == OP_GROUP &&
           root->inputs[0] && root->inputs[0]->opcode == OP_FILTER;
}

/* Pred on the AGG OUTPUT column — HAVING proper; must never push.
 * After GROUP BY k, SUM(v), the agg output column is named "v_sum". */
static ray_op_t* pred_on_agg(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "v_sum"), ray_const_i64(g, 10));
}

static test_result_t test_having_on_agg_not_pushed(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* root = build_having_plan(g, pred_on_agg, NULL);
    TEST_ASSERT(!plan_pushed(g, root), "agg-output pred must stay HAVING");
    ray_t* r = ray_execute(g, root);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3); /* sums 15,24,33 pass; 6 fails */
    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Hand-build the post-rewrite DAG (Task-3 optimizer shape) and execute:
 * GROUP.inputs[0] = FILTER(k >= 3, const_table).  Without the executor
 * hook the filter never runs and all 4 groups appear. */
static test_result_t test_exec_group_with_pushed_filter(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* v = ray_scan(g, "v");
    ray_op_t* keys[] = {k};
    uint16_t aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp = ray_group(g, keys, 1, aops, ains, 1);

    ray_op_t* tbl_node = ray_const_table(g, tbl);
    ray_op_t* pred = ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3));
    ray_op_t* filt = ray_filter(g, tbl_node, pred);

    /* Wire filt as GROUP's inputs[0] (Task-3 optimizer shape).
     * grp points into g->nodes[] — that's the live node exec_node sees. */
    grp->inputs[0] = filt;
    ray_op_ext_t* gext = find_ext(g, grp->id);
    TEST_ASSERT(gext != NULL, "group ext");
    gext->base.inputs[0] = filt;   /* keep ext copy in sync */
    /* Dual-slot sync check */
    TEST_ASSERT(grp->inputs[0] == gext->base.inputs[0], "dense/ext inputs in sync");

    ray_t* r = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);  /* k=3 (sum 24), k=4 (sum 33) only */

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ===================================================================
 * Task-3 optimizer tests
 * =================================================================== */

static ray_op_t* pred_on_key(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3));
}

static test_result_t test_having_on_key_pushed(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* root = build_having_plan(g, pred_on_key, NULL);
    TEST_ASSERT(plan_pushed(g, root), "keys-only pred must push below GROUP");

    /* Dual-slot sync check after optimizer rewrite */
    ray_op_ext_t* gext = find_ext(g, root->id);
    TEST_ASSERT(gext != NULL, "group ext after push");
    TEST_ASSERT(root->inputs[0] == gext->base.inputs[0], "dense/ext inputs in sync after push");

    ray_t* r = ray_execute(g, root);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);   /* k=3 (sum=24), k=4 (sum=33) */
    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ===================================================================
 * Differential helper: run same plan with pushdown ON (pass=0) and
 * OFF (pass=1, knob), sort by k, compare results cell-by-cell.
 * All test tables use I64 columns only.
 * =================================================================== */
static test_result_t diff_having(ray_t* tbl, gp_pred_builder_t pb) {
    ray_t* res[2] = {NULL, NULL};
    for (int pass = 0; pass < 2; pass++) {
        ray_opt_no_group_pushdown = (pass == 1);
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* root = build_having_plan(g, pb, NULL);
        /* Sort by k after optimize (both paths produce k column in output) */
        ray_op_t* skeys[1] = {ray_scan(g, "k")};
        uint8_t descs[1] = {0};
        root = ray_sort_op(g, root, skeys, descs, NULL, 1);
        res[pass] = ray_execute(g, root);
        ray_graph_free(g);
    }
    ray_opt_no_group_pushdown = false;

    TEST_ASSERT_FALSE(RAY_IS_ERR(res[0]));
    TEST_ASSERT_FALSE(RAY_IS_ERR(res[1]));
    TEST_ASSERT_EQ_I(ray_table_nrows(res[0]), ray_table_nrows(res[1]));

    int64_t ncols = ray_table_ncols(res[0]);
    int64_t nrows = ray_table_nrows(res[0]);
    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(res[0], c);
        ray_t* v0 = ray_table_get_col(res[0], col_name);
        ray_t* v1 = ray_table_get_col(res[1], col_name);
        if (!v0 || !v1) continue;
        for (int64_t row = 0; row < nrows; row++) {
            bool n0 = ray_vec_is_null(v0, row);
            bool n1 = ray_vec_is_null(v1, row);
            TEST_ASSERT(n0 == n1, "null mismatch between pushed and baseline");
            if (!n0) {
                int64_t a = ray_vec_get_i64(v0, row);
                int64_t b = ray_vec_get_i64(v1, row);
                TEST_ASSERT(a == b, "value mismatch between pushed and baseline");
            }
        }
    }
    ray_release(res[0]); ray_release(res[1]);
    PASS();
}

/* k >= 3 */
static ray_op_t* pred_key_ge3(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3));
}
static test_result_t test_diff_key_ge(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    test_result_t r = diff_having(tbl, pred_key_ge3);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* k == 2 */
static ray_op_t* pred_key_eq2(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 2));
}
static test_result_t test_diff_key_eq(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    test_result_t r = diff_having(tbl, pred_key_eq2);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* k > 100 — all rows filtered, expect 0 groups */
static ray_op_t* pred_key_all_filtered(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 100));
}
static test_result_t test_diff_all_filtered(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    test_result_t r = diff_having(tbl, pred_key_all_filtered);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Fixture with null keys at rows 0 and 5, k >= 3 predicate.
 * k: NULL 1 1 2 2 NULL 3 3 3 4 4 4
 * v: 1 2 3 4 5 6 7 8 9 10 11 12 */
static ray_t* make_null_key_table(void) {
    (void)ray_sym_init();
    int64_t kd[] = {0,1,1,2,2,0,3,3,3,4,4,4};  /* 0 is placeholder for NULL */
    int64_t vd[] = {1,2,3,4,5,6,7,8,9,10,11,12};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 12);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 12);
    ray_vec_set_null(kv, 0, true);
    ray_vec_set_null(kv, 5, true);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv); ray_release(vv);
    return tbl;
}

static ray_op_t* pred_null_key_ge3(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3));
}
static test_result_t test_diff_null_keys(void) {
    ray_heap_init();
    ray_t* tbl = make_null_key_table();
    test_result_t r = diff_having(tbl, pred_null_key_ge3);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Fixture with 3rd column j, GROUP by {k, j}, pred on k only.
 * k: 1 1 2 2 3 3  j: 10 10 20 20 30 30  v: 1 2 3 4 5 6 */
static ray_t* make_multi_key_table(void) {
    (void)ray_sym_init();
    int64_t kd[] = {1,1,2,2,3,3};
    int64_t jd[] = {10,10,20,20,30,30};
    int64_t vd[] = {1,2,3,4,5,6};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 6);
    ray_t* jv = ray_vec_from_raw(RAY_I64, jd, 6);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 6);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("j", 1), jv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv); ray_release(jv); ray_release(vv);
    return tbl;
}

/* Build FILTER(k >= 3, GROUP(sum v by {k,j})), run ray_optimize. */
static ray_op_t* build_multikey_plan(ray_graph_t* g, gp_pred_builder_t pb) {
    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* j = ray_scan(g, "j");
    ray_op_t* v = ray_scan(g, "v");
    ray_op_t* keys[] = {k, j};
    uint16_t aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp = ray_group(g, keys, 2, aops, ains, 1);
    ray_op_t* filt = ray_filter(g, grp, pb(g));
    return ray_optimize(g, filt);
}

static ray_op_t* pred_mk_key_ge3(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3));
}

static test_result_t test_diff_multi_key(void) {
    ray_heap_init();
    ray_t* tbl = make_multi_key_table();

    ray_t* res[2] = {NULL, NULL};
    for (int pass = 0; pass < 2; pass++) {
        ray_opt_no_group_pushdown = (pass == 1);
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* root = build_multikey_plan(g, pred_mk_key_ge3);
        ray_op_t* skeys[1] = {ray_scan(g, "k")};
        uint8_t descs[1] = {0};
        root = ray_sort_op(g, root, skeys, descs, NULL, 1);
        res[pass] = ray_execute(g, root);
        ray_graph_free(g);
    }
    ray_opt_no_group_pushdown = false;

    TEST_ASSERT_FALSE(RAY_IS_ERR(res[0]));
    TEST_ASSERT_FALSE(RAY_IS_ERR(res[1]));
    TEST_ASSERT_EQ_I(ray_table_nrows(res[0]), ray_table_nrows(res[1]));

    ray_release(res[0]); ray_release(res[1]);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* k IN {2, 4} */
static ray_op_t* pred_key_in(ray_graph_t* g) {
    int64_t vals[] = {2, 4};
    ray_t* set_vec = ray_vec_from_raw(RAY_I64, vals, 2);
    ray_op_t* set_op = ray_const_vec(g, set_vec);
    ray_release(set_vec);
    return ray_in(g, ray_scan(g, "k"), set_op);
}
static test_result_t test_diff_key_in(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    test_result_t r = diff_having(tbl, pred_key_in);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* HEAD(GROUP(FILTER(...))) — test that the HEAD fast-path handles
 * the pushed-filter shape correctly after GROUP predicate pushdown. */
static test_result_t test_head_group_pushed_filter(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build FILTER(k >= 3, GROUP(sum v by k)) and optimize.
     * After optimization the shape should be GROUP(FILTER(...)). */
    ray_op_t* root = build_having_plan(g, pred_on_key, NULL);
    TEST_ASSERT(plan_pushed(g, root), "must push for HEAD test");

    /* Wrap in HEAD(3): k=3 and k=4 pass the filter (2 rows), head keeps both */
    ray_op_t* head_op = ray_head(g, root, 3);
    TEST_ASSERT(head_op != NULL, "ray_head alloc");

    /* Sort by v_sum ascending so row order is deterministic (24 then 33). */
    ray_op_t* skeys[1] = {ray_scan(g, "v_sum")};
    uint8_t descs[1] = {0};
    ray_op_t* sorted = ray_sort_op(g, head_op, skeys, descs, NULL, 1);
    TEST_ASSERT(sorted != NULL, "ray_sort_op alloc");

    ray_t* r = ray_execute(g, sorted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Only k=3 (sum=24) and k=4 (sum=33) survive the filter; HEAD(3) keeps both. */
    int64_t nrows = ray_table_nrows(r);
    TEST_ASSERT_EQ_I(nrows, 2);
    int64_t sum_col = ray_sym_intern("v_sum", 5);
    ray_t* sv = ray_table_get_col(r, sum_col);
    TEST_ASSERT(sv != NULL, "v_sum column present");
    TEST_ASSERT_EQ_I(ray_vec_get_i64(sv, 0), 24);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(sv, 1), 33);

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

const test_entry_t group_pushdown_entries[] = {
    { "group_pushdown/agg_pred_not_pushed",   test_having_on_agg_not_pushed,     NULL, NULL },
    { "group_pushdown/exec_pushed_filter",    test_exec_group_with_pushed_filter, NULL, NULL },
    { "group_pushdown/having_on_key_pushed",  test_having_on_key_pushed,         NULL, NULL },
    { "group_pushdown/diff_key_ge",           test_diff_key_ge,                  NULL, NULL },
    { "group_pushdown/diff_key_eq",           test_diff_key_eq,                  NULL, NULL },
    { "group_pushdown/diff_all_filtered",     test_diff_all_filtered,            NULL, NULL },
    { "group_pushdown/diff_null_keys",        test_diff_null_keys,               NULL, NULL },
    { "group_pushdown/diff_multi_key",        test_diff_multi_key,               NULL, NULL },
    { "group_pushdown/diff_key_in",           test_diff_key_in,                  NULL, NULL },
    { "group_pushdown/head_group_pushed",     test_head_group_pushed_filter,     NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

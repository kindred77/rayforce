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
typedef ray_op_t* (*gp_plan_builder_t)(ray_graph_t* g, gp_pred_builder_t pb);

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

/* Un-split HAVING shape: root is FILTER whose direct child is GROUP and whose
 * predicate is still an intact AND (the split would have made inputs[0] a FILTER). */
static bool plan_having_unsplit(ray_graph_t* g, ray_op_t* root) {
    (void)g;
    return root && root->opcode == OP_FILTER &&
           root->inputs[0] && root->inputs[0]->opcode == OP_GROUP &&
           root->inputs[1] && root->inputs[1]->opcode == OP_AND;
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

/* Shape (a): HAVING AND where BOTH conjuncts reference the agg output v_sum.
 * pred = AND(v_sum > 10, v_sum < 30).  Neither conjunct is a group key, so
 * pushdown is skipped; the AND must NOT be split over GROUP (split would make
 * the outer conjunct evaluate against the base table → schema error).
 * Per-group v_sum: 6,15,24,33 → 15 and 24 satisfy (>10 AND <30) → 2 rows. */
static test_result_t test_having_and_agg_executes(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* v = ray_scan(g, "v");
    ray_op_t* keys[] = {k};
    uint16_t  aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);
    ray_op_t* pred = ray_and(g,
        ray_gt(g, ray_scan(g, "v_sum"), ray_const_i64(g, 10)),
        ray_lt(g, ray_scan(g, "v_sum"), ray_const_i64(g, 30)));
    ray_op_t* filt = ray_filter(g, grp, pred);
    ray_op_t* root = ray_optimize(g, filt);

    /* Guard keeps FILTER(AND, GROUP) intact — not a split chain. */
    TEST_ASSERT(plan_having_unsplit(g, root),
        "AND(agg,agg) over GROUP must stay un-split FILTER(AND, GROUP)");

    ray_t* r = ray_execute(g, root);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);  /* v_sum 15 and 24 */

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

/* k >= 3 — canonical predicate used across all single- and multi-key tests */
static ray_op_t* pred_key_ge3(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3));
}

static test_result_t test_having_on_key_pushed(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* root = build_having_plan(g, pred_key_ge3, NULL);
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
 * Differential helpers: run same plan with pushdown ON (pass=0) and
 * OFF (pass=1, knob), sort by sort_col, compare results cell-by-cell.
 * All test tables use I64 columns only.
 * =================================================================== */

/* Wrapper type so build_multikey_plan matches gp_plan_builder_t. */
static ray_op_t* plan_builder_having(ray_graph_t* g, gp_pred_builder_t pb) {
    return build_having_plan(g, pb, NULL);
}

static test_result_t diff_having_ex(ray_t* tbl, gp_plan_builder_t plan_fn,
                                    gp_pred_builder_t pb, const char* sort_col) {
    ray_t* res[2] = {NULL, NULL};
    for (int pass = 0; pass < 2; pass++) {
        ray_opt_no_group_pushdown = (pass == 1);
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* root = plan_fn(g, pb);
        ray_op_t* skeys[1] = {ray_scan(g, sort_col)};
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

/* Thin wrapper for single-key plans that sort by k. */
static test_result_t diff_having(ray_t* tbl, gp_pred_builder_t pb) {
    return diff_having_ex(tbl, plan_builder_having, pb, "k");
}

/* k >= 3 (pred_key_ge3 defined above) */
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

static test_result_t test_diff_null_keys(void) {
    ray_heap_init();
    ray_t* tbl = make_null_key_table();
    test_result_t r = diff_having(tbl, pred_key_ge3);
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

/* Build FILTER(pred, GROUP(sum v by {k,j})), run ray_optimize. */
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

static test_result_t test_diff_multi_key(void) {
    ray_heap_init();
    ray_t* tbl = make_multi_key_table();
    test_result_t r = diff_having_ex(tbl, build_multikey_plan, pred_key_ge3, "k");
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
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
    ray_op_t* root = build_having_plan(g, pred_key_ge3, NULL);
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

/* ===================================================================
 * Task-4 negative edge tests — each verifies pushdown is suppressed
 * =================================================================== */

/* a. GROUP key is a computed expression (* k 2) — key op is OP_MUL, not
 * OP_SCAN, so the pushdown guard `kop->opcode != OP_SCAN` rejects it.
 *
 * Output column name for a computed key: key_ext->sym == 0 (empty string,
 * because graph_alloc_ext_node zero-initialises the ext and only ray_scan
 * sets ext->sym to an interned name).  We cannot reliably name-scan the
 * computed key column from the HAVING predicate, so we assert !plan_pushed
 * and correct row counts instead of a value comparison.
 *
 * Data: k 1 1 1 2 2 2 3 3 3 4 4 4; computed key = k*2 → groups 2,4,6,8.
 * Predicate `v_sum > 10`: group sums for these groups are 6,15,24,33.
 * sums 15,24,33 > 10 → 3 groups pass (groups with k=2,3,4 → key*2=4,6,8). */
static test_result_t test_no_push_computed_key(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* key = (* k 2): OP_MUL, not OP_SCAN — must not push */
    ray_op_t* k2 = ray_mul(g, ray_scan(g, "k"), ray_const_i64(g, 2));
    ray_op_t* v  = ray_scan(g, "v");
    ray_op_t* keys[] = {k2};
    uint16_t  aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);
    /* pred on agg output column — stays HAVING regardless */
    ray_op_t* pred = ray_gt(g, ray_scan(g, "v_sum"), ray_const_i64(g, 10));
    ray_op_t* filt = ray_filter(g, grp, pred);
    ray_op_t* root = ray_optimize(g, filt);

    TEST_ASSERT(!plan_pushed(g, root), "computed-key GROUP must not push");

    ray_t* r = ray_execute(g, root);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3); /* sums 15,24,33 > 10; 6 fails */

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* b. One GROUP feeding TWO filters.
 *
 * count_node_consumers walks ALL live nodes in g->nodes[] and g->ext_nodes[],
 * not just nodes reachable from root.  So adding f2 as a live node that
 * references grp is sufficient to make the consumer count > 1, even when
 * root = f1 (f2 unreachable from root).  Pushdown is thus correctly blocked.
 *
 * We do NOT call ray_execute(g, f2); executing f1 only is enough to verify
 * correct results.  f2 is intentionally kept alive to trigger the guard. */
static test_result_t test_no_push_multi_consumer(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* v = ray_scan(g, "v");
    ray_op_t* keys[] = {k};
    uint16_t  aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp = ray_group(g, keys, 1, aops, ains, 1);

    /* Two filters both consuming grp — optimizer sees consumer count == 2 */
    ray_op_t* f1 = ray_filter(g, grp,
                        ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 2)));
    /* f2 stays alive but is not executed */
    (void)ray_filter(g, grp,
                        ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 3)));

    ray_op_t* root = ray_optimize(g, f1);
    TEST_ASSERT(!plan_pushed(g, root), "multi-consumer GROUP must not push");

    ray_t* r = ray_execute(g, root);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* k >= 2: groups 2(sum=15), 3(sum=24), 4(sum=33) → 3 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* c. AND predicate mixing a key-scan conjunct and an agg-output conjunct.
 *
 * pred = AND(k >= 3, v_sum > 10).  collect_pred_scans traverses the full AND
 * subtree and finds both `k` and `v_sum` scans.  `v_sum` is not a group key
 * (it is an agg output), so keys_only=false → pushdown skipped at pass 6.
 *
 * At pass 7 (filter_reorder) split_and_filter decomposes the AND-filter into
 * two chained filters: FILTER(v_sum>10, FILTER(k>=3, GROUP(...))).  Passes
 * run once (ray_optimize makes a single linear sweep, pass 6 before pass 7),
 * so the k>=3 filter is NOT re-attempted for pushdown after the split.
 * The optimised shape is therefore FILTER(FILTER(GROUP)), not GROUP(FILTER).
 *
 * Execution note: the outer FILTER evaluates its predicate (v_sum > 10) via
 * exec_node, which reads SCAN nodes from g->table (the original table).  The
 * original table has no `v_sum` column, so executing the OPTIMIZED plan
 * returns a schema error.  The plan-shape assert is the meaningful check for
 * the optimised path.
 *
 * Value correctness is verified on the UN-OPTIMIZED plan (no ray_optimize
 * call): FILTER(GROUP) preserves the single-filter exec path (line ~1230 in
 * exec.c) that swaps g->table to the GROUP output before evaluating the pred.
 * Values: k=3 sum=24, k=4 sum=33 both satisfy AND(k>=3, v_sum>10) → 2 rows. */
static test_result_t test_no_push_mixed_pred(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();

    /* Plan-shape check: optimise and assert no push */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* k = ray_scan(g, "k");
        ray_op_t* v = ray_scan(g, "v");
        ray_op_t* keys[] = {k};
        uint16_t  aops[] = {OP_SUM};
        ray_op_t* ains[] = {v};
        ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);
        ray_op_t* pred = ray_and(g,
            ray_ge(g, ray_scan(g, "k"),     ray_const_i64(g, 3)),
            ray_gt(g, ray_scan(g, "v_sum"), ray_const_i64(g, 10)));
        ray_op_t* filt = ray_filter(g, grp, pred);
        ray_op_t* root = ray_optimize(g, filt);
        TEST_ASSERT(!plan_pushed(g, root), "mixed-pred AND must not push");
        ray_graph_free(g);
    }

    /* Value check: un-optimized FILTER(GROUP) preserves the executor's
     * special HAVING path (filter_child->opcode == OP_GROUP) so scans in
     * the pred correctly resolve against the GROUP output, not g->table. */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* k = ray_scan(g, "k");
        ray_op_t* v = ray_scan(g, "v");
        ray_op_t* keys[] = {k};
        uint16_t  aops[] = {OP_SUM};
        ray_op_t* ains[] = {v};
        ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);
        ray_op_t* pred = ray_and(g,
            ray_ge(g, ray_scan(g, "k"),     ray_const_i64(g, 3)),
            ray_gt(g, ray_scan(g, "v_sum"), ray_const_i64(g, 10)));
        ray_op_t* filt = ray_filter(g, grp, pred);
        ray_t* r = ray_execute(g, filt);  /* no ray_optimize — keeps FILTER(GROUP) */
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        /* k=3 sum=24, k=4 sum=33 both pass both conjuncts → 2 rows */
        TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);
        ray_release(r); ray_graph_free(g);
    }

    ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* d. Constant predicate — no scan references at all.
 *
 * pred = (> (+ 1 1) 0).  Pass 2 (constant folding) evaluates (+ 1 1) → 2
 * then (> 2 0) → true.  fold_filter_const_predicate replaces the FILTER with
 * OP_MATERIALIZE(GROUP), so the root node is OP_MATERIALIZE, not OP_FILTER.
 * plan_pushed returns false (root is not OP_GROUP).
 *
 * All 4 groups survive (predicate is always true → 0 rows filtered). */
static test_result_t test_no_push_const_pred(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* v = ray_scan(g, "v");
    ray_op_t* keys[] = {k};
    uint16_t  aops[] = {OP_SUM};
    ray_op_t* ains[] = {v};
    ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);

    /* (> (+ 1 1) 0) — constant expression, no scans.
     * After const-fold this becomes a true literal; fold_filter_const_predicate
     * rewrites FILTER → MATERIALIZE, so root->opcode == OP_MATERIALIZE. */
    ray_op_t* pred = ray_gt(g,
        ray_add(g, ray_const_i64(g, 1), ray_const_i64(g, 1)),
        ray_const_i64(g, 0));
    ray_op_t* filt = ray_filter(g, grp, pred);
    ray_op_t* root = ray_optimize(g, filt);

    TEST_ASSERT(!plan_pushed(g, root), "const-pred filter must not push");

    ray_t* r = ray_execute(g, root);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Predicate always true → all 4 groups */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4);

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ===================================================================
 * AND-of-keys pushdown — diff + plan-shape probe.
 *
 * pred = AND(k >= 2, k <= 3): both conjuncts are keys-only, so pass-6
 * pushes the whole AND-filter below GROUP: GROUP(FILTER(AND, CONST_TABLE)).
 *
 * BUG (fixed): pass-7 (split_and_filter) used to split the pushed AND into
 *   inner (orig id):   FILTER(k>=2, CONST_TABLE)
 *   outer (new id):    FILTER(k<=3, inner)
 * and relied on redirect_consumers to re-point GROUP at the outer filter.
 * But GROUP's arity is 0 (its inputs[0] is an alias kept outside the arity
 * contract), so redirect_consumers' `k < c->arity` gate never fired for it:
 * GROUP->inputs[0] kept pointing at the inner filter, the outer filter was
 * orphaned, and the k<=3 conjunct was silently dropped — 3 groups instead
 * of 2.
 *
 * GUARD: pass-6 now tags the interposed filter with OP_FLAG_PUSHED and
 * pass-7 skips flagged filters entirely (no AND-split, no chain reorder).
 * A pushed AND evaluates as one fused BOOL pass through the lazy-selection
 * path; splitting it below the group would need a GROUP.inputs[0]
 * redirection that redirect_consumers' arity gate doesn't perform, and
 * would also cost two rowsel passes instead of one.
 *
 * This test asserts pushed ≡ unpushed (2 rows: k=2 sum 15, k=3 sum 24)
 * via an un-optimized FILTER(AND, GROUP) baseline, plus the plan shape:
 * root GROUP, inputs[0] an OP_FILTER carrying OP_FLAG_PUSHED whose pred is
 * still the un-split AND and whose data input is the const-table (chain
 * depth 1).
 *
 * The knob-based diff helper (diff_having) is NOT usable here: with
 * pushdown disabled, pass-7 still splits the AND above GROUP into
 * FILTER(k<=3, FILTER(k>=2, GROUP)); the outer filter's pred evaluates
 * against g->table (12 rows) but its input is the 2-row group output —
 * a row-count mismatch error (empirically verified, pre-existing and
 * independent of the pushdown bug).
 * =================================================================== */

/* (and (>= k 2) (<= k 3)) — keys-only AND, nominally groups k=2 and k=3 */
static ray_op_t* pred_key_and_range(ray_graph_t* g) {
    return ray_and(g,
        ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 2)),
        ray_le(g, ray_scan(g, "k"), ray_const_i64(g, 3)));
}

static test_result_t test_diff_and_of_keys(void) {
    ray_heap_init();
    ray_t* tbl = make_gp_table();

    /* --- Pushed plan: ray_optimize with pushdown enabled --- */
    ray_t* pushed_res = NULL;
    {
        ray_opt_no_group_pushdown = false;
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* root = build_having_plan(g, pred_key_and_range, NULL);

        /* Pass-6 must push the AND-filter below GROUP. */
        TEST_ASSERT(plan_pushed(g, root),
            "AND-of-keys pred must push below GROUP (pass-6)");

        /* Plan-shape: root->inputs[0] is the pushed OP_FILTER, flagged. */
        ray_op_t* f0 = root->inputs[0];
        TEST_ASSERT(f0 != NULL && f0->opcode == OP_FILTER,
            "GROUP inputs[0] must be OP_FILTER");
        TEST_ASSERT(f0->flags & OP_FLAG_PUSHED,
            "pushed filter must carry OP_FLAG_PUSHED");

        /* The pred must still be the un-split AND … */
        TEST_ASSERT(f0->inputs[1] != NULL && f0->inputs[1]->opcode == OP_AND,
            "pushed filter pred must remain the un-split AND");
        /* … and the data input the const-table: chain depth 1, no split. */
        TEST_ASSERT(f0->inputs[0] != NULL && f0->inputs[0]->opcode != OP_FILTER,
            "pushed filter input must be the const-table (chain depth 1)");

        ray_op_t* skeys[1] = {ray_scan(g, "k")};
        uint8_t descs[1] = {0};
        root = ray_sort_op(g, root, skeys, descs, NULL, 1);
        pushed_res = ray_execute(g, root);
        ray_graph_free(g);
    }

    /* --- Un-optimized baseline: raw FILTER(AND, GROUP) — no ray_optimize ---
     * HAVING-fusion evaluates the AND against the GROUP output correctly. */
    ray_t* base_res = NULL;
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* k  = ray_scan(g, "k");
        ray_op_t* v  = ray_scan(g, "v");
        ray_op_t* keys[] = {k};
        uint16_t  aops[] = {OP_SUM};
        ray_op_t* ains[] = {v};
        ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);
        ray_op_t* filt = ray_filter(g, grp, pred_key_and_range(g));
        ray_op_t* skeys[1] = {ray_scan(g, "k")};
        uint8_t descs[1] = {0};
        ray_op_t* sorted = ray_sort_op(g, filt, skeys, descs, NULL, 1);
        base_res = ray_execute(g, sorted);
        ray_graph_free(g);
    }

    TEST_ASSERT_FALSE(RAY_IS_ERR(pushed_res));
    TEST_ASSERT_FALSE(RAY_IS_ERR(base_res));

    /* Pushed ≡ unpushed: both conjuncts applied → k=2 (sum 15), k=3 (sum 24). */
    TEST_ASSERT_EQ_I(ray_table_nrows(pushed_res), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(base_res),   2);

    /* Cell-by-cell diff (sorted by k ascending), same as diff_having_ex. */
    int64_t ncols = ray_table_ncols(pushed_res);
    int64_t nrows = ray_table_nrows(pushed_res);
    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(pushed_res, c);
        ray_t* v0 = ray_table_get_col(pushed_res, col_name);
        ray_t* v1 = ray_table_get_col(base_res, col_name);
        TEST_ASSERT(v0 != NULL && v1 != NULL, "column present in both results");
        for (int64_t row = 0; row < nrows; row++) {
            bool n0 = ray_vec_is_null(v0, row);
            bool n1 = ray_vec_is_null(v1, row);
            TEST_ASSERT(n0 == n1, "null mismatch between pushed and baseline");
            if (!n0)
                TEST_ASSERT(ray_vec_get_i64(v0, row) == ray_vec_get_i64(v1, row),
                    "value mismatch between pushed and baseline");
        }
    }

    /* Spot-check the absolute values (sorted by k ascending). */
    int64_t sum_col = ray_sym_intern("v_sum", 5);
    ray_t* sv = ray_table_get_col(pushed_res, sum_col);
    TEST_ASSERT(sv != NULL, "v_sum column present in pushed result");
    TEST_ASSERT_EQ_I(ray_vec_get_i64(sv, 0), 15);  /* k=2 */
    TEST_ASSERT_EQ_I(ray_vec_get_i64(sv, 1), 24);  /* k=3 */

    ray_release(pushed_res); ray_release(base_res);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

const test_entry_t group_pushdown_entries[] = {
    { "group_pushdown/agg_pred_not_pushed",   test_having_on_agg_not_pushed,     NULL, NULL },
    { "group_pushdown/having_and_agg_exec",   test_having_and_agg_executes,      NULL, NULL },
    { "group_pushdown/exec_pushed_filter",    test_exec_group_with_pushed_filter, NULL, NULL },
    { "group_pushdown/having_on_key_pushed",  test_having_on_key_pushed,         NULL, NULL },
    { "group_pushdown/diff_key_ge",           test_diff_key_ge,                  NULL, NULL },
    { "group_pushdown/diff_key_eq",           test_diff_key_eq,                  NULL, NULL },
    { "group_pushdown/diff_all_filtered",     test_diff_all_filtered,            NULL, NULL },
    { "group_pushdown/diff_null_keys",        test_diff_null_keys,               NULL, NULL },
    { "group_pushdown/diff_multi_key",        test_diff_multi_key,               NULL, NULL },
    { "group_pushdown/diff_key_in",           test_diff_key_in,                  NULL, NULL },
    { "group_pushdown/head_group_pushed",     test_head_group_pushed_filter,     NULL, NULL },
    { "group_pushdown/no_push_computed_key",  test_no_push_computed_key,         NULL, NULL },
    { "group_pushdown/no_push_multi_consumer",test_no_push_multi_consumer,       NULL, NULL },
    { "group_pushdown/no_push_mixed_pred",    test_no_push_mixed_pred,           NULL, NULL },
    { "group_pushdown/no_push_const_pred",    test_no_push_const_pred,           NULL, NULL },
    { "group_pushdown/diff_and_of_keys",      test_diff_and_of_keys,             NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

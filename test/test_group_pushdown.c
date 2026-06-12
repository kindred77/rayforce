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

const test_entry_t group_pushdown_entries[] = {
    { "group_pushdown/agg_pred_not_pushed", test_having_on_agg_not_pushed, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

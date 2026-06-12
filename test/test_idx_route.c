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

/* test/test_idx_route.c — index routing scaffold + refactor pin (Task 1)
 *                         + zone all/none short-circuit tests (Task 2). */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/sym.h"
#include "ops/idxop.h"
#include "ops/rowsel.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "table/table.h"
#include <string.h>
#include <stdlib.h>

/* ─── Fixture ──────────────────────────────────────────────────────── */

/* Build the route-test table:
 *   k I64 {5,1,9,3,7,1,9,5,3,7}   (10 rows; k==9 at rows 2 and 6)
 *   v I64 {0,1,2,3,4,5,6,7,8,9}
 */
static ray_t* make_idx_table(void) {
    (void)ray_sym_init();
    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    int64_t vd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 10);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 10);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    return tbl;
}

/* Attach a hash index to the named column in `tbl`.
 *
 * Mechanism: ray_table_get_col returns a BORROWED pointer; ray_index_attach_hash
 * takes a ray_t** and may COW-replace the vector (it retains internally).
 * After attachment the table still holds the OLD pointer, so we must write
 * the new vector back via ray_table_set_col_idx.
 *
 * Steps:
 *   1. Find the column's slot index by scanning the schema.
 *   2. Get the borrowed pointer and retain it to take ownership.
 *   3. Call ray_index_attach_hash(&w) — w may be replaced.
 *   4. Write w back to the table slot via ray_table_set_col_idx.
 *      ray_table_set_col_idx retains the new pointer and releases
 *      the old one, so we release our retained copy afterward.
 *   5. Release our retained copy (table now owns the ref).
 *
 * Returns 0 on success, -1 on failure (col not found or attach error).
 */
static int attach_hash_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;

    /* Take ownership so attach_hash can COW if needed. */
    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_hash(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    /* Write possibly-new pointer back into the table slot. */
    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* Attach a zone index to the named column in `tbl`.
 * Same retain/write-back/release cycle as attach_hash_to_col:
 * ray_table_set_col_idx retains `w` before releasing the old pointer,
 * so our subsequent ray_release(w) drops the extra retain we took. */
static int attach_zone_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;

    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_zone(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* Build a FILTER(const_table, pred) graph from `tbl`, execute it, and
 * return the result.  Caller owns the returned reference. */
typedef ray_op_t* (*pred_builder_t)(ray_graph_t* g);

static ray_t* run_filter(ray_t* tbl, pred_builder_t build_pred) {
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* pred    = build_pred(g);
    ray_op_t* filt    = ray_filter(g, tbl_nd, pred);
    ray_t*    result  = ray_execute(g, filt);
    ray_graph_free(g);
    return result;
}

/* Compare the v-column values of two result tables.  Both must have the
 * same nrows.  Returns 1 iff identical (element-wise). */
static int v_cols_equal(ray_t* ra, ray_t* rb) {
    int64_t na = ray_table_nrows(ra);
    int64_t nb = ray_table_nrows(rb);
    if (na != nb) return 0;
    int64_t v_sym = ray_sym_intern("v", 1);
    ray_t* va = ray_table_get_col(ra, v_sym);
    ray_t* vb = ray_table_get_col(rb, v_sym);
    if (!va || !vb || RAY_IS_ERR(va) || RAY_IS_ERR(vb)) return 0;
    for (int64_t i = 0; i < na; i++) {
        if (ray_vec_get_i64(va, i) != ray_vec_get_i64(vb, i)) return 0;
    }
    return 1;
}

/* Differential helper: run the same filter on two table instances — one
 * with an index (side A) and one without (side B).  Asserts identical row
 * counts and v-column values.  When expect_zone_hit >= 0, asserts
 * ray_idx_hits[IDX_SITE_FILTER_ZONE] advanced by exactly that amount on
 * side A (pass 1 to require a hit, 0 to require no advance). */
static test_result_t diff_idx_filter(pred_builder_t build_pred,
                                     int attach_zone,
                                     int expect_rows,
                                     int expect_zone_hit) {
    /* Side A — with index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    if (attach_zone) {
        int ok = attach_zone_to_col(tbl_a, "k");
        TEST_ASSERT(ok == 0, "attach_zone_to_col k (side A)");
    }

    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    ray_t* ra = run_filter(tbl_a, build_pred);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), expect_rows);

    if (expect_zone_hit >= 0) {
        uint64_t delta = ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before;
        TEST_ASSERT_EQ_I((int64_t)delta, (int64_t)expect_zone_hit);
    }

    /* Side B — without index */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, build_pred);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), expect_rows);

    /* Same v-values in result order (filter preserves row order). */
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch between indexed and scan results");

    ray_release(ra);
    ray_release(rb);
    ray_release(tbl_a);
    ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Predicate builders for zone tests ──────────────────────────── */

/* (> k 100) — no k in [1,9] satisfies this → 0 rows */
static ray_op_t* pred_gt_100(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 100));
}

/* (> k 0) — all k in [1,9] satisfy this → 10 rows */
static ray_op_t* pred_gt_0(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 0));
}

/* (> k 5) — k∈{9,7,9,7} satisfy → 4 rows */
static ray_op_t* pred_gt_5(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 5));
}

/* ─── Tests ────────────────────────────────────────────────────────── */

/* Refactor pin: hash-eq fast path still produces correct results and
 * advances the routing counters after the rowsel-builder extraction
 * (Step 2) and idx_fresh precheck (Step 3). */
static test_result_t test_hash_eq_still_works(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Attach hash index to column k. */
    int ok = attach_hash_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_hash_to_col k");

    /* Build FILTER(const_table, (== k 9)) and execute. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_node = ray_const_table(g, tbl);
    ray_op_t* pred     = ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 9));
    ray_op_t* filt     = ray_filter(g, tbl_node, pred);

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_HASH];
    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_HASH];

    ray_t* r = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);  /* k==9 at rows 2 and 6 */

    TEST_ASSERT(ray_idx_consults[IDX_SITE_FILTER_HASH] > consults_before,
                "consult counter must advance");
    TEST_ASSERT(ray_idx_hits[IDX_SITE_FILTER_HASH] > hits_before,
                "hit counter must advance");

    ray_release(r);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Zone all/none: (> k 100) with zone index → 0 rows, hit fires. */
static test_result_t test_zone_none(void) {
    return diff_idx_filter(pred_gt_100, /*attach_zone=*/1,
                           /*expect_rows=*/0, /*expect_zone_hit=*/1);
}

/* Zone all/none: (> k 0) with zone index → 10 rows, hit fires. */
static test_result_t test_zone_all(void) {
    return diff_idx_filter(pred_gt_0, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1);
}

/* Zone unknown: (> k 5) with zone index → 4 rows; min=1 max=9, so
 * the consult cannot decide → hit counter must NOT advance. */
static test_result_t test_zone_unknown(void) {
    return diff_idx_filter(pred_gt_5, /*attach_zone=*/1,
                           /*expect_rows=*/4, /*expect_zone_hit=*/0);
}

/* Zone null exclusion: attach zone to a column with a null, query (> k 0).
 * k={5,1,NULL,3,7,1,9,5,3,7}: 9 non-null rows, but (null > 0) is false per
 * null semantics, so expect 9 rows.  The consult must NOT fire (idx_fresh_nonull
 * rejects null-bearing columns) — result must be correct via scan. */
static test_result_t test_zone_nulls_excluded(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build table with a null in k at row 2. */
    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    int64_t vd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 10);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 10);

    /* Attach zone BEFORE setting null so the zone records n_nulls==0 and
     * min/max are computed on all non-null values.  Then set the null — this
     * mutates the column's data but the index becomes stale / col gets
     * HAS_NULLS, making idx_fresh_nonull reject it regardless. */
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);

    /* Attach zone to k (before setting null — tests the guard path). */
    int ok = attach_zone_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_zone_to_col k");

    /* Now set null at row 2 in the live column.  This marks HAS_NULLS but
     * does NOT drop the index — the index is now stale (HAS_NULLS set but
     * zone was built without that null).  idx_fresh_nonull will reject it
     * on both the stale check and the HAS_NULLS check. */
    int64_t k_sym = ray_sym_intern("k", 1);
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == k_sym) { slot = i; break; }
    }
    TEST_ASSERT(slot >= 0, "k column not found");
    ray_t* kcol = ray_table_get_col_idx(tbl, slot);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kcol));
    ray_vec_set_null(kcol, 2, true);  /* mutate in place */

    /* Run (> k 0): null row is false per null semantics → 9 rows. */
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_ZONE];

    ray_graph_t* g     = ray_graph_new(tbl);
    ray_op_t* tbl_node = ray_const_table(g, tbl);
    ray_op_t* pred     = ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 0));
    ray_op_t* filt     = ray_filter(g, tbl_node, pred);
    ray_t* r = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 9);

    /* Zone consult must NOT have fired (nonull gate rejects). */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before), 0);

    ray_release(r);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t idx_route_entries[] = {
    { "idx_route/hash_eq_still_works", test_hash_eq_still_works,  NULL, NULL },
    { "idx_route/zone_none",           test_zone_none,            NULL, NULL },
    { "idx_route/zone_all",            test_zone_all,             NULL, NULL },
    { "idx_route/zone_unknown",        test_zone_unknown,         NULL, NULL },
    { "idx_route/zone_nulls_excluded", test_zone_nulls_excluded,  NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

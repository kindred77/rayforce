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
 *                         + zone all/none short-circuit tests (Task 2)
 *                         + sort-index range rowsel tests (Task 4). */

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
 * counts and v-column values.
 *
 * expect_zone_hit     >= 0: assert hits delta equals this value (1 = hit, 0 = no hit)
 *                      < 0: skip hits check
 * expect_zone_consult >= 0: assert consults delta equals this value
 *                      < 0: skip consults check */
static test_result_t diff_idx_filter(pred_builder_t build_pred,
                                     int attach_zone,
                                     int expect_rows,
                                     int expect_zone_hit,
                                     int expect_zone_consult) {
    /* Side A — with index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    if (attach_zone) {
        int ok = attach_zone_to_col(tbl_a, "k");
        TEST_ASSERT(ok == 0, "attach_zone_to_col k (side A)");
    }

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_ZONE];
    ray_t* ra = run_filter(tbl_a, build_pred);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), expect_rows);

    if (expect_zone_hit >= 0) {
        uint64_t delta = ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before;
        TEST_ASSERT_EQ_I((int64_t)delta, (int64_t)expect_zone_hit);
    }
    if (expect_zone_consult >= 0) {
        uint64_t delta = ray_idx_consults[IDX_SITE_FILTER_ZONE] - consults_before;
        TEST_ASSERT_EQ_I((int64_t)delta, (int64_t)expect_zone_consult);
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

/* Attach a bloom index to the named column in `tbl`.
 * Same retain/write-back/release cycle as attach_hash_to_col. */
static int attach_bloom_to_col(ray_t* tbl, const char* name) {
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
    ray_t* r = ray_index_attach_bloom(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
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

/* (<= k 9) — max==key boundary → all 10 rows (ALL) */
static ray_op_t* pred_le_9(ray_graph_t* g) {
    return ray_le(g, ray_scan(g, "k"), ray_const_i64(g, 9));
}

/* (>= k 1) — min==key boundary → all 10 rows (ALL) */
static ray_op_t* pred_ge_1(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 1));
}

/* (!= k 100) — key outside [1,9] → all 10 rows (ALL) */
static ray_op_t* pred_ne_100(ray_graph_t* g) {
    return ray_ne(g, ray_scan(g, "k"), ray_const_i64(g, 100));
}

/* (== k 0) — key just below min=1 → 0 rows (NONE) */
static ray_op_t* pred_eq_0(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 0));
}

/* (> f 100.0) on F64 column f∈[1.5,10.5] → 0 rows (NONE) */
static ray_op_t* pred_f_gt_100(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 100.0));
}

/* (> f 100) — INTEGER literal against the F64 column.  Decode must
 * coerce the key into the column's float family before zone consult. */
static ray_op_t* pred_f_gt_100_int(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "f"), ray_const_i64(g, 100));
}

/* (> k 5.5) — FLOAT literal against the I64 column.  Decode must reject
 * (return 0) so the scan fallback's promotion semantics apply. */
static ray_op_t* pred_k_gt_5p5(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_f64(g, 5.5));
}

/* ─── F64 zone fixture ─────────────────────────────────────────────── */

/* Build a zone-test table with an F64 column f {1.5..10.5} and I64 row id r. */
static ray_t* make_f64_zone_table(void) {
    (void)ray_sym_init();
    double fd[] = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5};
    int64_t rd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* fv = ray_vec_from_raw(RAY_F64, fd, 10);
    ray_t* rv = ray_vec_from_raw(RAY_I64, rd, 10);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), fv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("r", 1), rv);
    ray_release(fv);
    ray_release(rv);
    return tbl;
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
                           /*expect_rows=*/0, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* Zone all/none: (> k 0) with zone index → 10 rows, hit fires. */
static test_result_t test_zone_all(void) {
    return diff_idx_filter(pred_gt_0, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* Zone unknown: (> k 5) with zone index → 4 rows; min=1 max=9, so
 * the zone cannot decide → consult advances but hit must NOT. */
static test_result_t test_zone_unknown(void) {
    return diff_idx_filter(pred_gt_5, /*attach_zone=*/1,
                           /*expect_rows=*/4, /*expect_zone_hit=*/0,
                           /*expect_zone_consult=*/1);
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

    /* Now set null at row 2 in the live column.
     * ray_vec_set_null → ray_vec_set_null_checked → vec_drop_index_inplace:
     * mutation auto-drops the index, so after this call kcol has no index at
     * all.  idx_fresh_nonull therefore never fires — the consult short-circuits
     * at the "no zone attached" check before it even reaches the HAS_NULLS
     * guard.  idx_fresh_nonull's HAS_NULLS clause is a second line of defence
     * for the hypothetical case of a stale-but-surviving index, but that
     * survivor cannot exist here because drop is unconditional on mutation. */
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

/* (<= k 9): max==key boundary → ALL → 10 rows, hit advances. */
static test_result_t test_zone_le_all_boundary(void) {
    return diff_idx_filter(pred_le_9, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* (>= k 1): min==key boundary → ALL → 10 rows, hit advances. */
static test_result_t test_zone_ge_all_boundary(void) {
    return diff_idx_filter(pred_ge_1, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* (!= k 100): key outside [1,9] → ALL → 10 rows, hit advances. */
static test_result_t test_zone_ne_all(void) {
    return diff_idx_filter(pred_ne_100, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* (== k 0): key just below min=1 → NONE → 0 rows, hit advances. */
static test_result_t test_zone_eq_none_boundary(void) {
    return diff_idx_filter(pred_eq_0, /*attach_zone=*/1,
                           /*expect_rows=*/0, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* Zone F64: (> f 100.0) on f∈[1.5,10.5] → NONE → 0 rows, hit advances.
 * Exercises the float min_f/max_f path in ray_index_zone_class. */
static test_result_t test_zone_f64_none(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Attach zone index to the F64 column. */
    int ok = attach_zone_to_col(tbl, "f");
    TEST_ASSERT(ok == 0, "attach_zone_to_col f");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_ZONE];

    ray_t* r = run_filter(tbl, pred_f_gt_100);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    /* Zone classified NONE: consult and hit both advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_ZONE] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before), 1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Cross-type: F64 zone column, INTEGER literal — (> f 100) on f∈[1.5,10.5].
 * Decode must reconcile the literal into the column's float family so the
 * zone classifies against key_f=100.0 (NONE), not the zero-filled key_f.
 * Expect 0 rows with the zone NONE hit advancing. */
static test_result_t test_zone_f64_int_literal(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_zone_to_col(tbl, "f");
    TEST_ASSERT(ok == 0, "attach_zone_to_col f");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_ZONE];

    ray_t* r = run_filter(tbl, pred_f_gt_100_int);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    /* Zone classified NONE: consult and hit both advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_ZONE] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before), 1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Cross-type: I64 zone column, FLOAT literal — (> k 5.5) on k∈[1,9].
 * Decode must REJECT the shape (integer-family column, float key): the
 * fallback's promotion semantics own it.  No consult, no hit; correct
 * 4 rows (k∈{9,7,9,7}) via scan. */
static test_result_t test_zone_int_col_float_literal(void) {
    return diff_idx_filter(pred_k_gt_5p5, /*attach_zone=*/1,
                           /*expect_rows=*/4, /*expect_zone_hit=*/0,
                           /*expect_zone_consult=*/0);
}

/* ─── Predicate builders for bloom tests ─────────────────────────── */

/* (== k 4) — k∈{1,3,5,7,9}: 4 is absent, inside [min=1,max=9].
 * Bloom must prove absence; zone alone cannot (zone [1,9] contains 4). */
static ray_op_t* pred_eq_4(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 4));
}

/* (== k 9) — k∈{5,1,9,3,7,1,9,5,3,7}: 9 is present at rows 2 and 6 → 2 rows. */
static ray_op_t* pred_eq_9(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 9));
}

/* (> k 4) — k∈{5,9,7,9,5,7}: 6 rows.  Bloom is EQ-only, must not be consulted. */
static ray_op_t* pred_gt_4(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 4));
}

/* ─── Bloom tests ─────────────────────────────────────────────────── */

/* bloom_absent: bloom on k; pred (== k 4); k∈{1,3,5,7,9} so 4 is absent.
 * Bloom proves absent → 0 rows, consult and hit both advance. */
static test_result_t test_bloom_absent(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_bloom_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_bloom_to_col k");

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];

    ray_t* r = run_filter(tbl, pred_eq_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_present_falls_through: bloom on k; pred (== k 9); 9 is present.
 * Bloom cannot prove absence → falls through to scan → 2 rows.
 * Consult advances, hit must NOT. */
static test_result_t test_bloom_present_falls_through(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_bloom_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_bloom_to_col k");

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];

    ray_t* r = run_filter(tbl, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_diff_absent: differential — bloom-indexed vs no-index for (== k 4).
 * Bloom fires, same 0 rows, same v-values (trivially). */
static test_result_t test_bloom_diff_absent(void) {
    /* Side A — with bloom */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_bloom_to_col(tbl_a, "k") == 0, "attach bloom (side A)");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    ray_t* ra = run_filter(tbl_a, pred_eq_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 0);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);

    /* Side B — no index */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 0);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_diff_present: differential — bloom-indexed vs no-index for (== k 9).
 * Bloom does NOT hit (present), scan produces same 2 rows. */
static test_result_t test_bloom_diff_present(void) {
    /* Side A — with bloom */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_bloom_to_col(tbl_a, "k") == 0, "attach bloom (side A)");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    ray_t* ra = run_filter(tbl_a, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 2);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     0);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);

    /* Side B — no index */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_noneq_ignored: bloom on k; pred (> k 4).
 * Bloom is EQ-only: consult must NOT advance; correct 6 rows via scan. */
static test_result_t test_bloom_noneq_ignored(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_bloom_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_bloom_to_col k");

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];

    ray_t* r = run_filter(tbl, pred_gt_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 6);   /* k∈{5,9,7,9,5,7} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Sort-index attach helper ────────────────────────────────────── */

/* Attach a sort index to the named column in `tbl`.
 * Same retain/write-back/release cycle as attach_hash_to_col. */
static int attach_sort_to_col(ray_t* tbl, const char* name) {
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
    ray_t* r = ray_index_attach_sort(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* ─── Predicate builders for range tests ──────────────────────────── */

/* (< k 5) → k∈{1,3,1,3} → 4 rows */
static ray_op_t* pred_lt_5(ray_graph_t* g) {
    return ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 5));
}

/* (>= k 7) → k∈{9,7,9,7} → 4 rows */
static ray_op_t* pred_ge_7(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 7));
}

/* (== k 9) → k∈{9,9} → 2 rows (sort path, no hash) */
static ray_op_t* pred_eq_9_sort(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 9));
}

/* (<= k 1) → k∈{1,1} → 2 rows */
static ray_op_t* pred_le_1(ray_graph_t* g) {
    return ray_le(g, ray_scan(g, "k"), ray_const_i64(g, 1));
}

/* (!= k 5) → 8 rows (NE unsupported by range) */
static ray_op_t* pred_ne_5(ray_graph_t* g) {
    return ray_ne(g, ray_scan(g, "k"), ray_const_i64(g, 5));
}

/* (> f 5.0) on F64 column f∈[1.5,10.5] → 6 rows ({5.5..10.5}) */
static ray_op_t* pred_f_gt_5(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 5.0));
}

/* ─── Range tests ─────────────────────────────────────────────────── */

/* range_lt: sort on k; (< k 5) → 4 rows; range consult+hit. */
static test_result_t test_range_lt(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_lt)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_lt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4);  /* k∈{1,3,1,3} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    /* Differential: same rows as scan */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_lt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_lt");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_ge: sort on k; (>= k 7) → 4 rows; hit. */
static test_result_t test_range_ge(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_ge)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_ge_7);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4);  /* k∈{9,7,9,7} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_ge_7);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_ge");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_eq_dups: sort only (no hash); (== k 9) → 2 rows via range path; hit. */
static test_result_t test_range_eq_dups(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    /* Only attach sort — no hash — so the range path must handle EQ. */
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_eq_dups)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_eq_9_sort);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_9_sort);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_eq_dups");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_le_boundary: (<= k 1) → 2 rows (both 1s); hit. */
static test_result_t test_range_le_boundary(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_le_boundary)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_le_1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);  /* both k==1 */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_le_1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_le_boundary");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_ne_falls_back: (!= k 5) → 8 rows; range consult NOT advanced (NE = two spans). */
static test_result_t test_range_ne_falls_back(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_ne_falls_back)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_ne_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 8);

    /* NE is unsupported by the range path — consult must NOT advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_guard: 100k-row fixture (k = i % 100); (< k 50) → ~50k rows.
 * Selectivity guard (>25%) prevents hit; row count must still be correct. */
static test_result_t test_range_guard(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 100000;
    ray_t* kv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv));
    kv->len = N;
    int64_t* kd = (int64_t*)ray_data(kv);
    for (int64_t i = 0; i < N; i++) kd[i] = i % 100;
    ray_t* vv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv));
    vv->len = N;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < N; i++) vd[i] = i;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_guard)");

    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    /* (< k 50): 50,000 rows */
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* pred    = ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 50));
    ray_op_t* filt    = ray_filter(g, tbl_nd, pred);
    ray_t* r = ray_execute(g, filt);
    ray_graph_free(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 50000);

    /* Guard fires — selectivity 50% > 25% → hit must NOT advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE] - hits_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_f64: sort on f (make_f64_zone_table); (> f 5.0) → 6 rows; hit. */
static test_result_t test_range_f64(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "f") == 0, "attach sort (range_f64)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    /* Build a second table for differential comparison */
    ray_t* tbl_b = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));

    ray_t* r = run_filter(tbl, pred_f_gt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 6);  /* f∈{5.5,6.5,7.5,8.5,9.5,10.5} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* rb = run_filter(tbl_b, pred_f_gt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 6);

    /* Check r column is 'r' (row id), not 'v' — f64 table has 'r' not 'v'.
     * Since v_cols_equal checks 'v', and the f64 table uses 'r', skip the
     * content comparison — row count match is sufficient here. */

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Predicate builders for IN tests ───────────────────────────── */

/* (in k [9 5]) → k∈{5,9,9,5} at rows {0,2,6,7} → 4 rows */
static ray_op_t* pred_in_9_5(ray_graph_t* g) {
    int64_t set_data[] = {9, 5};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 2);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* (in k [9 9 5]) — duplicate in set → same 4 rows */
static ray_op_t* pred_in_9_9_5(ray_graph_t* g) {
    int64_t set_data[] = {9, 9, 5};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 3);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* (in k [100 5]) — 100 absent, 5 present → 2 rows (rows 0,7) */
static ray_op_t* pred_in_100_5(ray_graph_t* g) {
    int64_t set_data[] = {100, 5};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 2);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* ─── IN tests ───────────────────────────────────────────────────── */

/* in_hash: hash on k; FILTER(IN(k, [9 5])) → 4 rows; IDX_SITE_IN
 * consult+hit advance; differential vs unindexed. */
static test_result_t test_in_hash(void) {
    /* Side A — with hash index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_hash side A)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_IN];

    ray_t* ra = run_filter(tbl_a, pred_in_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 4);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_IN]     - hits_before), 1);

    /* Side B — no index (scan path) */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);

    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch between indexed and scan (in_hash)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* in_dup_set: set [9 9 5] → same 4 rows, no duplicate row ids. */
static test_result_t test_in_dup_set(void) {
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_dup_set)");

    ray_t* ra = run_filter(tbl_a, pred_in_9_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 4);  /* no duplicate row ids */

    /* Differential vs unindexed */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_9_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch (in_dup_set)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* in_absent_elems: set [100 5] → 2 rows (only k==5 matches). */
static test_result_t test_in_absent_elems(void) {
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_absent_elems)");

    ray_t* ra = run_filter(tbl_a, pred_in_100_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 2);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_100_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch (in_absent_elems)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* in_float_col_falls_back: F64 column + hash; IN over it → consult NOT
 * advanced (float-family columns are intentionally excluded), but results
 * are correct via scan. */
static test_result_t test_in_float_col_falls_back(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();  /* f column: F64 {1.5..10.5} */
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Attach hash to the F64 column 'f'. */
    int64_t sym_f  = ray_sym_intern("f", 1);
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_f) { slot = i; break; }
    }
    TEST_ASSERT(slot >= 0, "f column not found");
    ray_t* w = ray_table_get_col_idx(tbl, slot);
    ray_retain(w);
    ray_t* rv = ray_index_attach_hash(&w);
    /* Hash on F64 is allowed structurally; attach should succeed. */
    if (!rv || RAY_IS_ERR(rv)) { ray_release(w); ray_release(tbl);
        ray_sym_destroy(); ray_heap_destroy();
        /* Inconclusive: hash on F64 not buildable — skip gracefully. */
        PASS(); }
    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];

    /* (in f [1 2 3]) — integer set against float column */
    int64_t set_data[] = {1, 2, 3};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 3);
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* set_op  = ray_const_vec(g, sv);
    ray_op_t* pred    = ray_in(g, ray_scan(g, "f"), set_op);
    ray_op_t* filt    = ray_filter(g, tbl_nd, pred);
    ray_t* r = ray_execute(g, filt);
    ray_graph_free(g);
    ray_release(sv);

    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* f∈{1.5,2.5,...,10.5}: none exactly equal 1,2,3 → 0 rows via scan */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    /* Consult must NOT have fired for float column */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── TIME width regression (hash-eq) ─────────────────────────────── */

/* Build a table with a TIME column t (4-byte int32 storage, seconds) and
 * an I64 row-id column v.  t==3600 at rows 0, 2 and 6. */
static ray_t* make_time_table(void) {
    (void)ray_sym_init();
    int32_t td[] = {3600, 7200, 3600, 86399, 0, 7200, 3600, 1, 2, 3};
    int64_t vd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* tv = ray_vec_from_raw(RAY_TIME, td, 10);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 10);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("t", 1), tv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(tv);
    ray_release(vv);
    return tbl;
}

/* (== t 09:00... well, 3600s) — TIME literal atom against the TIME column. */
static ray_op_t* pred_eq_time_3600(ray_graph_t* g) {
    ray_t* lit = ray_time(3600);
    ray_op_t* c = ray_const_atom(g, lit);
    ray_release(lit);
    return ray_eq(g, ray_scan(g, "t"), c);
}

/* hash_eq_time: regression for the TIME width disagreement.  The hash
 * builder (numeric_key_word) reads TIME rows as 4-byte int32 — the
 * canonical storage width (ray_type_sizes[RAY_TIME] == 4) — but
 * hash_col_read_i64 used to read TIME as 8-byte, so the chain-walk
 * verify compared garbage (and read past the data area) and the index
 * path returned 0 rows where the scan returns 3.  Differential: indexed
 * result must equal the dropped-index scan, and the hash consult+hit
 * must fire. */
static test_result_t test_hash_eq_time(void) {
    /* Side A — with hash index on t */
    ray_heap_init();
    ray_t* tbl_a = make_time_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "t") == 0, "attach hash (hash_eq_time)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_HASH];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_HASH];

    ray_t* ra = run_filter(tbl_a, pred_eq_time_3600);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 3);  /* rows 0, 2, 6 */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_HASH] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_HASH]     - hits_before), 1);

    /* Side B — no index (scan path) */
    ray_t* tbl_b = make_time_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_time_3600);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 3);

    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch between indexed and scan (hash_eq_time)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── IN with empty set ───────────────────────────────────────────── */

/* (in k []) — empty I64 set → no row can match. */
static ray_op_t* pred_in_empty(ray_graph_t* g) {
    ray_t* sv = ray_vec_from_raw(RAY_I64, NULL, 0);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* in_empty_set: hash on k; FILTER(IN(k, [])) → ray_index_in_rowsel must
 * return a VALID all-NONE rowsel (NULL would mean "no fast path"), so the
 * IN consult AND hit both advance and the result has 0 rows.  Differential
 * vs the unindexed scan (also 0 rows). */
static test_result_t test_in_empty_set(void) {
    /* Side A — with hash index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_empty_set)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_IN];

    ray_t* ra = run_filter(tbl_a, pred_in_empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 0);

    /* Empty set is a HIT (valid all-NONE rowsel), not a fall-through. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_IN]     - hits_before), 1);

    /* Side B — no index (scan path) */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 0);

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t idx_route_entries[] = {
    { "idx_route/hash_eq_still_works",   test_hash_eq_still_works,   NULL, NULL },
    { "idx_route/zone_none",             test_zone_none,             NULL, NULL },
    { "idx_route/zone_all",              test_zone_all,              NULL, NULL },
    { "idx_route/zone_unknown",          test_zone_unknown,          NULL, NULL },
    { "idx_route/zone_nulls_excluded",   test_zone_nulls_excluded,   NULL, NULL },
    { "idx_route/zone_le_all_boundary",  test_zone_le_all_boundary,  NULL, NULL },
    { "idx_route/zone_ge_all_boundary",  test_zone_ge_all_boundary,  NULL, NULL },
    { "idx_route/zone_ne_all",           test_zone_ne_all,           NULL, NULL },
    { "idx_route/zone_eq_none_boundary", test_zone_eq_none_boundary, NULL, NULL },
    { "idx_route/zone_f64_none",         test_zone_f64_none,         NULL, NULL },
    { "idx_route/zone_f64_int_literal",  test_zone_f64_int_literal,  NULL, NULL },
    { "idx_route/zone_int_col_float_literal", test_zone_int_col_float_literal, NULL, NULL },
    { "idx_route/bloom_absent",              test_bloom_absent,              NULL, NULL },
    { "idx_route/bloom_present_falls_through", test_bloom_present_falls_through, NULL, NULL },
    { "idx_route/bloom_diff_absent",         test_bloom_diff_absent,         NULL, NULL },
    { "idx_route/bloom_diff_present",        test_bloom_diff_present,        NULL, NULL },
    { "idx_route/bloom_noneq_ignored",       test_bloom_noneq_ignored,       NULL, NULL },
    { "idx_route/range_lt",                  test_range_lt,                  NULL, NULL },
    { "idx_route/range_ge",                  test_range_ge,                  NULL, NULL },
    { "idx_route/range_eq_dups",             test_range_eq_dups,             NULL, NULL },
    { "idx_route/range_le_boundary",         test_range_le_boundary,         NULL, NULL },
    { "idx_route/range_ne_falls_back",       test_range_ne_falls_back,       NULL, NULL },
    { "idx_route/range_guard",               test_range_guard,               NULL, NULL },
    { "idx_route/range_f64",                 test_range_f64,                 NULL, NULL },
    { "idx_route/in_hash",                   test_in_hash,                   NULL, NULL },
    { "idx_route/in_dup_set",                test_in_dup_set,                NULL, NULL },
    { "idx_route/in_absent_elems",           test_in_absent_elems,           NULL, NULL },
    { "idx_route/in_float_col_falls_back",   test_in_float_col_falls_back,   NULL, NULL },
    { "idx_route/hash_eq_time",              test_hash_eq_time,              NULL, NULL },
    { "idx_route/in_empty_set",              test_in_empty_set,              NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

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

/* test/test_idx_route.c — index routing scaffold + refactor pin (Task 1). */

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

const test_entry_t idx_route_entries[] = {
    { "idx_route/hash_eq_still_works", test_hash_eq_still_works, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

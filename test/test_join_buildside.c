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

/* test/test_join_buildside.c — build-side selection knob + differential scaffold */

#include "test.h"
#include "rayforce.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <stdlib.h>
#include <string.h>

/* ── Fixtures ──────────────────────────────────────────────────────────────
 * jb_table1: allocate a single-column I64 table with column name `name`.
 * The returned table is owned by the caller (ray_release when done).
 * ──────────────────────────────────────────────────────────────────────── */
static ray_t* jb_table1(const char* name, const int64_t* vals, int64_t n) {
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, n);
    ray_t* tbl = ray_table_new(1);
    int64_t sym = ray_sym_intern(name, strlen(name));
    tbl = ray_table_add_col(tbl, sym, col);
    ray_release(col);
    return tbl;
}

/* ── Join helper ───────────────────────────────────────────────────────────
 * jb_inner_join: build and execute a single-key I64 inner join.
 *
 * Graph shape (mirrors query.c join_impl):
 *   g = ray_graph_new(lt)          — g->table = lt (used for type inference
 *                                     on lk scan; rk scan's sym resolves
 *                                     against right_table at exec time)
 *   lt_node = ray_const_table(g, lt)
 *   rt_node = ray_const_table(g, rt)
 *   lk_op   = ray_scan(g, lkey)   — OP_SCAN with sym=lkey; exec resolves
 *                                     against left_table via ray_table_get_col
 *   rk_op   = ray_scan(g, rkey)   — OP_SCAN with sym=rkey; exec resolves
 *                                     against right_table via ray_table_get_col
 *                                     (NOT g->table — see exec_join:827-828)
 *   jn      = ray_join(g, lt_node, {lk_op}, rt_node, {rk_op}, 1, 0)
 *
 * The caller owns the returned table (ray_release when done).
 * ──────────────────────────────────────────────────────────────────────── */
static ray_t* jb_inner_join(ray_t* lt, const char* lkey,
                             ray_t* rt, const char* rkey) {
    ray_graph_t* g = ray_graph_new(lt);
    if (!g) return ray_error("oom", "jb_inner_join: graph alloc");

    ray_op_t* lt_node = ray_const_table(g, lt);
    ray_op_t* rt_node = ray_const_table(g, rt);
    ray_op_t* lk_op   = ray_scan(g, lkey);
    ray_op_t* rk_op   = ray_scan(g, rkey);

    if (!lt_node || !rt_node || !lk_op || !rk_op) {
        ray_graph_free(g);
        return ray_error("oom", "jb_inner_join: node alloc");
    }

    ray_op_t* lk_arr[1] = { lk_op };
    ray_op_t* rk_arr[1] = { rk_op };
    ray_op_t* jn = ray_join(g, lt_node, lk_arr, rt_node, rk_arr, 1, 0);
    if (!jn) { ray_graph_free(g); return ray_error("oom", "jb_inner_join: join node"); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

/* ── Two-column table helper ──────────────────────────────────────────────
 * jb_table2: allocate a two-column I64 table with column names n0/n1.
 * v0[]/v1[] must have `n` elements each.  Caller owns the returned table.
 * ──────────────────────────────────────────────────────────────────────── */
static ray_t* jb_table2(const char* n0, const int64_t* v0,
                         const char* n1, const int64_t* v1, int64_t n) {
    ray_t* c0 = ray_vec_from_raw(RAY_I64, v0, n);
    ray_t* c1 = ray_vec_from_raw(RAY_I64, v1, n);
    ray_t* tbl = ray_table_new(2);
    int64_t s0 = ray_sym_intern(n0, strlen(n0));
    int64_t s1 = ray_sym_intern(n1, strlen(n1));
    tbl = ray_table_add_col(tbl, s0, c0);
    tbl = ray_table_add_col(tbl, s1, c1);
    ray_release(c0);
    ray_release(c1);
    return tbl;
}

/* ── Two-key inner join ────────────────────────────────────────────────────
 * jb_inner_join2: like jb_inner_join but for two composite keys.
 * ──────────────────────────────────────────────────────────────────────── */
static ray_t* jb_inner_join2(ray_t* lt, const char* lk0, const char* lk1,
                              ray_t* rt, const char* rk0, const char* rk1) {
    ray_graph_t* g = ray_graph_new(lt);
    if (!g) return ray_error("oom", "jb_inner_join2: graph alloc");

    ray_op_t* lt_node = ray_const_table(g, lt);
    ray_op_t* rt_node = ray_const_table(g, rt);
    ray_op_t* lk0_op  = ray_scan(g, lk0);
    ray_op_t* lk1_op  = ray_scan(g, lk1);
    ray_op_t* rk0_op  = ray_scan(g, rk0);
    ray_op_t* rk1_op  = ray_scan(g, rk1);

    if (!lt_node || !rt_node || !lk0_op || !lk1_op || !rk0_op || !rk1_op) {
        ray_graph_free(g);
        return ray_error("oom", "jb_inner_join2: node alloc");
    }

    ray_op_t* lk_arr[2] = { lk0_op, lk1_op };
    ray_op_t* rk_arr[2] = { rk0_op, rk1_op };
    ray_op_t* jn = ray_join(g, lt_node, lk_arr, rt_node, rk_arr, 2, 0);
    if (!jn) { ray_graph_free(g); return ray_error("oom", "jb_inner_join2: join node"); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

/* ── Row sort (no globals) ─────────────────────────────────────────────────
 * jb_sort_rows: sort index array idx[0..n) by lexicographic row order over
 * cols[0..ncols).  NULLs sort before non-NULLs.
 *
 * Implementation: iterative bottom-up merge sort.  O(n log n) time,
 * O(n) scratch space (tmp array allocated by the caller and passed in).
 * No file-scope globals — cols/ncols are threaded through every call.
 * ──────────────────────────────────────────────────────────────────────── */
static int jb_row_compare(int64_t ra, int64_t rb,
                           ray_t** cols, int64_t ncols) {
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = cols[c];
        bool na = ray_vec_is_null(col, ra);
        bool nb = ray_vec_is_null(col, rb);
        if (na && nb) continue;
        if (na)       return -1;
        if (nb)       return  1;
        int64_t va = ray_vec_get_i64(col, ra);
        int64_t vb = ray_vec_get_i64(col, rb);
        if (va < vb) return -1;
        if (va > vb) return  1;
    }
    return 0;
}

/* Merge two sorted runs [lo, mid) and [mid, hi) in idx[], using tmp[] as
 * scratch.  cols/ncols provide the row comparison context. */
static void jb_merge(int64_t* idx, int64_t* tmp,
                     int64_t lo, int64_t mid, int64_t hi,
                     ray_t** cols, int64_t ncols) {
    int64_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (jb_row_compare(idx[i], idx[j], cols, ncols) <= 0)
            tmp[k++] = idx[i++];
        else
            tmp[k++] = idx[j++];
    }
    while (i < mid) tmp[k++] = idx[i++];
    while (j < hi)  tmp[k++] = idx[j++];
    for (int64_t x = lo; x < hi; x++) idx[x] = tmp[x];
}

/* Iterative bottom-up merge sort.  tmp must be at least n elements. */
static void jb_sort_rows(ray_t** cols, int64_t ncols,
                         int64_t* idx, int64_t* tmp, int64_t n) {
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * width) {
            int64_t mid = lo + width;
            int64_t hi  = lo + 2 * width;
            if (mid > n) mid = n;
            if (hi  > n) hi  = n;
            if (mid < hi)
                jb_merge(idx, tmp, lo, mid, hi, cols, ncols);
        }
    }
}

/* ── Multiset comparison ───────────────────────────────────────────────────
 * jb_results_equal: compare two I64-only result tables as multisets.
 * Sort each by a lexicographic row order (column 0 primary, column 1
 * secondary, …) then compare cell-by-cell in sorted order.
 *
 * NULLs sort before non-NULLs (consistent within both tables).
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t jb_results_equal(ray_t* a, ray_t* b) {
    int64_t ncols = ray_table_ncols(a);
    int64_t nrows = ray_table_nrows(a);
    TEST_ASSERT_EQ_I(ncols, ray_table_ncols(b));
    TEST_ASSERT_EQ_I(nrows, ray_table_nrows(b));

    /* 0-row result: ncols already verified equal; nothing to sort/compare. */
    if (nrows == 0)
        return (test_result_t){ TEST_PASS, NULL };

    int64_t* ia      = NULL;
    int64_t* ib      = NULL;
    int64_t* tmp     = NULL;
    ray_t**  cols_a  = NULL;
    ray_t**  cols_b  = NULL;

    test_result_t result = { TEST_PASS, NULL };

    ia = (int64_t*)malloc((size_t)nrows * sizeof(int64_t));
    ib = (int64_t*)malloc((size_t)nrows * sizeof(int64_t));
    if (!ia || !ib) {
        result = (test_result_t){ TEST_FAIL, "jb_results_equal: malloc ia/ib" };
        goto cleanup;
    }
    for (int64_t r = 0; r < nrows; r++) { ia[r] = r; ib[r] = r; }

    tmp = (int64_t*)malloc((size_t)nrows * sizeof(int64_t));
    if (!tmp) {
        result = (test_result_t){ TEST_FAIL, "jb_results_equal: malloc tmp" };
        goto cleanup;
    }

    cols_a = (ray_t**)malloc((size_t)ncols * sizeof(ray_t*));
    if (!cols_a) {
        result = (test_result_t){ TEST_FAIL, "jb_results_equal: malloc cols_a" };
        goto cleanup;
    }
    for (int64_t c = 0; c < ncols; c++)
        cols_a[c] = ray_table_get_col_idx(a, c);
    jb_sort_rows(cols_a, ncols, ia, tmp, nrows);

    cols_b = (ray_t**)malloc((size_t)ncols * sizeof(ray_t*));
    if (!cols_b) {
        result = (test_result_t){ TEST_FAIL, "jb_results_equal: malloc cols_b" };
        goto cleanup;
    }
    for (int64_t c = 0; c < ncols; c++)
        cols_b[c] = ray_table_get_col_idx(b, c);
    jb_sort_rows(cols_b, ncols, ib, tmp, nrows);

    /* Compare sorted rows cell-by-cell */
    for (int64_t r = 0; r < nrows && result.status == TEST_PASS; r++) {
        int64_t ra = ia[r], rb = ib[r];
        for (int64_t c = 0; c < ncols; c++) {
            bool na = ray_vec_is_null(cols_a[c], ra);
            bool nb = ray_vec_is_null(cols_b[c], rb);
            if (na != nb) {
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "null mismatch at sorted row %lld col %lld",
                         (long long)r, (long long)c);
                result = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                break;
            }
            if (!na) {
                int64_t va = ray_vec_get_i64(cols_a[c], ra);
                int64_t vb = ray_vec_get_i64(cols_b[c], rb);
                if (va != vb) {
                    snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                             "value mismatch at sorted row %lld col %lld: %lld vs %lld",
                             (long long)r, (long long)c,
                             (long long)va, (long long)vb);
                    result = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                    break;
                }
            }
        }
    }

cleanup:
    free(ia);
    free(ib);
    free(tmp);
    free(cols_a);
    free(cols_b);
    return result;
}

/* ── Baseline test ─────────────────────────────────────────────────────────
 * Build a right-side table larger than RAY_PARALLEL_THRESHOLD to trigger
 * the radix path.  Run the join twice: once with the no-swap knob set
 * (legacy build-on-right) and once with it cleared (future swap logic).
 * Today both runs are identical, so jb_results_equal passes trivially.
 * This test pins the harness shape for Task 2.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_baseline_radix_inner(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000;   /* right > RAY_PARALLEL_THRESHOLD */
    int64_t n_l = 2000;

    int64_t* rv = (int64_t*)malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv = (int64_t*)malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");

    for (int64_t i = 0; i < n_r; i++) rv[i] = i % 1000;
    for (int64_t i = 0; i < n_l; i++) lv[i] = i % 1000;

    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);

    ray_join_no_build_swap = true;
    ray_t* a = jb_inner_join(lt, "lk", rt, "rk");

    ray_join_no_build_swap = false;
    ray_t* b = jb_inner_join(lt, "lk", rt, "rk");

    ray_join_no_build_swap = false;   /* always reset */

    TEST_ASSERT(a && !RAY_IS_ERR(a), "join (no-swap) returned error");
    TEST_ASSERT(b && !RAY_IS_ERR(b), "join (default) returned error");

    test_result_t rr = jb_results_equal(a, b);

    ray_release(a);
    ray_release(b);
    ray_release(lt);
    ray_release(rt);
    free(lv);
    free(rv);
    ray_sym_destroy();
    ray_heap_destroy();
    return rr;
}

/* ── Differential swap test ────────────────────────────────────────────────
 * Left side (2000 rows) is much smaller than the right side (>threshold), so
 * the build-side decision must fire and build the hash on the small left side.
 * The swapped result must be a multiset-identical match to the forced-legacy
 * (build-on-right) result, AND the swap counter must increment.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_swap_inner_matches(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000, n_l = 2000;
    int64_t* rv = malloc(n_r*sizeof(int64_t)); int64_t* lv = malloc(n_l*sizeof(int64_t));
    for (int64_t i=0;i<n_r;i++) rv[i]=i%1000;
    for (int64_t i=0;i<n_l;i++) lv[i]=i%1000;
    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    uint64_t before = ray_join_build_swaps;
    ray_join_no_build_swap = false;          /* allow swap */
    ray_t* swapped = jb_inner_join(lt,"lk",rt,"rk");
    bool fired = ray_join_build_swaps > before;
    ray_join_no_build_swap = true;           /* force no swap */
    ray_t* plain = jb_inner_join(lt,"lk",rt,"rk");
    ray_join_no_build_swap = false;
    test_result_t rr = jb_results_equal(swapped, plain);
    if (rr.status == TEST_PASS && !fired)
        rr = (test_result_t){ TEST_FAIL, "expected build-side swap to fire" };
    ray_release(swapped); ray_release(plain); ray_release(lt); ray_release(rt);
    free(lv); free(rv); ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Differential wrapper ──────────────────────────────────────────────────
 * jb_diff: run the inner join swap-enabled vs knob-forced-no-swap and assert
 * multiset equality.  When expect_swap is true the counter must advance.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t jb_diff(ray_t* lt, const char* lkey,
                              ray_t* rt, const char* rkey, bool expect_swap) {
    uint64_t before = ray_join_build_swaps;
    ray_join_no_build_swap = false;
    ray_t* sw = jb_inner_join(lt, lkey, rt, rkey);
    bool fired = ray_join_build_swaps > before;
    ray_join_no_build_swap = true;
    ray_t* pl = jb_inner_join(lt, lkey, rt, rkey);
    ray_join_no_build_swap = false;
    test_result_t rr = jb_results_equal(sw, pl);
    if (rr.status == TEST_PASS && expect_swap != fired)
        rr = (test_result_t){ TEST_FAIL,
            expect_swap ? "expected swap to fire" : "swap fired unexpectedly" };
    ray_release(sw); ray_release(pl);
    return rr;
}

/* ── Edge fixture: many-to-many ────────────────────────────────────────────
 * right n=T+5000 keys i%50, left n=500 keys i%50 — heavy m:n fanout.
 * Swap fires (left < right).
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_many_to_many(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000;
    int64_t n_l = 500;
    int64_t* rv = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv = malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i = 0; i < n_r; i++) rv[i] = i % 50;
    for (int64_t i = 0; i < n_l; i++) lv[i] = i % 50;

    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    test_result_t rr = jb_diff(lt, "lk", rt, "rk", /*expect_swap=*/true);

    ray_release(lt); ray_release(rt);
    free(lv); free(rv);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Edge fixture: no matches ──────────────────────────────────────────────
 * right keys i%1000, left keys 1000+(i%1000) — disjoint, 0 output rows.
 * Swap fires (left < right); jb_results_equal handles 0-row result.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_no_matches(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000;
    int64_t n_l = 2000;
    int64_t* rv = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv = malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i = 0; i < n_r; i++) rv[i] = i % 1000;
    for (int64_t i = 0; i < n_l; i++) lv[i] = 1000 + (i % 1000);

    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    test_result_t rr = jb_diff(lt, "lk", rt, "rk", /*expect_swap=*/true);

    ray_release(lt); ray_release(rt);
    free(lv); free(rv);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Edge fixture: all match ───────────────────────────────────────────────
 * right n=T+2000 all key 7, left n=1 all key 7 — full cross-product.
 * (right=67536 × left=1 = 67,536 output rows; stresses HT-grow path.)
 * Swap fires (left << right).
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_all_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 2000;
    int64_t n_l = 1;
    int64_t* rv = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv = malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i = 0; i < n_r; i++) rv[i] = 7;
    for (int64_t i = 0; i < n_l; i++) lv[i] = 7;

    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    test_result_t rr = jb_diff(lt, "lk", rt, "rk", /*expect_swap=*/true);

    ray_release(lt); ray_release(rt);
    free(lv); free(rv);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Edge fixture: null keys ───────────────────────────────────────────────
 * right n=T+5000 keys i%1000 (some null), left n=2000 keys i%1000 (some
 * null).  Swap path must match no-swap for whatever null-key semantics the
 * engine applies (NULLs never match NULLs in SQL inner join).
 *
 * Nulling a table column: get the column vec via ray_table_get_col_idx
 * (returns the live vec owned by the table), then call ray_vec_set_null on
 * it directly — the table owns the vec so the mutation is in-place.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_null_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000;
    int64_t n_l = 2000;
    int64_t* rv = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv = malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i = 0; i < n_r; i++) rv[i] = i % 1000;
    for (int64_t i = 0; i < n_l; i++) lv[i] = i % 1000;

    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);

    /* Null a handful of rows in each table's key column via the live vec. */
    ray_t* rc = ray_table_get_col_idx(rt, 0);
    ray_t* lc = ray_table_get_col_idx(lt, 0);
    TEST_ASSERT(rc && !RAY_IS_ERR(rc), "rt col 0");
    TEST_ASSERT(lc && !RAY_IS_ERR(lc), "lt col 0");
    ray_vec_set_null(rc, 0, true);
    ray_vec_set_null(rc, 100, true);
    ray_vec_set_null(rc, 999, true);
    ray_vec_set_null(lc, 1, true);
    ray_vec_set_null(lc, 500, true);

    test_result_t rr = jb_diff(lt, "lk", rt, "rk", /*expect_swap=*/true);

    ray_release(lt); ray_release(rt);
    free(lv); free(rv);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Edge fixture: near-equal, no swap ────────────────────────────────────
 * Both sides n=T+1000, keys i%10000.  left_rows == right_rows, so swap must
 * NOT fire (strict less-than condition fails).
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_near_equal_no_swap(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = RAY_PARALLEL_THRESHOLD + 1000;
    int64_t* rv = malloc((size_t)n * sizeof(int64_t));
    int64_t* lv = malloc((size_t)n * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i = 0; i < n; i++) rv[i] = i % 10000;
    for (int64_t i = 0; i < n; i++) lv[i] = i % 10000;

    ray_t* rt = jb_table1("rk", rv, n);
    ray_t* lt = jb_table1("lk", lv, n);
    test_result_t rr = jb_diff(lt, "lk", rt, "rk", /*expect_swap=*/false);

    ray_release(lt); ray_release(rt);
    free(lv); free(rv);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Edge fixture: multi-key ───────────────────────────────────────────────
 * Two-column inner join (k0=i%100, k1=i%7).  right n=T+5000, left n=2000.
 * Swap fires (left < right).
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_multi_key(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000;
    int64_t n_l = 2000;
    int64_t* rv0 = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* rv1 = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv0 = malloc((size_t)n_l * sizeof(int64_t));
    int64_t* lv1 = malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv0 && rv1 && lv0 && lv1, "malloc key arrays");
    for (int64_t i = 0; i < n_r; i++) { rv0[i] = i % 100; rv1[i] = i % 7; }
    for (int64_t i = 0; i < n_l; i++) { lv0[i] = i % 100; lv1[i] = i % 7; }

    ray_t* rt = jb_table2("rk0", rv0, "rk1", rv1, n_r);
    ray_t* lt = jb_table2("lk0", lv0, "lk1", lv1, n_l);

    /* jb_diff only handles single-key; run two-key inline. */
    uint64_t before = ray_join_build_swaps;
    ray_join_no_build_swap = false;
    ray_t* sw = jb_inner_join2(lt, "lk0", "lk1", rt, "rk0", "rk1");
    bool fired = ray_join_build_swaps > before;
    ray_join_no_build_swap = true;
    ray_t* pl = jb_inner_join2(lt, "lk0", "lk1", rt, "rk0", "rk1");
    ray_join_no_build_swap = false;

    test_result_t rr = jb_results_equal(sw, pl);
    if (rr.status == TEST_PASS && !fired)
        rr = (test_result_t){ TEST_FAIL, "expected swap to fire (multi-key)" };
    ray_release(sw); ray_release(pl);

    ray_release(lt); ray_release(rt);
    free(lv0); free(lv1); free(rv0); free(rv1);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Edge fixture: left bigger, no swap ───────────────────────────────────
 * right n=2000 (below RAY_PARALLEL_THRESHOLD → chained path, radix never
 * entered).  left n=T+5000.  Swap never fires; result is correct via the
 * chained path.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_left_bigger_no_swap(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = 2000;
    int64_t n_l = RAY_PARALLEL_THRESHOLD + 5000;
    int64_t* rv = malloc((size_t)n_r * sizeof(int64_t));
    int64_t* lv = malloc((size_t)n_l * sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i = 0; i < n_r; i++) rv[i] = i % 1000;
    for (int64_t i = 0; i < n_l; i++) lv[i] = i % 1000;

    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    test_result_t rr = jb_diff(lt, "lk", rt, "rk", /*expect_swap=*/false);

    ray_release(lt); ray_release(rt);
    free(lv); free(rv);
    ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Forced dup-fallback on ordinary data ──────────────────────────────────
 * The force knob makes every radix partition bail to the chained path before
 * allocating anything.  On ordinary low-duplication data the chained-build
 * result must be multiset-identical to the radix-build result, AND the
 * dup-fallback counter must advance (proving the routing fired).
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_force_fallback_ordinary(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000, n_l = 4000;   /* right > threshold → radix */
    int64_t* rv = malloc((size_t)n_r*sizeof(int64_t));
    int64_t* lv = malloc((size_t)n_l*sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc key arrays");
    for (int64_t i=0;i<n_r;i++) rv[i]=i%2000;      /* low dup ~35/key */
    for (int64_t i=0;i<n_l;i++) lv[i]=i%2000;
    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    uint64_t before = ray_join_dup_fallbacks;
    ray_join_force_dup_fallback = true;            /* → chained */
    ray_t* chained = jb_inner_join(lt,"lk",rt,"rk");
    bool fired = ray_join_dup_fallbacks > before;
    ray_join_force_dup_fallback = false;           /* → radix */
    ray_t* radix = jb_inner_join(lt,"lk",rt,"rk");
    test_result_t rr = jb_results_equal(chained, radix);
    if (rr.status == TEST_PASS && !fired)
        rr = (test_result_t){ TEST_FAIL, "forced dup-fallback did not fire" };
    ray_release(chained); ray_release(radix); ray_release(lt); ray_release(rt);
    free(lv); free(rv); ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* A join whose BUILD side has heavy per-key duplication (run > RADIX_DUP_RUN_MAX)
 * must auto-fall-back to the chained path and produce correct results.
 *
 * The radix path builds on the SMALLER side (INNER build-side swap), so to trip
 * the build-loop run counter the duplication must live on that smaller side:
 * the left side here has only 4 distinct keys (~2000 rows/key → run > 512), and
 * stays smaller than the right (so it is the build side and the right > the
 * parallel threshold to select the radix path).  The right side is low-dup with
 * few matching keys, keeping the PRE-FIX quadratic build and the join output
 * small (sub-0.1s, no catastrophic blow-up). */
static test_result_t test_jb_auto_fallback_dup(void) {
    ray_heap_init();
    (void)ray_sym_init();
    int64_t n_r = RAY_PARALLEL_THRESHOLD + 5000, n_l = 8000;
    int64_t* rv = malloc(n_r*sizeof(int64_t)); int64_t* lv = malloc(n_l*sizeof(int64_t));
    TEST_ASSERT(rv && lv, "malloc");
    for (int64_t i=0;i<n_r;i++) rv[i]=i%2000;      /* probe side: low dup, ~35/key */
    for (int64_t i=0;i<n_l;i++) lv[i]=i%4;          /* build side: ~2000/key → run > 512 → trips */
    ray_t* rt = jb_table1("rk", rv, n_r);
    ray_t* lt = jb_table1("lk", lv, n_l);
    uint64_t before = ray_join_dup_fallbacks;
    ray_t* got = jb_inner_join(lt,"lk",rt,"rk");   /* knob off; auto-trip expected */
    bool fired = ray_join_dup_fallbacks > before;
    ray_join_force_dup_fallback = true;            /* oracle: forced chained */
    ray_t* oracle = jb_inner_join(lt,"lk",rt,"rk");
    ray_join_force_dup_fallback = false;
    test_result_t rr = jb_results_equal(got, oracle);
    if (rr.status == TEST_PASS && !fired)
        rr = (test_result_t){ TEST_FAIL, "expected auto dup-fallback to fire" };
    ray_release(got); ray_release(oracle); ray_release(lt); ray_release(rt);
    free(lv); free(rv); ray_sym_destroy(); ray_heap_destroy();
    return rr;
}

/* ── Entry table ─────────────────────────────────────────────────────────── */

const test_entry_t join_buildside_entries[] = {
    { "join_buildside/force_fallback_ordinary", test_jb_force_fallback_ordinary, NULL, NULL },
    { "join_buildside/auto_fallback_dup", test_jb_auto_fallback_dup, NULL, NULL },
    { "join_buildside/baseline_radix_inner", test_jb_baseline_radix_inner, NULL, NULL },
    { "join_buildside/swap_inner_matches", test_jb_swap_inner_matches, NULL, NULL },
    { "join_buildside/many_to_many", test_jb_many_to_many, NULL, NULL },
    { "join_buildside/no_matches", test_jb_no_matches, NULL, NULL },
    { "join_buildside/all_match", test_jb_all_match, NULL, NULL },
    { "join_buildside/null_keys", test_jb_null_keys, NULL, NULL },
    { "join_buildside/near_equal_no_swap", test_jb_near_equal_no_swap, NULL, NULL },
    { "join_buildside/multi_key", test_jb_multi_key, NULL, NULL },
    { "join_buildside/left_bigger_no_swap", test_jb_left_bigger_no_swap, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

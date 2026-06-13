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

/* ── Multiset comparison ───────────────────────────────────────────────────
 * jb_results_equal: compare two I64-only result tables as multisets.
 * Sort each by a lexicographic row order (column 0 primary, column 1
 * secondary, …) then compare cell-by-cell in sorted order.
 *
 * NULLs sort before non-NULLs (consistent within both tables).
 * ──────────────────────────────────────────────────────────────────────── */

/* Context threaded through qsort comparator */
static ray_t**  jb_cmp_cols   = NULL;
static int64_t  jb_cmp_ncols  = 0;

static int jb_row_cmp(const void* pa, const void* pb) {
    int64_t ra = *(const int64_t*)pa;
    int64_t rb = *(const int64_t*)pb;
    for (int64_t c = 0; c < jb_cmp_ncols; c++) {
        ray_t* col = jb_cmp_cols[c];
        bool na = ray_vec_is_null(col, ra);
        bool nb = ray_vec_is_null(col, rb);
        if (na && nb) continue;
        if (na) return -1;
        if (nb) return  1;
        int64_t va = ray_vec_get_i64(col, ra);
        int64_t vb = ray_vec_get_i64(col, rb);
        if (va < vb) return -1;
        if (va > vb) return  1;
    }
    return 0;
}

static test_result_t jb_results_equal(ray_t* a, ray_t* b) {
    int64_t ncols = ray_table_ncols(a);
    int64_t nrows = ray_table_nrows(a);
    TEST_ASSERT_EQ_I(ncols, ray_table_ncols(b));
    TEST_ASSERT_EQ_I(nrows, ray_table_nrows(b));

    /* Build row-index arrays for sorting */
    int64_t* ia = (int64_t*)malloc((size_t)nrows * sizeof(int64_t));
    int64_t* ib = (int64_t*)malloc((size_t)nrows * sizeof(int64_t));
    TEST_ASSERT(ia && ib, "jb_results_equal: malloc");
    for (int64_t r = 0; r < nrows; r++) { ia[r] = r; ib[r] = r; }

    /* Sort table a */
    ray_t** cols_a = (ray_t**)malloc((size_t)ncols * sizeof(ray_t*));
    TEST_ASSERT(cols_a != NULL, "jb_results_equal: malloc cols_a");
    for (int64_t c = 0; c < ncols; c++)
        cols_a[c] = ray_table_get_col_idx(a, c);
    jb_cmp_cols  = cols_a;
    jb_cmp_ncols = ncols;
    qsort(ia, (size_t)nrows, sizeof(int64_t), jb_row_cmp);

    /* Sort table b */
    ray_t** cols_b = (ray_t**)malloc((size_t)ncols * sizeof(ray_t*));
    TEST_ASSERT(cols_b != NULL, "jb_results_equal: malloc cols_b");
    for (int64_t c = 0; c < ncols; c++)
        cols_b[c] = ray_table_get_col_idx(b, c);
    jb_cmp_cols  = cols_b;
    jb_cmp_ncols = ncols;
    qsort(ib, (size_t)nrows, sizeof(int64_t), jb_row_cmp);

    jb_cmp_cols  = NULL;
    jb_cmp_ncols = 0;

    /* Compare sorted rows cell-by-cell */
    test_result_t result = { TEST_PASS, NULL };
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

    free(ia); free(ib);
    free(cols_a); free(cols_b);
    return result;
}

/* ── Baseline test ─────────────────────────────────────────────────────────
 * Build a right-side table larger than RAY_PARALLEL_THRESHOLD (64*1024) to
 * trigger the radix path.  Run the join twice: once with the no-swap knob
 * set (legacy build-on-right) and once with it cleared (future swap logic).
 * Today both runs are identical, so jb_results_equal passes trivially.
 * This test pins the harness shape for Task 2.
 * ──────────────────────────────────────────────────────────────────────── */
static test_result_t test_jb_baseline_radix_inner(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_r = (64 * 1024) + 5000;   /* right > RAY_PARALLEL_THRESHOLD */
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

/* ── Entry table ─────────────────────────────────────────────────────────── */

const test_entry_t join_buildside_entries[] = {
    { "join_buildside/baseline_radix_inner", test_jb_baseline_radix_inner, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

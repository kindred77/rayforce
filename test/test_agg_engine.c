/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

/*
 * test_agg_engine.c — unit tests for the v2 aggregation engine admission
 * gate (agg_v2_can_handle).  These build group DAG nodes via the C API
 * (mirroring test_group_extra.c) and assert the gate verdict directly,
 * without executing the node.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "ops/agg_engine.h"
#include "ops/agg_acc.h"
#include "ops/agg_registry.h"
#include "core/pool.h"
#include "table/sym.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Build a single-column table with the given type, name, n rows = 8. */
static ray_t* make_typed_table(int8_t type, const char* col, int64_t n) {
    ray_t* vec = ray_vec_new(type, n);
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    vec->len = n;
    int64_t name = ray_sym_intern(col, (int32_t)strlen(col));
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);
    return tbl;
}

/* Build a 2-column table: key column "k" of ktype + value column "v" of vtype. */
static ray_t* make_kv_table(int8_t ktype, int8_t vtype, int64_t n) {
    ray_t* kvec = ray_vec_new(ktype, n);
    ray_t* vvec = ray_vec_new(vtype, n);
    if (!kvec || RAY_IS_ERR(kvec) || !vvec || RAY_IS_ERR(vvec)) return NULL;
    kvec->len = n;
    vvec->len = n;
    int64_t kname = ray_sym_intern("k", 1);
    int64_t vname = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, kname, kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, vname, vvec); ray_release(vvec);
    return tbl;
}

/* Case 1: single I64 key + one OP_SUM over an I64 column → admit. */
static test_result_t test_gate_admits_i64_key_sum_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_kv_table(RAY_I64, RAY_I64, 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    ray_op_t* scan_v = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 2: two supported-type keys → admit (1b multi-key). */
static test_result_t test_gate_admits_two_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a 3-col table: k1, k2 (I64), v (I64). */
    ray_t* k1 = ray_vec_new(RAY_I64, 8); k1->len = 8;
    ray_t* k2 = ray_vec_new(RAY_I64, 8); k2->len = 8;
    ray_t* vv = ray_vec_new(RAY_I64, 8); vv->len = 8;
    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, s_k2, k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, s_v,  vv); ray_release(vv);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k1, scan_k2 };
    ray_op_t* grp = ray_group(g, keys, 2, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 3: single I64 key + OP_SUM over an I32 column (not registered) → defer. */
static test_result_t test_gate_defers_sum_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_kv_table(RAY_I64, RAY_I32, 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    ray_op_t* scan_v = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_FALSE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 4: single I64 key + OP_COUNT → admit. */
static test_result_t test_gate_admits_count(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_typed_table(RAY_I64, "k", 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    uint16_t  ops[]  = { OP_COUNT };
    ray_op_t* ins[]  = { scan_k };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test for agg_group_keys (single key): first-occurrence dense gid
 * assignment. Key column {5,3,5,3,5,7} → gids {0,1,0,1,0,2},
 * first_row {0,1,5}, ngroups 3. */
static test_result_t test_group_keys_i_first_occurrence(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t src[] = { 5, 3, 5, 3, 5, 7 };
    const int64_t n = (int64_t)(sizeof(src) / sizeof(src[0]));
    ray_t* col = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    col->len = n;
    memcpy(ray_data(col), src, sizeof(src));

    agg_groups_t out;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(&col, 1, n, &out)));

    TEST_ASSERT_EQ_I((3), (out.ngroups));
    TEST_ASSERT_EQ_I((0), (out.first_row[0]));
    TEST_ASSERT_EQ_I((1), (out.first_row[1]));
    TEST_ASSERT_EQ_I((5), (out.first_row[2]));

    const uint32_t expect[] = { 0, 1, 0, 1, 0, 2 };
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(((int)expect[i]), ((int)out.gids[i]));
    }

    free(out.gids);
    free(out.first_row);
    ray_release(col);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test exercising the narrow-width path: I32 key column. */
static test_result_t test_group_keys_i_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int32_t src[] = { 100, 200, 100, 300, 200, 100 };
    const int64_t n = (int64_t)(sizeof(src) / sizeof(src[0]));
    ray_t* col = ray_vec_new(RAY_I32, n);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    col->len = n;
    memcpy(ray_data(col), src, sizeof(src));

    agg_groups_t out;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(&col, 1, n, &out)));

    TEST_ASSERT_EQ_I((3), (out.ngroups));
    TEST_ASSERT_EQ_I((0), (out.first_row[0]));
    TEST_ASSERT_EQ_I((1), (out.first_row[1]));
    TEST_ASSERT_EQ_I((3), (out.first_row[2]));

    const uint32_t expect[] = { 0, 1, 0, 2, 1, 0 };
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(((int)expect[i]), ((int)out.gids[i]));
    }

    free(out.gids);
    free(out.first_row);
    ray_release(col);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test for agg_run_one: key {1,1,2}, value {10,20,30}.
 * gids {0,0,1}, ngroups 2. SUM={30,30} MIN={10,30} MAX={20,30} COUNT={2,1}. */
static test_result_t test_agg_run_one_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t ksrc[] = { 1, 1, 2 };
    const int64_t vsrc[] = { 10, 20, 30 };
    const int64_t n = 3;

    ray_t* kcol = ray_vec_new(RAY_I64, n);
    ray_t* vcol = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(kcol);
    TEST_ASSERT_NOT_NULL(vcol);
    kcol->len = n; vcol->len = n;
    memcpy(ray_data(kcol), ksrc, sizeof(ksrc));
    memcpy(ray_data(vcol), vsrc, sizeof(vsrc));

    agg_groups_t gr;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(&kcol, 1, n, &gr)));
    TEST_ASSERT_EQ_I((2), (gr.ngroups));

    struct { uint16_t op; ray_t* val; int64_t e0; int64_t e1; } cases[] = {
        { OP_SUM,   vcol, 30, 30 },
        { OP_MIN,   vcol, 10, 30 },
        { OP_MAX,   vcol, 20, 30 },
        { OP_COUNT, NULL,  2,  1 },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* out = agg_run_one(vt, cases[c].val, gr.gids, n, gr.ngroups);
        TEST_ASSERT_NOT_NULL(out);
        TEST_ASSERT_FALSE(RAY_IS_ERR(out));
        TEST_ASSERT_EQ_I((RAY_I64), (out->type));
        TEST_ASSERT_EQ_I((2), ((int)out->len));
        const int64_t* d = (const int64_t*)ray_data(out);
        TEST_ASSERT_EQ_I((cases[c].e0), (d[0]));
        TEST_ASSERT_EQ_I((cases[c].e1), (d[1]));
        ray_release(out);
    }

    free(gr.gids);
    free(gr.first_row);
    ray_release(kcol);
    ray_release(vcol);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test for agg_group_keys with two I64 keys: tuple-hash grouping.
 * kA={1,1,2,1}, kB={9,9,8,8} → tuples (1,9),(1,9),(2,8),(1,8)
 * → ngroups 3, gids {0,0,1,2}, first_row {0,2,3}. */
static test_result_t test_group_keys_multi(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t a_src[] = { 1, 1, 2, 1 };
    const int64_t b_src[] = { 9, 9, 8, 8 };
    const int64_t n = (int64_t)(sizeof(a_src) / sizeof(a_src[0]));
    ray_t* ka = ray_vec_new(RAY_I64, n);
    ray_t* kb = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(ka);
    TEST_ASSERT_NOT_NULL(kb);
    ka->len = n; kb->len = n;
    memcpy(ray_data(ka), a_src, sizeof(a_src));
    memcpy(ray_data(kb), b_src, sizeof(b_src));

    ray_t* cols[2] = { ka, kb };
    agg_groups_t out;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(cols, 2, n, &out)));

    TEST_ASSERT_EQ_I((3), (out.ngroups));
    const uint32_t expect_gids[] = { 0, 0, 1, 2 };
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(((int)expect_gids[i]), ((int)out.gids[i]));
    }
    TEST_ASSERT_EQ_I((0), (out.first_row[0]));
    TEST_ASSERT_EQ_I((2), (out.first_row[1]));
    TEST_ASSERT_EQ_I((3), (out.first_row[2]));

    free(out.gids);
    free(out.first_row);
    ray_release(ka);
    ray_release(kb);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ──────────────────────────────────────────────────────────────────────
 * End-to-end differential: OP_GROUP executed with the v2 engine OFF then ON
 * must produce byte-equivalent tables.
 *
 * KEY-ORDER: the OLD engine's single-key path emits groups in *sorted*
 * (ascending key) order — the dense-accumulator path iterates DA slots
 * `for (s = 0; s < n_slots; s++)`, and slot = key - key_min, so output is
 * ordered by key value (group.c:6828).  At N=70000 the OLD engine also takes
 * a hash/parallel path which is NOT first-occurrence either.  The v2 engine
 * emits *first-occurrence* order (agg_group_keys).  These orders genuinely
 * differ, so table_expect_equal compares the two results as MULTISETS keyed
 * on the (single) key column: it builds a permutation of each table sorted by
 * the key column's int-coded value, then compares row-by-row through that
 * permutation.  This is the documented parity target — equality of grouped
 * results is order-independent, and sorting both sides by key is the minimal
 * canonical form that both engines agree on.
 * ────────────────────────────────────────────────────────────────────── */

/* Read column cell `i` widened to int64 (key/int columns); F64 read separately. */
static int64_t col_read_i64(ray_t* c, int64_t i) {
    uint8_t esz = ray_sym_elem_size(c->type, c->attrs);
    if (c->type == RAY_SYM)
        return ray_read_sym(ray_data(c), i, c->type, c->attrs);
    int64_t v = 0;
    memcpy(&v, (char*)ray_data(c) + i * esz, esz);
    if      (esz == 4) v = (int32_t)v;
    else if (esz == 2) v = (int16_t)v;
    else if (esz == 1) v = (int8_t)v;
    return v;
}

/* Composite less-than over key columns 0..n_keys-1 of `tbl`: compare column 0,
 * on tie column 1, etc.; final tie broken by original index for stability.
 * Nulls (sentinel-coded) sort by their int-coded value, which is deterministic
 * and identical in both engines, so they canonicalize consistently. */
static bool row_lt_composite(ray_t* tbl, int n_keys, int64_t pa, int64_t pb) {
    for (int c = 0; c < n_keys; c++) {
        ray_t* key = ray_table_get_col_idx(tbl, c);
        int64_t ka = col_read_i64(key, pa);
        int64_t kb = col_read_i64(key, pb);
        if (ka < kb) return true;
        if (ka > kb) return false;
    }
    return pa < pb;  /* stable tie-break */
}

/* Build a row permutation that sorts `tbl` ascending by the COMPOSITE of key
 * columns 0..n_keys-1 (int-coded), stable by original index. Caller frees. */
static int64_t* sort_perm_by_keys(ray_t* tbl, int n_keys) {
    int64_t n = ray_table_nrows(tbl);
    int64_t* perm = malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) perm[i] = i;
    /* insertion sort — n = ngroups is small for our shapes; stable */
    for (int64_t i = 1; i < n; i++) {
        int64_t p = perm[i];
        int64_t j = i - 1;
        while (j >= 0 && row_lt_composite(tbl, n_keys, p, perm[j])) {
            perm[j + 1] = perm[j];
            j--;
        }
        perm[j + 1] = p;
    }
    return perm;
}

/* Compare one column cell ra[ia] vs rb[ib] (same type asserted by caller). */
static bool cell_equal(ray_t* a, int64_t ia, ray_t* b, int64_t ib) {
    bool na = ray_vec_is_null(a, ia), nb = ray_vec_is_null(b, ib);
    if (na != nb) return false;
    if (na) return true;
    if (a->type == RAY_F64) {
        double xa = ((double*)ray_data(a))[ia];
        double xb = ((double*)ray_data(b))[ib];
        return fabs(xa - xb) < 1e-12 || (xa != xa && xb != xb);
    }
    return col_read_i64(a, ia) == col_read_i64(b, ib);
}

/* Compare two grouped result tables as multisets keyed on the COMPOSITE of
 * key columns 0..n_keys-1.  The result layout is {key cols [0..n_keys-1],
 * agg cols [n_keys..]}; the composite row-permutation preserves the
 * key<->aggregate association by applying the SAME permutation to all columns. */
static test_result_t table_expect_equal(ray_t* a, ray_t* b, int n_keys) {
    TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "valid tables");
    int64_t nca = ray_table_ncols(a), ncb = ray_table_ncols(b);
    TEST_ASSERT_FMT(nca == ncb, "ncols %lld != %lld",
                    (long long)nca, (long long)ncb);
    int64_t nra = ray_table_nrows(a), nrb = ray_table_nrows(b);
    TEST_ASSERT_FMT(nra == nrb, "nrows %lld != %lld",
                    (long long)nra, (long long)nrb);
    for (int64_t c = 0; c < nca; c++) {
        TEST_ASSERT_FMT(ray_table_col_name(a, c) == ray_table_col_name(b, c),
                        "col %lld name sym mismatch", (long long)c);
        ray_t* ca = ray_table_get_col_idx(a, c);
        ray_t* cb = ray_table_get_col_idx(b, c);
        TEST_ASSERT_FMT(ca->type == cb->type, "col %lld type %d != %d",
                        (long long)c, ca->type, cb->type);
    }
    int64_t* pa = sort_perm_by_keys(a, n_keys);
    int64_t* pb = sort_perm_by_keys(b, n_keys);
    test_result_t res = (test_result_t){ TEST_PASS, NULL };
    for (int64_t r = 0; r < nra && res.status == TEST_PASS; r++) {
        for (int64_t c = 0; c < nca; c++) {
            ray_t* ca = ray_table_get_col_idx(a, c);
            ray_t* cb = ray_table_get_col_idx(b, c);
            if (!cell_equal(ca, pa[r], cb, pb[r])) {
                res = (test_result_t){ TEST_FAIL,
                    "cell mismatch (old vs v2) at sorted row/col" };
                break;
            }
        }
    }
    free(pa); free(pb);
    return res;
}

/* STRICT row-order comparison: tables must match in EMIT order (no sort).
 * Used by the determinism gate — v2's first-occurrence ordering (sort groups
 * by min row index) is claimed worker-count-independent, so two runs that
 * differ only in worker count must be byte-identical row-for-row. */
static test_result_t table_expect_identical(ray_t* a, ray_t* b) {
    TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "valid tables");
    int64_t nca = ray_table_ncols(a), ncb = ray_table_ncols(b);
    TEST_ASSERT_FMT(nca == ncb, "ncols %lld != %lld",
                    (long long)nca, (long long)ncb);
    int64_t nra = ray_table_nrows(a), nrb = ray_table_nrows(b);
    TEST_ASSERT_FMT(nra == nrb, "nrows %lld != %lld",
                    (long long)nra, (long long)nrb);
    for (int64_t c = 0; c < nca; c++) {
        TEST_ASSERT_FMT(ray_table_col_name(a, c) == ray_table_col_name(b, c),
                        "col %lld name sym mismatch", (long long)c);
        ray_t* ca = ray_table_get_col_idx(a, c);
        ray_t* cb = ray_table_get_col_idx(b, c);
        TEST_ASSERT_FMT(ca->type == cb->type, "col %lld type %d != %d",
                        (long long)c, ca->type, cb->type);
        for (int64_t r = 0; r < nra; r++)
            TEST_ASSERT_FMT(cell_equal(ca, r, cb, r),
                            "row %lld col %lld differs (not row-order identical)",
                            (long long)r, (long long)c);
    }
    PASS();
}

typedef ray_op_t* (*group_builder_t)(ray_graph_t* g);

/* Execute build() with the v2 flag OFF then ON; compare results as multisets
 * over the first `n_keys` key columns. */
static test_result_t diff_group(ray_t* tbl, group_builder_t build, int n_keys) {
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* old_r = ray_execute(g1, build(g1));
    if (ray_is_lazy(old_r)) old_r = ray_lazy_materialize(old_r);

    ray_agg_engine_v2 = true;
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* new_r = ray_execute(g2, build(g2));
    if (ray_is_lazy(new_r)) new_r = ray_lazy_materialize(new_r);
    ray_agg_engine_v2 = false;

    test_result_t res = table_expect_equal(old_r, new_r, n_keys);
    ray_release(old_r); ray_release(new_r);
    ray_graph_free(g1); ray_graph_free(g2);
    return res;
}

/* ── table builders ─────────────────────────────────────────────────── */

/* I64 key column "k" with n rows: keys cycle through `nk` distinct values
 * in a non-sorted, non-trivial pattern. Value column "v" (I64) = i*7 % 101. */
static ray_t* diff_make_i64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;     /* distinct keys 0..nk-1, scrambled */
        vd[i] = (i * 7) % 101 - 50;   /* spans negatives */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* I64 key "k" + F64 value "v". */
static ray_t* diff_make_i64_f64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_F64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  vd = (double*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = (double)((i * 7) % 101) * 0.5 - 12.25;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* SYM key "k" + I64 value "v". Keys are interned "g<j>" for j in 0..nk-1. */
static ray_t* diff_make_sym_i64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_sym_vec_new(RAY_SYM_W64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* vd = (int64_t*)ray_data(vvec);
    /* pre-intern the nk group symbols */
    int64_t gid[64];
    for (int64_t j = 0; j < nk; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "g%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        int64_t j = (i * 3 + 1) % nk;
        ray_write_sym(ray_data(kvec), i, (uint64_t)gid[j], RAY_SYM, kvec->attrs);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* I64 key "k" + I64 value "v" with HAS_NULLS: every 5th value is null. */
static ray_t* diff_make_i64_nulls(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = (i * 7) % 101 - 50;
    }
    for (int64_t i = 0; i < n; i += 5) ray_vec_set_null(vvec, i, true);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* ── HIGH-CARDINALITY builders (stress the per-worker→global merge) ──────
 * These deliberately produce MANY groups so each per-worker local hash table
 * holds many groups and Phase B does real merging work. */

/* Single I64 key "k" = i % nk (≈nk groups) + I64 value "v" = i. SUM/COUNT. */
static ray_t* diff_make_i64_hc(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = i % nk;               /* ≈nk distinct groups */
        vd[i] = i - n / 2;            /* deterministic, spans negatives */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Single I64 key "k" = i (ALL DISTINCT → n groups, extreme cardinality)
 * + I64 value "v" = i. COUNT. */
static ray_t* diff_make_i64_distinct(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) { kd[i] = i; vd[i] = i; }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Two I64 keys "k1"=i%200, "k2"=(i/200)%150 + I64 value "v" = i. SUM.
 * The two keys vary at independent rates so the pair space is fully covered:
 * ≈30000 composite groups at N=70000 (NOT i%200/i%150, which collapse to 600
 * because lcm(200,150)=600 aligns the two residues). */
static ray_t* diff_make_2i64_hc(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = i % 200;
        d2[i] = (i / 200) % 150;
        vd[i] = i - n / 2;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── multi-key table builders ───────────────────────────────────────── */

/* Two I64 keys "k1","k2" + I64 value "v".  Keys vary at different rates so
 * many (k1,k2) tuples exist and some tuples share k1 (composite-sort coverage). */
static ray_t* diff_make_2i64(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Two I64 keys + F64 value "v" (for AVG/SUM parity). */
static ray_t* diff_make_2i64_f64(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_F64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    double*  vd = (double*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (double)((i * 7) % 101) * 0.5 - 12.25;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Three keys: I64 "k1", I32 "k2", SYM "k3" + I64 value "v". */
static ray_t* diff_make_3keys(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I32, n); k2->len = n;
    ray_t* k3 = ray_sym_vec_new(RAY_SYM_W64, n); k3->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int32_t* d2 = (int32_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    int64_t gid[64];
    for (int64_t j = 0; j < nk; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "g%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (int32_t)((i * 5 + 2) % (nk + 1));
        int64_t j = (i * 2 + 1) % nk;
        ray_write_sym(ray_data(k3), i, (uint64_t)gid[j], RAY_SYM, k3->attrs);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k3", 2), k3); ray_release(k3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* SYM key "k1" + I64 key "k2" + I64 value "v". */
static ray_t* diff_make_sym_i64_keys(int64_t n, int64_t nk) {
    ray_t* k1 = ray_sym_vec_new(RAY_SYM_W64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    int64_t gid[64];
    for (int64_t j = 0; j < nk; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "g%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        int64_t j = (i * 3 + 1) % nk;
        ray_write_sym(ray_data(k1), i, (uint64_t)gid[j], RAY_SYM, k1->attrs);
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Two I64 keys where key column "k2" has HAS_NULLS (sentinel + attr set):
 * every 3rd row's k2 is null → null-key grouping parity. */
static ray_t* diff_make_2i64_keynull(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 7) % 101 - 50;
    }
    for (int64_t i = 0; i < n; i += 3) ray_vec_set_null(k2, i, true);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── var/stddev builders: SMALL-range deterministic values ──────────────
 * v = i % 97 keeps per-group variances small so v2's running {sum,sumsq,cnt}
 * formula and the old engine agree to within the comparator's 1e-12 ABSOLUTE
 * F64 epsilon (cell_equal). nk small ⇒ cnt≥2 per group ⇒ sample var/stddev
 * are defined (no singleton-group nulls). */

/* Single I64 key "k" + I64 value "v" = i % 97. */
static ray_t* diff_make_i64_small(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = i % 97;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Single I64 key "k" + F64 value "v" = (i % 97) * 0.25. */
static ray_t* diff_make_i64_f64_small(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_F64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  vd = (double*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = (double)(i % 97) * 0.25;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Two I64 keys "k1","k2" + I64 value "v" = i % 97 (small range). */
static ray_t* diff_make_2i64_small(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = i % 97;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── per-shape group builders (graph-local) ─────────────────────────── */
static ray_op_t* gb_sum(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k");
    uint16_t ops[] = { OP_COUNT }; ray_op_t* ins[] = { k }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_minmax(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 2);
}
static ray_op_t* gb_four(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_COUNT, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 4);
}
static ray_op_t* gb_f64_four(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_MIN, OP_MAX, OP_AVG };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 4);
}
static ray_op_t* gb_sum_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_COUNT };
    ray_op_t* ins[] = { v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 2);
}
/* ── var/stddev group builders ──────────────────────────────────────── */
static ray_op_t* gb_stddev(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_STDDEV }; ray_op_t* ins[] = { v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_var_all(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_VAR, OP_VAR_POP, OP_STDDEV, OP_STDDEV_POP };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 4);
}
static ray_op_t* gb_2k_stddev_pop(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_STDDEV_POP }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}
static ray_op_t* gb_sum_stddev_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_STDDEV, OP_COUNT };
    ray_op_t* ins[] = { v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 3);
}
static ray_op_t* gb_nulls(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 3);
}

/* ── multi-key group builders ───────────────────────────────────────── */
static ray_op_t* gb_2k_sum(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}
static ray_op_t* gb_2k_four(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_COUNT, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 4);
}
static ray_op_t* gb_3k_count(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* k3 = ray_scan(g, "k3");
    uint16_t ops[] = { OP_COUNT }; ray_op_t* ins[] = { k1 };
    ray_op_t* keys[] = { k1, k2, k3 };
    return ray_group(g, keys, 3, ops, ins, 1);
}
static ray_op_t* gb_2k_avg(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_AVG }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}

/* ── runner: each shape over a small AND a large (N=70000) table ────── */
#define DIFF_SHAPE(name, maker, builder, nkeys)                           \
    static test_result_t name(void) {                                     \
        ray_heap_init(); (void)ray_sym_init();                            \
        test_result_t r;                                                  \
        ray_t* small = maker(13, 4);                                      \
        r = diff_group(small, builder, (nkeys)); ray_release(small);      \
        if (r.status != TEST_PASS) {                                      \
            ray_sym_destroy(); ray_heap_destroy(); return r; }            \
        ray_t* big = maker(70000, 37);                                    \
        r = diff_group(big, builder, (nkeys)); ray_release(big);          \
        ray_sym_destroy(); ray_heap_destroy();                            \
        return r;                                                         \
    }

DIFF_SHAPE(test_diff_group_i64_sum,      diff_make_i64,      gb_sum,   1)
DIFF_SHAPE(test_diff_group_i64_count,    diff_make_i64,      gb_count, 1)
DIFF_SHAPE(test_diff_group_i64_minmax,   diff_make_i64,      gb_minmax, 1)
DIFF_SHAPE(test_diff_group_i64_four,     diff_make_i64,      gb_four,  1)
DIFF_SHAPE(test_diff_group_sym_sum,      diff_make_sym_i64,  gb_sum,   1)
DIFF_SHAPE(test_diff_group_f64_four,     diff_make_i64_f64,  gb_f64_four, 1)
DIFF_SHAPE(test_diff_group_nulls_minmax, diff_make_i64_nulls, gb_nulls, 1)

/* var/stddev differential shapes (serial small + parallel N=70000) */
DIFF_SHAPE(test_diff_group_i64_stddev,    diff_make_i64_small,     gb_stddev,           1)
DIFF_SHAPE(test_diff_group_i64_var_all,   diff_make_i64_small,     gb_var_all,          1)
DIFF_SHAPE(test_diff_group_f64_stddev,    diff_make_i64_f64_small, gb_stddev,           1)
DIFF_SHAPE(test_diff_group_2k_stddev_pop, diff_make_2i64_small,    gb_2k_stddev_pop,    2)
DIFF_SHAPE(test_diff_group_mixed_stddev,  diff_make_i64_small,     gb_sum_stddev_count, 1)

/* multi-key shapes (Phase 1b) */
DIFF_SHAPE(test_diff_group_2k_sum,    diff_make_2i64,        gb_2k_sum,   2)
DIFF_SHAPE(test_diff_group_2k_four,   diff_make_2i64,        gb_2k_four,  2)
DIFF_SHAPE(test_diff_group_3k_count,  diff_make_3keys,       gb_3k_count, 3)
DIFF_SHAPE(test_diff_group_symk_sum,  diff_make_sym_i64_keys, gb_2k_sum,  2)
DIFF_SHAPE(test_diff_group_2k_avg,    diff_make_2i64_f64,    gb_2k_avg,   2)
DIFF_SHAPE(test_diff_group_2k_keynull, diff_make_2i64_keynull, gb_2k_sum, 2)

/* ══════════════════════════════════════════════════════════════════════
 * HIGH-CARDINALITY differentials (Phase 1c). N=70000 (≥ RAY_PARALLEL_THRESHOLD
 * 65536) with the v2 flag ON forces the parallel two-phase path; MANY groups
 * mean each per-worker local table holds many groups and Phase B merges real
 * work. Compared as MULTISET (key-sorted) vs the OLD engine.
 * ══════════════════════════════════════════════════════════════════════ */

#define HC_N 70000

/* Execute build() on tbl with the v2 engine and return the group count
 * (result nrows).  Confirms a shape truly is high-cardinality before we lean
 * on it to stress the parallel merge. */
static int64_t v2_group_count(ray_t* tbl, group_builder_t build) {
    ray_agg_engine_v2 = true;
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* out = ray_execute(g, build(g));
    if (ray_is_lazy(out)) out = ray_lazy_materialize(out);
    ray_agg_engine_v2 = false;
    int64_t ng = (out && !RAY_IS_ERR(out)) ? ray_table_nrows(out) : -1;
    if (out) ray_release(out);
    ray_graph_free(g);
    return ng;
}

/* ≈20000 groups: k = i % 20000, SUM + COUNT. */
static test_result_t test_diff_group_hc_i64_sumcount(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_hc(HC_N, 20000);
    int64_t ng = v2_group_count(big, gb_sum_count);
    TEST_ASSERT_FMT(ng == 20000, "expected 20000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_sum_count, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ≈30000 composite groups: (i%200, (i/200)%150), SUM. */
static test_result_t test_diff_group_hc_2k_sum(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_2i64_hc(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_2k_sum);
    TEST_ASSERT_FMT(ng == 30000, "expected 30000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_2k_sum, 2);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Extreme: k = i (all 70000 distinct), COUNT. */
static test_result_t test_diff_group_hc_distinct_count(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_distinct(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_count);
    TEST_ASSERT_FMT(ng == HC_N, "expected %d groups, got %lld", HC_N, (long long)ng);
    test_result_t r = diff_group(big, gb_count, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * DETERMINISM: v2's first-occurrence order is "sort groups by min(row index)",
 * claimed worker-count-independent by construction (agg_engine.c Phase A tracks
 * MIN first_row per local group, Phase B keeps the global MIN, Phase C sorts by
 * it).  exec_group_v2_parallel allocates exactly ray_pool_total_workers()
 * per-worker locals and agg_phaseA_fn indexes locals[wid] — so worker count
 * DIRECTLY changes how rows partition into local tables and how the merge runs.
 *
 * The pool worker count IS controllable from a test: ray_pool_destroy() +
 * ray_pool_init(n) reconfigures the singleton (precedent: test_pool.c).
 * ray_pool_total_workers(p) = n_workers + 1 (the calling thread counts), so to
 * get total worker counts {1,2,8} we init with n_workers {0,1,7}.
 *
 * We run the SAME high-card query at N=70000 across those worker counts and
 * assert ALL results are byte-identical INCLUDING EMIT ROW ORDER
 * (table_expect_identical) — proving first-occurrence order is worker-count-
 * independent.  A full {1,2,8}-worker CI matrix (design §9 / GAPS) is the
 * proper gate; this single-process version reaches it via pool reinit.
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_diff_group_determinism_workers(void) {
    ray_heap_init(); (void)ray_sym_init();

    const uint32_t worker_cfgs[] = { 0, 1, 7 };  /* total workers 1, 2, 8 */
    const int      ncfg = (int)(sizeof(worker_cfgs) / sizeof(worker_cfgs[0]));
    ray_t* results[3] = { NULL, NULL, NULL };
    test_result_t res = (test_result_t){ TEST_PASS, NULL };

    ray_agg_engine_v2 = true;
    for (int c = 0; c < ncfg; c++) {
        ray_pool_destroy();
        ray_pool_init(worker_cfgs[c]);

        ray_t* tbl = diff_make_i64_hc(HC_N, 20000);
        ray_graph_t* g = ray_graph_new(tbl);
        ray_t* out = ray_execute(g, gb_sum_count(g));
        if (ray_is_lazy(out)) out = ray_lazy_materialize(out);
        results[c] = out;
        ray_graph_free(g);
        ray_release(tbl);
    }
    ray_agg_engine_v2 = false;

    /* Restore default pool for subsequent tests. */
    ray_pool_destroy();
    ray_pool_init(0);

    /* All three must be row-order identical to the first. */
    for (int c = 1; c < ncfg && res.status == TEST_PASS; c++)
        res = table_expect_identical(results[0], results[c]);

    for (int c = 0; c < ncfg; c++)
        if (results[c]) ray_release(results[c]);

    ray_sym_destroy(); ray_heap_destroy();
    return res;
}

const test_entry_t agg_engine_entries[] = {
    { "diff_group_i64_sum",          test_diff_group_i64_sum,      NULL, NULL },
    { "diff_group_i64_count",        test_diff_group_i64_count,    NULL, NULL },
    { "diff_group_i64_minmax",       test_diff_group_i64_minmax,   NULL, NULL },
    { "diff_group_i64_four",         test_diff_group_i64_four,     NULL, NULL },
    { "diff_group_sym_sum",          test_diff_group_sym_sum,      NULL, NULL },
    { "diff_group_f64_four",         test_diff_group_f64_four,     NULL, NULL },
    { "diff_group_nulls_minmax",     test_diff_group_nulls_minmax, NULL, NULL },
    { "diff_group_i64_stddev",       test_diff_group_i64_stddev,    NULL, NULL },
    { "diff_group_i64_var_all",      test_diff_group_i64_var_all,   NULL, NULL },
    { "diff_group_f64_stddev",       test_diff_group_f64_stddev,    NULL, NULL },
    { "diff_group_2k_stddev_pop",    test_diff_group_2k_stddev_pop, NULL, NULL },
    { "diff_group_mixed_stddev",     test_diff_group_mixed_stddev,  NULL, NULL },
    { "diff_group_2k_sum",           test_diff_group_2k_sum,       NULL, NULL },
    { "diff_group_2k_four",          test_diff_group_2k_four,      NULL, NULL },
    { "diff_group_3k_count",         test_diff_group_3k_count,     NULL, NULL },
    { "diff_group_symk_sum",         test_diff_group_symk_sum,     NULL, NULL },
    { "diff_group_2k_avg",           test_diff_group_2k_avg,       NULL, NULL },
    { "diff_group_2k_keynull",       test_diff_group_2k_keynull,   NULL, NULL },
    { "diff_group_hc_i64_sumcount",  test_diff_group_hc_i64_sumcount,  NULL, NULL },
    { "diff_group_hc_2k_sum",        test_diff_group_hc_2k_sum,        NULL, NULL },
    { "diff_group_hc_distinct_count", test_diff_group_hc_distinct_count, NULL, NULL },
    { "diff_group_determinism_workers", test_diff_group_determinism_workers, NULL, NULL },
    { "gate_admits_i64_key_sum_i64", test_gate_admits_i64_key_sum_i64, NULL, NULL },
    { "gate_admits_two_keys",        test_gate_admits_two_keys,        NULL, NULL },
    { "gate_defers_sum_i32",         test_gate_defers_sum_i32,         NULL, NULL },
    { "gate_admits_count",           test_gate_admits_count,           NULL, NULL },
    { "group_keys_i_first_occurrence", test_group_keys_i_first_occurrence, NULL, NULL },
    { "group_keys_i_i32",            test_group_keys_i_i32,            NULL, NULL },
    { "group_keys_multi",            test_group_keys_multi,            NULL, NULL },
    { "agg_run_one_i64",             test_agg_run_one_i64,             NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

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

/* Build a row permutation that sorts `tbl` ascending by key column 0
 * (int-coded), stable by original index. Caller frees. */
static int64_t* sort_perm_by_key0(ray_t* tbl) {
    int64_t n = ray_table_nrows(tbl);
    ray_t* key = ray_table_get_col_idx(tbl, 0);
    int64_t* perm = malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) perm[i] = i;
    /* insertion sort — n = ngroups is small for our shapes; stable */
    for (int64_t i = 1; i < n; i++) {
        int64_t p = perm[i];
        int64_t kp = col_read_i64(key, p);
        int64_t j = i - 1;
        while (j >= 0) {
            int64_t kj = col_read_i64(key, perm[j]);
            if (kj < kp || (kj == kp && perm[j] < p)) break;
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

/* Compare two grouped result tables as multisets keyed on column 0. */
static test_result_t table_expect_equal(ray_t* a, ray_t* b) {
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
    int64_t* pa = sort_perm_by_key0(a);
    int64_t* pb = sort_perm_by_key0(b);
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

typedef ray_op_t* (*group_builder_t)(ray_graph_t* g);

/* Execute build() with the v2 flag OFF then ON; compare results. */
static test_result_t diff_group(ray_t* tbl, group_builder_t build) {
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* old_r = ray_execute(g1, build(g1));
    if (ray_is_lazy(old_r)) old_r = ray_lazy_materialize(old_r);

    ray_agg_engine_v2 = true;
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* new_r = ray_execute(g2, build(g2));
    if (ray_is_lazy(new_r)) new_r = ray_lazy_materialize(new_r);
    ray_agg_engine_v2 = false;

    test_result_t res = table_expect_equal(old_r, new_r);
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
static ray_op_t* gb_nulls(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 3);
}

/* ── runner: each shape over a small AND a large (N=70000) table ────── */
#define DIFF_SHAPE(name, maker, builder)                                  \
    static test_result_t name(void) {                                     \
        ray_heap_init(); (void)ray_sym_init();                            \
        test_result_t r;                                                  \
        ray_t* small = maker(13, 4);                                      \
        r = diff_group(small, builder); ray_release(small);               \
        if (r.status != TEST_PASS) {                                      \
            ray_sym_destroy(); ray_heap_destroy(); return r; }            \
        ray_t* big = maker(70000, 37);                                    \
        r = diff_group(big, builder); ray_release(big);                   \
        ray_sym_destroy(); ray_heap_destroy();                            \
        return r;                                                         \
    }

DIFF_SHAPE(test_diff_group_i64_sum,      diff_make_i64,      gb_sum)
DIFF_SHAPE(test_diff_group_i64_count,    diff_make_i64,      gb_count)
DIFF_SHAPE(test_diff_group_i64_minmax,   diff_make_i64,      gb_minmax)
DIFF_SHAPE(test_diff_group_i64_four,     diff_make_i64,      gb_four)
DIFF_SHAPE(test_diff_group_sym_sum,      diff_make_sym_i64,  gb_sum)
DIFF_SHAPE(test_diff_group_f64_four,     diff_make_i64_f64,  gb_f64_four)
DIFF_SHAPE(test_diff_group_nulls_minmax, diff_make_i64_nulls, gb_nulls)

const test_entry_t agg_engine_entries[] = {
    { "diff_group_i64_sum",          test_diff_group_i64_sum,      NULL, NULL },
    { "diff_group_i64_count",        test_diff_group_i64_count,    NULL, NULL },
    { "diff_group_i64_minmax",       test_diff_group_i64_minmax,   NULL, NULL },
    { "diff_group_i64_four",         test_diff_group_i64_four,     NULL, NULL },
    { "diff_group_sym_sum",          test_diff_group_sym_sum,      NULL, NULL },
    { "diff_group_f64_four",         test_diff_group_f64_four,     NULL, NULL },
    { "diff_group_nulls_minmax",     test_diff_group_nulls_minmax, NULL, NULL },
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

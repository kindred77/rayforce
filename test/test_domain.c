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

/*
 * test_domain.c -- SYM domain objects (src/table/domain.c), Phase 1.
 *
 * Covers: runtime-singleton identity + delegation to ray_sym_*; the
 * non-NULL domain invariant across every SYM vec constructor path
 * (vec_new, append growth, COW, slice, concat, serde round-trip,
 * col_save/col_load, col_mmap); FILE domain open/find/str/count/release
 * semantics over a real symfile; and that the domain pointer never
 * reaches disk.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "store/col.h"
#include "store/serde.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define TMP_DOM_SYM_PATH "/tmp/rayforce_test_domain_sym"
#define TMP_DOM_COL_PATH "/tmp/rayforce_test_domain_col"

/* ---- Setup / Teardown -------------------------------------------------- */

static void domain_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void domain_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- runtime singleton -------------------------------------------------- */

static test_result_t test_domain_runtime_identity(void) {
    ray_sym_domain_t* d1 = ray_sym_runtime_domain();
    ray_sym_domain_t* d2 = ray_sym_runtime_domain();
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_EQ_PTR(d1, d2);

    /* retain/release are no-ops on the singleton — must not crash and
     * must not change identity. */
    ray_sym_domain_retain(d1);
    ray_sym_domain_release(d1);
    ray_sym_domain_release(d1);
    TEST_ASSERT_EQ_PTR(ray_sym_runtime_domain(), d1);

    /* No backing file. */
    TEST_ASSERT_NULL(ray_sym_domain_path(d1));
    PASS();
}

static test_result_t test_domain_runtime_delegation(void) {
    ray_sym_domain_t* d = ray_sym_runtime_domain();

    int64_t id_a = ray_sym_intern("dom_alpha", 9);
    int64_t id_b = ray_sym_intern("dom_beta", 8);
    TEST_ASSERT(id_a >= 1 && id_b >= 1, "intern ids");

    /* count agrees */
    TEST_ASSERT_EQ_I(ray_sym_domain_count(d), (int64_t)ray_sym_count());

    /* find agrees: hit + miss */
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_alpha", 9), id_a);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_beta", 8), id_b);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_missing", 11), -1);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_missing", 11), ray_sym_find("dom_missing", 11));

    /* str agrees (borrowed atoms from the same table) */
    ray_t* s_dom = ray_sym_domain_str(d, id_a);
    ray_t* s_sym = ray_sym_str(id_a);
    TEST_ASSERT_EQ_PTR(s_dom, s_sym);

    /* intern delegates on the runtime domain */
    int64_t id_c = ray_sym_domain_intern(d, "dom_gamma", 9);
    TEST_ASSERT_EQ_I(id_c, ray_sym_find("dom_gamma", 9));

    /* flush is Phase 2 */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d, false), RAY_ERR_NYI);
    PASS();
}

/* ---- constructor paths all yield runtime-domain vecs -------------------- */

static test_result_t test_domain_vec_new_attach(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    ray_t* v8  = ray_sym_vec_new(RAY_SYM_W8, 4);
    ray_t* v64 = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v8));
    TEST_ASSERT_FALSE(RAY_IS_ERR(v64));
    TEST_ASSERT_EQ_PTR(v8->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(v64->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(v8), rt);

    /* ray_vec_new(RAY_SYM, …) delegates through the same chokepoint */
    ray_t* vg = ray_vec_new(RAY_SYM, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vg));
    TEST_ASSERT_EQ_PTR(vg->sym_domain, rt);

    ray_release(v8);
    ray_release(v64);
    ray_release(vg);
    PASS();
}

static test_result_t test_domain_append_growth_and_cow(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    /* Growth: start at capacity 0, append enough to force reallocs. */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    for (int64_t i = 0; i < 100; i++) {
        int64_t id = ray_sym_intern("grow_x", 6);
        v = ray_vec_append(v, &id);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 100);
    TEST_ASSERT_EQ_PTR(v->sym_domain, rt);

    /* COW: share the vec, then append — the appended copy is a fresh
     * header copy via ray_alloc_copy and must carry the domain. */
    ray_retain(v);
    int64_t id = ray_sym_intern("cow_y", 5);
    ray_t* w = ray_vec_append(v, &id);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    TEST_ASSERT(w != v, "COW must copy a shared vec");
    TEST_ASSERT_EQ_PTR(w->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(v->sym_domain, rt);

    ray_release(w);
    ray_release(v);  /* drops the extra retain */
    ray_release(v);
    PASS();
}

static test_result_t test_domain_slice_accessor(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 8;
    int64_t* d = (int64_t*)ray_data(v);
    for (int i = 0; i < 8; i++) d[i] = 0;

    ray_t* s = ray_vec_slice(v, 2, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_SLICE);
    /* Slice header carries slice_parent/slice_offset in aux — the
     * accessor must resolve the domain through the parent. */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(s), rt);

    /* Slice-of-slice resolves to the ultimate parent. */
    ray_t* s2 = ray_vec_slice(s, 1, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s2));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(s2), rt);

    ray_release(s2);
    ray_release(s);
    ray_release(v);
    PASS();
}

static test_result_t test_domain_concat_attach(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    int64_t ida = ray_sym_intern("cc_a", 4);
    int64_t idb = ray_sym_intern("cc_b", 4);

    ray_t* a = ray_sym_vec_new(RAY_SYM_W64, 2);
    ray_t* b = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    a->len = 2; b->len = 2;
    ((int64_t*)ray_data(a))[0] = ida; ((int64_t*)ray_data(a))[1] = idb;
    ((int64_t*)ray_data(b))[0] = idb; ((int64_t*)ray_data(b))[1] = ida;

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 4);
    TEST_ASSERT_EQ_PTR(c->sym_domain, rt);

    /* Mixed-width concat takes the widen loop — same contract. */
    ray_t* n8 = ray_sym_vec_new(RAY_SYM_W8, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(n8));
    n8->len = 2;
    ((uint8_t*)ray_data(n8))[0] = (uint8_t)ida;
    ((uint8_t*)ray_data(n8))[1] = (uint8_t)idb;
    ray_t* m = ray_vec_concat(n8, a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(m));
    TEST_ASSERT_EQ_PTR(m->sym_domain, rt);

    /* Concat with a slice input resolves through the parent. */
    ray_t* sl = ray_vec_slice(a, 0, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sl));
    ray_t* cs = ray_vec_concat(sl, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(cs));
    TEST_ASSERT_EQ_PTR(cs->sym_domain, rt);

    ray_release(cs);
    ray_release(sl);
    ray_release(m);
    ray_release(n8);
    ray_release(c);
    ray_release(b);
    ray_release(a);
    PASS();
}

static test_result_t test_domain_serde_roundtrip(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    int64_t ida = ray_sym_intern("ser_a", 5);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 3;
    int64_t* d = (int64_t*)ray_data(v);
    d[0] = ida; d[1] = ida; d[2] = 0;

    ray_t* bytes = ray_ser(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(bytes));

    /* The domain pointer must never appear in the wire bytes. */
    {
        ray_sym_domain_t* dom = v->sym_domain;
        const uint8_t* hay = (const uint8_t*)ray_data(bytes);
        int64_t n = bytes->len;
        int found = 0;
        for (int64_t i = 0; i + (int64_t)sizeof(dom) <= n; i++) {
            if (memcmp(hay + i, &dom, sizeof(dom)) == 0) { found = 1; break; }
        }
        TEST_ASSERT_FALSE(found);
    }

    ray_t* back = ray_de(bytes);
    TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, RAY_SYM);
    TEST_ASSERT_EQ_I(back->len, 3);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(back), rt);
    TEST_ASSERT_EQ_PTR(back->sym_domain, rt);

    ray_release(back);
    ray_release(bytes);
    ray_release(v);
    PASS();
}

static test_result_t test_domain_col_save_load_mmap(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();
    unlink(TMP_DOM_COL_PATH);

    int64_t ida = ray_sym_intern("col_a", 5);
    int64_t idb = ray_sym_intern("col_b", 5);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W16, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 4;
    uint16_t* d = (uint16_t*)ray_data(v);
    d[0] = (uint16_t)ida; d[1] = (uint16_t)idb;
    d[2] = (uint16_t)ida; d[3] = (uint16_t)idb;
    /* HAS_NULLS exercises the save branch that previously preserved aux
     * verbatim — the on-disk header must still be pointer-free. */
    v->attrs |= RAY_ATTR_HAS_NULLS;

    TEST_ASSERT_EQ_I(ray_col_save(v, TMP_DOM_COL_PATH), RAY_OK);

    /* Domain pointer never serialized: scan the file bytes. */
    {
        ray_sym_domain_t* dom = v->sym_domain;
        FILE* f = fopen(TMP_DOM_COL_PATH, "rb");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        TEST_ASSERT(n >= 32, "column file too small");
        int found = 0;
        for (size_t i = 0; i + sizeof(dom) <= n; i++) {
            if (memcmp(buf + i, &dom, sizeof(dom)) == 0) { found = 1; break; }
        }
        TEST_ASSERT_FALSE(found);
    }

    ray_t* loaded = ray_col_load(TMP_DOM_COL_PATH);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(loaded->sym_domain, rt);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(TMP_DOM_COL_PATH);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(mapped->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(mapped), rt);
    ray_release(mapped);

    ray_release(v);
    unlink(TMP_DOM_COL_PATH);
    PASS();
}

/* ---- FILE domains over a real symfile ----------------------------------- */

static test_result_t test_domain_open_basic(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    int64_t id_h = ray_sym_intern("file_hello", 10);
    int64_t id_w = ray_sym_intern("file_world", 10);
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT(dom != ray_sym_runtime_domain(), "FILE domain is not the singleton");

    /* count matches the persisted dictionary */
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), (int64_t)count_at_save);

    /* find: hit + miss */
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_hello", 10), id_h);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_world", 10), id_w);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_absent", 11), -1);

    /* str round-trips: position resolves to the same bytes */
    ray_t* s = ray_sym_domain_str(dom, id_h);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ_U(ray_str_len(s), 10);
    TEST_ASSERT_MEM_EQ(10, ray_str_ptr(s), "file_hello");
    /* borrowed: a second call returns the same atom */
    TEST_ASSERT_EQ_PTR(ray_sym_domain_str(dom, id_h), s);
    /* position 0 is the reserved empty string */
    ray_t* s0 = ray_sym_domain_str(dom, 0);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);
    /* out of range -> NULL */
    TEST_ASSERT_NULL(ray_sym_domain_str(dom, (int64_t)count_at_save));
    TEST_ASSERT_NULL(ray_sym_domain_str(dom, -1));

    /* path is the resolved file path */
    TEST_ASSERT_NOT_NULL(ray_sym_domain_path(dom));
    TEST_ASSERT(strstr(ray_sym_domain_path(dom), "rayforce_test_domain_sym") != NULL,
                "path mentions the symfile");

    /* Phase-2 stubs: FILE intern / flush are NYI */
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "file_new", 8), -1);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, true), RAY_ERR_NYI);

    ray_sym_domain_release(dom);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

static test_result_t test_domain_open_identity_and_release(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    (void)ray_sym_intern("ident_x", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    /* Same resolved path => same object (pointer equality = identity). */
    ray_sym_domain_t* d1 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_EQ_PTR(d1, d2);

    /* retain/release are real on FILE domains */
    ray_sym_domain_retain(d1);
    ray_sym_domain_release(d1);

    ray_sym_domain_release(d1);
    ray_sym_domain_release(d2);

    /* Last release dropped the cache entry: a fresh open must build a
     * fresh, fully functional mapping (and not crash under ASan). */
    ray_sym_domain_t* d3 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d3);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d3, "ident_x", 7),
                     ray_sym_find("ident_x", 7));
    ray_sym_domain_release(d3);

    /* Missing file -> NULL, no crash. */
    TEST_ASSERT_NULL(ray_sym_domain_open("/tmp/rayforce_test_domain_nonexistent"));

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 1: resolution helpers + adopt-domain ------------------- */

/* ray_sym_vec_cell on a runtime-domain vec must be exactly equivalent to
 * the legacy two-step (ray_read_sym + ray_sym_str) — same borrowed atom. */
static test_result_t test_domain_vec_cell_runtime_equiv(void) {
    int64_t ida = ray_sym_intern("cell_a", 6);
    int64_t idb = ray_sym_intern("cell_b", 6);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 3;
    int64_t* d = (int64_t*)ray_data(v);
    d[0] = ida; d[1] = idb; d[2] = ida;

    for (int64_t i = 0; i < 3; i++) {
        int64_t id = ray_read_sym(ray_data(v), i, RAY_SYM, v->attrs);
        TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(v, i), ray_sym_str(id));
    }

    /* narrow width takes the same path through ray_read_sym */
    ray_t* v8 = ray_sym_vec_new(RAY_SYM_W8, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v8));
    v8->len = 2;
    ((uint8_t*)ray_data(v8))[0] = (uint8_t)ida;
    ((uint8_t*)ray_data(v8))[1] = (uint8_t)idb;
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(v8, 0), ray_sym_str(ida));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(v8, 1), ray_sym_str(idb));

    /* slices resolve data AND domain through the parent */
    ray_t* s = ray_vec_slice(v, 1, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(s, 0), ray_sym_str(idb));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(s, 1), ray_sym_str(ida));

    ray_release(s);
    ray_release(v8);
    ray_release(v);
    PASS();
}

/* ray_sym_vec_lookup on a runtime vec delegates to the global find:
 * hit returns the interned id, miss returns -1 (matches nothing). */
static test_result_t test_domain_vec_lookup_hit_miss(void) {
    int64_t ida = ray_sym_intern("lkp_hit", 7);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 1;
    ((int64_t*)ray_data(v))[0] = ida;

    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(v, "lkp_hit", 7), ida);
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(v, "lkp_hit", 7),
                     ray_sym_find("lkp_hit", 7));
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(v, "lkp_missing", 11), -1);

    /* slice: domain through the parent */
    ray_t* s = ray_vec_slice(v, 0, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(s, "lkp_hit", 7), ida);
    ray_release(s);
    ray_release(v);
    PASS();
}

/* adopt_domain: ref balance with a real FILE domain (ASan is the leak
 * oracle), plus cell/lookup resolution through the adopted domain. */
static test_result_t test_domain_adopt_domain_refcount(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    (void)ray_sym_intern("adopt_x", 7);
    (void)ray_sym_intern("adopt_y", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);

    int64_t pos_x = ray_sym_domain_find(dom, "adopt_x", 7);
    int64_t pos_y = ray_sym_domain_find(dom, "adopt_y", 7);
    TEST_ASSERT(pos_x >= 0 && pos_y >= 0, "positions resolve");

    /* a: heap vec manually attached to the FILE domain (simulates the
     * Task-7 load path; the vec holds its own ref, dropped on free). */
    ray_t* a = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    a->len = 2;
    ((int64_t*)ray_data(a))[0] = pos_x;
    ((int64_t*)ray_data(a))[1] = pos_y;
    ray_sym_domain_retain(dom);
    a->sym_domain = dom;

    /* helpers resolve through the FILE domain, not the global table */
    ray_t* sx = ray_sym_vec_cell(a, 0);
    TEST_ASSERT_NOT_NULL(sx);
    TEST_ASSERT_EQ_U(ray_str_len(sx), 7);
    TEST_ASSERT_MEM_EQ(7, ray_str_ptr(sx), "adopt_x");
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(a, "adopt_y", 7), pos_y);
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(a, "adopt_absent", 12), -1);

    /* b adopts a's domain: out's runtime ref is a no-op release, the FILE
     * domain is retained. */
    ray_t* b = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    ray_sym_vec_adopt_domain(b, a);
    TEST_ASSERT_EQ_PTR(b->sym_domain, dom);
    TEST_ASSERT_EQ_PTR(a->sym_domain, dom);

    /* idempotent: adopting the same domain again must not unbalance */
    ray_sym_vec_adopt_domain(b, a);
    TEST_ASSERT_EQ_PTR(b->sym_domain, dom);

    /* adopting a runtime vec's domain over a FILE ref releases the FILE ref */
    ray_t* r = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_sym_vec_adopt_domain(b, r);
    TEST_ASSERT_EQ_PTR(b->sym_domain, ray_sym_runtime_domain());

    /* non-SYM pairs are a no-op */
    ray_t* i64v = ray_vec_new(RAY_I64, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(i64v));
    ray_sym_vec_adopt_domain(i64v, a);
    TEST_ASSERT(i64v->sym_domain != dom, "non-SYM out untouched");
    ray_sym_vec_adopt_domain(b, i64v);
    TEST_ASSERT_EQ_PTR(b->sym_domain, ray_sym_runtime_domain());
    ray_release(i64v);

    /* re-adopt the FILE domain into b so b's FREE path (heap.c owned-ref
     * release) carries one of the outstanding refs. */
    ray_sym_vec_adopt_domain(b, a);
    TEST_ASSERT_EQ_PTR(b->sym_domain, dom);

    ray_release(r);
    ray_release(b);              /* drops b's adopt ref */
    ray_release(a);              /* drops a's manual attach ref */
    ray_sym_domain_release(dom); /* drops the open ref — last one */

    /* Balance proof: the cache entry must be gone; a fresh open builds a
     * fully functional mapping (ASan flags any leak/double-free). */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d2, "adopt_x", 7), pos_x);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 2: pivot output-vec adoption --------------------------- */

/* Pivot needs the full eval runtime, not just heap + sym table. */
static ray_runtime_t* pvt_rt;
static void domain_rt_setup(void)    { pvt_rt = ray_runtime_create(0, NULL); }
static void domain_rt_teardown(void) { ray_runtime_destroy(pvt_rt); }

/* A SYM index column carrying a FILE domain must hand that domain to the
 * pivot output's index column (the output raw-copies cell ids, so it must
 * resolve over the same dictionary).  Meaningful even pre-flip: col_vec_new
 * attaches the runtime singleton, and only the adopt call at the pivot
 * scatter site transfers the FILE-domain pointer. */
static test_result_t test_domain_pivot_index_adopts(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    int64_t id_A = ray_sym_intern("pd_A", 4);
    int64_t id_B = ray_sym_intern("pd_B", 4);
    int64_t id_x = ray_sym_intern("pd_x", 4);
    int64_t id_y = ray_sym_intern("pd_y", 4);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);

    /* Index col 'a': manually attached to the FILE domain (positions
     * coincide with global ids — the symfile was saved just above). */
    ray_t* a = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    a->len = 4;
    int64_t* ad = (int64_t*)ray_data(a);
    ad[0] = id_A; ad[1] = id_A; ad[2] = id_B; ad[3] = id_B;
    ray_sym_domain_retain(dom);
    a->sym_domain = dom;

    /* Pivot col 'c' and value col 'v' stay runtime-domain. */
    ray_t* cv = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(cv));
    cv->len = 4;
    int64_t* cd = (int64_t*)ray_data(cv);
    cd[0] = id_x; cd[1] = id_y; cd[2] = id_x; cd[3] = id_y;
    ray_t* v = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 4;
    int64_t* vd = (int64_t*)ray_data(v);
    vd[0] = 1; vd[1] = 2; vd[2] = 3; vd[3] = 4;

    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    t = ray_table_add_col(t, ray_sym_intern("c", 1), cv);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    ray_release(a); ray_release(cv); ray_release(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));

    int64_t tname = ray_sym_intern("pvtdom_t", 8);
    TEST_ASSERT_EQ_I(ray_env_set(tname, t), RAY_OK);

    ray_t* r = ray_eval_str("(pivot pvtdom_t ['a] 'c 'v sum)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    ray_t* out_a = ray_table_get_col(r, ray_sym_intern("a", 1));
    TEST_ASSERT_NOT_NULL(out_a);
    TEST_ASSERT_EQ_I(out_a->type, RAY_SYM);
    /* The adoption under test: the output index col resolves over the
     * SOURCE column's FILE domain, not the runtime singleton. */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_a), dom);

    /* And its cells resolve through that domain to the right strings. */
    for (int64_t i = 0; i < 2; i++) {
        ray_t* s = ray_sym_vec_cell(out_a, i);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_U(ray_str_len(s), 4);
        TEST_ASSERT_TRUE(memcmp(ray_str_ptr(s), "pd_A", 4) == 0 ||
                         memcmp(ray_str_ptr(s), "pd_B", 4) == 0);
    }

    ray_release(r);

    /* Drop the env binding, our table ref, then the open ref: every
     * FILE-domain ref must be back before teardown (fresh open proves the
     * cache entry died; ASan flags any imbalance). */
    ray_release(ray_eval_str("(set pvtdom_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 3: query.c resolution / re-expression ------------------ */

#define TMP_DOM_QSYM_PATH "/tmp/rayforce_test_domain_qsym"

/* Build a DIVERGENT file-domain fixture: a symfile in which the
 * positions of "dq_a"/"dq_b" are SWAPPED relative to the current
 * runtime's global ids.  Technique: intern a→b, save the whole global
 * table, then restart the runtime (fresh sym table, identical builtin
 * prefix) and intern b→a.  Now resolving a cell id through the global
 * table yields the WRONG symbol — only domain-aware code stays correct.
 *
 * On return: pvt_rt is a FRESH runtime, *out_dom is the opened FILE
 * domain (caller releases), pos_a / pos_b receive the FILE positions
 * of dq_a/dq_b.  Returns false if the fixture could not be built. */
static int build_divergent_qsym_fixture(ray_sym_domain_t** out_dom,
                                        int64_t* pos_a, int64_t* pos_b) {
    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");

    (void)ray_sym_intern("dq_a", 4);
    (void)ray_sym_intern("dq_b", 4);
    if (ray_sym_save(TMP_DOM_QSYM_PATH) != RAY_OK) return 0;

    /* Restart: fresh global table, REVERSED intern order. */
    ray_runtime_destroy(pvt_rt);
    pvt_rt = ray_runtime_create(0, NULL);
    if (!pvt_rt) return 0;
    (void)ray_sym_intern("dq_b", 4);
    (void)ray_sym_intern("dq_a", 4);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    if (!dom) return 0;
    *pos_a = ray_sym_domain_find(dom, "dq_a", 4);
    *pos_b = ray_sym_domain_find(dom, "dq_b", 4);
    *out_dom = dom;
    return *pos_a >= 0 && *pos_b >= 0;
}

/* Table {k: SYM FILE-domain cells [pos_a pos_b], v: I64 [v0 v1]} bound
 * to env name `tname`.  Returns the owned table (caller releases). */
static ray_t* build_qsym_table(ray_sym_domain_t* dom, int64_t pos_a,
                               int64_t pos_b, int64_t v0, int64_t v1,
                               const char* tname) {
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 2);
    if (!k || RAY_IS_ERR(k)) return NULL;
    k->len = 2;
    ((int64_t*)ray_data(k))[0] = pos_a;
    ((int64_t*)ray_data(k))[1] = pos_b;
    ray_sym_domain_retain(dom);
    k->sym_domain = dom;

    ray_t* v = ray_vec_new(RAY_I64, 2);
    if (!v || RAY_IS_ERR(v)) { ray_release(k); return NULL; }
    v->len = 2;
    ((int64_t*)ray_data(v))[0] = v0;
    ((int64_t*)ray_data(v))[1] = v1;

    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    ray_release(k); ray_release(v);
    if (!t || RAY_IS_ERR(t)) return NULL;
    if (ray_env_set(ray_sym_intern(tname, strlen(tname)), t) != RAY_OK) {
        ray_release(t);
        return NULL;
    }
    return t;
}

static int sym_cell_is(ray_t* col, int64_t row, const char* want) {
    ray_t* s = ray_sym_vec_cell(col, row);
    size_t n = strlen(want);
    return s && ray_str_len(s) == n && memcmp(ray_str_ptr(s), want, n) == 0;
}

/* Upsert's literal-vs-column key match (query.c): the runtime literal
 * 'dq_a must be resolved into the KEY COLUMN'S domain before the raw
 * row scan, and the rebuilt SYM column must re-express the untouched
 * cells through that domain.  With the divergent fixture, the legacy
 * global-id compare matches the WRONG row (silent wrong-symbol bug). */
static test_result_t test_domain_query_upsert_key_lookup(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    /* discrimination precondition: file positions != runtime ids */
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    ray_t* t = build_qsym_table(dom, pos_a, pos_b, 10, 20, "dmq_t");
    TEST_ASSERT_NOT_NULL(t);

    ray_t* r = ray_eval_str("(upsert dmq_t 'k (list 'dq_a 99))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* match found in the file domain → UPDATE, not insert */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    /* …and the row whose file-domain string is "dq_a" (row 0) got the
     * new value.  The legacy global compare matched row 1 instead. */
    ray_t* out_v = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[0], 99);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[1], 20);

    /* The rebuilt SYM column is runtime-domain (it mixes the replacement
     * atom with re-expressed cells) and resolves to the right strings. */
    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), ray_sym_runtime_domain());
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));

    ray_release(r);
    ray_release(ray_eval_str("(set dmq_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    /* ref balance: the cache entry must be gone (fresh open rebuilds) */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* Insert's cell-to-output materialization (query.c): the rebuilt SYM
 * column re-expresses every copied cell through the SOURCE column's
 * FILE domain before mixing in the appended runtime atom.  The legacy
 * raw copy produced swapped strings under the divergent fixture. */
static test_result_t test_domain_query_insert_cell_reexpress(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    ray_t* t = build_qsym_table(dom, pos_a, pos_b, 1, 2, "dmi_t");
    TEST_ASSERT_NOT_NULL(t);

    ray_t* r = ray_eval_str("(insert dmi_t (list 'dq_a 3))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);

    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), ray_sym_runtime_domain());
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));   /* was "dq_b" raw */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));   /* was "dq_a" raw */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 2, "dq_a"));   /* appended atom  */

    ray_t* out_v = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[2], 3);

    ray_release(r);
    ray_release(ray_eval_str("(set dmi_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- registration -------------------------------------------------------- */

const test_entry_t domain_entries[] = {
    { "domain/runtime_identity",        test_domain_runtime_identity,        domain_setup, domain_teardown },
    { "domain/runtime_delegation",      test_domain_runtime_delegation,      domain_setup, domain_teardown },
    { "domain/vec_new_attach",          test_domain_vec_new_attach,          domain_setup, domain_teardown },
    { "domain/append_growth_cow",       test_domain_append_growth_and_cow,   domain_setup, domain_teardown },
    { "domain/slice_accessor",          test_domain_slice_accessor,          domain_setup, domain_teardown },
    { "domain/concat_attach",           test_domain_concat_attach,           domain_setup, domain_teardown },
    { "domain/serde_roundtrip",         test_domain_serde_roundtrip,         domain_setup, domain_teardown },
    { "domain/col_save_load_mmap",      test_domain_col_save_load_mmap,      domain_setup, domain_teardown },
    { "domain/open_basic",              test_domain_open_basic,              domain_setup, domain_teardown },
    { "domain/open_identity_release",   test_domain_open_identity_and_release, domain_setup, domain_teardown },
    { "domain/vec_cell_runtime_equiv",  test_domain_vec_cell_runtime_equiv,  domain_setup, domain_teardown },
    { "domain/vec_lookup_hit_miss",     test_domain_vec_lookup_hit_miss,     domain_setup, domain_teardown },
    { "domain/adopt_domain_refcount",   test_domain_adopt_domain_refcount,   domain_setup, domain_teardown },
    { "domain/pivot_index_adopts",      test_domain_pivot_index_adopts,      domain_rt_setup, domain_rt_teardown },
    { "domain/query_upsert_key_lookup", test_domain_query_upsert_key_lookup, domain_rt_setup, domain_rt_teardown },
    { "domain/query_insert_reexpress",  test_domain_query_insert_cell_reexpress, domain_rt_setup, domain_rt_teardown },
    { NULL, NULL, NULL, NULL },
};

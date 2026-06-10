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
    { NULL, NULL, NULL, NULL },
};

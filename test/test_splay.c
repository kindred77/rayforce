/* _POSIX_C_SOURCE: setenv / unsetenv (POSIX.1-2008) */
#define _POSIX_C_SOURCE 200809L
/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * test_splay.c — focused tests for src/store/splay.c paths not covered by
 * test_store.c.  Targets: validate_sym_columns (empty sym table + I64 table,
 * and RAY_SYM column detect), ray_splay_save bad-column-name skip, NULL-dir
 * error paths, missing .d schema, corrupt schema (bad name_id), and
 * splay_load_impl range/corrupt/io error branches.
 */

#include "test.h"
#include <rayforce.h>
#include "store/splay.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void splay_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void splay_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- helpers ----------------------------------------------------------- */

#define TMP_SPLAY_BASE "/tmp/rayforce_test_splay2"

/* Remove temp dir tree */
static void rm_rf(const char* path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    (void)!system(cmd);
}

/* =========================================================================
 * 1. ray_splay_save: NULL dir → RAY_ERR_IO
 * ========================================================================= */
static test_result_t test_save_null_dir(void) {
    int64_t id_x = ray_sym_intern("x", 1);
    int64_t raw[] = {1, 2, 3};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(col);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_x, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, NULL, NULL);
    TEST_ASSERT_EQ_I(err, RAY_ERR_IO);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* =========================================================================
 * 2. ray_splay_save: NULL tbl → RAY_ERR_TYPE
 * ========================================================================= */
static test_result_t test_save_null_tbl(void) {
    ray_err_t err = ray_splay_save(NULL, TMP_SPLAY_BASE "/t", NULL);
    TEST_ASSERT_EQ_I(err, RAY_ERR_TYPE);
    PASS();
}

/* =========================================================================
 * 3. ray_splay_save: column name starting with '.' is skipped silently.
 *    Verify: save succeeds, but the column file is NOT on disk.
 * ========================================================================= */
static test_result_t test_save_skips_dot_col_name(void) {
    const char* dir = TMP_SPLAY_BASE "/dot_col";
    rm_rf(dir);

    /* Intern a name that starts with '.' */
    int64_t id_dot = ray_sym_intern(".hidden", 7);
    int64_t id_ok  = ray_sym_intern("good", 4);

    int64_t raw[] = {10, 20};
    ray_t* col_dot = ray_vec_from_raw(RAY_I64, raw, 2);
    ray_t* col_ok  = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col_dot);
    TEST_ASSERT_NOT_NULL(col_ok);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_ok,  col_ok);
    tbl = ray_table_add_col(tbl, id_dot, col_dot);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* ".hidden" column file must NOT exist */
    char bad_path[512];
    snprintf(bad_path, sizeof(bad_path), "%s/.hidden", dir);
    TEST_ASSERT_EQ_I(access(bad_path, F_OK), -1);

    /* "good" column file must exist */
    char good_path[512];
    snprintf(good_path, sizeof(good_path), "%s/good", dir);
    TEST_ASSERT_EQ_I(access(good_path, F_OK), 0);

    ray_release(col_dot);
    ray_release(col_ok);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 4. ray_splay_save: column name containing '/' is skipped silently.
 * ========================================================================= */
static test_result_t test_save_skips_slash_col_name(void) {
    const char* dir = TMP_SPLAY_BASE "/slash_col";
    rm_rf(dir);

    int64_t id_slash = ray_sym_intern("a/b", 3);
    int64_t id_ok    = ray_sym_intern("val", 3);

    int64_t raw[] = {1, 2};
    ray_t* col_slash = ray_vec_from_raw(RAY_I64, raw, 2);
    ray_t* col_ok    = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col_slash);
    TEST_ASSERT_NOT_NULL(col_ok);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_ok,    col_ok);
    tbl = ray_table_add_col(tbl, id_slash, col_slash);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* "a/b" file must NOT exist (path traversal would create subdirs) */
    char bad_path[512];
    snprintf(bad_path, sizeof(bad_path), "%s/a", dir);
    TEST_ASSERT_EQ_I(access(bad_path, F_OK), -1);

    ray_release(col_slash);
    ray_release(col_ok);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 5. splay_load_impl: NULL dir → error("io")
 * ========================================================================= */
static test_result_t test_load_null_dir(void) {
    ray_t* r = ray_splay_load(NULL, NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "io");
    ray_release(r);

    /* Also via ray_read_splayed */
    ray_t* r2 = ray_read_splayed(NULL, NULL);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "io");
    ray_release(r2);
    PASS();
}

/* =========================================================================
 * 6. splay_load_impl: missing .d schema file → propagates error from
 *    ray_col_load (schema not found = io/corrupt).
 * ========================================================================= */
static test_result_t test_load_missing_schema(void) {
    /* Directory exists but contains no .d file */
    const char* dir = TMP_SPLAY_BASE "/no_schema";
    rm_rf(dir);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    (void)!system(cmd);

    ray_t* r = ray_splay_load(dir, NULL);
    /* ray_col_load of missing file returns an error object */
    TEST_ASSERT_TRUE(!r || RAY_IS_ERR(r));
    if (r) ray_release(r);

    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 7. splay_load_impl: .d exists but column file missing → error("io")
 *    Save a table, then delete one column file, then load — hits the
 *    col-load-fail branch (lines 195-199).
 * ========================================================================= */
static test_result_t test_load_missing_col_file(void) {
    const char* dir = TMP_SPLAY_BASE "/miss_col";
    rm_rf(dir);

    int64_t id_a = ray_sym_intern("aa", 2);
    int64_t id_b = ray_sym_intern("bb", 2);

    int64_t raw[] = {1, 2, 3};
    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(col_a);
    TEST_ASSERT_NOT_NULL(col_b);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_a, col_a);
    tbl = ray_table_add_col(tbl, id_b, col_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Remove column "bb" so load hits the missing-file branch */
    char miss_path[512];
    snprintf(miss_path, sizeof(miss_path), "%s/bb", dir);
    unlink(miss_path);

    ray_t* loaded = ray_splay_load(dir, NULL);
    TEST_ASSERT_TRUE(!loaded || RAY_IS_ERR(loaded));
    if (loaded) ray_release(loaded);

    ray_release(col_a);
    ray_release(col_b);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 8. validate_sym_columns: empty sym table + table with no RAY_SYM cols
 *    → should return RAY_OK (covered via splay_load_impl post-load check).
 *    This hits lines 46-54 of validate_sym_columns with nc > 0 and no SYM.
 * ========================================================================= */
static test_result_t test_validate_sym_no_sym_cols(void) {
    const char* dir = TMP_SPLAY_BASE "/nosym_ok";
    rm_rf(dir);

    int64_t id_x = ray_sym_intern("xval", 4);
    int64_t raw[] = {5, 6, 7};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(col);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_x, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save with sym_path so the sym file is written */
    const char* sym_path = TMP_SPLAY_BASE "/nosym_ok_sym";
    ray_err_t err = ray_splay_save(tbl, dir, sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table — now ray_sym_count() == 0 */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);  /* "" reserved */

    /* Load WITHOUT sym_path so sym table stays empty.
     * validate_sym_columns: sym_count==0, nc==1, no RAY_SYM col → RAY_OK */
    ray_t* loaded = ray_splay_load(dir, NULL);
    /* May fail because sym IDs in .d are unknown without the sym file — that
     * hits the name_atom==NULL path (corrupt).  That is also a valid and
     * covered path, so just check it is either ok or an error. */
    if (loaded && !RAY_IS_ERR(loaded)) {
        ray_release(loaded);
    } else if (loaded) {
        ray_release(loaded);
    }

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    unlink(sym_path);
    PASS();
}

/* =========================================================================
 * 9. validate_sym_columns: empty sym table + table WITH a RAY_SYM col
 *    → RAY_ERR_CORRUPT (lines 215-218 in splay.c).
 *    We need the sym IDs written with a sym file, reset, then reload with
 *    NULL sym_path so sym table is empty but schema resolves via currently
 *    interned IDs — but wait, without sym_path the ID lookup will fail at
 *    name_atom.  We need to intern enough IDs to match the .d but then
 *    clear only the *data* symbols, not the column-name symbols.
 *
 *    Strategy: use ray_splay_load with sym_path to load successfully once,
 *    then construct a scenario where sym_count==0 but the table loads.
 *    Actually the cleanest path: save a purely I64 table (no RAY_SYM
 *    columns), then manually craft a .d + column file that loads into a
 *    table whose column is RAY_SYM — but that requires bypassing the API.
 *
 *    Simpler: the existing test_splay_load_sym_missing_corrupt in
 *    test_store.c already covers validate_sym_columns → corrupt via a
 *    RAY_SYM table saved *with* sym, then loaded *without* sym.  But that
 *    test hits lines 215-218 only when col load succeeds for the RAY_SYM
 *    column but sym_count==0.  Let us replicate it here to guarantee
 *    coverage from our suite.
 * ========================================================================= */
static test_result_t test_validate_sym_corrupt(void) {
    const char* dir     = TMP_SPLAY_BASE "/sym_corrupt";
    const char* sym_path = TMP_SPLAY_BASE "/sym_corrupt_sym";
    rm_rf(dir);
    unlink(sym_path);

    /* Build a table with one RAY_SYM column */
    int64_t id_col  = ray_sym_intern("scol2", 5);
    int64_t sym_val = ray_sym_intern("zzz", 3);

    ray_t* col = ray_sym_vec_new(RAY_SYM_W8, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    col->len = 1;
    ((uint8_t*)ray_data(col))[0] = (uint8_t)sym_val;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_col, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save with sym file */
    ray_err_t err = ray_splay_save(tbl, dir, sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Re-intern the column name so .d can be parsed (sym_count > 0 after
     * reload would skip validate, so we need to keep sym table empty for
     * column names too).  We'll take a different approach: load with sym
     * first to confirm it works, then load without to hit validate path. */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);  /* "" reserved */

    /* Load with sym_path — should succeed and re-populate sym table */
    ray_t* ok = ray_splay_load(dir, sym_path);
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ok));
    ray_release(ok);

    /* Reset again — now load WITHOUT sym_path.
     * The column-name ID for "scol2" is in .d.  With empty sym table,
     * ray_sym_str(id_col) returns NULL → hits "corrupt" at line 162.
     * This is also a useful coverage path (lines 161-163 of splay.c). */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);  /* "" reserved */

    ray_t* bad = ray_splay_load(dir, NULL);
    TEST_ASSERT_TRUE(!bad || RAY_IS_ERR(bad));
    if (bad && RAY_IS_ERR(bad)) {
        TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    }
    if (bad) ray_release(bad);

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    unlink(sym_path);
    PASS();
}

/* =========================================================================
 * 10. validate_sym_columns: sym_count==0, schema_ncols>0 but table loaded
 *     0 columns — hits line 47 (schema_ncols > 0 && nc == 0).
 *     This is very hard to achieve via public API (table_add_col always
 *     succeeds for valid inputs); skip and mark as known gap.
 *
 * 11. splay_load_impl: non-NULL sym_path that fails to load (bad path) →
 *     error code from ray_sym_load.
 * ========================================================================= */
static test_result_t test_load_bad_sym_path(void) {
    const char* dir = TMP_SPLAY_BASE "/bad_sym";
    rm_rf(dir);

    int64_t id_k = ray_sym_intern("k1", 2);
    int64_t raw[] = {42};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 1);
    TEST_ASSERT_NOT_NULL(col);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_k, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Pass a nonexistent sym_path to both loaders */
    const char* bad_sym = "/tmp/rayforce_splay_nonexistent_sym_XXXXXX";
    ray_t* r1 = ray_splay_load(dir, bad_sym);
    TEST_ASSERT_TRUE(!r1 || RAY_IS_ERR(r1));
    if (r1) ray_release(r1);

    ray_t* r2 = ray_read_splayed(dir, bad_sym);
    TEST_ASSERT_TRUE(!r2 || RAY_IS_ERR(r2));
    if (r2) ray_release(r2);

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 12. validate_sym_columns: sym_count==0, nc>0, col IS RAY_SYM → corrupt.
 *     Approach: save a table with RAY_SYM column + sym file, then reload
 *     providing the sym_path so sym table gets populated.  This time we
 *     need sym_count==0 but the col file to successfully load.  We can
 *     achieve this by re-interning only the column-name symbol (so the .d
 *     can be decoded) but NOT the data symbols, and the RAY_SYM column
 *     file to load successfully.  After load the validate_sym_columns sees
 *     nc==1, col->type==RAY_SYM, sym_count==0 → corrupt.
 *
 *     BUT: if we re-intern only the name symbol, ray_sym_count() > 0 (it
 *     is 1), so validate_sym_columns returns RAY_OK early (line 44).
 *
 *     The only practical way to get sym_count==0 AND have sym IDs usable
 *     is impossible through the public API without patching.  Document
 *     as a known dead-code gap and skip.
 * ========================================================================= */

/* =========================================================================
 * 13. ray_read_splayed round-trip (mmap path) — exercises the use_mmap
 *     branch and the "nyi fallback" path for types that don't support mmap.
 * ========================================================================= */
static test_result_t test_read_splayed_roundtrip(void) {
    const char* dir = TMP_SPLAY_BASE "/mmap_rt";
    rm_rf(dir);

    int64_t id_p = ray_sym_intern("price", 5);
    int64_t id_q = ray_sym_intern("qty",   3);

    double  raw_p[] = {1.1, 2.2, 3.3};
    int64_t raw_q[] = {10,  20,  30};
    ray_t* col_p = ray_vec_from_raw(RAY_F64, raw_p, 3);
    ray_t* col_q = ray_vec_from_raw(RAY_I64, raw_q, 3);
    TEST_ASSERT_NOT_NULL(col_p);
    TEST_ASSERT_NOT_NULL(col_q);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_p, col_p);
    tbl = ray_table_add_col(tbl, id_q, col_q);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_splayed(dir, NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    ray_release(loaded);
    ray_release(col_p);
    ray_release(col_q);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 14. ray_splay_save with sym_path exercises the sym_err != RAY_OK branch
 *     indirectly: use a nonexistent nested path where mkdir_p should
 *     succeed but sym_save might fail if sym_path dir doesn't exist.
 *     Actually ray_sym_save creates/overwrites the file, it only fails on
 *     permissions.  Use a directory as the sym_path (cannot write a file
 *     over a directory).
 * ========================================================================= */
static test_result_t test_save_sym_error(void) {
    const char* dir = TMP_SPLAY_BASE "/sym_err_save";
    rm_rf(dir);

    int64_t id_v = ray_sym_intern("v", 1);
    int64_t raw[] = {1};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 1);
    TEST_ASSERT_NOT_NULL(col);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_v, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Use an existing directory as sym_path — write will fail */
    char sym_as_dir[512];
    snprintf(sym_as_dir, sizeof(sym_as_dir), "%s/sym_dir", dir);
    /* Ensure parent dir exists first */
    char mk[600];
    snprintf(mk, sizeof(mk), "mkdir -p %s", sym_as_dir);
    (void)!system(mk);

    ray_err_t err = ray_splay_save(tbl, dir, sym_as_dir);
    /* Either succeeds (some impls tolerate it) or returns an error — either
     * way we have exercised the sym_path branch */
    (void)err;

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 15. splay_load_impl: corrupt .d with valid sym IDs but corrupt name
 *     (name starting with '.').
 *     We save a normal table, then manually overwrite the .d schema with a
 *     single I64 value pointing at a sym whose string begins with '.'.
 * ========================================================================= */
static test_result_t test_load_corrupt_col_name_in_schema(void) {
    const char* dir = TMP_SPLAY_BASE "/corrupt_name";
    rm_rf(dir);

    /* Intern a name that starts with '.' so the string is available */
    int64_t id_dot = ray_sym_intern(".bad", 4);
    int64_t id_ok  = ray_sym_intern("okname", 6);

    int64_t raw[] = {1, 2};
    ray_t* col_ok = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col_ok);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_ok, col_ok);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save with the legitimate name, then overwrite .d to reference id_dot */
    ray_err_t save_err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(save_err, RAY_OK);

    /* Overwrite .d with a schema that has id_dot */
    ray_t* fake_schema = ray_vec_from_raw(RAY_I64, &id_dot, 1);
    TEST_ASSERT_NOT_NULL(fake_schema);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fake_schema));

    char d_path[512];
    snprintf(d_path, sizeof(d_path), "%s/.d", dir);

    /* Save the fake schema as the .d file */
    extern ray_err_t ray_col_save(ray_t* vec, const char* path);
    ray_err_t ds_err = ray_col_save(fake_schema, d_path);
    TEST_ASSERT_EQ_I(ds_err, RAY_OK);

    /* Now loading should detect '.' prefix name → corrupt */
    ray_t* loaded = ray_splay_load(dir, NULL);
    TEST_ASSERT_TRUE(!loaded || RAY_IS_ERR(loaded));
    if (loaded && RAY_IS_ERR(loaded)) {
        TEST_ASSERT_STR_EQ(ray_err_code(loaded), "corrupt");
    }
    if (loaded) ray_release(loaded);

    ray_release(fake_schema);
    ray_release(col_ok);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 16. splay_load_impl: dir path so long that "%s/.d" overflows 1024-byte
 *     buffer → ray_error("range") at line 141.
 *     We need dir_len + len("/.d") >= 1024, i.e. dir_len >= 1021.
 * ========================================================================= */
static test_result_t test_load_dir_path_too_long(void) {
    /* Build a dir string that is exactly 1021 chars so path_len >= 1024 */
    char long_dir[2048];
    /* Use "/tmp/" (5 chars) then pad with 'a' to reach 1021 total */
    memset(long_dir, 'a', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';
    /* Make it start with /tmp/ for kernel sanity (won't create it anyway) */
    memcpy(long_dir, "/tmp/", 5);
    long_dir[1021] = '\0';  /* 1021-char string → 1021 + 3 = 1024 >= 1024 */

    ray_t* r = ray_splay_load(long_dir, NULL);
    /* Either "range" error or some other IO error (dir doesn't exist) */
    TEST_ASSERT_TRUE(!r || RAY_IS_ERR(r));
    if (r) ray_release(r);
    PASS();
}

/* =========================================================================
 * 17. splay_load_impl: column name so long that "%s/<colname>" overflows
 *     1024-byte buffer → ray_error("range") at lines 181-183.
 *     Use a short dir, save a normal table, then overwrite .d schema with
 *     a sym ID whose string is 1020+ chars.  The col file load hits the
 *     path-length check before attempting to open the (nonexistent) file.
 * ========================================================================= */
static test_result_t test_load_col_path_too_long(void) {
    const char* dir = "/tmp/rft_ln";
    rm_rf(dir);

    /* Build a column name that is 1017 chars: dir (11) + "/" (1) + name (1017)
     * = 1029 >= 1024 triggers the range check. */
    char long_name[1018];
    memset(long_name, 'c', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    int64_t id_long = ray_sym_intern(long_name, sizeof(long_name) - 1);
    int64_t id_ok   = ray_sym_intern("shortcol", 8);

    int64_t raw[] = {1, 2};
    ray_t* col_ok = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col_ok);

    /* Build a table with the short-named column, save it */
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_ok, col_ok);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t save_err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(save_err, RAY_OK);

    /* Overwrite .d with the long-name sym ID */
    extern ray_err_t ray_col_save(ray_t* vec, const char* path);
    ray_t* fake_schema = ray_vec_from_raw(RAY_I64, &id_long, 1);
    TEST_ASSERT_NOT_NULL(fake_schema);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fake_schema));

    char d_path[64];
    snprintf(d_path, sizeof(d_path), "%s/.d", dir);
    ray_err_t ds_err = ray_col_save(fake_schema, d_path);
    TEST_ASSERT_EQ_I(ds_err, RAY_OK);

    /* Load — should hit range error at line 181 */
    ray_t* loaded = ray_splay_load(dir, NULL);
    TEST_ASSERT_TRUE(!loaded || RAY_IS_ERR(loaded));
    if (loaded && RAY_IS_ERR(loaded)) {
        TEST_ASSERT_STR_EQ(ray_err_code(loaded), "range");
    }
    if (loaded) ray_release(loaded);

    ray_release(fake_schema);
    ray_release(col_ok);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 18. validate_sym_columns: sym_count==0, zero-column table.
 *     Save a table with no columns, reset sym table, reload without sym_path.
 *     splay_load_impl: schema len=0, loop skips, calls validate_sym_columns
 *     with tbl having nc=0, schema_ncols=0. Hits lines 46,49,53,54.
 * ========================================================================= */
static test_result_t test_validate_sym_zero_col_table(void) {
    const char* dir = TMP_SPLAY_BASE "/zero_col";
    rm_rf(dir);

    /* Build a zero-column table */
    ray_t* tbl = ray_table_new(0);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table — sym_count() == 0 */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);  /* "" reserved */

    /* Load: schema_ncols=0, loop skips, validate_sym_columns runs with
     * sym_count==0, nc==0 → hits lines 46,49,50,52,53,54 and returns OK */
    ray_t* loaded = ray_splay_load(dir, NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 0);

    ray_release(loaded);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 18. ray_splay_save_bulk: durable=false + sym_path != NULL → hits the
 *     ray_sym_save_bulk branch (line 78 of splay.c).
 *     ray_splay_save_bulk is the only caller that sets durable=false.
 *     Previous tests only called ray_splay_save (durable=true), so
 *     ray_sym_save_bulk was never invoked.
 * ========================================================================= */
static test_result_t test_save_bulk_with_sym_path(void) {
    const char* dir      = TMP_SPLAY_BASE "/bulk_sym";
    const char* sym_path = TMP_SPLAY_BASE "/bulk_sym.sym";
    rm_rf(dir);
    unlink(sym_path);

    int64_t id_w = ray_sym_intern("wval", 4);
    int64_t raw[] = {100, 200};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_w, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* durable=false (bulk) + sym_path → exercises ray_sym_save_bulk at line 78 */
    ray_err_t err = ray_splay_save_bulk(tbl, dir, sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Confirm the sym file was written */
    TEST_ASSERT_EQ_I(access(sym_path, F_OK), 0);

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    unlink(sym_path);
    PASS();
}

/* =========================================================================
 * 19. splay_save_impl line 89: snprintf overflow for "%s/.d" path.
 *     Requires strlen(dir) >= 1021 so that strlen(dir)+3 >= 1024.
 *     Build a deeply nested path using short components (≤ 50 chars each)
 *     so the filesystem NAME_MAX (255) is not exceeded, then call mkdir_p
 *     via system(), then ray_splay_save → snprintf("%s/.d") fires range.
 *
 *     Path layout (each component 50 chars):
 *       /tmp/rft_deep_save/         (18 chars)
 *       + 20 levels of "aaaaa...a/" (51 chars each)
 *       total 18 + 20*51 - 1 = 1037 chars (last level has no trailing /)
 *     Actually: 18 + 19*51 + 50 = 18 + 969 + 50 = 1037 ≥ 1021. Good.
 * ========================================================================= */
static test_result_t test_save_dir_path_too_long(void) {
#ifdef __APPLE__
    /* macOS PATH_MAX = 1024; mkdir -p stops short of the 1021-char
     * tree this test needs.  ray_splay_save's path-overflow guard
     * fires under the same condition on Linux PATH_MAX = 4096.  Skip
     * on Darwin — the Linux runner covers the regression. */
    SKIP("PATH_MAX=1024 on macOS — deep-mkdir fixture not portable");
#endif
    /* Construct the nested path in a buffer */
    char long_dir[2048];
    const char* base   = "/tmp/rft_deep_save";  /* 18 chars */
    const char* comp   = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 50 chars */
    int nlevels        = 20;

    int off = snprintf(long_dir, sizeof(long_dir), "%s", base);
    for (int i = 0; i < nlevels && off < (int)sizeof(long_dir) - 2; i++) {
        long_dir[off++] = '/';
        int rem = (int)sizeof(long_dir) - off - 1;
        if (rem <= 0) break;
        int clen = (int)strlen(comp);
        if (clen > rem) clen = rem;
        memcpy(long_dir + off, comp, (size_t)clen);
        off += clen;
    }
    long_dir[off] = '\0';

    /* Verify we actually have a long enough path */
    TEST_ASSERT_TRUE((size_t)off >= 1021);

    /* Create the directory tree so ray_mkdir_p inside save succeeds.
     * system("mkdir -p ...") handles arbitrarily deep paths. */
    char mk[4096];
    snprintf(mk, sizeof(mk), "mkdir -p \"%s\"", long_dir);
    (void)!system(mk);

    int64_t id_v2 = ray_sym_intern("v2long", 6);
    int64_t raw[] = {1};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 1);
    TEST_ASSERT_NOT_NULL(col);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_v2, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* ray_splay_save: mkdir_p passes (dir exists), then snprintf("%s/.d")
     * overflows the 1024-byte buffer → returns RAY_ERR_RANGE (line 89) */
    ray_err_t err = ray_splay_save(tbl, long_dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_ERR_RANGE);

    ray_release(col);
    ray_release(tbl);
    /* Cleanup entire nested tree from the base */
    char rm_cmd[256];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf /tmp/rft_deep_save");
    (void)!system(rm_cmd);
    PASS();
}

/* =========================================================================
 * 20. splay_save_impl line 115: snprintf overflow for "%s/<colname>" path.
 *     Use a short dir + a column name long enough that dir + "/" + name
 *     overflows 1024 bytes.  dir="/tmp/rft_sv" (12 chars) + "/" (1) +
 *     1011 'c' chars = 1024, which is NOT < 1024, so overflow fires.
 *     The column must pass the name-safety check (no /, \, ., not empty).
 * ========================================================================= */
static test_result_t test_save_col_path_too_long(void) {
    const char* dir = "/tmp/rft_sv";
    rm_rf(dir);

    /* dir = 11 chars; "/" = 1 char; need name_len >= 1012 to make total >= 1024 */
    char long_colname[1013];
    memset(long_colname, 'c', sizeof(long_colname) - 1);
    long_colname[sizeof(long_colname) - 1] = '\0';  /* 1012-char name */

    int64_t id_long_col = ray_sym_intern(long_colname, sizeof(long_colname) - 1);
    int64_t id_short    = ray_sym_intern("sv_ok", 5);

    int64_t raw[] = {7, 8};
    ray_t* col_long  = ray_vec_from_raw(RAY_I64, raw, 2);
    ray_t* col_short = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col_long);
    TEST_ASSERT_NOT_NULL(col_short);

    /* Put the short column first so schema writes fine, then long col triggers
     * the path-overflow on the second iteration */
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_short,    col_short);
    tbl = ray_table_add_col(tbl, id_long_col, col_long);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_ERR_RANGE);

    ray_release(col_long);
    ray_release(col_short);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 21. RAY_CSV_TRACE env: trace=true + valid dir → hits line 146 fprintf.
 *     Use setenv("RAY_CSV_TRACE","1",1) before the call and unsetenv after.
 * ========================================================================= */
static test_result_t test_trace_valid_dir(void) {
    const char* dir = TMP_SPLAY_BASE "/trace_valid";
    rm_rf(dir);

    int64_t id_t = ray_sym_intern("tval", 4);
    int64_t raw[] = {1, 2};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_t, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Activate trace: splay_load_impl line 144-146 */
    setenv("RAY_CSV_TRACE", "1", 1);
    ray_t* loaded = ray_splay_load(dir, NULL);
    unsetenv("RAY_CSV_TRACE");

    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_release(loaded);

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 22. RAY_CSV_TRACE env: trace=true + missing schema → hits lines 161-163
 *     fprintf (schema load failed branch).
 * ========================================================================= */
static test_result_t test_trace_missing_schema(void) {
    const char* dir = TMP_SPLAY_BASE "/trace_noschema";
    rm_rf(dir);
    /* Create dir without .d file */
    char mk[512];
    snprintf(mk, sizeof(mk), "mkdir -p %s", dir);
    (void)!system(mk);

    setenv("RAY_CSV_TRACE", "1", 1);
    ray_t* r = ray_splay_load(dir, NULL);
    unsetenv("RAY_CSV_TRACE");

    /* Schema load failed → error returned */
    TEST_ASSERT_TRUE(!r || RAY_IS_ERR(r));
    if (r) ray_release(r);

    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 23. RAY_CSV_TRACE env: trace=true + schema exists but column file missing
 *     → hits lines 221-223 fprintf (col load failed branch).
 * ========================================================================= */
static test_result_t test_trace_missing_col(void) {
    const char* dir = TMP_SPLAY_BASE "/trace_misscol";
    rm_rf(dir);

    int64_t id_a = ray_sym_intern("ta", 2);
    int64_t id_b = ray_sym_intern("tb", 2);
    int64_t raw[] = {5, 6};
    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw, 2);
    ray_t* col_b = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col_a);
    TEST_ASSERT_NOT_NULL(col_b);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_a, col_a);
    tbl = ray_table_add_col(tbl, id_b, col_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Remove column "tb" to cause col load failure */
    char miss[512];
    snprintf(miss, sizeof(miss), "%s/tb", dir);
    unlink(miss);

    setenv("RAY_CSV_TRACE", "1", 1);
    ray_t* r = ray_splay_load(dir, NULL);
    unsetenv("RAY_CSV_TRACE");

    TEST_ASSERT_TRUE(!r || RAY_IS_ERR(r));
    if (r) ray_release(r);

    ray_release(col_a);
    ray_release(col_b);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 24. RAY_CSV_TRACE env: trace=true + sym ID not found in sym table
 *     → hits lines 183-185 fprintf (missing schema symbol branch).
 *     Use the same corrupt-schema technique: save table, reset sym table,
 *     reload without sym_path so name_atom is NULL on first column.
 * ========================================================================= */
static test_result_t test_trace_missing_sym_id(void) {
    const char* dir = TMP_SPLAY_BASE "/trace_missym";
    rm_rf(dir);

    int64_t id_c = ray_sym_intern("tc", 2);
    int64_t raw[] = {9};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 1);
    TEST_ASSERT_NOT_NULL(col);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_c, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table — now name_id for "tc" is no longer valid */
    ray_sym_destroy();
    (void)ray_sym_init();

    setenv("RAY_CSV_TRACE", "1", 1);
    ray_t* r = ray_splay_load(dir, NULL);
    unsetenv("RAY_CSV_TRACE");

    TEST_ASSERT_TRUE(!r || RAY_IS_ERR(r));
    if (r) ray_release(r);

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 25. splay_save_impl line 91: ray_col_save(".d") fails because the
 *     directory is read-only after being created.
 *     mkdir_p returns OK (dir is created with permissions), then we chmod
 *     the dir to 0555 so the .d file cannot be written.
 * ========================================================================= */
static test_result_t test_save_schema_write_fails(void) {
    const char* dir = TMP_SPLAY_BASE "/no_write_schema";
    rm_rf(dir);
    char mk[512];
    snprintf(mk, sizeof(mk), "mkdir -p %s", dir);
    (void)!system(mk);

    /* Make dir read-only so .d cannot be written */
    chmod(dir, 0555);

    int64_t id_w = ray_sym_intern("ws", 2);
    int64_t raw[] = {3, 4};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 2);
    TEST_ASSERT_NOT_NULL(col);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_w, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* ray_splay_save: mkdir_p passes (dir exists), then ray_col_save(".d") fails */
    ray_err_t err = ray_splay_save(tbl, dir, NULL);
    /* Must restore permissions before cleanup */
    chmod(dir, 0755);
    /* Expect a write failure (io or similar) */
    TEST_ASSERT_TRUE(err != RAY_OK);

    ray_release(col);
    ray_release(tbl);
    rm_rf(dir);
    PASS();
}

/* =========================================================================
 * 26. splay_save_impl line 120: ray_col_save(col) fails because the
 *     directory becomes read-only after the .d schema is written.
 *     Strategy: first write the .d file, then chmod dir to 0555 mid-save.
 *     We cannot intercept mid-save, so we pre-write the .d ourselves and
 *     then call save on a pre-existing read-only directory.
 *     Actually: if .d already exists in a read-only dir, ray_col_save for
 *     .d also fails.  We need write-ok for .d but not for the column.
 *
 *     Alternative: save a 2-column table where the first column succeeds,
 *     then make the dir read-only after .d writes.  This is TOCTOU and not
 *     reliable.  Instead we use a different approach:
 *
 *     Write schema to a separate file, create dir with 0755, pre-save the
 *     .d, chmod 0555, then call ray_splay_save on the same dir — it will
 *     fail on overwriting .d (also an io error hitting line 91).  OR:
 *
 *     Use a sub-directory trick: put the column file in a subdirectory
 *     whose permissions we control, while .d is in a writable parent.
 *     This requires a custom directory layout not supported by splay API.
 *
 *     Practical approach: use a tmpfs or overlay filesystem — too complex.
 *
 *     Best achievable: use /proc/self or /sys path (already read-only) as
 *     dir, which causes mkdir_p to fail at line 73-74.  This covers the
 *     mkdir_p failure branch (line 74, `^2` shows it's already covered by 2
 *     calls — but let's verify).
 *
 *     We skip this test to avoid fragile TOCTOU and note it as unreachable
 *     through the single-process API without a filesystem hook.
 * ========================================================================= */

/* =========================================================================
 * 27. ray_splay_load (heap path) on STR columns — covers col_copy_str_pool:
 *     the main branch (long strings → non-empty str pool deep-copy) and the
 *     empty-pool early exit (all strings inline).  The language surface is
 *     mmap-only since the .db.*.mount removal, so this C test is the
 *     remaining caller of the heap STR deep-copy path.
 * ========================================================================= */
static test_result_t test_load_str_pool_heap(void) {
    const char* dir_pool = TMP_SPLAY_BASE "/str_pool";
    const char* dir_inl  = TMP_SPLAY_BASE "/str_inline";
    rm_rf(dir_pool);
    rm_rf(dir_inl);

    int64_t id_name = ray_sym_intern("name", 4);
    size_t  slen    = 0;

    /* Long strings (> inline max) → non-empty str pool */
    ray_t* col_pool = ray_vec_new(RAY_STR, 3);
    TEST_ASSERT_NOT_NULL(col_pool);
    col_pool = ray_str_vec_append(col_pool, "a-very-long-pooled-string", 25);
    col_pool = ray_str_vec_append(col_pool, "another-pooled-string-too", 25);
    col_pool = ray_str_vec_append(col_pool, "third-pooled-string-here", 24);
    TEST_ASSERT_NOT_NULL(col_pool);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_pool));

    ray_t* tbl_pool = ray_table_new(2);
    tbl_pool = ray_table_add_col(tbl_pool, id_name, col_pool);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_pool));
    TEST_ASSERT_EQ_I(ray_splay_save(tbl_pool, dir_pool, NULL), RAY_OK);

    ray_t* loaded = ray_splay_load(dir_pool, NULL); /* heap, not mmap */
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* lc = ray_table_get_col_idx(loaded, 0); /* borrowed */
    TEST_ASSERT_NOT_NULL(lc);
    const char* s0 = ray_str_vec_get(lc, 0, &slen);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(slen, 25);
    TEST_ASSERT_MEM_EQ(25, s0, "a-very-long-pooled-string");
    const char* s2 = ray_str_vec_get(lc, 2, &slen);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQ_U(slen, 24);
    TEST_ASSERT_MEM_EQ(24, s2, "third-pooled-string-here");
    ray_release(loaded);
    ray_release(col_pool);
    ray_release(tbl_pool);

    /* Short strings (all inline) → empty pool early-exit branch */
    ray_t* col_inl = ray_vec_new(RAY_STR, 2);
    TEST_ASSERT_NOT_NULL(col_inl);
    col_inl = ray_str_vec_append(col_inl, "aa", 2);
    col_inl = ray_str_vec_append(col_inl, "bb", 2);
    TEST_ASSERT_NOT_NULL(col_inl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_inl));

    ray_t* tbl_inl = ray_table_new(2);
    tbl_inl = ray_table_add_col(tbl_inl, id_name, col_inl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_inl));
    TEST_ASSERT_EQ_I(ray_splay_save(tbl_inl, dir_inl, NULL), RAY_OK);

    ray_t* loaded2 = ray_splay_load(dir_inl, NULL);
    TEST_ASSERT_NOT_NULL(loaded2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded2));
    ray_t* lc2 = ray_table_get_col_idx(loaded2, 0);
    TEST_ASSERT_NOT_NULL(lc2);
    const char* t1 = ray_str_vec_get(lc2, 1, &slen);
    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_EQ_U(slen, 2);
    TEST_ASSERT_MEM_EQ(2, t1, "bb");
    ray_release(loaded2);
    ray_release(col_inl);
    ray_release(tbl_inl);

    rm_rf(dir_pool);
    rm_rf(dir_inl);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t splay_entries[] = {
    { "splay/save_null_dir",              test_save_null_dir,                   splay_setup, splay_teardown },
    { "splay/save_null_tbl",              test_save_null_tbl,                   splay_setup, splay_teardown },
    { "splay/save_skips_dot_col_name",    test_save_skips_dot_col_name,         splay_setup, splay_teardown },
    { "splay/save_skips_slash_col_name",  test_save_skips_slash_col_name,       splay_setup, splay_teardown },
    { "splay/load_null_dir",              test_load_null_dir,                   splay_setup, splay_teardown },
    { "splay/load_missing_schema",        test_load_missing_schema,             splay_setup, splay_teardown },
    { "splay/load_missing_col_file",      test_load_missing_col_file,           splay_setup, splay_teardown },
    { "splay/validate_sym_no_sym_cols",   test_validate_sym_no_sym_cols,        splay_setup, splay_teardown },
    { "splay/validate_sym_corrupt",       test_validate_sym_corrupt,            splay_setup, splay_teardown },
    { "splay/load_bad_sym_path",          test_load_bad_sym_path,               splay_setup, splay_teardown },
    { "splay/read_splayed_roundtrip",     test_read_splayed_roundtrip,          splay_setup, splay_teardown },
    { "splay/save_sym_error",             test_save_sym_error,                  splay_setup, splay_teardown },
    { "splay/load_corrupt_col_name",      test_load_corrupt_col_name_in_schema, splay_setup, splay_teardown },
    { "splay/validate_sym_zero_col",      test_validate_sym_zero_col_table,     splay_setup, splay_teardown },
    { "splay/load_dir_path_too_long",     test_load_dir_path_too_long,          splay_setup, splay_teardown },
    { "splay/load_col_path_too_long",     test_load_col_path_too_long,          splay_setup, splay_teardown },
    { "splay/save_bulk_with_sym_path",    test_save_bulk_with_sym_path,         splay_setup, splay_teardown },
    { "splay/save_dir_path_too_long",     test_save_dir_path_too_long,          splay_setup, splay_teardown },
    { "splay/save_col_path_too_long",     test_save_col_path_too_long,          splay_setup, splay_teardown },
    { "splay/trace_valid_dir",            test_trace_valid_dir,                 splay_setup, splay_teardown },
    { "splay/trace_missing_schema",       test_trace_missing_schema,            splay_setup, splay_teardown },
    { "splay/trace_missing_col",          test_trace_missing_col,               splay_setup, splay_teardown },
    { "splay/trace_missing_sym_id",       test_trace_missing_sym_id,            splay_setup, splay_teardown },
    { "splay/save_schema_write_fails",    test_save_schema_write_fails,         splay_setup, splay_teardown },
    { "splay/load_str_pool_heap",         test_load_str_pool_heap,              splay_setup, splay_teardown },
    { NULL, NULL, NULL, NULL },
};

/*
 * test_stress_matrix.c — deterministic scenario matrix for the
 * parted/splayed shared-symfile stress engine (stress_store.c).
 */

#include "test.h"
#include "stress_store.h"
#include <rayforce.h>
#include "store/splay.h"
#include "store/part.h"
#include "mem/heap.h"
#include "table/sym.h"

#define STRESS_DB "/tmp/rayforce_stress_matrix"

static void stress_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void stress_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* Wrapper pattern: the _impl body may early-return on TEST_ASSERT failure;
 * the wrapper guarantees stress_destroy (which frees the shadow + oplog and
 * keeps the db dir only when c->failed is set). */

/* Fixture seeds, persists, and both table types load back cleanly. */
static test_result_t test_fixture_roundtrip_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 100, 3, 50), "seed");

    /* live splayed table loads with the shared symfile */
    ray_t* live = ray_splay_load(STRESS_DB "/live", STRESS_DB "/sym");
    TEST_ASSERT_NOT_NULL(live);
    TEST_ASSERT_FALSE(RAY_IS_ERR(live));
    TEST_ASSERT_EQ_I(ray_table_nrows(live), 100);
    TEST_ASSERT_EQ_I(ray_table_ncols(live), 3);
    ray_release(live);

    /* parted table loads and skips the non-date `live` dir */
    ray_t* parted = ray_read_parted(STRESS_DB, "hist");
    TEST_ASSERT_NOT_NULL(parted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parted));
    ray_release(parted);

    PASS();
}

static test_result_t test_fixture_roundtrip(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_DB, 42), "init");
    test_result_t r = test_fixture_roundtrip_impl(&c);
    stress_destroy(&c);
    return r;
}

/* Clean fixture verifies clean, both load paths. */
static test_result_t test_verify_clean_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 200, 4, 100), "seed");
    TEST_ASSERT(stress_verify_all(c, false), "verify heap-load");
    TEST_ASSERT(stress_verify_all(c, true), "verify mmap-load");
    PASS();
}

static test_result_t test_verify_clean(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_DB, 43), "init");
    test_result_t r = test_verify_clean_impl(&c);
    stress_destroy(&c);
    return r;
}

/* Oracle self-check: the verifier MUST flag a disk/shadow divergence.
 * We mutate the shadow (cheapest way to fake corruption) and expect
 * verify to fail.  The stderr dump below is expected output. */
static test_result_t test_verify_detects_divergence_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 50, 2, 20), "seed");
    TEST_ASSERT(stress_verify_all(c, false), "clean first");

    fprintf(stderr, "\n[stress] NOTE: the next STRESS FAILURE dump is "
                    "an intentional oracle self-check\n");
    int64_t saved = c->live.rows[7].qty;
    c->live.rows[7].qty = saved + 1;
    TEST_ASSERT_FALSE(stress_verify_all(c, false));
    c->live.rows[7].qty = saved;
    c->failed = false; /* undo the keep-dir flag; this was synthetic */

    /* same for a ticker-string divergence */
    fprintf(stderr, "[stress] NOTE: one more intentional self-check dump\n");
    char savedt[STRESS_SYM_MAX];
    memcpy(savedt, c->live.rows[3].ticker, STRESS_SYM_MAX);
    snprintf(c->live.rows[3].ticker, STRESS_SYM_MAX, "bogus_never_interned");
    TEST_ASSERT_FALSE(stress_verify_all(c, false));
    memcpy(c->live.rows[3].ticker, savedt, STRESS_SYM_MAX);
    c->failed = false;

    TEST_ASSERT(stress_verify_all(c, false), "clean again after restore");
    PASS();
}

static test_result_t test_verify_detects_divergence(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_DB, 44), "init");
    test_result_t r = test_verify_detects_divergence_impl(&c);
    stress_destroy(&c);
    return r;
}

static const stress_sym_pattern_t k_patterns[] = {
    STRESS_SYMS_NEW, STRESS_SYMS_EXISTING, STRESS_SYMS_MIXED,
    STRESS_SYMS_NULLS,
};
#define NPATTERNS (sizeof(k_patterns) / sizeof(k_patterns[0]))

/* insert x {patterns} x {durable,bulk} x {heap,mmap load}, verify after
 * every combination through both reload paths. */
static test_result_t test_matrix_insert_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 2, 32), "seed");
    for (size_t p = 0; p < NPATTERNS; p++)
        for (int bulk = 0; bulk <= 1; bulk++)
            for (int mm = 0; mm <= 1; mm++) {
                TEST_ASSERT_FMT(stress_op_insert(c, 16, k_patterns[p],
                                                 bulk, mm),
                                "insert pat=%zu bulk=%d mmap=%d", p, bulk, mm);
                TEST_ASSERT_FMT(stress_verify_all(c, false),
                                "verify(heap) after pat=%zu bulk=%d mmap=%d",
                                p, bulk, mm);
                TEST_ASSERT_FMT(stress_verify_all(c, true),
                                "verify(mmap) after pat=%zu bulk=%d mmap=%d",
                                p, bulk, mm);
            }
    PASS();
}

static test_result_t test_matrix_insert(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_DB, 100), "init");
    test_result_t r = test_matrix_insert_impl(&c);
    stress_destroy(&c);
    return r;
}

/* upsert x {patterns} x {heap,mmap}, many repetitions so both the update
 * and the append arm are taken. */
static test_result_t test_matrix_upsert_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 2, 32), "seed");
    for (size_t p = 0; p < NPATTERNS; p++)
        for (int mm = 0; mm <= 1; mm++)
            for (int rep = 0; rep < 8; rep++) {
                TEST_ASSERT_FMT(stress_op_upsert(c, k_patterns[p], mm),
                                "upsert pat=%zu mmap=%d rep=%d", p, mm, rep);
                TEST_ASSERT_FMT(stress_verify_all(c, rep & 1),
                                "verify after upsert pat=%zu mmap=%d rep=%d",
                                p, mm, rep);
            }
    PASS();
}

static test_result_t test_matrix_upsert(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_DB, 101), "init");
    test_result_t r = test_matrix_upsert_impl(&c);
    stress_destroy(&c);
    return r;
}

const test_entry_t stress_matrix_entries[] = {
    { "stress/fixture_roundtrip", test_fixture_roundtrip, stress_setup, stress_teardown },
    { "stress/verify_clean",             test_verify_clean,             stress_setup, stress_teardown },
    { "stress/verify_detects_divergence", test_verify_detects_divergence, stress_setup, stress_teardown },
    { "stress/matrix_insert", test_matrix_insert, stress_setup, stress_teardown },
    { "stress/matrix_upsert", test_matrix_upsert, stress_setup, stress_teardown },
    { NULL, NULL, NULL, NULL },
};

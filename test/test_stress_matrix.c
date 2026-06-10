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

/* Fixture seeds, persists, and both table types load back cleanly. */
static test_result_t test_fixture_roundtrip(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_DB, 42), "init");
    TEST_ASSERT(stress_seed_initial(&c, 100, 3, 50), "seed");

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

    stress_destroy(&c);
    PASS();
}

const test_entry_t stress_matrix_entries[] = {
    { "stress/fixture_roundtrip", test_fixture_roundtrip, stress_setup, stress_teardown },
    { NULL, NULL, NULL, NULL },
};

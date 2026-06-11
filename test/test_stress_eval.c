/*
 * test_stress_eval.c — Rayfall eval-driver suite for the stress engine.
 * Same shadow/oracle/verifier as the C-driver suites (stress_store.c);
 * the real side goes through generated Rayfall via ray_eval_str.
 */

#include "test.h"
#include "stress_eval.h"
#include <rayforce.h>

#define STRESS_EVAL_DB "/tmp/rayforce_stress_eval"

/* ray_runtime_create() implies ray_heap_init() + ray_sym_init(), and
 * ray_runtime_destroy() tears both down (src/core/runtime.c) — so setup/
 * teardown pair the runtime ALONE, like test_domain.c's runtime tests. */
static void stress_eval_setup(void)    { (void)stress_eval_runtime_up(); }
static void stress_eval_teardown(void) { stress_eval_runtime_down(); }

/* Wrapper pattern (see test_stress_matrix.c): the _impl body may
 * early-return on TEST_ASSERT failure; the wrapper guarantees
 * stress_destroy (frees shadow + oplog; keeps the db dir on c->failed). */

/* eval-driven seed produces a fixture the C verifier accepts */
static test_result_t test_eval_fixture_roundtrip_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_eval_seed_initial(c, 64, 2, 32), "eval seed");
    TEST_ASSERT(stress_verify_all(c, false), "verify heap-load");
    TEST_ASSERT(stress_verify_all(c, true), "verify mmap-load");
    PASS();
}

static test_result_t test_eval_fixture_roundtrip(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_EVAL_DB, 200), "init");
    test_result_t r = test_eval_fixture_roundtrip_impl(&c);
    stress_destroy(&c);
    return r;
}

const test_entry_t stress_eval_entries[] = {
    { "stress_eval/fixture_roundtrip", test_eval_fixture_roundtrip,
      stress_eval_setup, stress_eval_teardown },
    { NULL, NULL, NULL, NULL },
};

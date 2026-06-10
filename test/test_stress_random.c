/*
 * test_stress_random.c — seeded random op sequences against the
 * parted/splayed shared-symfile fixture (engine: stress_store.c).
 *
 * CI default: 3 seeds x 500 ops (seconds).  Env overrides:
 *   RAY_STRESS_SEED=<n>   run only this seed
 *   RAY_STRESS_ITERS=<n>  ops per seed (soak: 50000+)
 *
 * Reproduce any reported failure with:
 *   RAY_STRESS_SEED=<seed> RAY_STRESS_ITERS=<n> ./rayforce.test -f stress/random
 */

#include "test.h"
#include "stress_store.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include <stdio.h>
#include <stdlib.h>

#define STRESS_RND_DB     "/tmp/rayforce_stress_random"
#define DEFAULT_ITERS     500
#define FULL_VERIFY_EVERY 25

static void stress_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void stress_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

static int64_t env_i64(const char* name, int64_t dflt) {
    const char* v = getenv(name);
    if (!v || !*v) return dflt;
    return strtoll(v, NULL, 10);
}

/* One random op.  Weights favor the mixed-producer interleaving on the
 * shared symfile (the prod risk surface): writes to live and to
 * partitions alternate freely, restarts are sprinkled in. */
static bool random_op(stress_ctx_t* c) {
    stress_sym_pattern_t pat =
        (stress_sym_pattern_t)(stress_rand(c) % 4); /* enum order matches */
    uint64_t roll = stress_rand(c) % 100;
    if (roll < 30)
        return stress_op_insert(c, 1 + (int64_t)(stress_rand(c) % 32), pat,
                                stress_rand(c) & 1, stress_rand(c) & 1);
    if (roll < 50)
        return stress_op_upsert(c, pat, stress_rand(c) & 1);
    if (roll < 65)
        return stress_op_part_append(
            c, (int)(stress_rand(c) % (uint64_t)c->nparts),
            1 + (int64_t)(stress_rand(c) % 16), pat);
    if (roll < 75) {
        int target = (int)(stress_rand(c) % (uint64_t)(c->nparts + 1)) - 1;
        return stress_op_trim(c, target, stress_rand(c) & 1,
                              1 + (int64_t)(stress_rand(c) % 8));
    }
    if (roll < 85)
        return stress_op_part_new(c, 1 + (int64_t)(stress_rand(c) % 16), pat);
    return stress_op_restart(c);
}

static test_result_t run_seed(uint64_t seed) {
    int64_t iters = env_i64("RAY_STRESS_ITERS", DEFAULT_ITERS);
    stress_ctx_t c;
    if (!stress_init(&c, STRESS_RND_DB, seed)) FAILF("init failed");
    if (!stress_seed_initial(&c, 64, 3, 32)) {
        stress_destroy(&c);
        FAILF("seed_initial failed (seed=%llu)", (unsigned long long)seed);
    }
    for (int64_t i = 0; i < iters; i++) {
        if (!random_op(&c)) {
            stress_destroy(&c); /* keeps dir if verify flagged it */
            FAILF("op %lld errored (seed=%llu) — see op log above",
                  (long long)i, (unsigned long long)seed);
        }
        if (!stress_check_invariants(&c)) {
            stress_destroy(&c);
            FAILF("invariant broken at op %lld (seed=%llu)", (long long)i,
                  (unsigned long long)seed);
        }
        if (i % FULL_VERIFY_EVERY == FULL_VERIFY_EVERY - 1 &&
            !stress_verify_all(&c, (i / FULL_VERIFY_EVERY) & 1)) {
            stress_destroy(&c);
            FAILF("verify failed at op %lld (seed=%llu)", (long long)i,
                  (unsigned long long)seed);
        }
    }
    bool ok = stress_verify_all(&c, false) && stress_verify_all(&c, true);
    stress_destroy(&c);
    if (!ok) FAILF("final verify failed (seed=%llu)",
                   (unsigned long long)seed);
    PASS();
}

static test_result_t test_random_seed1(void) {
    int64_t s = env_i64("RAY_STRESS_SEED", 0);
    return run_seed(s ? (uint64_t)s : 0x5EEDu);
}

static test_result_t test_random_seed2(void) {
    if (env_i64("RAY_STRESS_SEED", 0)) SKIP("RAY_STRESS_SEED pins seed1");
    return run_seed(0xC0FFEEu);
}

static test_result_t test_random_seed3(void) {
    if (env_i64("RAY_STRESS_SEED", 0)) SKIP("RAY_STRESS_SEED pins seed1");
    return run_seed(0xDEADBEAFu);
}

const test_entry_t stress_random_entries[] = {
    { "stress/random_seed1", test_random_seed1, stress_setup, stress_teardown },
    { "stress/random_seed2", test_random_seed2, stress_setup, stress_teardown },
    { "stress/random_seed3", test_random_seed3, stress_setup, stress_teardown },
    { NULL, NULL, NULL, NULL },
};

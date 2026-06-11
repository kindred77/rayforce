/*
 * test_stress_random.c — seeded random op sequences against the
 * parted/splayed shared-symfile fixture (engine: stress_store.c).
 *
 * CI default: 3 seeds x 500 ops (seconds).  Env overrides:
 *   RAY_STRESS_SEED=<n>   run only this seed
 *   RAY_STRESS_ITERS=<n>  ops per seed (soak: 50000+)
 *   RAY_STRESS_EVAL=1     drive every op through the Rayfall executors
 *                         (stress_eval.c) instead of the C-API ones; the
 *                         full-verify cadence then also runs the query
 *                         oracle.  Eval is ~10x slower than the C driver —
 *                         the CI-budget soak form is
 *                         RAY_STRESS_EVAL=1 RAY_STRESS_ITERS=200.
 *
 * Reproduce any reported failure with the SAME env (the two modes draw
 * from the rng differently, so a seed replays only within its mode):
 *   [RAY_STRESS_EVAL=1] RAY_STRESS_SEED=<seed> RAY_STRESS_ITERS=<n> \
 *       ./rayforce.test -f stress/random
 */

#include "test.h"
#include "stress_eval.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include <stdio.h>
#include <stdlib.h>

#define STRESS_RND_DB     "/tmp/rayforce_stress_random"
#define DEFAULT_ITERS     500
#define FULL_VERIFY_EVERY 25

static int64_t env_i64(const char* name, int64_t dflt) {
    const char* v = getenv(name);
    if (!v || !*v) return dflt;
    return strtoll(v, NULL, 10);
}

static bool eval_mode(void) { return env_i64("RAY_STRESS_EVAL", 0) != 0; }

/* One setup/teardown pair serves both modes.  C mode pairs heap+sym alone
 * (the C executors and the C restart op own the global sym table).  Eval
 * mode needs a live runtime instead — ray_runtime_create() itself runs
 * ray_heap_init()+ray_sym_init() and destroy tears both down
 * (src/core/runtime.c), so the runtime is paired ALONE; the C-mode restart
 * op (sym destroy/init/load underneath a live runtime) never runs in eval
 * mode — random_op dispatches to stress_eval_op_restart, a full runtime
 * recreate. */
static void stress_setup(void) {
    if (eval_mode()) {
        (void)stress_eval_runtime_up();
        return;
    }
    ray_heap_init();
    (void)ray_sym_init();
}

static void stress_teardown(void) {
    if (eval_mode()) {
        stress_eval_runtime_down();
        return;
    }
    ray_sym_destroy();
    ray_heap_destroy();
}

/* One random op.  Weights favor the mixed-producer interleaving on the
 * shared symfile (the prod risk surface): writes to live and to
 * partitions alternate freely, restarts are sprinkled in.
 *
 * RAY_STRESS_EVAL=1 dispatches each arm to the Rayfall executor with the
 * SAME roll table and weights.  The eval executors have no bulk/mmap
 * flavors, so they draw fewer rng values per op than the C arms —
 * cross-mode sequences are NOT aligned and don't need to be (determinism
 * is per mode; cross-driver semantics are pinned by
 * stress_eval/equiv_with_c_driver).  The C arms are textually untouched so
 * C-mode draw order (incl. unspecified argument-evaluation order inside
 * one call) is byte-identical to before this mode existed. */
static bool random_op(stress_ctx_t* c, bool ev) {
    stress_sym_pattern_t pat =
        (stress_sym_pattern_t)(stress_rand(c) % 4); /* enum order matches */
    uint64_t roll = stress_rand(c) % 100;
    if (roll < 30) {
        if (ev)
            return stress_eval_op_insert(
                c, 1 + (int64_t)(stress_rand(c) % 32), pat);
        return stress_op_insert(c, 1 + (int64_t)(stress_rand(c) % 32), pat,
                                stress_rand(c) & 1, stress_rand(c) & 1);
    }
    if (roll < 50) {
        if (ev) return stress_eval_op_upsert(c, pat);
        return stress_op_upsert(c, pat, stress_rand(c) & 1);
    }
    if (roll < 65) {
        if (c->nparts == 0) { /* no partitions yet: redirect to live insert */
            if (ev)
                return stress_eval_op_insert(
                    c, 1 + (int64_t)(stress_rand(c) % 16), pat);
            return stress_op_insert(c, 1 + (int64_t)(stress_rand(c) % 16),
                                    pat, false, false);
        }
        if (ev)
            return stress_eval_op_part_append(
                c, (int)(stress_rand(c) % (uint64_t)c->nparts),
                1 + (int64_t)(stress_rand(c) % 16), pat);
        return stress_op_part_append(
            c, (int)(stress_rand(c) % (uint64_t)c->nparts),
            1 + (int64_t)(stress_rand(c) % 16), pat);
    }
    if (roll < 75) {
        int target = (int)(stress_rand(c) % (uint64_t)(c->nparts + 1)) - 1;
        if (ev)
            return stress_eval_op_trim(c, target, stress_rand(c) & 1,
                                       1 + (int64_t)(stress_rand(c) % 8));
        return stress_op_trim(c, target, stress_rand(c) & 1,
                              1 + (int64_t)(stress_rand(c) % 8));
    }
    if (roll < 85) {
        if (ev)
            return stress_eval_op_part_new(
                c, 1 + (int64_t)(stress_rand(c) % 16), pat);
        return stress_op_part_new(c, 1 + (int64_t)(stress_rand(c) % 16), pat);
    }
    return ev ? stress_eval_op_restart(c) : stress_op_restart(c);
}

static test_result_t run_seed(uint64_t seed) {
    int64_t iters = env_i64("RAY_STRESS_ITERS", DEFAULT_ITERS);
    bool    ev    = eval_mode(); /* read once; fixed for the whole run */
    stress_ctx_t c;
    if (!stress_init(&c, STRESS_RND_DB, seed)) FAILF("init failed");
    if (!(ev ? stress_eval_seed_initial(&c, 64, 3, 32)
             : stress_seed_initial(&c, 64, 3, 32))) {
        stress_destroy(&c);
        FAILF("seed_initial failed (seed=%llu)", (unsigned long long)seed);
    }
    for (int64_t i = 0; i < iters; i++) {
        if (!random_op(&c, ev)) {
            stress_destroy(&c); /* keeps dir if verify flagged it */
            FAILF("op %lld errored (seed=%llu) — see op log above",
                  (long long)i, (unsigned long long)seed);
        }
        if (!stress_check_invariants(&c)) {
            stress_destroy(&c);
            FAILF("invariant broken at op %lld (seed=%llu)", (long long)i,
                  (unsigned long long)seed);
        }
        if (i % FULL_VERIFY_EVERY == FULL_VERIFY_EVERY - 1) {
            if (!stress_verify_all(&c, (i / FULL_VERIFY_EVERY) & 1)) {
                stress_destroy(&c);
                FAILF("verify failed at op %lld (seed=%llu)", (long long)i,
                      (unsigned long long)seed);
            }
            /* eval mode: the consumer sweep — shadow-computed query
             * expectations vs the language over the persisted fixture
             * (draws from the ctx rng; cadence is deterministic, so
             * per-mode replay holds) */
            if (ev && !stress_eval_verify_queries(&c)) {
                stress_destroy(&c);
                FAILF("query oracle failed at op %lld (seed=%llu)",
                      (long long)i, (unsigned long long)seed);
            }
        }
    }
    bool ok = stress_verify_all(&c, false) && stress_verify_all(&c, true) &&
              (!ev || stress_eval_verify_queries(&c));
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

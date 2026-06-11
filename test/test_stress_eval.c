/*
 * test_stress_eval.c — Rayfall eval-driver suite for the stress engine.
 * Same shadow/oracle/verifier as the C-driver suites (stress_store.c);
 * the real side goes through generated Rayfall via ray_eval_str.
 */

#include "test.h"
#include "stress_eval.h"
#include <rayforce.h>
#include <math.h>
#include <string.h>

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

/* ---- matrix: every eval op kind x {NEW, MIXED, NULLS} ------------------- */

static test_result_t test_eval_matrix_ops_impl(stress_ctx_t* c) {
    static const stress_sym_pattern_t pats[] = {
        STRESS_SYMS_NEW, STRESS_SYMS_MIXED, STRESS_SYMS_NULLS,
    };
    TEST_ASSERT(stress_eval_seed_initial(c, 48, 2, 24), "seed");

#define VERIFY_BOTH(what, pidx)                                               \
    do {                                                                      \
        TEST_ASSERT_FMT(stress_verify_all(c, false), "verify(heap) %s p=%d", \
                        what, (int)(pidx));                                   \
        TEST_ASSERT_FMT(stress_verify_all(c, true), "verify(mmap) %s p=%d",  \
                        what, (int)(pidx));                                   \
        TEST_ASSERT_FMT(stress_eval_verify_queries(c),                        \
                        "query oracle %s p=%d", what, (int)(pidx));           \
    } while (0)

    VERIFY_BOTH("after seed", -1);
    for (size_t p = 0; p < sizeof(pats) / sizeof(pats[0]); p++) {
        stress_sym_pattern_t pat = pats[p];
        TEST_ASSERT_FMT(stress_eval_op_insert(c, 12, pat), "insert p=%zu", p);
        VERIFY_BOTH("after insert", p);

        TEST_ASSERT_FMT(stress_eval_op_upsert(c, pat), "upsert p=%zu", p);
        VERIFY_BOTH("after upsert", p);

        /* trim has no sym pattern; vary target (live vs part) and end */
        TEST_ASSERT_FMT(stress_eval_op_trim(c, p == 1 ? 0 : -1, p > 0, 3),
                        "trim p=%zu", p);
        VERIFY_BOTH("after trim", p);

        TEST_ASSERT_FMT(stress_eval_op_part_append(c, (int)(p % 2), 8, pat),
                        "part_append p=%zu", p);
        VERIFY_BOTH("after part_append", p);

        TEST_ASSERT_FMT(stress_eval_op_part_new(c, 6, pat), "part_new p=%zu",
                        p);
        VERIFY_BOTH("after part_new", p);

        /* restart mid-sequence: full runtime recreate, disk = only truth */
        if (p == 0) {
            TEST_ASSERT(stress_eval_op_restart(c), "restart mid-sequence");
            VERIFY_BOTH("post-restart(mid)", p);
        }
    }
    TEST_ASSERT(stress_eval_op_restart(c), "restart at end");
    VERIFY_BOTH("post-restart(end)", -2);
#undef VERIFY_BOTH
    PASS();
}

static test_result_t test_eval_matrix_ops(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_EVAL_DB, 201), "init");
    test_result_t r = test_eval_matrix_ops_impl(&c);
    stress_destroy(&c);
    return r;
}

/* ---- query-oracle self-check: a shadow/disk divergence MUST fail ---------- */

/* Clean fixture passes the query oracle; corrupting ONE shadow qty under
 * the ticker the oracle's (a) query will pick must make it FAIL (the sum
 * diverges while per-row count does not — pins that the oracle compares
 * VALUES, not just shapes).  The stderr dump is expected output, phase-1
 * style. */
static test_result_t test_eval_query_oracle_detects_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_eval_seed_initial(c, 64, 2, 24), "seed");
    TEST_ASSERT(stress_eval_verify_queries(c), "clean queries first");

    /* Peek which pooled ticker the NEXT oracle call picks: query (a)
     * consumes exactly one stress_rand, so draw it, then restore the rng
     * so the oracle re-draws the same value. */
    uint64_t saved_rng = c->rng;
    uint64_t draw      = stress_rand(c);
    c->rng             = saved_rng;
    int span = c->pool_len < STRESS_POOL_CAP ? c->pool_len : STRESS_POOL_CAP;
    const char* tkr = c->pool[draw % (uint64_t)span];
    int64_t     hit = stress_find_first_by_ticker(&c->live, tkr);
    /* seed 203 + (64,2,24) MIXED seeding makes the picked ticker live;
     * if this trips after an engine change, adjust seed/sizes */
    TEST_ASSERT_FMT(hit >= 0, "picked ticker '%s' has live rows", tkr);

    fprintf(stderr, "\n[stress] NOTE: the next STRESS FAILURE dump is an "
                    "intentional query-oracle self-check\n");
    int64_t saved_qty = c->live.rows[hit].qty;
    c->live.rows[hit].qty = saved_qty + 7; /* qty < 100000: no overflow */
    TEST_ASSERT_FALSE(stress_eval_verify_queries(c));
    c->live.rows[hit].qty = saved_qty;
    c->failed = false; /* undo the keep-dir flag; this was synthetic */

    TEST_ASSERT(stress_eval_verify_queries(c), "clean again after restore");
    PASS();
}

static test_result_t test_eval_query_oracle_detects(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, STRESS_EVAL_DB, 203), "init");
    test_result_t r = test_eval_query_oracle_detects_impl(&c);
    stress_destroy(&c);
    return r;
}

/* ---- cross-driver equivalence: C executors vs eval executors ------------- */

/* Same seed + same op sequence through both drivers (separate fixtures);
 * because both consume the ctx rng identically and share the shadow logic,
 * the resulting shadows must be bit-identical and both fixtures must
 * verify.  This pins the two drivers to one semantics (esp. upsert
 * first-match and the float text-round-trip discipline). */

static bool eq_run_sequence(stress_ctx_t* c, bool eval) {
    if (eval ? !stress_eval_seed_initial(c, 32, 2, 16)
             : !stress_seed_initial(c, 32, 2, 16))
        return false;
    if (eval ? !stress_eval_op_insert(c, 10, STRESS_SYMS_MIXED)
             : !stress_op_insert(c, 10, STRESS_SYMS_MIXED, false, false))
        return false;
    if (eval ? !stress_eval_op_upsert(c, STRESS_SYMS_MIXED)
             : !stress_op_upsert(c, STRESS_SYMS_MIXED, false))
        return false;
    /* an upsert drawn from NULLS so the "" key path runs in both drivers */
    if (eval ? !stress_eval_op_upsert(c, STRESS_SYMS_NULLS)
             : !stress_op_upsert(c, STRESS_SYMS_NULLS, false))
        return false;
    if (eval ? !stress_eval_op_trim(c, -1, true, 4)
             : !stress_op_trim(c, -1, true, 4))
        return false;
    if (eval ? !stress_eval_op_trim(c, 0, false, 3)
             : !stress_op_trim(c, 0, false, 3))
        return false;
    if (eval ? !stress_eval_op_part_append(c, 1, 6, STRESS_SYMS_NULLS)
             : !stress_op_part_append(c, 1, 6, STRESS_SYMS_NULLS))
        return false;
    if (eval ? !stress_eval_op_restart(c) : !stress_op_restart(c))
        return false;
    if (eval ? !stress_eval_op_insert(c, 8, STRESS_SYMS_NEW)
             : !stress_op_insert(c, 8, STRESS_SYMS_NEW, false, false))
        return false;
    return true;
}

/* Cell-by-cell shadow compare; dumps the first divergence through ce's
 * failure dump and marks BOTH ctxs failed so both db dirs survive. */
static bool eq_shadows_equal(stress_ctx_t* cc, stress_ctx_t* ce,
                             const stress_rows_t* a, const stress_rows_t* b,
                             const char* what) {
    if (a->len != b->len) {
        cc->failed = true;
        stress_dump_failure(ce, "equiv %s: C rows %lld != eval rows %lld",
                            what, (long long)a->len, (long long)b->len);
        return false;
    }
    for (int64_t i = 0; i < a->len; i++) {
        const stress_row_t* ra = &a->rows[i];
        const stress_row_t* rb = &b->rows[i];
        bool price_eq = (ra->price == rb->price) ||
                        (isnan(ra->price) && isnan(rb->price));
        if (strcmp(ra->ticker, rb->ticker) != 0 || !price_eq ||
            ra->qty != rb->qty) {
            cc->failed = true;
            stress_dump_failure(
                ce, "equiv %s row %lld: C ('%s' %g %lld) != eval ('%s' %g %lld)",
                what, (long long)i, ra->ticker, ra->price, (long long)ra->qty,
                rb->ticker, rb->price, (long long)rb->qty);
            return false;
        }
    }
    return true;
}

static test_result_t test_eval_equiv_impl(stress_ctx_t* cc, stress_ctx_t* ce) {
    /* Eval sequence FIRST: the C driver's restart op swaps the global sym
     * table (destroy + init + load of ITS fixture's symfile) underneath
     * the live runtime, so no eval may run after it — prelude bindings
     * were registered under the old table's ids.  The C-API-only sequence
     * and verifies are immune. */
    TEST_ASSERT(eq_run_sequence(ce, true), "eval-driver sequence");
    TEST_ASSERT(eq_run_sequence(cc, false), "C-driver sequence");

    TEST_ASSERT_EQ_I(ce->nparts, cc->nparts);
    TEST_ASSERT(eq_shadows_equal(cc, ce, &cc->live, &ce->live, "live"),
                "live shadows identical");
    for (int p = 0; p < cc->nparts; p++) {
        TEST_ASSERT_FMT(strcmp(cc->part_dates[p], ce->part_dates[p]) == 0,
                        "part date %d", p);
        TEST_ASSERT_FMT(eq_shadows_equal(cc, ce, &cc->parts[p], &ce->parts[p],
                                         "part"),
                        "part %d shadows identical", p);
    }

    TEST_ASSERT(stress_verify_all(cc, false), "C fixture verify(heap)");
    TEST_ASSERT(stress_verify_all(cc, true), "C fixture verify(mmap)");
    TEST_ASSERT(stress_verify_all(ce, false), "eval fixture verify(heap)");
    TEST_ASSERT(stress_verify_all(ce, true), "eval fixture verify(mmap)");
    PASS();
}

static test_result_t test_eval_equiv_with_c_driver(void) {
    stress_ctx_t cc, ce;
    TEST_ASSERT(stress_init(&cc, STRESS_EVAL_DB "_eqc", 202), "init C fixture");
    if (!stress_init(&ce, STRESS_EVAL_DB "_eqe", 202)) {
        stress_destroy(&cc);
        TEST_ASSERT(false, "init eval fixture");
    }
    test_result_t r = test_eval_equiv_impl(&cc, &ce);
    stress_destroy(&cc);
    stress_destroy(&ce);
    return r;
}

const test_entry_t stress_eval_entries[] = {
    { "stress_eval/fixture_roundtrip", test_eval_fixture_roundtrip,
      stress_eval_setup, stress_eval_teardown },
    { "stress_eval/matrix_ops", test_eval_matrix_ops,
      stress_eval_setup, stress_eval_teardown },
    { "stress_eval/query_oracle_detects", test_eval_query_oracle_detects,
      stress_eval_setup, stress_eval_teardown },
    { "stress_eval/equiv_with_c_driver", test_eval_equiv_with_c_driver,
      stress_eval_setup, stress_eval_teardown },
    { NULL, NULL, NULL, NULL },
};

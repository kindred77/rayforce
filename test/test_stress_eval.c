/*
 * test_stress_eval.c — Rayfall eval-driver suite for the stress engine.
 * Same shadow/oracle/verifier as the C-driver suites (stress_store.c);
 * the real side goes through generated Rayfall via ray_eval_str.
 */

#include "test.h"
#include "stress_eval.h"
#include <rayforce.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


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
    TEST_ASSERT(stress_init(&c, stress_db_path("eval"), 200), "init");
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
    TEST_ASSERT(stress_init(&c, stress_db_path("eval"), 201), "init");
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
    TEST_ASSERT(stress_init(&c, stress_db_path("eval"), 203), "init");
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
    TEST_ASSERT(stress_init(&cc, stress_db_path("eval_eqc"), 202), "init C fixture");
    if (!stress_init(&ce, stress_db_path("eval_eqe"), 202)) {
        stress_destroy(&cc);
        TEST_ASSERT(false, "init eval fixture");
    }
    test_result_t r = test_eval_equiv_impl(&cc, &ce);
    stress_destroy(&cc);
    stress_destroy(&ce);
    return r;
}

/* ---- client layout: the full prod mirror, eval-only ---------------------- */

/* The client's exact shape — date-parted hist + splayed live on ONE shared
 * root symfile — driven end-to-end through the language: seeded hist
 * partitions, then rounds of the live/hist producer interleaving with the
 * query oracle and both C loaders checked after every round, and two
 * full-runtime-recreate restarts (one mid-stream, one before the final
 * verify) so disk is the only carrier of truth across them. */
static test_result_t test_eval_client_layout_impl(stress_ctx_t* c) {
    static const stress_sym_pattern_t pats[3] = {
        STRESS_SYMS_MIXED, STRESS_SYMS_NULLS, STRESS_SYMS_NEW,
    };
    TEST_ASSERT(stress_eval_seed_initial(c, 96, 3, 48), "seed");
    TEST_ASSERT(stress_verify_all(c, false), "verify after seed");
    TEST_ASSERT(stress_eval_verify_queries(c), "queries after seed");

    for (int r = 0; r < 3; r++) {
        stress_sym_pattern_t pat = pats[r];
        TEST_ASSERT_FMT(stress_eval_op_insert(c, 16, pat), "insert r=%d", r);
        TEST_ASSERT_FMT(stress_eval_op_part_append(c, r % c->nparts, 10, pat),
                        "part_append r=%d", r);
        TEST_ASSERT_FMT(stress_eval_op_upsert(c, pat), "upsert r=%d", r);
        TEST_ASSERT_FMT(stress_eval_op_part_new(c, 8, pat), "part_new r=%d",
                        r);
        TEST_ASSERT_FMT(stress_eval_op_trim(c, -1, r & 1, 4), "trim r=%d", r);

        TEST_ASSERT_FMT(stress_verify_all(c, false), "verify(heap) r=%d", r);
        TEST_ASSERT_FMT(stress_verify_all(c, true), "verify(mmap) r=%d", r);
        TEST_ASSERT_FMT(stress_eval_verify_queries(c), "queries r=%d", r);

        if (r == 1) { /* mid-stream restart, then prove disk carried it */
            TEST_ASSERT(stress_eval_op_restart(c), "mid restart");
            TEST_ASSERT(stress_verify_all(c, false),
                        "verify after mid restart");
            TEST_ASSERT(stress_eval_verify_queries(c),
                        "queries after mid restart");
        }
    }

    TEST_ASSERT(stress_eval_op_restart(c), "end restart");
    TEST_ASSERT(stress_verify_all(c, false), "final verify(heap)");
    TEST_ASSERT(stress_verify_all(c, true), "final verify(mmap)");
    TEST_ASSERT(stress_eval_verify_queries(c), "final queries");
    PASS();
}

static test_result_t test_eval_client_layout(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("eval"), 204), "init");
    test_result_t r = test_eval_client_layout_impl(&c);
    stress_destroy(&c);
    return r;
}

/* ---- determinism: same seed twice => identical op logs + shadows ---------- */

/* A miniature random runner over the eval executors: op choice and every
 * op parameter come from the ctx rng, the query oracle (which also draws
 * one rng value per call) runs at a cadence, and restarts are in the mix —
 * the same shape as the random runner's eval mode.  Draws are hoisted into
 * locals so the consumption ORDER is specified by the C source, not by the
 * compiler's argument evaluation order. */
static bool det_run(stress_ctx_t* c) {
    if (!stress_eval_seed_initial(c, 32, 2, 16)) return false;
    for (int i = 0; i < 32; i++) {
        stress_sym_pattern_t pat =
            (stress_sym_pattern_t)(stress_rand(c) % 4);
        uint64_t roll = stress_rand(c) % 100;
        bool     ok;
        if (roll < 35) {
            int64_t n = 1 + (int64_t)(stress_rand(c) % 8);
            ok = stress_eval_op_insert(c, n, pat);
        } else if (roll < 55) {
            ok = stress_eval_op_upsert(c, pat);
        } else if (roll < 70) {
            int     pi = (int)(stress_rand(c) % (uint64_t)c->nparts);
            int64_t n  = 1 + (int64_t)(stress_rand(c) % 8);
            ok = stress_eval_op_part_append(c, pi, n, pat);
        } else if (roll < 80) {
            int target = (int)(stress_rand(c) % (uint64_t)(c->nparts + 1)) - 1;
            bool    tail = stress_rand(c) & 1;
            int64_t n    = 1 + (int64_t)(stress_rand(c) % 4);
            ok = stress_eval_op_trim(c, target, tail, n);
        } else if (roll < 90) {
            int64_t n = 1 + (int64_t)(stress_rand(c) % 8);
            ok = stress_eval_op_part_new(c, n, pat);
        } else {
            ok = stress_eval_op_restart(c);
        }
        if (!ok) return false;
        if (i % 8 == 7 && !stress_eval_verify_queries(c)) return false;
    }
    return stress_verify_all(c, false);
}

/* Deep snapshot of run 1's observable trace (op log ring + shadows), taken
 * BEFORE stress_destroy frees them.  Both runs use the SAME db path (the
 * second stress_init rm-rfs it), so the generated source embedded in the
 * op log is byte-comparable. */
typedef struct {
    char (*oplog)[128];
    int   oplog_len;
    stress_rows_t live;
    stress_rows_t parts[STRESS_MAX_PARTS];
    char  part_dates[STRESS_MAX_PARTS][16];
    int   nparts;
} det_snap_t;

static bool det_rows_copy(stress_rows_t* dst, const stress_rows_t* src) {
    dst->rows = NULL;
    dst->len = dst->cap = src->len;
    if (src->len == 0) return true;
    dst->rows = (stress_row_t*)malloc((size_t)src->len * sizeof(stress_row_t));
    if (!dst->rows) return false;
    memcpy(dst->rows, src->rows, (size_t)src->len * sizeof(stress_row_t));
    return true;
}

static bool det_snapshot(const stress_ctx_t* c, det_snap_t* s) {
    s->oplog = (char(*)[128])malloc((size_t)STRESS_OPLOG_CAP * 128);
    if (!s->oplog) return false;
    memcpy(s->oplog, c->oplog, (size_t)STRESS_OPLOG_CAP * 128);
    s->oplog_len = c->oplog_len;
    s->nparts    = c->nparts;
    memcpy(s->part_dates, c->part_dates, sizeof(s->part_dates));
    if (!det_rows_copy(&s->live, &c->live)) return false;
    for (int p = 0; p < c->nparts; p++)
        if (!det_rows_copy(&s->parts[p], &c->parts[p])) return false;
    return true;
}

static void det_snap_free(det_snap_t* s) {
    free(s->oplog);
    free(s->live.rows);
    for (int p = 0; p < s->nparts; p++) free(s->parts[p].rows);
    memset(s, 0, sizeof(*s));
}

/* bounded strlen (strnlen is POSIX-gated under this build's std mode) */
static size_t det_slot_len(const char* p) {
    size_t n = 0;
    while (n < 128 && p[n]) n++;
    return n;
}

/* Used-slot compare: lengths, then memcmp of each used ring slot over its
 * string (strlen+1) — bytes past the NUL are malloc garbage in both
 * buffers, never written by op_logf, so they are excluded. */
static bool det_logs_equal(stress_ctx_t* c2, const det_snap_t* s) {
    if (s->oplog_len != c2->oplog_len) {
        stress_dump_failure(c2, "determinism: op log length %d != run 1's %d",
                            c2->oplog_len, s->oplog_len);
        return false;
    }
    int total = c2->oplog_len;
    int n     = total < STRESS_OPLOG_CAP ? total : STRESS_OPLOG_CAP;
    for (int i = total - n; i < total; i++) {
        const char* a = s->oplog[i % STRESS_OPLOG_CAP];
        const char* b = c2->oplog[i % STRESS_OPLOG_CAP];
        size_t la = det_slot_len(a);
        if (det_slot_len(b) != la || memcmp(a, b, la) != 0) {
            stress_dump_failure(c2, "determinism: op %d diverged\n"
                                    "  run1: %.*s\n  run2: %.*s",
                                i, (int)la, a, (int)det_slot_len(b), b);
            return false;
        }
    }
    return true;
}

static test_result_t test_eval_determinism_impl(stress_ctx_t* c2,
                                                const det_snap_t* s) {
    TEST_ASSERT(det_run(c2), "run 2");
    TEST_ASSERT(det_logs_equal(c2, s), "op logs identical");
    TEST_ASSERT_EQ_I(c2->nparts, s->nparts);
    TEST_ASSERT(eq_shadows_equal(c2, c2, &s->live, &c2->live, "det live"),
                "live shadows identical");
    for (int p = 0; p < s->nparts; p++) {
        TEST_ASSERT_FMT(strcmp(s->part_dates[p], c2->part_dates[p]) == 0,
                        "part date %d", p);
        TEST_ASSERT_FMT(eq_shadows_equal(c2, c2, &s->parts[p], &c2->parts[p],
                                         "det part"),
                        "part %d shadows identical", p);
    }
    PASS();
}

static test_result_t test_eval_determinism(void) {
    det_snap_t s;
    memset(&s, 0, sizeof(s));

    stress_ctx_t c1;
    TEST_ASSERT(stress_init(&c1, stress_db_path("eval"), 205), "init run 1");
    bool ok1 = det_run(&c1) && !c1.failed && det_snapshot(&c1, &s);
    stress_destroy(&c1);
    if (!ok1) {
        det_snap_free(&s);
        TEST_ASSERT(false, "run 1 + snapshot");
    }

    /* fresh runtime between runs: run 2 must not inherit run 1's interned
     * vocabulary/env, exactly like the suite's own setup gave run 1 */
    stress_eval_runtime_down();
    if (!stress_eval_runtime_up()) {
        det_snap_free(&s);
        TEST_ASSERT(false, "runtime recreate between runs");
    }

    stress_ctx_t c2;
    if (!stress_init(&c2, stress_db_path("eval"), 205)) {
        det_snap_free(&s);
        TEST_ASSERT(false, "init run 2");
    }
    test_result_t r = test_eval_determinism_impl(&c2, &s);
    stress_destroy(&c2);
    det_snap_free(&s);
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
    { "stress_eval/client_layout", test_eval_client_layout,
      stress_eval_setup, stress_eval_teardown },
    { "stress_eval/determinism", test_eval_determinism,
      stress_eval_setup, stress_eval_teardown },
    { NULL, NULL, NULL, NULL },
};

/* Join dup-fallback perf gate.
 *
 * The radix per-partition open-addressing build trips RADIX_DUP_RUN_MAX (512)
 * when a linear-probe run grows too long, sets the `pathological` flag, and
 * falls back to the chained-HT path (O(n) build).  Without that trip the build
 * is O(dup²) on a pathologically duplicated build side.
 *
 * This gate measures post-fix (auto-fallback ON) vs pre-fix (auto-fallback
 * DISABLED via the ray_join_no_dup_fallback bypass knob) in one binary.
 *
 * Cases:
 *   CATASTROPHIC-INNER  right=10M all key 7, left=10K all key 7, INNER,
 *                       ray_join_no_build_swap=true → build the dup'd 10M right.
 *                       post-fix trips → chained; pre-fix runs O(dup²).  Headline.
 *   CATASTROPHIC-LEFT   right=10M all key 7, left=100K, LEFT join (build=right
 *                       always, no swap).  New-coverage headline.
 *   ZERO-REGRESSION     right=10M key i (unique), left=10K, INNER → never trips.
 *                       post-fix ≈ pre-fix (the added ++run/branch costs ~0).
 *                       Counter must stay UNCHANGED.  THE regression check.
 *   MODERATE-DUP        right=10M key i%100000 (~100/key) build side
 *                       (ray_join_no_build_swap=true), left=10K, INNER.
 *                       Must NOT trip prematurely (counter unchanged) and stay
 *                       radix (post-fix ≈ pre-fix).  If it trips at ~100/key,
 *                       that is a FINDING (threshold may be too low).
 *   MODERATE-DUP-10     same but right key i%1000000 (~10/key), cleaner moderate.
 *
 * Mechanism: assert ray_join_dup_fallbacks advances on the catastrophic cases;
 * for the control + moderate cases a trip is reported, not fatal (moderate trip
 * is a tuning finding).
 *
 * Timing: CLOCK_MONOTONIC around ray_execute only.  Tables built once outside
 * the timed loop; graph rebuilt per rep.  The pre-fix catastrophic build is
 * SLOW (~seconds), so pre-fix reps on catastrophic cases are capped (PREFIX_SLOW
 * _REPS); medians of fewer reps, noted in output.
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "table/sym.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- timing ---------- */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* ---------- median/min (qsort on small N) ---------- */
static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static double medianN(double arr[], int n) {
    double tmp[64];
    memcpy(tmp, arr, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), cmp_double);
    return tmp[n / 2];
}
static double minN(double arr[], int n) {
    double m = arr[0];
    for (int i = 1; i < n; i++) if (arr[i] < m) m = arr[i];
    return m;
}

/* ---------- build a single-column I64 table ---------- */
static ray_t* make_table1(const char* name, const int64_t* vals, int64_t n) {
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, n);
    if (!col || RAY_IS_ERR(col)) {
        fprintf(stderr, "make_table1: ray_vec_from_raw failed (%s, n=%lld)\n",
                name, (long long)n);
        abort();
    }
    ray_t* tbl = ray_table_new(1);
    int64_t sym = ray_sym_intern(name, strlen(name));
    tbl = ray_table_add_col(tbl, sym, col);
    ray_release(col);
    if (!tbl || RAY_IS_ERR(tbl)) {
        fprintf(stderr, "make_table1: table_add_col failed (%s)\n", name);
        abort();
    }
    return tbl;
}

/* ---------- run one join rep, return exec wall ms + output rows ---------- */
static double run_join_rep(ray_t* lt, ray_t* rt, int join_type, int64_t* rows_out) {
    ray_graph_t* g = ray_graph_new(lt);
    if (!g) { fprintf(stderr, "run_join_rep: graph alloc\n"); abort(); }

    ray_op_t* lt_node = ray_const_table(g, lt);
    ray_op_t* rt_node = ray_const_table(g, rt);
    ray_op_t* lk_op   = ray_scan(g, "lk");
    ray_op_t* rk_op   = ray_scan(g, "rk");
    if (!lt_node || !rt_node || !lk_op || !rk_op) {
        fprintf(stderr, "run_join_rep: node alloc\n"); abort();
    }

    ray_op_t* lk_arr[1] = { lk_op };
    ray_op_t* rk_arr[1] = { rk_op };
    ray_op_t* jn = ray_join(g, lt_node, lk_arr, rt_node, rk_arr, 1, join_type);
    if (!jn) { fprintf(stderr, "run_join_rep: join node\n"); abort(); }

    jn = ray_optimize(g, jn);

    double t0 = now_ms();
    ray_t* result = ray_execute(g, jn);
    double t1 = now_ms();

    if (!result || RAY_IS_ERR(result)) {
        fprintf(stderr, "run_join_rep: execute returned error\n"); abort();
    }
    if (result->type != RAY_TABLE) {
        fprintf(stderr, "run_join_rep: result not a table (type=%d)\n",
                result->type); abort();
    }
    if (rows_out) *rows_out = ray_table_nrows(result);
    ray_release(result);
    ray_graph_free(g);
    return t1 - t0;
}

#define NREPS           9   /* normal rep count (post-fix, and non-slow pre-fix) */
#define PREFIX_SLOW_REPS 3  /* capped reps for the SLOW pre-fix O(dup²) builds */

/* join_type encoding (matches ray_join): 0=inner, 1=left, ... */
#define JT_INNER 0
#define JT_LEFT  1

typedef struct {
    const char* name;
    int  post_n;                 /* reps actually taken on post-fix side */
    int  pre_n;                  /* reps actually taken on pre-fix side */
    double post_ms[NREPS];       /* auto-fallback ON  (no_dup=false) */
    double pre_ms[NREPS];        /* auto-fallback OFF (no_dup=true)  */
    int64_t rows_post;
    int64_t rows_pre;
    uint64_t fb_post_delta;      /* dup-fallback counter delta over post-fix reps */
    uint64_t fb_pre_delta;       /* dup-fallback counter delta over pre-fix reps  */
    bool no_build_swap;          /* knob: force build on right */
    int  join_type;
} case_result_t;

/* Run a case.  post-fix uses NREPS; pre-fix uses pre_reps (capped for slow). */
static void run_case(const char* name, ray_t* lt, ray_t* rt,
                     int join_type, bool no_build_swap,
                     int pre_reps, case_result_t* cr) {
    cr->name          = name;
    cr->rows_post     = -1;
    cr->rows_pre      = -1;
    cr->no_build_swap = no_build_swap;
    cr->join_type     = join_type;
    cr->post_n        = NREPS;
    cr->pre_n         = pre_reps;

    printf("Running case %-20s (post-fix %d reps, pre-fix %d reps)...\n",
           name, NREPS, pre_reps);
    fflush(stdout);

    /* ---- post-fix: auto-fallback ON ---- */
    ray_join_no_dup_fallback = false;
    uint64_t fb0 = ray_join_dup_fallbacks;
    for (int rep = 0; rep < NREPS; rep++) {
        ray_join_no_build_swap = no_build_swap;
        int64_t rows = -1;
        cr->post_ms[rep] = run_join_rep(lt, rt, join_type, &rows);
        cr->rows_post = rows;
        ray_join_no_build_swap = false;
    }
    cr->fb_post_delta = ray_join_dup_fallbacks - fb0;

    /* ---- pre-fix: auto-fallback DISABLED (O(dup²) on catastrophic) ---- */
    ray_join_no_dup_fallback = true;
    uint64_t fb1 = ray_join_dup_fallbacks;
    for (int rep = 0; rep < pre_reps; rep++) {
        ray_join_no_build_swap = no_build_swap;
        int64_t rows = -1;
        cr->pre_ms[rep] = run_join_rep(lt, rt, join_type, &rows);
        cr->rows_pre = rows;
        ray_join_no_build_swap = false;
    }
    cr->fb_pre_delta = ray_join_dup_fallbacks - fb1;
    ray_join_no_dup_fallback = false;

    printf("  dup-fallback counter: post-fix delta=%llu  pre-fix delta=%llu\n",
           (unsigned long long)cr->fb_post_delta,
           (unsigned long long)cr->fb_pre_delta);
    printf("  rows_out: post=%lld pre=%lld\n",
           (long long)cr->rows_post, (long long)cr->rows_pre);
    if (cr->rows_post != cr->rows_pre) {
        fprintf(stderr, "CARDINALITY MISMATCH case %s: post=%lld pre=%lld\n",
                name, (long long)cr->rows_post, (long long)cr->rows_pre);
        abort();
    }
    fflush(stdout);
}

/* Key generators.  Probe sides use NONMATCH/UNIQUE so the join OUTPUT stays
 * bounded while the BUILD side carries the duplication under test — otherwise
 * an all-key-7 ⋈ all-key-7 inner join emits 10^11 rows and the build cost is
 * swamped by output materialisation. */
#define GEN_UNIQUE   0   /* v[i] = i                       (distinct, ≥0) */
#define GEN_CONST7   1   /* v[i] = 7                       (all-dup)      */
#define GEN_NONMATCH (-2)/* v[i] = -1 - i                  (distinct, <0; never matches a ≥0 build key) */
/* mod ≥ 2 → v[i] = i % mod */
static int64_t* gen(int64_t n, int64_t mode) {
    int64_t* v = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!v) { fprintf(stderr, "OOM gen(%lld)\n", (long long)n); abort(); }
    if (mode == GEN_UNIQUE) {
        for (int64_t i = 0; i < n; i++) v[i] = i;
    } else if (mode == GEN_CONST7) {
        for (int64_t i = 0; i < n; i++) v[i] = 7;
    } else if (mode == GEN_NONMATCH) {
        for (int64_t i = 0; i < n; i++) v[i] = -1 - i;
    } else {
        for (int64_t i = 0; i < n; i++) v[i] = i % mode;
    }
    return v;
}

/* Build side for the CATASTROPHIC cases: n rows total, of which the first
 * `dup` rows all share key 7 (one pathological partition → O(dup²) pre-fix
 * build), and the remaining rows are distinct (key = 1000 + i, all ≠ 7 and
 * disjoint from the negative non-matching probe keys).  `dup` is kept in the
 * tens-of-thousands so the pre-fix O(dup²) build runs in seconds, not hours
 * (all 10M sharing key 7 would be ~10^14 ops). */
#define CAT_DUP 60000L
static int64_t* gen_cat(int64_t n, int64_t dup) {
    int64_t* v = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!v) { fprintf(stderr, "OOM gen_cat(%lld)\n", (long long)n); abort(); }
    for (int64_t i = 0; i < n; i++) v[i] = (i < dup) ? 7 : (1000 + i);
    return v;
}

int main(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_join_no_build_swap   = false;
    ray_join_no_dup_fallback = false;

    printf("=== bench-join-dup (dup-fallback perf gate) ===\n");
    fflush(stdout);

#if defined(__linux__)
    {
        FILE* f = fopen("/proc/loadavg", "r");
        if (f) {
            char buf[128] = {0};
            if (fgets(buf, sizeof(buf), f)) { printf("load: %s", buf); fflush(stdout); }
            fclose(f);
        }
    }
#endif
    printf("NREPS=%d  PREFIX_SLOW_REPS=%d  RADIX_DUP_RUN_MAX=%d  RAY_PARALLEL_THRESHOLD=%d\n\n",
           NREPS, PREFIX_SLOW_REPS, 512, (int)RAY_PARALLEL_THRESHOLD);
    fflush(stdout);

    const int64_t N10M = 10000000L;

    case_result_t cr[5];

    /* ---- 0: CATASTROPHIC-INNER (headline) ----------------------------------
     * build = right 10M all key 7 (no_swap forces build on the dup'd side);
     * probe = left 10K distinct negative keys → 0 matches → output 0.
     * Output is bounded; the pathological BUILD is what we time. */
    printf("Building CATASTROPHIC-INNER tables (build=right 10M, %ld dup key 7, probe=left 10K nonmatch)...\n",
           (long)CAT_DUP);
    fflush(stdout);
    { int64_t* rv = gen_cat(N10M, CAT_DUP); int64_t* lv = gen(10000L, GEN_NONMATCH);
      ray_t* rt = make_table1("rk", rv, N10M);
      ray_t* lt = make_table1("lk", lv, 10000L);
      free(rv); free(lv);
      run_case("CATASTROPHIC-INNER", lt, rt, JT_INNER, /*no_swap=*/true,
               PREFIX_SLOW_REPS, &cr[0]);
      ray_release(lt); ray_release(rt);
    }

    /* ---- 1: CATASTROPHIC-LEFT (new-coverage headline) ----------------------
     * LEFT join always builds the right side (no swap).  build = right 10M
     * all key 7; probe = left 100K distinct negative keys → all unmatched →
     * output = 100K rows (left + null right).  Bounded. */
    printf("Building CATASTROPHIC-LEFT tables (build=right 10M, %ld dup key 7, probe=left 100K nonmatch)...\n",
           (long)CAT_DUP);
    fflush(stdout);
    { int64_t* rv = gen_cat(N10M, CAT_DUP); int64_t* lv = gen(100000L, GEN_NONMATCH);
      ray_t* rt = make_table1("rk", rv, N10M);
      ray_t* lt = make_table1("lk", lv, 100000L);
      free(rv); free(lv);
      run_case("CATASTROPHIC-LEFT", lt, rt, JT_LEFT, /*no_swap=*/false,
               PREFIX_SLOW_REPS, &cr[1]);
      ray_release(lt); ray_release(rt);
    }

    /* ---- 2: ZERO-REGRESSION (the regression check) -------------------------
     * build = right 10M unique (no_swap=true so the big side is built and the
     * per-row ++run/branch runs 10M times); probe = left 10K = keys [0,10K)
     * → 10K matches → output 10K.  Unique keys → run length 1 → never trips.
     * post-fix ≈ pre-fix proves the added branch costs ~nothing. */
    printf("Building ZERO-REGRESSION tables (build=right 10M unique, probe=left 10K matching)...\n");
    fflush(stdout);
    { int64_t* rv = gen(N10M, GEN_UNIQUE); int64_t* lv = gen(10000L, GEN_UNIQUE);
      ray_t* rt = make_table1("rk", rv, N10M);
      ray_t* lt = make_table1("lk", lv, 10000L);
      free(rv); free(lv);
      run_case("ZERO-REGRESSION", lt, rt, JT_INNER, /*no_swap=*/true,
               NREPS, &cr[2]);
      ray_release(lt); ray_release(rt);
    }

    /* ---- 3: MODERATE-DUP-100 (no-premature-trip) ---------------------------
     * build = right 10M key i%100000 (~100/key); probe = left 10K nonmatch →
     * output 0.  Must NOT trip at ~100/key and must stay radix. */
    printf("Building MODERATE-DUP-100 tables (build=right 10M key i%%100000 ~100/key)...\n");
    fflush(stdout);
    { int64_t* rv = gen(N10M, 100000L); int64_t* lv = gen(10000L, GEN_NONMATCH);
      ray_t* rt = make_table1("rk", rv, N10M);
      ray_t* lt = make_table1("lk", lv, 10000L);
      free(rv); free(lv);
      run_case("MODERATE-DUP-100", lt, rt, JT_INNER, /*no_swap=*/true,
               NREPS, &cr[3]);
      ray_release(lt); ray_release(rt);
    }

    /* ---- 4: MODERATE-DUP-10 (cleaner moderate) -----------------------------
     * build = right 10M key i%1000000 (~10/key); probe = left 10K nonmatch. */
    printf("Building MODERATE-DUP-10 tables (build=right 10M key i%%1000000 ~10/key)...\n");
    fflush(stdout);
    { int64_t* rv = gen(N10M, 1000000L); int64_t* lv = gen(10000L, GEN_NONMATCH);
      ray_t* rt = make_table1("rk", rv, N10M);
      ray_t* lt = make_table1("lk", lv, 10000L);
      free(rv); free(lv);
      run_case("MODERATE-DUP-10", lt, rt, JT_INNER, /*no_swap=*/true,
               NREPS, &cr[4]);
      ray_release(lt); ray_release(rt);
    }

    /* ---------------------------------------------------------------
     * Results table (median + min, post-fix vs pre-fix)
     * --------------------------------------------------------------- */
    printf("\n");
    printf("%-20s  %-8s  %5s  %14s  %12s  %12s  %s\n",
           "case", "side", "reps", "median_ms", "min_ms", "fb_delta", "rows_out");
    printf("%-20s  %-8s  %5s  %14s  %12s  %12s  %s\n",
           "--------------------", "--------", "-----",
           "--------------", "------------", "------------", "----------");
    for (int ci = 0; ci < 5; ci++) {
        case_result_t* c = &cr[ci];
        double med_post = medianN(c->post_ms, c->post_n);
        double min_post = minN(c->post_ms, c->post_n);
        double med_pre  = medianN(c->pre_ms, c->pre_n);
        double min_pre  = minN(c->pre_ms, c->pre_n);
        printf("%-20s  %-8s  %5d  %14.3f  %12.3f  %12llu  %lld\n",
               c->name, "post-fix", c->post_n, med_post, min_post,
               (unsigned long long)c->fb_post_delta, (long long)c->rows_post);
        printf("%-20s  %-8s  %5d  %14.3f  %12.3f  %12llu  %lld\n",
               "", "pre-fix", c->pre_n, med_pre, min_pre,
               (unsigned long long)c->fb_pre_delta, (long long)c->rows_pre);
        /* speedup / delta line */
        double speedup = med_post > 0 ? med_pre / med_post : 0;
        printf("%-20s  %-8s  pre/post median speedup = %.2fx  (delta = %+.3f ms)\n",
               "", "", speedup, med_pre - med_post);
    }

    /* ---------------------------------------------------------------
     * Mechanism summary
     * --------------------------------------------------------------- */
    printf("\n--- mechanism (ray_join_dup_fallbacks deltas) ---\n");
    for (int ci = 0; ci < 5; ci++) {
        case_result_t* c = &cr[ci];
        printf("  %-20s post-fix trips=%llu (over %d reps)  pre-fix trips=%llu (over %d reps)\n",
               c->name,
               (unsigned long long)c->fb_post_delta, c->post_n,
               (unsigned long long)c->fb_pre_delta, c->pre_n);
    }

    /* Catastrophic cases MUST trip on post-fix (auto-fallback fires). */
    if (cr[0].fb_post_delta == 0) {
        fprintf(stderr, "MECHANISM FAILURE: CATASTROPHIC-INNER post-fix did not trip\n");
        abort();
    }
    if (cr[1].fb_post_delta == 0) {
        fprintf(stderr, "MECHANISM FAILURE: CATASTROPHIC-LEFT post-fix did not trip\n");
        abort();
    }
    /* Pre-fix (bypass) must NEVER trip — the whole point of the knob. */
    for (int ci = 0; ci < 5; ci++) {
        if (cr[ci].fb_pre_delta != 0) {
            fprintf(stderr, "MECHANISM FAILURE: %s pre-fix tripped despite bypass (delta=%llu)\n",
                    cr[ci].name, (unsigned long long)cr[ci].fb_pre_delta);
            abort();
        }
    }
    /* ZERO-REGRESSION must NOT trip on either side. */
    if (cr[2].fb_post_delta != 0) {
        fprintf(stderr, "MECHANISM FAILURE: ZERO-REGRESSION post-fix tripped (delta=%llu)\n",
                (unsigned long long)cr[2].fb_post_delta);
        abort();
    }
    /* MODERATE-DUP trips are a FINDING, not a failure — report prominently. */
    printf("\n--- MODERATE-DUP finding ---\n");
    printf("  MODERATE-DUP-100 (~100/key build): post-fix trips=%llu → %s\n",
           (unsigned long long)cr[3].fb_post_delta,
           cr[3].fb_post_delta ? "TRIPPED PREMATURELY (threshold 512 may be too low)"
                               : "stayed radix (no premature trip)");
    printf("  MODERATE-DUP-10  (~10/key build):  post-fix trips=%llu → %s\n",
           (unsigned long long)cr[4].fb_post_delta,
           cr[4].fb_post_delta ? "TRIPPED PREMATURELY"
                               : "stayed radix (no premature trip)");

    /* ---------------------------------------------------------------
     * Raw per-rep numbers
     * --------------------------------------------------------------- */
    printf("\n--- raw per-rep ms ---\n");
    for (int ci = 0; ci < 5; ci++) {
        case_result_t* c = &cr[ci];
        printf("%-20s  %-8s", c->name, "post-fix");
        for (int r = 0; r < c->post_n; r++) printf("  %9.3f", c->post_ms[r]);
        printf("\n");
        printf("%-20s  %-8s", "", "pre-fix");
        for (int r = 0; r < c->pre_n; r++) printf("  %9.3f", c->pre_ms[r]);
        printf("\n");
    }

    printf("\nDone.\n");
    fflush(stdout);

    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}

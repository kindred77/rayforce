/* Join build-side selection perf gate.
 *
 * Measures the speedup from building the hash table on the smaller (left)
 * side when left < right in a radix inner join.  Four cases, each run
 * with swap enabled (knob off) and swap forced-off (legacy, knob on),
 * NREPS reps interleaved per case, median+min exec wall time reported.
 *
 * Cases:
 *   WIN          right=10M key i%1000000 (10 dup/key), left=10K key i%1000000.
 *                swap builds 10K hash + probes 10M; legacy builds 10M hash.
 *   HEAVY-DUP-WIN right=10M key i%1000 (10000 dup/key), left=10K key i%1000.
 *                swap builds 10K hash + probes 10M; legacy builds 10M hash.
 *                Probes deeper chains in the big hash. Tests duplication scaling.
 *   CONTROL      right=10M, left=10M (key i%1000000).
 *                swap must NOT fire (equal sizes); knob-on/off medians within noise.
 *   MANY-TO-MANY right=10M key i%100000 (~100/key), left=100K key i%100000
 *                (~1/key) → output ~10M rows.  Swap fires; must not pessimize.
 *
 * Mechanism: assert ray_join_build_swaps advanced on WIN, HEAVY-DUP-WIN, and
 * MANY-TO-MANY; assert it did NOT advance on CONTROL.
 *
 * Timing: CLOCK_MONOTONIC around ray_execute only.  Tables built once outside
 * the timed loop; graph rebuilt per rep.
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

/* ---------- median/min (qsort on small N, max 64 elements) ---------- */
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

/* ---------- run one inner join rep, return exec wall ms + output rows ---------- */
static double run_join_rep(ray_t* lt, ray_t* rt, int64_t* rows_out) {
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
    /* join_type=0 → inner join */
    ray_op_t* jn = ray_join(g, lt_node, lk_arr, rt_node, rk_arr, 1, 0);
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

#define NREPS 15

/* ---------- per-case runner ---------- */
typedef struct {
    const char* name;
    double swap_ms[NREPS];    /* knob off — swap allowed */
    double legacy_ms[NREPS];  /* knob on  — force legacy (build on right) */
    int64_t rows_out_swap;    /* output row count (from last swap rep) */
    int64_t rows_out_legacy;  /* output row count (from last legacy rep) */
} case_result_t;

static void run_case(const char* name,
                     ray_t* lt, ray_t* rt,
                     bool expect_swap,
                     case_result_t* cr) {
    cr->name = name;
    cr->rows_out_swap   = -1;
    cr->rows_out_legacy = -1;

    printf("Running case %-16s (%d reps)...\n", name, NREPS);
    fflush(stdout);

    uint64_t swaps_before = ray_join_build_swaps;

    for (int rep = 0; rep < NREPS; rep++) {
        /* swap side (knob off) */
        ray_join_no_build_swap = false;
        int64_t rows_sw = -1;
        cr->swap_ms[rep] = run_join_rep(lt, rt, &rows_sw);
        cr->rows_out_swap = rows_sw;

        /* legacy side (knob on) */
        ray_join_no_build_swap = true;
        int64_t rows_lg = -1;
        cr->legacy_ms[rep] = run_join_rep(lt, rt, &rows_lg);
        cr->rows_out_legacy = rows_lg;

        ray_join_no_build_swap = false;   /* reset after each rep */
    }

    uint64_t swaps_after = ray_join_build_swaps;
    bool fired = swaps_after > swaps_before;

    if (expect_swap && !fired) {
        fprintf(stderr,
            "MECHANISM FAILURE case %s: expected build-side swap to fire "
            "(before=%llu after=%llu)\n",
            name,
            (unsigned long long)swaps_before,
            (unsigned long long)swaps_after);
        abort();
    }
    if (!expect_swap && fired) {
        fprintf(stderr,
            "MECHANISM FAILURE case %s: swap fired unexpectedly "
            "(before=%llu after=%llu)\n",
            name,
            (unsigned long long)swaps_before,
            (unsigned long long)swaps_after);
        abort();
    }

    printf("  swap counter: before=%llu after=%llu fired=%s\n",
           (unsigned long long)swaps_before,
           (unsigned long long)swaps_after,
           fired ? "YES" : "NO");
    fflush(stdout);
}

int main(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_join_no_build_swap = false;   /* start clean */

    printf("=== bench-join-buildside ===\n");
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

    printf("NREPS=%d  RAY_PARALLEL_THRESHOLD=%d\n\n",
           NREPS, (int)RAY_PARALLEL_THRESHOLD);
    fflush(stdout);

    /* ---------------------------------------------------------------
     * WIN case: right=10M, left=10K
     *   key pattern: right i%1000000, left i%1000000
     *   swap builds 10K hash + probes 10M
     *   legacy builds 10M hash + probes 10K
     * --------------------------------------------------------------- */
    printf("Building WIN tables (right=10M, left=10K)...\n"); fflush(stdout);
    int64_t win_nr = 10000000L;
    int64_t win_nl =    10000L;
    int64_t* win_rv = (int64_t*)malloc((size_t)win_nr * sizeof(int64_t));
    int64_t* win_lv = (int64_t*)malloc((size_t)win_nl * sizeof(int64_t));
    if (!win_rv || !win_lv) { fprintf(stderr, "OOM WIN tables\n"); abort(); }
    for (int64_t i = 0; i < win_nr; i++) win_rv[i] = i % 1000000L;
    for (int64_t i = 0; i < win_nl; i++) win_lv[i] = i % 1000000L;
    ray_t* win_rt = make_table1("rk", win_rv, win_nr);
    ray_t* win_lt = make_table1("lk", win_lv, win_nl);
    free(win_rv); free(win_lv);
    printf("  right=%lld rows, left=%lld rows\n\n",
           (long long)ray_table_nrows(win_rt),
           (long long)ray_table_nrows(win_lt));
    fflush(stdout);

    /* ---------------------------------------------------------------
     * CONTROL case: right=10M, left=10M
     *   key pattern: i%1000000 both sides
     *   swap must NOT fire (left == right, strict < condition fails)
     * --------------------------------------------------------------- */
    printf("Building CONTROL tables (right=10M, left=10M)...\n"); fflush(stdout);
    int64_t ctl_n = 10000000L;
    int64_t* ctl_rv = (int64_t*)malloc((size_t)ctl_n * sizeof(int64_t));
    int64_t* ctl_lv = (int64_t*)malloc((size_t)ctl_n * sizeof(int64_t));
    if (!ctl_rv || !ctl_lv) { fprintf(stderr, "OOM CONTROL tables\n"); abort(); }
    for (int64_t i = 0; i < ctl_n; i++) { ctl_rv[i] = i % 1000000L; ctl_lv[i] = i % 1000000L; }
    ray_t* ctl_rt = make_table1("rk", ctl_rv, ctl_n);
    ray_t* ctl_lt = make_table1("lk", ctl_lv, ctl_n);
    free(ctl_rv); free(ctl_lv);
    printf("  right=%lld rows, left=%lld rows\n\n",
           (long long)ray_table_nrows(ctl_rt),
           (long long)ray_table_nrows(ctl_lt));
    fflush(stdout);

    /* ---------------------------------------------------------------
     * MANY-TO-MANY case: right=10M key i%100000 (~100/key),
     *                    left=100K key i%100000 (~1/key)
     *   per-key fan-out: 100 right × 1 left = 100 output rows/key
     *   100000 keys × 100 = 10M output rows
     *   Swap fires (left=100K < right=10M).
     * --------------------------------------------------------------- */
    printf("Building MANY-TO-MANY tables (right=10M, left=100K)...\n"); fflush(stdout);
    int64_t m2m_nr = 10000000L;
    int64_t m2m_nl =   100000L;
    int64_t* m2m_rv = (int64_t*)malloc((size_t)m2m_nr * sizeof(int64_t));
    int64_t* m2m_lv = (int64_t*)malloc((size_t)m2m_nl * sizeof(int64_t));
    if (!m2m_rv || !m2m_lv) { fprintf(stderr, "OOM MANY-TO-MANY tables\n"); abort(); }
    for (int64_t i = 0; i < m2m_nr; i++) m2m_rv[i] = i % 100000L;
    for (int64_t i = 0; i < m2m_nl; i++) m2m_lv[i] = i % 100000L;
    ray_t* m2m_rt = make_table1("rk", m2m_rv, m2m_nr);
    ray_t* m2m_lt = make_table1("lk", m2m_lv, m2m_nl);
    free(m2m_rv); free(m2m_lv);
    printf("  right=%lld rows (~100/key), left=%lld rows (~1/key)\n",
           (long long)ray_table_nrows(m2m_rt),
           (long long)ray_table_nrows(m2m_lt));
    printf("  expected output: 100000 keys × 100 right/key × 1 left/key = ~10M rows\n\n");
    fflush(stdout);

    /* ---------------------------------------------------------------
     * HEAVY-DUP-WIN case: right=10M key i%1000 (10000 dup/key),
     *                     left=10K key i%1000 (10 dup/key)
     *   Swap builds 10K hash + probes 10M; legacy builds 10M hash.
     *   Heavy chains in the big hash when using legacy path.
     *   Tests whether the swap win scales with large-side key duplication.
     * --------------------------------------------------------------- */
    printf("Building HEAVY-DUP-WIN tables (right=10M, left=10K)...\n"); fflush(stdout);
    int64_t hdw_nr = 10000000L;
    int64_t hdw_nl =    10000L;
    int64_t* hdw_rv = (int64_t*)malloc((size_t)hdw_nr * sizeof(int64_t));
    int64_t* hdw_lv = (int64_t*)malloc((size_t)hdw_nl * sizeof(int64_t));
    if (!hdw_rv || !hdw_lv) { fprintf(stderr, "OOM HEAVY-DUP-WIN tables\n"); abort(); }
    for (int64_t i = 0; i < hdw_nr; i++) hdw_rv[i] = i % 1000L;    /* 10000 dup/key */
    for (int64_t i = 0; i < hdw_nl; i++) hdw_lv[i] = i % 1000L;    /* 10 dup/key */
    ray_t* hdw_rt = make_table1("rk", hdw_rv, hdw_nr);
    ray_t* hdw_lt = make_table1("lk", hdw_lv, hdw_nl);
    free(hdw_rv); free(hdw_lv);
    printf("  right=%lld rows (10000 dup/key), left=%lld rows (10 dup/key)\n\n",
           (long long)ray_table_nrows(hdw_rt),
           (long long)ray_table_nrows(hdw_lt));
    fflush(stdout);

    /* ---------------------------------------------------------------
     * Run all four cases
     * --------------------------------------------------------------- */
    case_result_t cr_win, cr_hdw, cr_ctl, cr_m2m;

    run_case("WIN",           win_lt, win_rt, /*expect_swap=*/true,  &cr_win);
    run_case("HEAVY-DUP-WIN", hdw_lt, hdw_rt, /*expect_swap=*/true,  &cr_hdw);
    run_case("CONTROL",       ctl_lt, ctl_rt, /*expect_swap=*/false, &cr_ctl);
    run_case("MANY-TO-MANY",  m2m_lt, m2m_rt, /*expect_swap=*/true,  &cr_m2m);

    /* Sanity: swap must not change output cardinality. */
#define CHECK_ROWS(cr) do { \
    if ((cr).rows_out_swap != (cr).rows_out_legacy) { \
        fprintf(stderr, \
            "CARDINALITY MISMATCH case %s: swap=%lld legacy=%lld\n", \
            (cr).name, \
            (long long)(cr).rows_out_swap, \
            (long long)(cr).rows_out_legacy); \
        abort(); \
    } \
    assert((cr).rows_out_swap == (cr).rows_out_legacy); \
} while (0)
    CHECK_ROWS(cr_win);
    CHECK_ROWS(cr_hdw);
    CHECK_ROWS(cr_ctl);
    CHECK_ROWS(cr_m2m);
#undef CHECK_ROWS

    /* ---------------------------------------------------------------
     * Results table (median + min)
     * --------------------------------------------------------------- */
    printf("\n");
    printf("%-16s  %-8s  %14s  %10s  %14s  %10s  %12s  %12s\n",
           "case", "side", "median_ms", "min_ms", "delta_med_ms", "delta_min_ms", "rows_out", "swap_fired");
    printf("%-16s  %-8s  %14s  %10s  %14s  %10s  %12s  %12s\n",
           "----------------", "--------",
           "--------------", "--------",
           "------------", "----------",
           "------------", "----------");

    case_result_t* cases[4] = { &cr_win, &cr_hdw, &cr_ctl, &cr_m2m };
    const char* expect_swap[4] = { "YES", "YES", "NO", "YES" };
    for (int ci = 0; ci < 4; ci++) {
        case_result_t* cr = cases[ci];
        double med_swap   = medianN(cr->swap_ms, NREPS);
        double med_legacy = medianN(cr->legacy_ms, NREPS);
        double min_swap   = minN(cr->swap_ms, NREPS);
        double min_legacy = minN(cr->legacy_ms, NREPS);
        double delta_med  = med_swap - med_legacy;   /* negative = swap is faster */
        double delta_min  = min_swap - min_legacy;
        printf("%-16s  %-8s  %14.3f  %10.3f  %14s  %10s  %12lld  %12s\n",
               cr->name, "swap", med_swap, min_swap, "", "", (long long)cr->rows_out_swap, expect_swap[ci]);
        printf("%-16s  %-8s  %14.3f  %10.3f  %14.3f  %10.3f  %12lld  %12s\n",
               "", "legacy", med_legacy, min_legacy, delta_med, delta_min, (long long)cr->rows_out_legacy, "");
    }

    /* ---------------------------------------------------------------
     * Many-to-many fan-out note
     * --------------------------------------------------------------- */
    printf("\nMany-to-many fan-out: right=%lld key%%100000 → each key has ~100 right rows;\n",
           (long long)m2m_nr);
    printf("  left=%lld key%%100000 → each key has ~1 left row;\n", (long long)m2m_nl);
    printf("  output ~%lld rows (actual: swap=%lld legacy=%lld)\n",
           (long long)(m2m_nr),
           (long long)cr_m2m.rows_out_swap,
           (long long)cr_m2m.rows_out_legacy);

    /* ---------------------------------------------------------------
     * Raw per-rep numbers
     * --------------------------------------------------------------- */
    printf("\n--- raw per-rep ms ---\n");
    printf("%-16s  %-8s", "case", "side");
    for (int r = 0; r < NREPS; r++) printf("   rep%02d", r + 1);
    printf("\n");

    case_result_t* all4[4] = { &cr_win, &cr_hdw, &cr_ctl, &cr_m2m };
    for (int ci = 0; ci < 4; ci++) {
        case_result_t* cr = all4[ci];
        printf("%-16s  %-8s", cr->name, "swap");
        for (int r = 0; r < NREPS; r++) printf("  %7.3f", cr->swap_ms[r]);
        printf("\n");
        printf("%-16s  %-8s", "", "legacy");
        for (int r = 0; r < NREPS; r++) printf("  %7.3f", cr->legacy_ms[r]);
        printf("\n");
    }

    printf("\nMechanism: ray_join_build_swaps counter verified per case (aborts on failure)\n");
    fflush(stdout);

    /* cleanup */
    ray_release(win_lt); ray_release(win_rt);
    ray_release(hdw_lt); ray_release(hdw_rt);
    ray_release(ctl_lt); ray_release(ctl_rt);
    ray_release(m2m_lt); ray_release(m2m_rt);
    ray_sym_destroy();
    ray_heap_destroy();

    return 0;
}

/* Group predicate pushdown perf gate.
 * Build: make bench-group-pushdown
 * Runs: FILTER(k < 10000, GROUP(sum v by k)) pushed vs unpushed
 *       FILTER(k >= 0,    GROUP(sum v by k)) as control (no filtering benefit)
 * Reports medians of exec and optimize time, pushed vs unpushed.
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

/* ---------- median (simple insertion sort on small N) ---------- */
static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static double median(double* arr, int n) {
    qsort(arr, (size_t)n, sizeof(double), cmp_double);
    if (n % 2 == 1) return arr[n / 2];
    return (arr[n / 2 - 1] + arr[n / 2]) * 0.5;
}

/* ---------- build 10M-row table ---------- */
#define NROWS       10000000L
#define N_DISTINCT  1000000L   /* 1M distinct keys, 10 rows each */

static ray_t* g_tbl = NULL;

static void build_table(void) {
    int64_t* kd = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* vd = malloc((size_t)NROWS * sizeof(int64_t));
    if (!kd || !vd) { fprintf(stderr, "OOM building table\n"); abort(); }

    for (int64_t i = 0; i < NROWS; i++) {
        kd[i] = i % N_DISTINCT;
        vd[i] = i;
    }

    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, NROWS);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, NROWS);
    free(kd); free(vd);

    g_tbl = ray_table_new(2);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("k", 1), kv);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv); ray_release(vv);
}

/* ---------- query shapes ----------
 * WIN:     FILTER(k < 10000, GROUP(sum v by k))   — keeps 1% of groups
 * CONTROL: FILTER(k >= 0,    GROUP(sum v by k))   — passes all groups
 */
typedef enum { SHAPE_WIN = 0, SHAPE_CONTROL = 1 } shape_t;
typedef enum { SIDE_PUSHED = 0, SIDE_UNPUSHED = 1 } side_t;

static const char* shape_name(shape_t s) {
    return s == SHAPE_WIN ? "WIN    (k<10000)" : "CONTROL(k>=0)   ";
}
static const char* side_name(side_t s) {
    return s == SIDE_PUSHED ? "pushed  " : "unpushed";
}
static int64_t expected_rows(shape_t s) {
    return s == SHAPE_WIN ? 10000L : N_DISTINCT;
}

/* Run one rep; returns 0 on success.
 * opt_ms and exec_ms are out-params. */
static int run_rep(shape_t shape, side_t side,
                   double* opt_ms_out, double* exec_ms_out) {
    ray_opt_no_group_pushdown = (side == SIDE_UNPUSHED);

    ray_graph_t* g = ray_graph_new(g_tbl);

    ray_op_t* k   = ray_scan(g, "k");
    ray_op_t* v   = ray_scan(g, "v");
    ray_op_t* keys[]  = {k};
    uint16_t  aops[]  = {OP_SUM};
    ray_op_t* ains[]  = {v};
    ray_op_t* grp  = ray_group(g, keys, 1, aops, ains, 1);

    ray_op_t* pred;
    if (shape == SHAPE_WIN) {
        /* k < 10000 — keeps 1% of 1M distinct keys */
        pred = ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 10000));
    } else {
        /* k >= 0 — passes everything */
        pred = ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 0));
    }
    ray_op_t* filt = ray_filter(g, grp, pred);

    /* Time optimize separately */
    double t0 = now_ms();
    ray_op_t* root = ray_optimize(g, filt);
    double t1 = now_ms();
    *opt_ms_out = t1 - t0;

    /* Mechanism evidence: WIN + pushed → root must be OP_GROUP with OP_FILTER input */
    if (shape == SHAPE_WIN && side == SIDE_PUSHED) {
        if (!root || root->opcode != OP_GROUP ||
            !root->inputs[0] || root->inputs[0]->opcode != OP_FILTER) {
            fprintf(stderr,
                "MECHANISM FAILURE: WIN+pushed: expected OP_GROUP(OP_FILTER,...) "
                "but got root->opcode=%d inputs[0]->opcode=%d\n",
                root ? (int)root->opcode : -1,
                (root && root->inputs[0]) ? (int)root->inputs[0]->opcode : -1);
            abort();
        }
    }

    /* Time execute only */
    t0 = now_ms();
    ray_t* result = ray_execute(g, root);
    t1 = now_ms();
    *exec_ms_out = t1 - t0;

    if (RAY_IS_ERR(result)) {
        fprintf(stderr, "ray_execute returned error (shape=%d side=%d)\n",
                (int)shape, (int)side);
        abort();
    }

    int64_t nrows = ray_table_nrows(result);
    int64_t exp   = expected_rows(shape);
    if (nrows != exp) {
        fprintf(stderr, "row-count mismatch: shape=%d side=%d got=%lld expected=%lld\n",
                (int)shape, (int)side, (long long)nrows, (long long)exp);
        abort();
    }

    ray_release(result);
    ray_graph_free(g);
    ray_opt_no_group_pushdown = false;
    return 0;
}

#define NREPS 9

int main(void) {
    ray_heap_init();
    (void)ray_sym_init();

    printf("Building 10M-row table (k=i%%1000000, v=i)...\n");
    build_table();
    printf("Table built: %lld rows\n\n", (long long)ray_table_nrows(g_tbl));

    /* Storage for per-rep timings: [shape][side][rep] */
    double exec_ms[2][2][NREPS];
    double opt_ms[2][2][NREPS];

    printf("Running %d reps (interleaved pushed/unpushed per shape)...\n", NREPS);
    for (int rep = 0; rep < NREPS; rep++) {
        for (int sh = 0; sh < 2; sh++) {
            for (int si = 0; si < 2; si++) {
                run_rep((shape_t)sh, (side_t)si,
                        &opt_ms[sh][si][rep],
                        &exec_ms[sh][si][rep]);
            }
        }
        printf("  rep %d done\n", rep + 1);
    }

    printf("\n");
    printf("%-18s  %-10s  %12s  %12s  %8s\n",
           "shape", "side", "exec_med_ms", "opt_med_ms", "rows");
    printf("%-18s  %-10s  %12s  %12s  %8s\n",
           "------------------", "----------",
           "------------", "------------", "--------");

    for (int sh = 0; sh < 2; sh++) {
        for (int si = 0; si < 2; si++) {
            double em = median(exec_ms[sh][si], NREPS);
            double om = median(opt_ms[sh][si],  NREPS);
            int64_t exp = expected_rows((shape_t)sh);
            printf("%-18s  %-10s  %12.3f  %12.3f  %8lld\n",
                   shape_name((shape_t)sh), side_name((side_t)si),
                   em, om, (long long)exp);
        }
    }

    printf("\nMechanism: WIN+pushed root=OP_GROUP(OP_FILTER,...) — verified each rep (aborts on failure)\n");

    /* Raw per-rep numbers */
    printf("\n--- raw per-rep exec_ms ---\n");
    printf("%-18s  %-10s", "shape", "side");
    for (int r = 0; r < NREPS; r++) printf("  rep%d", r + 1);
    printf("\n");
    for (int sh = 0; sh < 2; sh++) {
        for (int si = 0; si < 2; si++) {
            printf("%-18s  %-10s", shape_name((shape_t)sh), side_name((side_t)si));
            for (int r = 0; r < NREPS; r++)
                printf("  %6.1f", exec_ms[sh][si][r]);
            printf("\n");
        }
    }

    printf("\n--- raw per-rep opt_ms ---\n");
    printf("%-18s  %-10s", "shape", "side");
    for (int r = 0; r < NREPS; r++) printf("  rep%d", r + 1);
    printf("\n");
    for (int sh = 0; sh < 2; sh++) {
        for (int si = 0; si < 2; si++) {
            printf("%-18s  %-10s", shape_name((shape_t)sh), side_name((side_t)si));
            for (int r = 0; r < NREPS; r++)
                printf("  %6.3f", opt_ms[sh][si][r]);
            printf("\n");
        }
    }

    ray_release(g_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}

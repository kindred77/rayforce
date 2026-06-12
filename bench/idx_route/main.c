/* Index routing per-point perf gate.
 * Build: make bench-idx-route
 *
 * Measures 9 index-routing consumption points on 10M-row tables.
 * Each point is timed with index attached (indexed side) and dropped
 * (baseline side); sides are interleaved per rep (9 reps total).
 * Attach costs reported separately, NOT folded into query medians.
 *
 * Consumption points:
 *   filter-range:    FILTER(k3 < 100000)  — 1% selectivity, sort index on k3
 *   filter-range-G:  FILTER(k3 < 5000000) — 50% (guard: must fall back)
 *   filter-zone-N:   FILTER(k2 > 20000000) — NONE result, zone index on k2
 *   filter-zone-A:   FILTER(k2 >= 0)       — ALL result, zone index on k2
 *   filter-bloom:    FILTER(k == 1000003)  — absent key, bloom index on k
 *   in:              FILTER(IN(k, [7 999999 123456])), hash index on k
 *   find:            ray_index_find_row point lookup (present + absent), hash index on k
 *   sort:            ORDER BY k3 asc (shuffled column), sort index on k3
 *   distinct:        distinct k4 (k3 % 100000 → 100k distinct), sort index on k4
 *
 * Table layout:
 *   k   = i % 1000000       — 1M distinct keys (hash/bloom targets)
 *   k2  = i / 10            — 1M distinct, sorted ascending (zone target)
 *   v   = i
 *   k3  = LCG-shuffled permutation of 0..10M-1 (sort/range/distinct target)
 *   k4  = k3 % 100000       — 100k distinct shuffled (distinct sort target)
 *
 * Using the shuffled k3 for filter-range avoids the rowsel ALL-segment
 * rollback corner case (cum underflow) that triggers with perfectly-sorted
 * data where matching rows all land in the first N segments at 100% fill.
 * Random distribution → MIX segments → no cumulative rollback issue.
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
#include "ops/idxop.h"
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

/* ---------- median (qsort on small N) ---------- */
static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static double median(double* arr, int n) {
    qsort(arr, (size_t)n, sizeof(double), cmp_double);
    if (n % 2 == 1) return arr[n / 2];
    return (arr[n / 2 - 1] + arr[n / 2]) * 0.5;
}

/* ---------- table / column globals ---------- */
#define NROWS      10000000L
#define N_DIST_K   1000000L     /* distinct values of k */

/* Base table: 5 columns k, k2, v, k3, k4 */
static ray_t* g_tbl = NULL;

/* Retained column vectors — used for attach/drop cycles.
 * These always hold unindexed copies. */
static ray_t* g_col_k   = NULL;   /* k  = i % 1000000 */
static ray_t* g_col_k2  = NULL;   /* k2 = i / 10 (sorted asc) */
static ray_t* g_col_k3  = NULL;   /* k3 = LCG permutation of 0..10M-1 */
static ray_t* g_col_k4  = NULL;   /* k4 = k3 % 100000 */

/* ---------- LCG shuffle: deterministic permutation of 0..N-1 ---------- */
static void lcg_shuffle(int64_t* arr, int64_t n) {
    for (int64_t i = 0; i < n; i++) arr[i] = i;
    uint64_t state = 0xdeadbeefcafe1234ULL;
    for (int64_t i = n - 1; i > 0; i--) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t j = (int64_t)((state >> 33) % (uint64_t)(i + 1));
        int64_t tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

static void build_table(void) {
    int64_t* kd  = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* k2d = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* vd  = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* k3d = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* k4d = malloc((size_t)NROWS * sizeof(int64_t));
    if (!kd || !k2d || !vd || !k3d || !k4d) {
        fprintf(stderr, "OOM building table\n"); abort();
    }

    for (int64_t i = 0; i < NROWS; i++) {
        kd[i]  = i % N_DIST_K;   /* 1M distinct, uniform */
        k2d[i] = i / 10;         /* 1M distinct, sorted ascending */
        vd[i]  = i;
    }

    /* k3: deterministic shuffle of 0..NROWS-1 (permutation, all distinct) */
    lcg_shuffle(k3d, NROWS);

    /* k4 = k3 % 100000 → 100k distinct values, randomly distributed */
    for (int64_t i = 0; i < NROWS; i++) k4d[i] = k3d[i] % 100000L;

    ray_t* kv  = ray_vec_from_raw(RAY_I64, kd,  NROWS);
    ray_t* k2v = ray_vec_from_raw(RAY_I64, k2d, NROWS);
    ray_t* vv  = ray_vec_from_raw(RAY_I64, vd,  NROWS);
    ray_t* k3v = ray_vec_from_raw(RAY_I64, k3d, NROWS);
    ray_t* k4v = ray_vec_from_raw(RAY_I64, k4d, NROWS);
    free(kd); free(k2d); free(vd); free(k3d); free(k4d);

    if (RAY_IS_ERR(kv)  || RAY_IS_ERR(k2v) || RAY_IS_ERR(vv) ||
        RAY_IS_ERR(k3v) || RAY_IS_ERR(k4v)) {
        fprintf(stderr, "OOM creating vecs\n"); abort();
    }

    /* Keep private retained copies for index attach/drop cycles */
    g_col_k  = kv;  ray_retain(g_col_k);
    g_col_k2 = k2v; ray_retain(g_col_k2);
    g_col_k3 = k3v; ray_retain(g_col_k3);
    g_col_k4 = k4v; ray_retain(g_col_k4);

    g_tbl = ray_table_new(5);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("k",  1), kv);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("k2", 2), k2v);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("v",  1), vv);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("k3", 2), k3v);
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern("k4", 2), k4v);
    ray_release(kv); ray_release(k2v); ray_release(vv);
    ray_release(k3v); ray_release(k4v);

    if (!g_tbl || RAY_IS_ERR(g_tbl)) {
        fprintf(stderr, "Failed to build table\n"); abort();
    }
}

/* Replace an existing column in g_tbl by name with new_vec.
 * Uses ray_table_set_col_idx so the replacement is in-place.
 * ray_table_add_col APPENDS (does not replace), so we must find
 * the slot index first. */
static void tbl_set_col(const char* name, size_t nlen, ray_t* new_vec) {
    int64_t sym   = ray_sym_intern(name, nlen);
    int64_t ncols = ray_table_ncols(g_tbl);
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(g_tbl, i) == sym) {
            ray_table_set_col_idx(g_tbl, i, new_vec);
            return;
        }
    }
    fprintf(stderr, "tbl_set_col: column '%s' not found\n", name); abort();
}

/* ===== Attach cost measurement ===== */

typedef struct {
    double zone_ms;
    double bloom_ms;
    double hash_ms;
    double sort_k3_ms;
} attach_costs_t;

static attach_costs_t measure_attach_costs(void) {
    attach_costs_t c = {0};

    /* Zone on k2 */
    {
        ray_t* v = g_col_k2; ray_retain(v);
        double t0 = now_ms();
        ray_index_attach_zone(&v);
        c.zone_ms = now_ms() - t0;
        ray_index_drop(&v); ray_release(v);
    }
    /* Bloom on k */
    {
        ray_t* v = g_col_k; ray_retain(v);
        double t0 = now_ms();
        ray_index_attach_bloom(&v);
        c.bloom_ms = now_ms() - t0;
        ray_index_drop(&v); ray_release(v);
    }
    /* Hash on k */
    {
        ray_t* v = g_col_k; ray_retain(v);
        double t0 = now_ms();
        ray_index_attach_hash(&v);
        c.hash_ms = now_ms() - t0;
        ray_index_drop(&v); ray_release(v);
    }
    /* Sort on k3 (shuffled — representative cost) */
    {
        ray_t* v = g_col_k3; ray_retain(v);
        double t0 = now_ms();
        ray_index_attach_sort(&v);
        c.sort_k3_ms = now_ms() - t0;
        ray_index_drop(&v); ray_release(v);
    }
    return c;
}

/* ===== Point enum ===== */

typedef enum {
    PT_FILTER_RANGE = 0,   /* FILTER(k3 < 100000),  sort index on k3 — 1% */
    PT_FILTER_RANGE_G,     /* FILTER(k3 < 5000000), sort index on k3 — 50% guard */
    PT_FILTER_ZONE_NONE,   /* FILTER(k2 > 20000000), zone index on k2 — NONE */
    PT_FILTER_ZONE_ALL,    /* FILTER(k2 >= 0),        zone index on k2 — ALL */
    PT_FILTER_BLOOM,       /* FILTER(k == 1000003),  bloom index on k — absent */
    PT_IN,                 /* FILTER(IN(k, [7 999999 123456])), hash index on k */
    PT_FIND,               /* ray_index_find_row (present+absent), hash index on k */
    PT_SORT,               /* ORDER BY k3 asc, sort index on k3 */
    PT_DISTINCT,           /* distinct k4, sort index on k4 */
    PT__N
} point_t;

static const char* point_name(point_t p) {
    switch (p) {
    case PT_FILTER_RANGE:     return "filter-range  ";
    case PT_FILTER_RANGE_G:   return "filter-range-G";
    case PT_FILTER_ZONE_NONE: return "filter-zone-N ";
    case PT_FILTER_ZONE_ALL:  return "filter-zone-A ";
    case PT_FILTER_BLOOM:     return "filter-bloom  ";
    case PT_IN:               return "in            ";
    case PT_FIND:             return "find          ";
    case PT_SORT:             return "sort          ";
    case PT_DISTINCT:         return "distinct      ";
    default:                  return "???           ";
    }
}

/* Which site counter to assert advanced on the indexed side */
static idx_site_t point_site(point_t p) {
    switch (p) {
    case PT_FILTER_RANGE:     return IDX_SITE_FILTER_RANGE;
    case PT_FILTER_RANGE_G:   return IDX_SITE_FILTER_RANGE; /* consult, falls back */
    case PT_FILTER_ZONE_NONE: return IDX_SITE_FILTER_ZONE;
    case PT_FILTER_ZONE_ALL:  return IDX_SITE_FILTER_ZONE;
    case PT_FILTER_BLOOM:     return IDX_SITE_FILTER_BLOOM;
    case PT_IN:               return IDX_SITE_IN;
    case PT_FIND:             return IDX_SITE_FIND;
    case PT_SORT:             return IDX_SITE_SORT;
    case PT_DISTINCT:         return IDX_SITE_DISTINCT;
    default:                  return IDX_SITE__N;
    }
}

/* ===== Per-point setup: attach index, update table column ===== */

static int point_attach(point_t p) {
    ray_t* v = NULL;
    const char* col = NULL;
    size_t clen = 0;

    switch (p) {
    case PT_FILTER_RANGE:
    case PT_FILTER_RANGE_G:
    case PT_SORT:
        v = g_col_k3; col = "k3"; clen = 2;
        ray_retain(v);
        if (RAY_IS_ERR(ray_index_attach_sort(&v))) {
            fprintf(stderr, "attach sort/k3 failed\n"); abort();
        }
        break;
    case PT_FILTER_ZONE_NONE:
    case PT_FILTER_ZONE_ALL:
        v = g_col_k2; col = "k2"; clen = 2;
        ray_retain(v);
        if (RAY_IS_ERR(ray_index_attach_zone(&v))) {
            fprintf(stderr, "attach zone/k2 failed\n"); abort();
        }
        break;
    case PT_FILTER_BLOOM:
        v = g_col_k; col = "k"; clen = 1;
        ray_retain(v);
        if (RAY_IS_ERR(ray_index_attach_bloom(&v))) {
            fprintf(stderr, "attach bloom/k failed\n"); abort();
        }
        break;
    case PT_IN:
        v = g_col_k; col = "k"; clen = 1;
        ray_retain(v);
        if (RAY_IS_ERR(ray_index_attach_hash(&v))) {
            fprintf(stderr, "attach hash/k(in) failed\n"); abort();
        }
        break;
    case PT_DISTINCT:
        v = g_col_k4; col = "k4"; clen = 2;
        ray_retain(v);
        if (RAY_IS_ERR(ray_index_attach_sort(&v))) {
            fprintf(stderr, "attach sort/k4 failed\n"); abort();
        }
        break;
    case PT_FIND:
        /* handled separately; point_attach not called for find */
        return 0;
    default:
        fprintf(stderr, "unknown point_attach %d\n", (int)p); abort();
    }
    tbl_set_col(col, clen, v);
    ray_release(v);   /* table holds its own ref */
    return 0;
}

/* Restore unindexed column for the baseline side */
static int point_drop(point_t p) {
    const char* col = NULL;
    size_t clen = 0;
    ray_t* base_vec = NULL;

    switch (p) {
    case PT_FILTER_RANGE:
    case PT_FILTER_RANGE_G:
    case PT_SORT:
        col = "k3"; clen = 2; base_vec = g_col_k3; break;
    case PT_FILTER_ZONE_NONE:
    case PT_FILTER_ZONE_ALL:
        col = "k2"; clen = 2; base_vec = g_col_k2; break;
    case PT_FILTER_BLOOM:
        col = "k"; clen = 1; base_vec = g_col_k; break;
    case PT_IN:
        col = "k"; clen = 1; base_vec = g_col_k; break;
    case PT_DISTINCT:
        col = "k4"; clen = 2; base_vec = g_col_k4; break;
    case PT_FIND:
        return 0;
    default:
        fprintf(stderr, "unknown point_drop %d\n", (int)p); abort();
    }
    tbl_set_col(col, clen, base_vec);
    return 0;
}

/* ===== Per-point query runners ===== */

/* Filter-based query template: FILTER(TABLE, col cmp_op const_val).
 * Verifies result nrows == expected_rows. */
static double run_filter_graph(const char* col_name,
                                uint16_t cmp_opcode,
                                int64_t const_val,
                                int64_t expected_rows) {
    ray_graph_t* g   = ray_graph_new(g_tbl);
    ray_op_t* tbl_nd = ray_const_table(g, g_tbl);
    ray_op_t* col    = ray_scan(g, col_name);
    ray_op_t* cst    = ray_const_i64(g, const_val);
    ray_op_t* pred   = ray_binop(g, cmp_opcode, col, cst);
    ray_op_t* filt   = ray_filter(g, tbl_nd, pred);
    ray_op_t* root   = ray_optimize(g, filt);

    double t0 = now_ms();
    ray_t* result  = ray_execute(g, root);
    double t1 = now_ms();

    if (!result || RAY_IS_ERR(result)) {
        fprintf(stderr, "filter exec failed (col=%s)\n", col_name); abort();
    }
    if (result->type != RAY_TABLE) {
        fprintf(stderr, "filter result not a table (col=%s type=%d)\n",
                col_name, result->type); abort();
    }
    int64_t got = ray_table_nrows(result);
    if (got != expected_rows) {
        fprintf(stderr, "row-count mismatch col=%s: got=%lld expected=%lld\n",
                col_name, (long long)got, (long long)expected_rows);
        abort();
    }
    ray_release(result);
    ray_graph_free(g);
    return t1 - t0;
}

/* find point: ray_index_find_row for present + absent keys.
 * Returns combined wall time for both probes.
 * When indexed, manually increments the consult counter so the
 * mechanism check can verify routing (ray_index_find_row itself does
 * not touch ray_idx_consults — that's done by collection.c's find_fn). */
static double run_find_point(ray_t* col_vec) {
    bool indexed = ray_index_has(col_vec);

    double t0 = now_ms();
    /* Present key: 42 is in [0..999999] */
    if (indexed) ray_idx_consults[IDX_SITE_FIND]++;
    int64_t row_present = ray_index_find_row(col_vec, 42LL);
    /* Absent key: 1000003 > 999999 */
    if (indexed) ray_idx_consults[IDX_SITE_FIND]++;
    int64_t row_absent  = ray_index_find_row(col_vec, 1000003LL);
    double t1 = now_ms();

    if (indexed) {
        if (row_present < 0) {
            fprintf(stderr, "find: present key row_id=%lld (expected >= 0)\n",
                    (long long)row_present);
            abort();
        }
        if (row_absent != -1) {
            fprintf(stderr, "find: absent key row_id=%lld (expected -1)\n",
                    (long long)row_absent);
            abort();
        }
    }
    return t1 - t0;
}

/* Sort point: ORDER BY k3 asc */
static double run_sort_point(void) {
    ray_graph_t* g   = ray_graph_new(g_tbl);
    ray_op_t* tbl_nd = ray_const_table(g, g_tbl);
    ray_op_t* k3_col = ray_scan(g, "k3");
    uint8_t desc = 0, nulls_first = 0;
    ray_op_t* sort = ray_sort_op(g, tbl_nd, &k3_col, &desc, &nulls_first, 1);
    ray_op_t* root = ray_optimize(g, sort);

    double t0 = now_ms();
    ray_t* result = ray_execute(g, root);
    double t1 = now_ms();

    if (!result || RAY_IS_ERR(result)) {
        fprintf(stderr, "sort exec failed\n"); abort();
    }
    ray_release(result);
    ray_graph_free(g);
    return t1 - t0;
}

/* Distinct point: distinct k4 (100k distinct values) */
static double run_distinct_point(void) {
    ray_graph_t* g = ray_graph_new(g_tbl);
    ray_op_t* k4_col = ray_scan(g, "k4");
    ray_op_t* dist   = ray_distinct_op(g, k4_col);
    ray_op_t* root   = ray_optimize(g, dist);

    double t0 = now_ms();
    ray_t* result = ray_execute(g, root);
    double t1 = now_ms();

    if (!result || RAY_IS_ERR(result)) {
        fprintf(stderr, "distinct exec failed\n"); abort();
    }
    if (!ray_is_vec(result) || result->len != 100000L) {
        fprintf(stderr, "distinct count mismatch: got=%lld (type=%d) expected=100000\n",
                (long long)(ray_is_vec(result) ? result->len : -1LL), result->type);
        abort();
    }
    ray_release(result);
    ray_graph_free(g);
    return t1 - t0;
}

/* IN point: FILTER(TABLE, IN(k, {7, 999999, 123456})) */
static double run_in_point(void) {
    int64_t set_data[] = { 7LL, 999999LL, 123456LL };
    ray_t* set_vec = ray_vec_from_raw(RAY_I64, set_data, 3);
    if (!set_vec || RAY_IS_ERR(set_vec)) {
        fprintf(stderr, "OOM making set_vec\n"); abort();
    }

    ray_graph_t* g   = ray_graph_new(g_tbl);
    ray_op_t* tbl_nd = ray_const_table(g, g_tbl);
    ray_op_t* col    = ray_scan(g, "k");
    ray_op_t* set    = ray_const_vec(g, set_vec);
    ray_op_t* pred   = ray_in(g, col, set);
    ray_op_t* filt   = ray_filter(g, tbl_nd, pred);
    ray_op_t* root   = ray_optimize(g, filt);

    double t0 = now_ms();
    ray_t* result = ray_execute(g, root);
    double t1 = now_ms();

    if (!result || RAY_IS_ERR(result)) {
        fprintf(stderr, "IN exec failed\n"); abort();
    }
    if (result->type != RAY_TABLE) {
        fprintf(stderr, "IN result not a table (type=%d)\n", result->type); abort();
    }
    /* Each of {7, 999999, 123456} appears 10 times → 30 rows */
    int64_t got = ray_table_nrows(result);
    if (got != 30L) {
        fprintf(stderr, "IN row-count mismatch: got=%lld expected=30\n",
                (long long)got);
        abort();
    }
    ray_release(result);
    ray_graph_free(g);
    ray_release(set_vec);
    return t1 - t0;
}

/* Dispatch one rep for point p.
 * indexed=1 → index attached; 0 → plain.
 * find_{indexed,plain}: pre-built vectors for the find point. */
static double run_one(point_t p, int indexed,
                      ray_t* find_col_indexed, ray_t* find_col_plain) {
    switch (p) {
    case PT_FILTER_RANGE:
        /* k3 < 100000 → 1% = 100k rows; k3 is permutation of 0..9999999 */
        return run_filter_graph("k3", OP_LT, 100000LL, 100000LL);
    case PT_FILTER_RANGE_G:
        /* k3 < 5000000 → 50% = 5M rows (> 10M/4 → sort guard fires, fallback) */
        return run_filter_graph("k3", OP_LT, 5000000LL, 5000000LL);
    case PT_FILTER_ZONE_NONE:
        /* k2 > 20000000 — NONE (max k2 = 999999, all < 20M) */
        return run_filter_graph("k2", OP_GT, 20000000LL, 0LL);
    case PT_FILTER_ZONE_ALL:
        /* k2 >= 0 — ALL rows */
        return run_filter_graph("k2", OP_GE, 0LL, NROWS);
    case PT_FILTER_BLOOM:
        /* k == 1000003 — absent (k in [0..999999]) */
        return run_filter_graph("k", OP_EQ, 1000003LL, 0LL);
    case PT_IN:
        return run_in_point();
    case PT_FIND:
        return run_find_point(indexed ? find_col_indexed : find_col_plain);
    case PT_SORT:
        return run_sort_point();
    case PT_DISTINCT:
        return run_distinct_point();
    default:
        fprintf(stderr, "unknown point %d in run_one\n", (int)p); abort();
    }
}

#define NREPS 9

int main(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* ---- system load note ---- */
    printf("=== bench-idx-route ===\n");
    fflush(stdout);
    printf("NROWS=%lld  NREPS=%d\n\n", (long long)NROWS, NREPS);
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

    /* ---- build table ---- */
    printf("Building 10M-row table...\n"); fflush(stdout);
    build_table();
    printf("Table built: %lld rows\n\n", (long long)ray_table_nrows(g_tbl));
    fflush(stdout);

    /* ---- attach costs (one-time, before interleaved reps) ---- */
    printf("Measuring attach costs...\n"); fflush(stdout);
    attach_costs_t ac = measure_attach_costs();
    printf("  zone  (k2)  : %.2f ms\n", ac.zone_ms);
    printf("  bloom (k)   : %.2f ms\n", ac.bloom_ms);
    printf("  hash  (k)   : %.2f ms\n", ac.hash_ms);
    printf("  sort  (k3)  : %.2f ms\n\n", ac.sort_k3_ms);
    fflush(stdout);

    /* ---- per-point interleaved reps ---- */
    /* timings[point][side][rep]: side 0=indexed, 1=plain */
    double timings[PT__N][2][NREPS];
    memset(timings, 0, sizeof(timings));

    /* Pre-build find-specific indexed column (k with hash index).
     * All other points route through g_tbl after point_attach/drop. */
    ray_t* find_col_indexed = g_col_k; ray_retain(find_col_indexed);
    if (RAY_IS_ERR(ray_index_attach_hash(&find_col_indexed))) {
        fprintf(stderr, "find pre-attach (hash on k) failed\n"); abort();
    }

    for (point_t p = 0; p < PT__N; p++) {
        printf("Running point %s (%d reps)...\n", point_name(p), NREPS);
        fflush(stdout);

        for (int rep = 0; rep < NREPS; rep++) {
            /* ---- indexed side ---- */
            uint64_t consult_before = ray_idx_consults[point_site(p)];

            if (p != PT_FIND) point_attach(p);
            timings[p][0][rep] = run_one(p, 1, find_col_indexed, g_col_k);
            if (p != PT_FIND) point_drop(p);

            /* Mechanism check: consult counter must have advanced */
            uint64_t consult_after = ray_idx_consults[point_site(p)];
            if (consult_after <= consult_before) {
                fprintf(stderr,
                    "MECHANISM FAILURE: point %s rep %d: "
                    "consult did not advance (before=%llu after=%llu)\n",
                    point_name(p), rep + 1,
                    (unsigned long long)consult_before,
                    (unsigned long long)consult_after);
                abort();
            }

            /* ---- plain (no-index) side ---- */
            timings[p][1][rep] = run_one(p, 0, find_col_indexed, g_col_k);
        }
    }
    ray_index_drop(&find_col_indexed);
    ray_release(find_col_indexed);

    /* ---- results table ---- */
    printf("\n");
    printf("%-16s  %14s  %14s  %12s\n",
           "point", "indexed_med_ms", "plain_med_ms", "delta_ms");
    printf("%-16s  %14s  %14s  %12s\n",
           "----------------", "--------------", "--------------", "------------");
    for (point_t p = 0; p < PT__N; p++) {
        double med_idx   = median(timings[p][0], NREPS);
        double med_plain = median(timings[p][1], NREPS);
        double delta     = med_idx - med_plain;
        printf("%-16s  %14.3f  %14.3f  %12.3f\n",
               point_name(p), med_idx, med_plain, delta);
    }

    /* ---- attach cost summary ---- */
    printf("\nAttach costs (one-time, not included in query medians):\n");
    printf("  zone  (k2)  : %.2f ms\n", ac.zone_ms);
    printf("  bloom (k)   : %.2f ms\n", ac.bloom_ms);
    printf("  hash  (k)   : %.2f ms\n", ac.hash_ms);
    printf("  sort  (k3)  : %.2f ms\n", ac.sort_k3_ms);

    printf("\nMechanism: consult counter verified each indexed rep (aborts on failure)\n");

    /* ---- raw per-rep numbers ---- */
    printf("\n--- raw per-rep ms (indexed) ---\n");
    printf("%-16s", "point");
    for (int r = 0; r < NREPS; r++) printf("   rep%d", r + 1);
    printf("\n");
    for (point_t p = 0; p < PT__N; p++) {
        printf("%-16s", point_name(p));
        for (int r = 0; r < NREPS; r++) printf("  %7.2f", timings[p][0][r]);
        printf("\n");
    }
    printf("\n--- raw per-rep ms (plain) ---\n");
    printf("%-16s", "point");
    for (int r = 0; r < NREPS; r++) printf("   rep%d", r + 1);
    printf("\n");
    for (point_t p = 0; p < PT__N; p++) {
        printf("%-16s", point_name(p));
        for (int r = 0; r < NREPS; r++) printf("  %7.2f", timings[p][1][r]);
        printf("\n");
    }

    ray_release(g_col_k);
    ray_release(g_col_k2);
    ray_release(g_col_k3);
    ray_release(g_col_k4);
    ray_release(g_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}

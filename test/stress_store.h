/*
 * stress_store.h — shared engine for parted/splayed shared-symfile stress
 * tests.  Fixture mirrors the production layout: one db root with a
 * date-parted table `hist`, a splayed table `live`, and a single shared
 * symfile `db_root/sym`.  An independent shadow model (plain malloc, syms
 * as strings) is the oracle; see test_stress_matrix.c / test_stress_random.c.
 */

#ifndef RAY_TEST_STRESS_STORE_H
#define RAY_TEST_STRESS_STORE_H

#include <rayforce.h>
#include <stdbool.h>
#include <stdint.h>

#define STRESS_SYM_MAX    32    /* ticker string buffer (incl. NUL) */
#define STRESS_MAX_PARTS  16    /* fixture partition limit */
#define STRESS_POOL_CAP   256   /* ring of known ticker names */
#define STRESS_OPLOG_CAP  8192  /* op log ring */

typedef struct {
    char    ticker[STRESS_SYM_MAX];
    double  price;
    int64_t qty;
} stress_row_t;

typedef struct {
    stress_row_t* rows;
    int64_t       len;
    int64_t       cap;
} stress_rows_t;

typedef enum {
    STRESS_SYMS_NEW,       /* every ticker freshly generated (forces interning) */
    STRESS_SYMS_EXISTING,  /* tickers drawn from the already-used pool */
    STRESS_SYMS_MIXED,     /* ~50/50 new vs existing */
    STRESS_SYMS_NULLS,     /* ~50% fully-null rows (sym 0 / NULL_F64 / NULL_I64) */
} stress_sym_pattern_t;

typedef struct {
    char          db_root[256];
    char          sym_path[300];
    stress_rows_t live;                              /* shadow of db_root/live */
    char          part_dates[STRESS_MAX_PARTS][16];  /* "YYYY.MM.DD" */
    stress_rows_t parts[STRESS_MAX_PARTS];           /* shadow per partition */
    int           nparts;
    uint64_t      rng;
    uint64_t      seed;
    int64_t       last_sym_count;  /* symfile monotonicity watermark */
    int           sym_uniq;        /* counter for fresh ticker names */
    char          pool[STRESS_POOL_CAP][STRESS_SYM_MAX];
    int           pool_len;
    char        (*oplog)[128];
    int           oplog_len;
    bool          failed;          /* set on verify failure; keeps db dir */
} stress_ctx_t;

/* lifecycle */
bool stress_init(stress_ctx_t* c, const char* db_root, uint64_t seed);
void stress_destroy(stress_ctx_t* c);  /* rm -rf db_root unless c->failed */

/* seed the fixture: live table + nparts date partitions, persisted + verified
 * loadable.  Must be the first op after stress_init. */
bool stress_seed_initial(stress_ctx_t* c, int64_t live_rows, int nparts,
                         int64_t rows_per_part);

/* destructive ops — return false on unexpected API error (test FAILs).
 * part_idx: -1 = live splayed table, 0..nparts-1 = that partition. */
bool stress_op_insert(stress_ctx_t* c, int64_t n, stress_sym_pattern_t pat,
                      bool bulk, bool via_mmap);
bool stress_op_upsert(stress_ctx_t* c, stress_sym_pattern_t pat, bool via_mmap);
bool stress_op_trim(stress_ctx_t* c, int part_idx, bool tail, int64_t n);
bool stress_op_part_append(stress_ctx_t* c, int part_idx, int64_t n,
                           stress_sym_pattern_t pat);
bool stress_op_part_new(stress_ctx_t* c, int64_t n, stress_sym_pattern_t pat);

/* vocabulary growth: insert n rows with FRESH tickers into the live
 * table.  Thin alias over stress_op_insert(STRESS_SYMS_NEW) — fresh-sym
 * interning through a save IS the growth mechanism (the save merges the
 * new distinct set into the shared symfile and re-derives the on-disk
 * column width from the grown vocabulary).  Named so width-transition
 * tests read as intent. */
bool stress_op_sym_grow(stress_ctx_t* c, int64_t n);

/* simulated process restart: destroy + re-init the global sym table, then
 * re-load db_root/sym from disk.  Invalidates every live sym ID. */
bool stress_op_restart(stress_ctx_t* c);

/* verification — true if clean.  On mismatch: dumps seed + op log to
 * stderr, sets c->failed.  use_mmap picks ray_read_splayed vs
 * ray_splay_load for the per-directory reloads. */
bool stress_verify_all(stress_ctx_t* c, bool use_mmap);
/* Precondition: stress_seed_initial has run (the shared symfile exists);
 * calling earlier reports a confusing "symfile missing" failure. */
bool stress_check_invariants(stress_ctx_t* c);

/* prng (xorshift64*), exposed for the random runner */
uint64_t stress_rand(stress_ctx_t* c);

#endif /* RAY_TEST_STRESS_STORE_H */

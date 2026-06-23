/*
 * stress_store.h — shared engine for parted/splayed shared-symfile stress
 * tests.  Fixture mirrors the production layout: one db root with a
 * date-parted table `hist`, a splayed table `live`, and a single shared
 * symfile `db_root/.sym`.  An independent shadow model (plain malloc, syms
 * as strings) is the oracle; see test_stress_matrix.c / test_stress_random.c.
 */

#ifndef RAY_TEST_STRESS_STORE_H
#define RAY_TEST_STRESS_STORE_H

#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>
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

/* Per-process scratch db path: "/tmp/rayforce_stress_<name>.<pid>".
 * Each stress test rm -rf's its db_root in stress_init, so two test
 * binaries sharing a fixed path race destructively (one deletes the
 * other's dir mid-save → spurious CORRUPT/IO failures).  The pid suffix
 * keeps concurrent runs isolated.  Returns a pointer to a static buffer
 * reused on every call; stress_init copies the string immediately, so
 * back-to-back calls (e.g. two fixtures in one test) are safe. */
const char* stress_db_path(const char* name);

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
 * re-load db_root/.sym from disk.  Invalidates every live sym ID. */
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

/* ---- engine internals exported for the eval driver (stress_eval.c) ----
 * Minimal surface so eval-mode executors REUSE the shadow logic (row
 * generation, shadow mutation, path layout, op log, failure dump) and the
 * two drivers cannot drift.  Everything else in stress_store.c stays
 * static. */

#if defined(__GNUC__) || defined(__clang__)
#define STRESS_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define STRESS_PRINTF(a, b)
#endif

/* row generation (consumes the ctx rng; updates the ticker pool) */
void stress_gen_row(stress_ctx_t* c, stress_sym_pattern_t pat,
                    stress_row_t* out);

/* shadow row-array mutations (plain malloc, oracle side) */
bool stress_rows_append(stress_rows_t* r, const stress_row_t* row);
void stress_rows_trim(stress_rows_t* r, bool tail, int64_t n);

/* upsert key semantics: index of FIRST row matching ticker, or -1 — the
 * language's upsert updates the first key hit (query.c), and both drivers
 * follow the language. */
int64_t stress_find_first_by_ticker(const stress_rows_t* rows,
                                    const char* ticker);

/* fixture path layout */
void stress_live_dir(const stress_ctx_t* c, char* buf, size_t n);
void stress_part_dir(const stress_ctx_t* c, int i, char* buf, size_t n);

/* op log + failure dump (dump sets c->failed, keeps the db dir) */
void stress_op_logf(stress_ctx_t* c, const char* fmt, ...)
    STRESS_PRINTF(2, 3);
void stress_dump_failure(stress_ctx_t* c, const char* fmt, ...)
    STRESS_PRINTF(2, 3);

#endif /* RAY_TEST_STRESS_STORE_H */

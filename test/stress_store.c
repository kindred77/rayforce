/*
 * stress_store.c — engine for parted/splayed shared-symfile stress tests.
 * Shadow model uses plain malloc/free so the oracle never depends on the
 * Rayforce heap under test.
 */

#include "stress_store.h"
#include "store/splay.h"
#include "store/part.h"
#include "table/sym.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- prng (xorshift64*) ------------------------------------------------ */

uint64_t stress_rand(stress_ctx_t* c) {
    uint64_t x = c->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    c->rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ---- op log + failure dump --------------------------------------------- */

static void op_logf(stress_ctx_t* c, const char* fmt, ...) {
    if (!c->oplog) return;
    char* slot = c->oplog[c->oplog_len % STRESS_OPLOG_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot, 128, fmt, ap);
    va_end(ap);
    c->oplog_len++;
}

__attribute__((unused)) /* used by later tasks */
static void dump_failure(stress_ctx_t* c, const char* fmt, ...) {
    char why[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n=== STRESS FAILURE: %s\n", why);
    fprintf(stderr, "=== seed=%llu  db=%s (left on disk for inspection)\n",
            (unsigned long long)c->seed, c->db_root);
    int total = c->oplog_len;
    int n     = total < STRESS_OPLOG_CAP ? total : STRESS_OPLOG_CAP;
    fprintf(stderr, "=== op log (last %d of %d ops):\n", n, total);
    for (int i = total - n; i < total; i++)
        fprintf(stderr, "  %5d: %s\n", i, c->oplog[i % STRESS_OPLOG_CAP]);
    c->failed = true;
}

/* ---- shadow row arrays (plain malloc — independent of rayforce heap) ---- */

static bool rows_reserve(stress_rows_t* r, int64_t need) {
    if (need <= r->cap) return true;
    int64_t cap = r->cap ? r->cap : 64;
    while (cap < need) cap *= 2;
    stress_row_t* p = (stress_row_t*)realloc(r->rows,
                                             (size_t)cap * sizeof(stress_row_t));
    if (!p) return false;
    r->rows = p;
    r->cap  = cap;
    return true;
}

static bool rows_append(stress_rows_t* r, const stress_row_t* row) {
    if (!rows_reserve(r, r->len + 1)) return false;
    r->rows[r->len++] = *row;
    return true;
}

/* used by later tasks */
__attribute__((unused))
static void rows_trim(stress_rows_t* r, bool tail, int64_t n) {
    if (n > r->len) n = r->len;
    if (!tail)
        memmove(r->rows, r->rows + n,
                (size_t)(r->len - n) * sizeof(stress_row_t));
    r->len -= n;
}

static void rows_free(stress_rows_t* r) {
    free(r->rows);
    r->rows = NULL;
    r->len = r->cap = 0;
}

/* ---- row generation ----------------------------------------------------- */

__attribute__((unused)) /* used by later tasks */
static bool row_is_null(const stress_row_t* row) {
    return row->ticker[0] == '\0';
}

static void gen_row(stress_ctx_t* c, stress_sym_pattern_t pat,
                    stress_row_t* out) {
    memset(out, 0, sizeof(*out));
    if (pat == STRESS_SYMS_NULLS && (stress_rand(c) & 1)) {
        out->ticker[0] = '\0';        /* SYM null = sym 0, the empty string */
        out->price     = NULL_F64;
        out->qty       = NULL_I64;
        return;
    }
    bool fresh = (pat == STRESS_SYMS_NEW) || c->pool_len == 0 ||
                 ((pat == STRESS_SYMS_MIXED || pat == STRESS_SYMS_NULLS) &&
                  (stress_rand(c) & 1));
    if (fresh) {
        snprintf(out->ticker, STRESS_SYM_MAX, "tkr%06d", c->sym_uniq++);
        memcpy(c->pool[c->pool_len % STRESS_POOL_CAP], out->ticker,
               STRESS_SYM_MAX);
        c->pool_len++;
    } else {
        int span = c->pool_len < STRESS_POOL_CAP ? c->pool_len
                                                 : STRESS_POOL_CAP;
        memcpy(out->ticker, c->pool[stress_rand(c) % (uint64_t)span],
               STRESS_SYM_MAX);
    }
    out->price = (double)(stress_rand(c) % 1000000) / 100.0;
    out->qty   = (int64_t)(stress_rand(c) % 100000);
}

/* ---- paths --------------------------------------------------------------- */

static void live_dir(const stress_ctx_t* c, char* buf, size_t n) {
    snprintf(buf, n, "%s/live", c->db_root);
}

static void part_dir(const stress_ctx_t* c, int i, char* buf, size_t n) {
    snprintf(buf, n, "%s/%s/hist", c->db_root, c->part_dates[i]);
}

static void rm_rf(const char* path) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

/* ---- ray table <-> shadow rows ------------------------------------------ */

/* Build a fresh in-memory table from a row array.  Interns tickers.
 * Caller releases the returned table. */
static ray_t* build_table_from_rows(const stress_rows_t* rows) {
    int64_t cap   = rows->len > 0 ? rows->len : 1;
    ray_t*  tick  = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t*  price = ray_vec_new(RAY_F64, cap);
    ray_t*  qty   = ray_vec_new(RAY_I64, cap);
    if (!tick || !price || !qty || RAY_IS_ERR(tick) || RAY_IS_ERR(price) ||
        RAY_IS_ERR(qty)) {
        if (tick)  ray_release(tick);
        if (price) ray_release(price);
        if (qty)   ray_release(qty);
        return NULL;
    }
    for (int64_t i = 0; i < rows->len; i++) {
        const stress_row_t* r = &rows->rows[i];
        int64_t id = ray_sym_intern(r->ticker, strlen(r->ticker));
        tick  = ray_vec_append(tick, &id);
        price = ray_vec_append(price, &r->price);
        qty   = ray_vec_append(qty, &r->qty);
        if (!tick || RAY_IS_ERR(tick) || !price || RAY_IS_ERR(price) ||
            !qty || RAY_IS_ERR(qty)) {
            if (tick)  ray_release(tick);
            if (price) ray_release(price);
            if (qty)   ray_release(qty);
            return NULL;
        }
    }
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, ray_sym_intern("ticker", 6), tick);
    tbl = ray_table_add_col(tbl, ray_sym_intern("price", 5), price);
    tbl = ray_table_add_col(tbl, ray_sym_intern("qty", 3), qty);
    ray_release(tick);   /* add_col retained its own refs */
    ray_release(price);
    ray_release(qty);
    if (RAY_IS_ERR(tbl)) {
        ray_release(tbl);
        return NULL;
    }
    return tbl;
}

/* Extract a loaded table's content into a plain row array, resolving sym
 * IDs to strings through the CURRENT global sym table.  Returns false on
 * structural problems (wrong cols, unresolvable sym). */
/* used by later tasks */
__attribute__((unused))
static bool extract_rows(stress_ctx_t* c, ray_t* tbl, stress_rows_t* out) {
    if (!tbl || RAY_IS_ERR(tbl)) return false;
    if (ray_table_ncols(tbl) != 3) {
        op_logf(c, "extract: ncols=%lld != 3", (long long)ray_table_ncols(tbl));
        return false;
    }
    ray_t* tick  = ray_table_get_col_idx(tbl, 0); /* borrowed */
    ray_t* price = ray_table_get_col_idx(tbl, 1);
    ray_t* qty   = ray_table_get_col_idx(tbl, 2);
    if (!tick || !price || !qty) return false;
    int64_t n = tick->len;
    if (price->len != n || qty->len != n) {
        op_logf(c, "extract: col length mismatch %lld/%lld/%lld",
                (long long)n, (long long)price->len, (long long)qty->len);
        return false;
    }
    if (!rows_reserve(out, n)) return false;
    const void*    td = ray_data(tick);
    const double*  pd = (const double*)ray_data(price);
    const int64_t* qd = (const int64_t*)ray_data(qty);
    for (int64_t i = 0; i < n; i++) {
        stress_row_t* r = &out->rows[i];
        memset(r, 0, sizeof(*r));
        int64_t id = ray_read_sym(td, i, RAY_SYM, tick->attrs);
        ray_t* s = ray_sym_str(id); /* borrowed, do not release */
        if (!s) {
            op_logf(c, "extract: sym id %lld unresolvable at row %lld",
                    (long long)id, (long long)i);
            return false;
        }
        size_t len = ray_str_len(s);
        if (len >= STRESS_SYM_MAX) len = STRESS_SYM_MAX - 1;
        memcpy(r->ticker, ray_str_ptr(s), len);
        r->ticker[len] = '\0';
        r->price = pd[i];
        r->qty   = qd[i];
    }
    out->len = n;
    return true;
}

/* Persist a row array as a splayed dir, merging syms into the shared
 * symfile. */
static bool save_rows(stress_ctx_t* c, const char* dir,
                      const stress_rows_t* rows, bool bulk) {
    ray_t* tbl = build_table_from_rows(rows);
    if (!tbl) {
        op_logf(c, "save_rows: build failed for %s", dir);
        return false;
    }
    ray_err_t e = bulk ? ray_splay_save_bulk(tbl, dir, c->sym_path)
                       : ray_splay_save(tbl, dir, c->sym_path);
    ray_release(tbl);
    if (e != RAY_OK) {
        op_logf(c, "save_rows: save err=%d for %s", (int)e, dir);
        return false;
    }
    return true;
}

/* Load one splayed dir (heap or mmap) against the shared symfile. */
/* used by later tasks */
__attribute__((unused))
static ray_t* load_dir(stress_ctx_t* c, const char* dir, bool use_mmap) {
    return use_mmap ? ray_read_splayed(dir, c->sym_path)
                    : ray_splay_load(dir, c->sym_path);
}

/* ---- lifecycle ----------------------------------------------------------- */

bool stress_init(stress_ctx_t* c, const char* db_root, uint64_t seed) {
    memset(c, 0, sizeof(*c));
    snprintf(c->db_root, sizeof(c->db_root), "%s", db_root);
    snprintf(c->sym_path, sizeof(c->sym_path), "%s/sym", db_root);
    c->seed = seed;
    c->rng  = seed | 1; /* xorshift state must be nonzero */
    c->oplog = (char(*)[128])malloc((size_t)STRESS_OPLOG_CAP * 128);
    if (!c->oplog) return false;
    rm_rf(c->db_root);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", c->db_root);
    if (system(cmd) != 0) {
        free(c->oplog);
        c->oplog = NULL;
        return false;
    }
    op_logf(c, "init seed=%llu db=%s", (unsigned long long)seed, db_root);
    return true;
}

void stress_destroy(stress_ctx_t* c) {
    rows_free(&c->live);
    for (int i = 0; i < c->nparts; i++) rows_free(&c->parts[i]);
    free(c->oplog);
    c->oplog = NULL;
    if (!c->failed) rm_rf(c->db_root);
}

/* ---- initial seeding ------------------------------------------------------ */

bool stress_seed_initial(stress_ctx_t* c, int64_t live_rows, int nparts,
                         int64_t rows_per_part) {
    if (nparts > STRESS_MAX_PARTS) return false;
    op_logf(c, "seed live=%lld nparts=%d rpp=%lld", (long long)live_rows,
            nparts, (long long)rows_per_part);
    stress_row_t row;
    for (int64_t i = 0; i < live_rows; i++) {
        gen_row(c, STRESS_SYMS_MIXED, &row);
        if (!rows_append(&c->live, &row)) return false;
    }
    char dir[512];
    live_dir(c, dir, sizeof(dir));
    if (!save_rows(c, dir, &c->live, false)) return false;
    for (int p = 0; p < nparts; p++) {
        snprintf(c->part_dates[p], sizeof(c->part_dates[p]), "2024.01.%02d",
                 p + 1);
        for (int64_t i = 0; i < rows_per_part; i++) {
            gen_row(c, STRESS_SYMS_MIXED, &row);
            if (!rows_append(&c->parts[p], &row)) return false;
        }
        part_dir(c, p, dir, sizeof(dir));
        if (!save_rows(c, dir, &c->parts[p], false)) return false;
    }
    c->nparts = nparts;
    return true;
}

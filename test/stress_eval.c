/*
 * stress_eval.c — Rayfall eval driver for the stress engine
 * (stress_store.c).  One engine, two drivers: each eval op builds Rayfall
 * source from the SAME stress_gen_row output that feeds the shadow, runs
 * it via ray_eval_str, and applies the identical logical mutation to the
 * shadow.  The phase-1 C verifier (stress_verify_all) is shared.
 *
 * Verified syntax sources of truth (never guessed):
 *   - null literals (test/rfl/null/ + REPL probe): sym null = `0Ns`
 *     (the empty symbol — SYM columns are no-null by design, sym 0 = "");
 *     f64 null = `0Nf`; i64 null = `0Nl`.  All three are accepted inside
 *     plain vector literals, including in leading position.
 *   - the .db trio (test/rfl/system/db_get.rfl, db_sym_resolution.rfl):
 *     `(.db.splayed.set "<dir>" <table> "<root>/sym")` — the EXPLICIT
 *     third argument is required for the shared root symfile (no
 *     root-walk for plain dirs post-flip).
 */

#include "stress_eval.h"
#include <rayforce.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- runtime lifecycle ---------------------------------------------------
 * ray_runtime_create() itself runs ray_heap_init() + ray_sym_init(), and
 * ray_runtime_destroy() runs ray_sym_destroy() + ray_heap_destroy()
 * (src/core/runtime.c) — so suites pair runtime_up/runtime_down ALONE,
 * with no separate heap/sym setup (mirrors test_domain.c's
 * domain_rt_setup/teardown). */

static ray_runtime_t* g_rt;

bool stress_eval_runtime_up(void) {
    if (g_rt) return true;
    g_rt = ray_runtime_create(0, NULL);
    return g_rt != NULL;
}

void stress_eval_runtime_down(void) {
    if (!g_rt) return;
    ray_runtime_destroy(g_rt);
    g_rt = NULL;
}

/* ---- eval ----------------------------------------------------------------- */

bool stress_eval_exec(stress_ctx_t* c, const char* src) {
    /* Log every source line first (op_logf truncates to the 128-byte log
     * line) so any failure is replayable by hand in the REPL. */
    for (const char* p = src; *p;) {
        const char* nl  = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        stress_op_logf(c, "eval %.*s", (int)len, p);
        p += len + (nl ? 1 : 0);
    }
    if (!g_rt) {
        stress_dump_failure(c, "eval: no runtime (stress_eval_runtime_up "
                               "not called?)");
        return false;
    }
    ray_t* r = ray_eval_str(src);  /* NULL = void/null result, fine */
    if (r && RAY_IS_ERR(r)) {
        stress_dump_failure(c, "eval error '%s' for: %.300s",
                            ray_err_code(r), src);
        ray_release(r);
        return false;
    }
    if (r) ray_release(r);
    return true;
}

/* ---- float-exactness discipline -------------------------------------------
 * The shadow must hold EXACTLY the double the language will produce from
 * the generated source, or the verifier's bit-for-bit price compare (and
 * later the query oracle's sums) diverge in ULPs.  Discipline: prices are
 * emitted with one fixed format (PRICE_FMT), and a generated row's shadow
 * price is strtod() OF THAT EMITTED TEXT (canon_row_price below, applied
 * to every generated row before it reaches the shadow).  The table builder
 * re-emits PRICE_FMT from the stored double; format→parse→format is a
 * fixed point over the generated range (0.00..9999.99: the parsed double
 * is within half an ulp of the decimal value, far closer than half a
 * hundredth), so source text and shadow agree bit-for-bit. */

#define PRICE_FMT "%.2f"

static void canon_row_price(stress_row_t* r) {
    if (isnan(r->price)) return;  /* null price stays NULL_F64 */
    char txt[64];
    snprintf(txt, sizeof(txt), PRICE_FMT, r->price);
    r->price = strtod(txt, NULL);
}

/* ---- table-literal builder ------------------------------------------------ */

/* snprintf-append into buf at *off; false if it would not fit. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static bool emit(char* buf, size_t bufsz, size_t* off, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, bufsz - *off, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= bufsz - *off) return false;
    *off += (size_t)w;
    return true;
}

bool stress_eval_table_src(const stress_rows_t* rows, int64_t from, int64_t n,
                           char* buf, size_t bufsz) {
    if (from < 0 || n < 0 || from + n > rows->len) return false;
    size_t off = 0;
    if (!emit(buf, bufsz, &off, "(table [ticker price qty] (list ")) return false;
    /* empty slice: typed empty columns (a bare [] is I64) */
    if (n == 0)
        return emit(buf, bufsz, &off,
                    "(as 'SYM []) (as 'F64 []) []))");
    /* ticker column: 'sym literals; null ticker ("") = 0Ns */
    if (!emit(buf, bufsz, &off, "[")) return false;
    for (int64_t i = 0; i < n; i++) {
        const stress_row_t* r = &rows->rows[from + i];
        const char* sp = i ? " " : "";
        if (r->ticker[0] == '\0') {
            if (!emit(buf, bufsz, &off, "%s0Ns", sp)) return false;
        } else {
            if (!emit(buf, bufsz, &off, "%s'%s", sp, r->ticker)) return false;
        }
    }
    /* price column: PRICE_FMT (see discipline above); null = 0Nf */
    if (!emit(buf, bufsz, &off, "] [")) return false;
    for (int64_t i = 0; i < n; i++) {
        const stress_row_t* r = &rows->rows[from + i];
        const char* sp = i ? " " : "";
        if (isnan(r->price)) {
            if (!emit(buf, bufsz, &off, "%s0Nf", sp)) return false;
        } else {
            if (!emit(buf, bufsz, &off, "%s" PRICE_FMT, sp, r->price))
                return false;
        }
    }
    /* qty column; null = 0Nl */
    if (!emit(buf, bufsz, &off, "] [")) return false;
    for (int64_t i = 0; i < n; i++) {
        const stress_row_t* r = &rows->rows[from + i];
        const char* sp = i ? " " : "";
        if (r->qty == NULL_I64) {
            if (!emit(buf, bufsz, &off, "%s0Nl", sp)) return false;
        } else {
            if (!emit(buf, bufsz, &off, "%s%lld", sp, (long long)r->qty))
                return false;
        }
    }
    return emit(buf, bufsz, &off, "]))");
}

/* Conservative source-buffer size for a slice of n rows wrapped in one
 * surrounding form (set call + two quoted paths). */
static size_t src_bufsz(int64_t n) {
    /* per row: ticker (≤31+2) + price (≤8) + qty (≤21) + separators */
    return 2048 + (size_t)n * (3 * STRESS_SYM_MAX + 16);
}

/* (.db.splayed.set "<dir>" <table literal> "<shared sym>") — the explicit
 * third argument shares the root symfile (client layout; see header
 * comment). */
static bool eval_set_dir(stress_ctx_t* c, const char* dir,
                         const stress_rows_t* rows) {
    size_t cap = src_bufsz(rows->len);
    char*  src = (char*)malloc(cap);
    if (!src) return false;
    size_t off = 0;
    bool ok = emit(src, cap, &off, "(.db.splayed.set \"%s\" ", dir);
    if (ok) {
        ok = stress_eval_table_src(rows, 0, rows->len, src + off, cap - off);
        off += strlen(src + off);
    }
    if (ok) ok = emit(src, cap, &off, " \"%s\")", c->sym_path);
    if (!ok)
        stress_op_logf(c, "eval_set_dir: source buffer too small for %s", dir);
    if (ok) ok = stress_eval_exec(c, src);
    free(src);
    return ok;
}

/* ---- initial seeding -------------------------------------------------------
 * Shadow exactly like the engine's stress_seed_initial (same patterns, same
 * rng consumption, same dates); real side through generated Rayfall. */

bool stress_eval_seed_initial(stress_ctx_t* c, int64_t live_rows, int nparts,
                              int64_t rows_per_part) {
    if (nparts > STRESS_MAX_PARTS) return false;
    stress_op_logf(c, "eval-seed live=%lld nparts=%d rpp=%lld",
                   (long long)live_rows, nparts, (long long)rows_per_part);
    stress_row_t row;
    for (int64_t i = 0; i < live_rows; i++) {
        stress_gen_row(c, STRESS_SYMS_MIXED, &row);
        canon_row_price(&row);
        if (!stress_rows_append(&c->live, &row)) return false;
    }
    char dir[512];
    stress_live_dir(c, dir, sizeof(dir));
    if (!eval_set_dir(c, dir, &c->live)) return false;
    for (int p = 0; p < nparts; p++) {
        snprintf(c->part_dates[p], sizeof(c->part_dates[p]), "2024.01.%02d",
                 (p + 1) % 100); /* % keeps -Wformat-truncation in range */
        for (int64_t i = 0; i < rows_per_part; i++) {
            stress_gen_row(c, STRESS_SYMS_MIXED, &row);
            canon_row_price(&row);
            if (!stress_rows_append(&c->parts[p], &row)) return false;
        }
        stress_part_dir(c, p, dir, sizeof(dir));
        if (!eval_set_dir(c, dir, &c->parts[p])) return false;
    }
    c->nparts = nparts;
    return true;
}

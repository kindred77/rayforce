# Aggregation Engine Redesign — Phase 0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit the design/plan docs under `docs/design/` (standing user preference).

**Goal:** Build the composable accumulator interface + registry + a differential harness that proves vtable accumulators are byte-equivalent to the current reduction functions — sitting *beside* the live engine, wired into nothing, so it carries zero risk.

**Architecture:** New flat files in `src/ops/` (`agg_acc.h`, `agg_registry.{h,c}`, `agg_stream.c`) defining `agg_vtable_t` keyed by `(agg_kind, in_type)`. Each accumulator declares minimal state + `init/update_batch/merge/finalize`. A C unit test drives each accumulator over a single group and asserts the finalized result equals the existing `ray_*_fn` reduction — the built-in oracle. No planner/executor changes.

**Tech Stack:** C (C11), the in-repo test driver (`make test` → `./rayforce.test`), existing kernels in `src/ops/group.c` as the reference to mirror.

**Reference (design):** `docs/design/2026-06-14-aggregation-engine-redesign-design.md` §3, §4, §6, §9.

**Scope note:** This plan is Phase 0 of §10 of the design. Phase 1 (agg_engine + radix strategy + buffered-eval oracle accumulator), Phase 2 (direct-array + selector), Phase 3 (delete the rowform zoo), and Phase 4 (re-baseline + flip default) are separate plans, each producing working, testable software.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_acc.h` | Create | The `agg_vtable_t` interface, `acc_kind_t`, `ray_valid_t` + `ray_valid_at`, `acc_arena_t` (opaque, stubbed for Phase 0). |
| `src/ops/agg_registry.h` | Create | `agg_resolve(uint16_t agg_kind, int8_t in_type) → const agg_vtable_t*`. |
| `src/ops/agg_stream.c` | Create | Streaming accumulators: `sum/count/min/max` for I64 and F64, plus the `agg_resolve` table (Phase 0 is a single .c; no separate registry.c). |
| `test/test_agg_registry.c` | Create | Differential harness: each registered `(agg, type)` vtable vs the current `ray_*_fn` over random columns, with and without nulls. |
| `test/main.c` | Modify | Register the new suite (extern decl near line 105–176; array entry near line 179+). |

All new `src/ops/*.c` are picked up automatically by `LIB_SRC = $(wildcard src/*/*.c)`; `test/*.c` by `TEST_SRC = $(wildcard test/*.c)`. No Makefile edits.

**Reference patterns to mirror:**
- `reduce_acc_t` + `reduce_acc_init` — `src/ops/group.c:46–48`.
- `REDUCE_LOOP_I` (i64 sentinel skip) — `src/ops/group.c:175–194`; `REDUCE_LOOP_F` (NaN skip) — `:198–211`.
- Building a tiny table/vector in a test — `test/test_fused_group.c:46–60` (`ray_vec_new`, `ray_data`, `memcpy`, `ray_release`).
- Scalar reductions to diff against — `ray_sum_fn` / `ray_min_fn` / `ray_max_fn` / `ray_avg_fn` dispatched in `src/ops/group.c:2050–2056`.

---

## Task 1: Accumulator interface header

**Files:**
- Create: `src/ops/agg_acc.h`

- [ ] **Step 1: Write the header**

```c
/* src/ops/agg_acc.h — composable accumulator interface (design §4).
 * Aggregates are referenced ONLY through this vtable; adding an aggregate
 * is registering one vtable, never a new operator + gates. */
#ifndef RAY_OPS_AGG_ACC_H
#define RAY_OPS_AGG_ACC_H

#include <rayforce.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Per-column validity view, derived ONCE by the engine and honored by every
 * accumulator (design §4.6, §6 option A). Sentinel representation is unchanged;
 * what changes is that null detection is centralized here, not attr-gated per
 * kernel. has_nulls=false means the no-null fast path is provable for this batch. */
typedef struct {
    const void* base;     /* column data base pointer */
    int8_t      type;     /* RAY_I64 / RAY_F64 / ... */
    bool        has_nulls;
} ray_valid_t;

/* True iff row is a live (non-null) value. Mirrors the sentinel checks in
 * group.c REDUCE_LOOP_I/REDUCE_LOOP_F. */
static inline bool ray_valid_at(const ray_valid_t* v, int64_t row) {
    if (!v->has_nulls) return true;
    switch (v->type) {
        case RAY_I64: case RAY_TIMESTAMP:
            return ((const int64_t*)v->base)[row] != NULL_I64;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            return ((const int32_t*)v->base)[row] != NULL_I32;
        case RAY_I16:
            return ((const int16_t*)v->base)[row] != NULL_I16;
        case RAY_F64: {
            double d = ((const double*)v->base)[row];
            return d == d;  /* only NaN fails self-equality */
        }
        default:
            return true;    /* BOOL/U8 etc. are non-nullable */
    }
}

/* Per-group side storage for ACC_BUFFERED accumulators (median/top-k).
 * Opaque + unused in Phase 0 — only streaming accumulators ship here. */
typedef struct acc_arena acc_arena_t;

typedef enum { ACC_STREAMING, ACC_BUFFERED } acc_kind_t;

typedef struct {
    uint16_t   state_size;   /* bytes of inline per-group state */
    acc_kind_t kind;
    int8_t     out_type;     /* result column/atom type */
    void   (*init)        (void* state);
    /* For row i in [0,n): apply vals[i] to group gids[i].
     * The group's inline state is at states_base + gids[i]*stride. */
    void   (*update_batch)(void* states_base, size_t stride,
                           const uint32_t* gids, const void* vals,
                           const ray_valid_t* valid, int64_t n,
                           acc_arena_t* arena);
    void   (*merge)       (void* dst, const void* src, acc_arena_t* arena);
    ray_t* (*finalize)    (const void* state, acc_arena_t* arena);
} agg_vtable_t;

#endif /* RAY_OPS_AGG_ACC_H */
```

- [ ] **Step 2: Verify it compiles standalone**

Run: `cc -fsyntax-only -Iinclude -Isrc src/ops/agg_acc.h`
Expected: no output, exit 0. (If `NULL_I64` / `RAY_TIMESTAMP` are not visible from `<rayforce.h>`, add the include that defines them — find with `grep -rn "define NULL_I64\|NULL_I64 " src/ include/ | head`, then include that header.)

- [ ] **Step 3: Commit (checkpoint)**

```bash
git add src/ops/agg_acc.h
git commit -m "feat(agg): accumulator vtable interface + centralized validity view"
```

---

## Task 2: Registry header

**Files:**
- Create: `src/ops/agg_registry.h`

(`agg_resolve` is *defined* in `agg_stream.c` in Task 3, next to the accumulators — Phase 0 is a single .c. This task only publishes the declaration; nothing references it yet, so the library builds unchanged.)

- [ ] **Step 1: Write the header**

```c
/* src/ops/agg_registry.h — resolve (agg_kind, in_type) → accumulator vtable.
 * agg_kind values are the existing OP_SUM/OP_MIN/... opcodes from ops/ops.h. */
#ifndef RAY_OPS_AGG_REGISTRY_H
#define RAY_OPS_AGG_REGISTRY_H

#include "ops/agg_acc.h"

/* Returns NULL when no specialized vtable is registered for (agg_kind, in_type).
 * In later phases NULL routes to the buffered-eval oracle accumulator. */
const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type);

#endif /* RAY_OPS_AGG_REGISTRY_H */
```

- [ ] **Step 2: Build to confirm no breakage**

Run: `make 2>&1 | tail -5`
Expected: library builds; the header is referenced nowhere yet.

- [ ] **Step 3: Commit (checkpoint)**

```bash
git add src/ops/agg_registry.h
git commit -m "feat(agg): registry resolve declaration"
```

---

## Task 3: sum_i64 accumulator + differential test

**Files:**
- Create: `src/ops/agg_stream.c`
- Create: `test/test_agg_registry.c`
- Modify: `test/main.c`

- [ ] **Step 1: Write the failing differential test**

Single-group harness: build an I64 column, run the accumulator with all gids=0, finalize, and assert it equals `ray_sum_fn` over the same column.

```c
/* test/test_agg_registry.c — differential: vtable accumulator ≡ current ray_*_fn.
 * Phase 0 oracle = the existing reduction functions. */
#include "test.h"
#include <rayforce.h>
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include <string.h>
#include <stdlib.h>

/* Build an I64 vector from a C array. */
static ray_t* vec_i64(const int64_t* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    memcpy(ray_data(v), xs, (size_t)n * sizeof(int64_t));
    return v;
}

/* Run a vtable accumulator over the whole vector as a single group (gid 0). */
static ray_t* run_single_group(const agg_vtable_t* vt, ray_t* col) {
    void* state = calloc(1, vt->state_size);
    vt->init(state);
    uint32_t* gids = calloc((size_t)col->len, sizeof(uint32_t));  /* all zero */
    ray_valid_t valid = { ray_data(col), col->type,
                          (col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    vt->update_batch(state, vt->state_size, gids, ray_data(col),
                     &valid, col->len, NULL);
    ray_t* out = vt->finalize(state, NULL);
    free(gids); free(state);
    return out;
}

static test_result_t test_sum_i64_matches_reduction(void) {
    int64_t xs[] = { 10, 20, 30, -5, 7 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);

    const agg_vtable_t* vt = agg_resolve(OP_SUM, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);

    ray_t* got = run_single_group(vt, col);
    ray_t* want = ray_sum_fn(col);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_EQ_I(got->i64, want->i64);

    ray_release(got); ray_release(want); ray_release(col);
    return TEST_OK;
}

const test_entry_t agg_registry_entries[] = {
    { "agg_registry/sum_i64_matches_reduction", test_sum_i64_matches_reduction, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
```

Register the suite in `test/main.c`: add `extern const test_entry_t agg_registry_entries[];` alongside the other externs (near line 126), and add `agg_registry_entries,` to the suite array (near line 179).

> Note: confirm the scalar accessor (`got->i64`) and the `TEST_OK`/`test_result_t` return convention against an existing test — open `test/test_fused_group.c` and copy its exact return value and result-field access. Confirm `ray_sum_fn` is declared in an includable header (`grep -rn "ray_sum_fn" src/ops/*.h include/`); include it.

- [ ] **Step 2: Run the test, expect it to fail (vtable is NULL)**

Run: `make test 2>&1 | tail -20` then `./rayforce.test agg_registry`
Expected: FAIL at `TEST_ASSERT_NOT_NULL(vt)` — `agg_resolve(OP_SUM, RAY_I64)` still returns NULL.

- [ ] **Step 3: Implement sum_i64 and register it**

```c
/* src/ops/agg_stream.c — streaming accumulators (design §4.5).
 * Inner loops mirror group.c REDUCE_LOOP_I/F but carry ONLY this aggregate's
 * state, recovering the rowform density win generically. */
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include <math.h>

/* ---- sum, I64 -------------------------------------------------------- */
typedef struct { int64_t sum; } sum_i64_state;

static void sum_i64_init(void* s) { ((sum_i64_state*)s)->sum = 0; }

static void sum_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        /* unsigned wrap: matches group.c:185 overflow convention */
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]);
    }
}

static void sum_i64_merge(void* dst, const void* src, acc_arena_t* a) {
    (void)a;
    ((sum_i64_state*)dst)->sum = (int64_t)((uint64_t)((sum_i64_state*)dst)->sum
                                         + (uint64_t)((const sum_i64_state*)src)->sum);
}

static ray_t* sum_i64_final(const void* s, acc_arena_t* a) {
    (void)a; return ray_i64(((const sum_i64_state*)s)->sum);
}

static const agg_vtable_t SUM_I64 = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_i64_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final,
};

const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type) {
    if (agg_kind == OP_SUM && in_type == RAY_I64) return &SUM_I64;
    return NULL;
}
```

`agg_resolve` is defined here in `agg_stream.c` (its only definition; the header just declares it).

> Note: confirm `ray_i64(...)` is the scalar constructor (seen at `group.c:2219`) and is declared in an includable header.

- [ ] **Step 4: Run the test, expect PASS**

Run: `make test 2>&1 | tail -5` then `./rayforce.test agg_registry`
Expected: `agg_registry/sum_i64_matches_reduction` PASS.

- [ ] **Step 5: Commit (checkpoint)**

```bash
git add src/ops/agg_stream.c src/ops/agg_registry.c test/test_agg_registry.c test/main.c
git commit -m "feat(agg): sum_i64 accumulator + differential harness vs ray_sum_fn"
```

---

## Task 4: count / min / max for I64

**Files:**
- Modify: `src/ops/agg_stream.c`
- Modify: `test/test_agg_registry.c`

- [ ] **Step 1: Write failing tests for count/min/max I64**

Add to `test/test_agg_registry.c`, mirroring `test_sum_i64_matches_reduction` but resolving `OP_COUNT` / `OP_MIN` / `OP_MAX` and diffing against `ray_min_fn` / `ray_max_fn`, and for count against the literal element count.

```c
static test_result_t test_minmax_count_i64_match(void) {
    int64_t xs[] = { 10, 20, 30, -5, 7 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);

    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_MIN, ray_min_fn }, { OP_MAX, ray_max_fn },
    };
    for (size_t c = 0; c < 2; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = cases[c].ref(col);
        TEST_ASSERT_EQ_I(got->i64, want->i64);
        ray_release(got); ray_release(want);
    }
    /* count = live element count (here, no nulls → 5) */
    const agg_vtable_t* vt = agg_resolve(OP_COUNT, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);
    ray_t* got = run_single_group(vt, col);
    TEST_ASSERT_EQ_I(got->i64, 5);
    ray_release(got); ray_release(col);
    return TEST_OK;
}
```

Add `{ "agg_registry/minmax_count_i64_match", test_minmax_count_i64_match, NULL, NULL },` to `agg_registry_entries[]`.

- [ ] **Step 2: Run, expect FAIL** — `agg_resolve(OP_MIN, RAY_I64)` is NULL.

Run: `make test 2>&1 | tail -5 && ./rayforce.test agg_registry`
Expected: FAIL at `TEST_ASSERT_NOT_NULL(vt)` for OP_MIN.

- [ ] **Step 3: Implement count/min/max I64**

```c
/* ---- count (type-agnostic over live rows) ---------------------------- */
typedef struct { int64_t n; } count_state;
static void count_init(void* s) { ((count_state*)s)->n = 0; }
static void count_update(void* base, size_t stride, const uint32_t* gids,
                         const void* vals, const ray_valid_t* valid,
                         int64_t n, acc_arena_t* a) {
    (void)vals; (void)a;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ((count_state*)((char*)base + (size_t)gids[i]*stride))->n++;
    }
}
static void count_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((count_state*)d)->n += ((const count_state*)s)->n;
}
static ray_t* count_final(const void* s, acc_arena_t* a) {
    (void)a; return ray_i64(((const count_state*)s)->n);
}
static const agg_vtable_t COUNT_ANY = {
    .state_size = sizeof(count_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = count_init, .update_batch = count_update,
    .merge = count_merge, .finalize = count_final,
};

/* ---- min / max I64 (empty group → typed null, matching group.c) ------ */
typedef struct { int64_t v; int64_t cnt; } ext_i64_state;
static void min_i64_init(void* s) { ((ext_i64_state*)s)->v = INT64_MAX; ((ext_i64_state*)s)->cnt = 0; }
static void max_i64_init(void* s) { ((ext_i64_state*)s)->v = INT64_MIN; ((ext_i64_state*)s)->cnt = 0; }
static void min_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    }
}
static void max_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    }
}
static void min_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* src = s; ext_i64_state* dst = d;
    if (src->cnt && src->v < dst->v) dst->v = src->v; dst->cnt += src->cnt;
}
static void max_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* src = s; ext_i64_state* dst = d;
    if (src->cnt && src->v > dst->v) dst->v = src->v; dst->cnt += src->cnt;
}
static ray_t* ext_i64_final(const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* st = s;
    return st->cnt ? ray_i64(st->v) : ray_typed_null(-RAY_I64);
}
static const agg_vtable_t MIN_I64 = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = min_i64_init, .update_batch = min_i64_update,
    .merge = min_i64_merge, .finalize = ext_i64_final,
};
static const agg_vtable_t MAX_I64 = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = max_i64_init, .update_batch = max_i64_update,
    .merge = max_i64_merge, .finalize = ext_i64_final,
};
```

Extend `agg_resolve`:

```c
    if (agg_kind == OP_COUNT)                          return &COUNT_ANY;
    if (agg_kind == OP_MIN && in_type == RAY_I64)      return &MIN_I64;
    if (agg_kind == OP_MAX && in_type == RAY_I64)      return &MAX_I64;
```

> Note: confirm `ray_typed_null(-RAY_I64)` is the typed-null constructor (seen at `group.c:2187`). Confirm `ray_min_fn` ignores nulls the same way (empty/all-null group convention) so the diff holds; if `ray_min_fn` of a no-null vector returns a plain atom, `got->i64 == want->i64` is the right check.

- [ ] **Step 4: Run, expect PASS**

Run: `make test 2>&1 | tail -5 && ./rayforce.test agg_registry`
Expected: both `agg_registry/*` tests PASS.

- [ ] **Step 5: Commit (checkpoint)**

```bash
git add src/ops/agg_stream.c test/test_agg_registry.c
git commit -m "feat(agg): count/min/max I64 accumulators + differential"
```

---

## Task 5: F64 accumulators (sum / min / max / avg)

**Files:**
- Modify: `src/ops/agg_stream.c`
- Modify: `test/test_agg_registry.c`

- [ ] **Step 1: Write failing F64 differential test**

```c
static ray_t* vec_f64(const double* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_F64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n; memcpy(ray_data(v), xs, (size_t)n * sizeof(double));
    return v;
}

static test_result_t test_f64_match(void) {
    double xs[] = { 1.5, 2.0, -3.25, 10.0 };
    ray_t* col = vec_f64(xs, 4);
    TEST_ASSERT_NOT_NULL(col);

    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_SUM, ray_sum_fn }, { OP_MIN, ray_min_fn },
        { OP_MAX, ray_max_fn }, { OP_AVG, ray_avg_fn },
    };
    for (size_t c = 0; c < 4; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = cases[c].ref(col);
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-12);
        ray_release(got); ray_release(want);
    }
    ray_release(col);
    return TEST_OK;
}
```

Register `{ "agg_registry/f64_match", test_f64_match, NULL, NULL },`.

- [ ] **Step 2: Run, expect FAIL** — `agg_resolve(OP_SUM, RAY_F64)` is NULL.

Run: `make test 2>&1 | tail -5 && ./rayforce.test agg_registry`
Expected: FAIL at first F64 `TEST_ASSERT_NOT_NULL(vt)`.

- [ ] **Step 3: Implement F64 sum/min/max/avg**

Mirror the I64 accumulators with `double` state. `avg` carries `{ double sum; int64_t cnt; }` and finalizes `cnt ? ray_f64(sum/cnt) : ray_typed_null(-RAY_F64)` (matches `group.c:2187`). Null skip uses the same `ray_valid_at` (NaN self-inequality path). Extend `agg_resolve`:

```c
    if (agg_kind == OP_SUM && in_type == RAY_F64)  return &SUM_F64;
    if (agg_kind == OP_MIN && in_type == RAY_F64)  return &MIN_F64;
    if (agg_kind == OP_MAX && in_type == RAY_F64)  return &MAX_F64;
    if (agg_kind == OP_AVG && in_type == RAY_F64)  return &AVG_F64;
```

(Write `SUM_F64`/`MIN_F64`/`MAX_F64`/`AVG_F64` as the `double` analogues of Task 3–4; min init = `+INFINITY`, max init = `-INFINITY`, tracked with a `cnt` for the empty-group null.)

> Note: confirm `ray_avg_fn` returns F64 for an F64 input and that `got->f64` is the scalar accessor.

- [ ] **Step 4: Run, expect PASS**

Run: `make test 2>&1 | tail -5 && ./rayforce.test agg_registry`
Expected: all `agg_registry/*` PASS.

- [ ] **Step 5: Commit (checkpoint)**

```bash
git add src/ops/agg_stream.c test/test_agg_registry.c
git commit -m "feat(agg): F64 sum/min/max/avg accumulators + differential"
```

---

## Task 6: Null-bearing differential (the centralization payoff)

**Files:**
- Modify: `test/test_agg_registry.c`

This task proves the design's §6 claim: an accumulator honoring the centralized `ray_valid_t` produces exactly what the current attr-gated reduction produces — and never sums a sentinel.

- [ ] **Step 1: Write the failing null-differential test**

Build a column WITH sentinels and the `RAY_ATTR_HAS_NULLS` attribute set, then diff vtable vs `ray_*_fn` for every registered `(agg, type)`.

```c
static test_result_t test_nulls_match_reduction(void) {
    int64_t xs[] = { 10, NULL_I64, 20, NULL_I64, 30 };  /* 3 live, 2 null */
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);
    col->attrs |= RAY_ATTR_HAS_NULLS;

    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_SUM, ray_sum_fn }, { OP_MIN, ray_min_fn }, { OP_MAX, ray_max_fn },
    };
    for (size_t c = 0; c < 3; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = cases[c].ref(col);
        TEST_ASSERT_EQ_I(got->i64, want->i64);   /* sentinels skipped identically */
        ray_release(got); ray_release(want);
    }
    /* count over a HAS_NULLS column = live rows only = 3 */
    ray_t* got = run_single_group(agg_resolve(OP_COUNT, RAY_I64), col);
    TEST_ASSERT_EQ_I(got->i64, 3);
    ray_release(got); ray_release(col);
    return TEST_OK;
}
```

Register `{ "agg_registry/nulls_match_reduction", test_nulls_match_reduction, NULL, NULL },`.

> Note: confirm the current `ray_sum_fn`/`ray_min_fn` honor `RAY_ATTR_HAS_NULLS` for a single ungrouped vector (they should, via the `has_nulls` dispatch). If `count`'s current convention counts slots-including-null (see design §2.10 / `ARCH-EXEC §2.2`), assert the value the *redesign* commits to — live-rows-only — and record the intentional divergence in a comment; this is a deliberate corrected-behavior point, not a bug to match.

- [ ] **Step 2: Run, expect PASS immediately** (the accumulators already honor `ray_valid_at`)

Run: `make test 2>&1 | tail -5 && ./rayforce.test agg_registry`
Expected: PASS for sum/min/max. If `count` diverges from a slot-counting `ray_*_fn`, that is the one intentional difference — the assert pins the corrected value (3), not the legacy one.

- [ ] **Step 3: Run the full suite to confirm no regressions**

Run: `make test 2>&1 | tail -20`
Expected: the whole suite passes; the new `agg_registry/*` group is green and nothing else changed (this code is wired into nothing).

- [ ] **Step 4: Commit (checkpoint)**

```bash
git add test/test_agg_registry.c
git commit -m "test(agg): null-bearing differential proves centralized validity ≡ reductions"
```

---

## Phase 0 Done — exit criteria

- `agg_acc.h` interface + `agg_registry` + `agg_stream.c` exist, compiled into the library.
- `sum/count/min/max` (I64) and `sum/min/max/avg` (F64) accumulators registered.
- The `agg_registry/*` differential suite passes: every registered `(agg, type)` vtable is byte-equivalent to the current reduction, with and without nulls.
- Zero behavioral change to the live engine (wired into nothing).

**Next plan (Phase 1):** introduce `agg_engine` (per-group arena + drive accumulators over morsels), the `radix` grouping strategy, the `output_assembler` with first-occurrence order, and the buffered-eval oracle accumulator for arbitrary expressions — gated behind a flag and differentially tested against the old engine over the full conformance set.
```

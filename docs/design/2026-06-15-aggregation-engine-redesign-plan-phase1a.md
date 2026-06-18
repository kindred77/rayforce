# Aggregation Engine Redesign — Phase 1a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit the design/plan docs under `docs/design/` (standing user preference).

**Goal:** Put the composable v2 aggregation engine on a real query path behind a runtime flag (`ray_agg_engine_v2`), handling the simple aggregate class (single key + Phase-0 accumulators), and prove it byte-equivalent to the existing engine via a differential test toggling the flag.

**Architecture:** A gate `agg_v2_can_handle(ext, tbl)` admits only queries the v2 engine fully supports; everything else falls through to the existing `exec_group` unchanged. When admitted, `exec_group_v2` does: single-pass hash grouping (key → dense gid, first-occurrence order) → drive Phase-0 accumulators over the column via SoA per-group state → assemble `{key col, agg cols...}` result table. Single-threaded in 1a (parallelism is Phase 1b). Reuses the Phase-0 `agg_acc.h`/`agg_registry`/`agg_stream.c` accumulators unchanged.

**Tech Stack:** C11, in-repo test harness (`make test` → `./rayforce.test`), `ray_pool` (not yet used in 1a), `ray_table_*` construction API.

**Reference (design):** `docs/design/2026-06-14-aggregation-engine-redesign-design.md` §3 (modules), §5 (driver/selector), §7 (assembler/first-occurrence order). Builds on Phase 0 (branch `feat/agg-engine-phase0`, commits 6f9171c2..71180196).

**Prereq:** This plan continues the `feat/agg-engine-phase0` branch (Phase 0 accumulators must be present).

---

## Scope (read before implementing)

**In scope (1a):**
- Runtime flag `ray_agg_engine_v2` routing `OP_GROUP` to `exec_group_v2` when the gate admits.
- Gate admits ONLY: exactly one key that is an `OP_SCAN` of a supported type (I64/I32/I16/U8/BOOL/DATE/TIME/TIMESTAMP/SYM); every aggregate resolvable via `agg_resolve` (Phase-0 set: SUM/COUNT/MIN/MAX over I64, SUM/MIN/MAX/AVG over F64); no `where`/pushed filter, no `group_limit`, no active selection, no non-agg expressions.
- Single-threaded hash grouping, first-occurrence group order.
- Differential test vs the old engine (flag off vs on).

**Explicitly NOT in scope (later plans):**
- Phase 1b: multi-key (composite key packing) + parallel grouping (radix-style fork-join).
- Phase 1c: `ACC_BUFFERED` aggregates (median/top-k) + the buffered-eval oracle accumulator for arbitrary expressions.
- Phase 2: `direct-array` strategy + the cost-based strategy selector + canonicalization + `explain`.
- Phase 3: delete the six `OP_GROUP_*_ROWFORM` operators and their gates.

**Single-threaded-first is deliberate.** The bench history warns "a fused operator must be parallel from day one" — but that warning is about *racing the 28-thread generic path*. 1a is behind a flag and not yet competing; correctness-first then parallelize (1b) before the flag is ever flipped to default. No perf gate applies to 1a.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_engine.h` | Create | Public decls: `exec_group_v2(...)`, `agg_v2_can_handle(...)`, and `extern bool ray_agg_engine_v2`. |
| `src/ops/agg_engine.c` | Create | The v2 engine: gate, hash grouping, accumulator driving, output assembly, and the flag definition. |
| `src/ops/group.c` | Modify | At `exec_group` entry (after `find_ext`, ~:5513): route to `exec_group_v2` when `ray_agg_engine_v2 && agg_v2_can_handle(...)`. |
| `src/ops/internal.h` | Modify | `extern bool ray_agg_engine_v2;` alongside the other knobs (~:627). |
| `test/test_agg_engine.c` | Create | Differential test: build tables, run `OP_GROUP` with the flag off then on, compare results column-by-column. |
| `test/main.c` | Modify | Register the new suite (extern + array entry). |

`src/ops/*.c` auto-compiles (`LIB_SRC = $(wildcard src/*/*.c)`). `test/*.c` auto-compiles.

**Confirmed facts (verified in source, not to be re-guessed):**
- `ray_op_ext_t` is defined at `src/ops/ops.h:371-465`. The OP_GROUP arm has exactly: `ray_op_t** keys; uint8_t n_keys; uint8_t n_aggs; uint16_t* agg_ops; ray_op_t** agg_ins; ray_op_t** agg_ins2; int64_t* agg_k;` (`agg_ins2`/`agg_k` are for binary/holistic aggs — out of 1a scope).
- For an `OP_SCAN` node, the column-name symbol is the ext's `int64_t sym` field (same union). Resolve a column: `ray_table_get_col(tbl, find_ext(g, scan_op->id)->sym)`.
- Result column naming (old engine, `emit_agg_columns` at group.c:3855, names at :4017-4054): the key column keeps its own name (`kext->sym`, see group.c:6833). Each aggregate column is named `<input-col-name><suffix>` where suffix per op is `_sum`/`_count`/`_mean`(AVG)/`_min`/`_max` (full map at :4037-4051); on buffer overflow it falls back to the input's own sym.

**Reference patterns to mirror:**
- `exec_group` signature + `ext` field access — `src/ops/group.c:5478`, `:5508-5513`, `:5659-5680`.
- Result-table construction — `src/ops/group.c:5605-5632`, `:6800-6873` (`ray_table_new`, `ray_table_add_col`, `emit_agg_columns`).
- Knob pattern — `ray_expr_disable` decl `src/ops/internal.h:621`, def `src/ops/expr.c:410`, use `src/ops/expr.c:501`.
- Differential harness — `test/test_expr_null.c:102-128` (`diff_run` toggling the knob) + `:46-82` (`vec_expect_equal` column compare incl. null positions + F64 tolerance).
- C-level table builders + `N=70000` parallel-threshold trick — `test/test_group_extra.c`.
- Phase-0 accumulators — `src/ops/agg_acc.h` (`agg_vtable_t`, `ray_valid_t`, `ray_valid_at`), `src/ops/agg_registry.h` (`agg_resolve`).

---

## Task 1: Flag + routing skeleton (defer-everything gate)

**Files:** Create `src/ops/agg_engine.h`, `src/ops/agg_engine.c`; Modify `src/ops/internal.h`, `src/ops/group.c`.

- [ ] **Step 1: Create the header**

```c
/* src/ops/agg_engine.h — v2 composable aggregation engine entry (design §3). */
#ifndef RAY_OPS_AGG_ENGINE_H
#define RAY_OPS_AGG_ENGINE_H

#include <rayforce.h>
#include "ops/internal.h"   /* ray_graph_t, ray_op_t, ray_op_ext_t, find_ext */

/* Test/feature knob: route OP_GROUP through the v2 engine when it can handle
 * the query (see agg_v2_can_handle). Default false → zero behavioral change. */
extern bool ray_agg_engine_v2;

/* True iff the v2 engine fully supports this group node over this table.
 * Conservative: any uncertainty → false → caller uses the existing engine. */
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Precondition: agg_v2_can_handle(g, op, tbl) returned true. */
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

#endif /* RAY_OPS_AGG_ENGINE_H */
```
> Confirm the include for `ray_graph_t`/`ray_op_t`/`ray_op_ext_t`/`find_ext` — check what `group.c` includes for these (likely `"ops/internal.h"`). Match it.

- [ ] **Step 2: Create the .c with the flag + a defer-everything gate + stub**

```c
/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"

bool ray_agg_engine_v2 = false;   /* knob; default off */

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    (void)g; (void)op; (void)tbl;
    return false;   /* Task 2 fills the real predicate */
}

ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    (void)g; (void)op; (void)tbl;
    return ray_error("nyi", "agg v2: not reachable until gate admits");
}
```

- [ ] **Step 3: Declare the knob in internal.h**

Add next to the other knobs (after `extern bool ray_join_no_dup_fallback;`, ~:627):
```c
extern bool ray_agg_engine_v2;
```

- [ ] **Step 4: Route in exec_group**

In `src/ops/group.c`, at the top of `exec_group` right after the `ext` is fetched and null-checked (~:5513), add:
```c
    if (ray_agg_engine_v2 && agg_v2_can_handle(g, op, tbl))
        return exec_group_v2(g, op, tbl);
```
Add `#include "ops/agg_engine.h"` to group.c's includes. (The flag is off and the gate returns false, so this is dead at runtime for now — purely structural.)

- [ ] **Step 5: Build + full suite (flag default off → nothing changes)**

Run: `make test 2>&1 | tail -8`
Expected: builds clean; suite passes at the Phase-0 count (3464 of 3466, 0 failed). No behavior change because the flag defaults off and the gate is false.

- [ ] **Step 6: Commit (checkpoint)**

```bash
git add src/ops/agg_engine.h src/ops/agg_engine.c src/ops/internal.h src/ops/group.c
git commit -m "feat(agg): v2 engine routing skeleton behind ray_agg_engine_v2 (defers everything)"
```

---

## Task 2: The admission gate

**Files:** Modify `src/ops/agg_engine.c`; Create test in `test/test_agg_engine.c` + register in `test/main.c`.

- [ ] **Step 1: Write a failing gate unit test**

Build a single-I64-key table with one SUM aggregate, construct the `OP_GROUP` node (mirror how `test_group_extra.c` / `test_fused_group.c` build a group node + ext), and assert `agg_v2_can_handle` returns true. Build a second case the gate must reject (e.g. an aggregate type not in the Phase-0 set, or two keys) and assert false.
> The exact group-node construction (how to set `ext->n_keys`, `ext->keys`, `ext->agg_ops`, `ext->agg_ins`) must be copied from an existing group test — read `test/test_group_extra.c` and `test/test_fused_group.c` for the real builder calls (`ray_scan`, `ray_group`, `find_ext`). Use the heap/sym init bracket like the other tests.

Register suite: `extern const test_entry_t agg_engine_entries[];` + `agg_engine_entries,` in `test/main.c`.

- [ ] **Step 2: Run, expect FAIL** (gate still returns false unconditionally)

Run: `make test 2>&1 | tail -8 && ./rayforce.test -f agg_engine`
Expected: FAIL on the "should admit" assertion.

- [ ] **Step 3: Implement the gate**

```c
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return false;
    if (ext->n_keys != 1) return false;                 /* 1a: single key only */
    if (ext->n_aggs == 0) return false;                 /* need ≥1 aggregate   */
    if (g->selection) return false;                     /* no active selection */
    /* no non-agg expressions, no where/pushed filter, no group_limit:
     * confirm the ext/op fields that signal these (mirror how exec_group
     * detects them) and reject if present. */

    /* key must be a plain column scan of a supported type */
    ray_op_t* key = ext->keys[0];
    if (!key || key->opcode != OP_SCAN) return false;
    ray_op_ext_t* kext = find_ext(g, key->id);
    ray_t* kc = (kext && tbl) ? ray_table_get_col(tbl, kext->sym) : NULL;
    if (!kc) return false;
    switch (kc->type) {
        case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
        case RAY_BOOL: case RAY_DATE: case RAY_TIME:
        case RAY_TIMESTAMP: case RAY_SYM: break;
        default: return false;
    }

    /* every aggregate must be a plain column scan resolvable by the registry */
    for (uint8_t a = 0; a < ext->n_aggs; a++) {
        ray_op_t* in = ext->agg_ins[a];
        if (ext->agg_ops[a] == OP_COUNT) continue;      /* count needs no typed input */
        if (!in || in->opcode != OP_SCAN) return false;
        ray_op_ext_t* ie = find_ext(g, in->id);
        ray_t* ic = (ie && tbl) ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!ic) return false;
        if (!agg_resolve(ext->agg_ops[a], ic->type)) return false;
    }
    return true;
}
```
> Field names (`ext->keys`, `ext->agg_ins`, `ext->agg_ops`, `ext->n_keys/n_aggs`, OP_SCAN `ext->sym`) are confirmed (ops.h:371-465). STILL TO CONFIRM: how `exec_group` / the OP_GROUP case detect a pushed `where` filter, a `group_limit`, and non-agg projection expressions (read group.c around :5508-5700 and exec.c:1488-1554) — reject all three in the gate. Add includes for `ray_table_get_col`, `OP_SCAN`, `OP_COUNT`.

- [ ] **Step 4: Run, expect PASS**; then full suite green.

Run: `make test 2>&1 | tail -8 && ./rayforce.test -f agg_engine`

- [ ] **Step 5: Commit (checkpoint)**

```bash
git add src/ops/agg_engine.c test/test_agg_engine.c test/main.c
git commit -m "feat(agg): v2 admission gate (single supported key + registry-resolvable aggs)"
```

---

## Task 3: Hash grouping → dense gid + first-occurrence keys

**Files:** Modify `src/ops/agg_engine.c`.

Implement a single-pass grouping over one key column producing: a `uint32_t* gids` (one per row), the group count, and the per-group representative key value in **first-occurrence order** (gid assigned incrementally on first sight → gid order IS first-occurrence order).

- [ ] **Step 1: Write the grouping helper**

```c
/* Maps each row's key to a dense group id, assigned in first-occurrence order.
 * 1a: single-threaded open-addressing hash over an integer-coded key.
 * SYM/temporal keys are read as their integer storage (intern id / epoch). */
typedef struct {
    uint32_t* gids;     /* len = nrows */
    int64_t*  keys;     /* len = ngroups, representative key per group (int-coded) */
    int64_t   ngroups;
} agg_groups_t;

/* Returns 0 on success, -1 on OOM. Caller frees groups->gids / groups->keys. */
static int agg_group_keys_i(ray_t* key_col, agg_groups_t* out);
```
Implementation notes (write the full body):
- Read the key as int64 regardless of width: switch on `key_col->type` to load each element widened to int64 (I64/TIMESTAMP direct; I32/DATE/TIME from int32; I16; U8/BOOL; SYM as its int64 intern id). Mirror the widening in `REDUCE_LOOP_I` / `reduce_range` (group.c:240-269).
- Open-addressing hash table sized to next-power-of-two ≥ `2*nrows` (or capacity heuristic); linear probe; on miss assign `gid = ngroups++` and record the key. Use a simple integer hash (e.g. `key * 0x9E3779B97F4A7C15` >> shift). Allocate via `malloc`/`calloc` (1a is single-threaded; `scratch_calloc` is for the parallel 1b path).
- Null keys: a null key (sentinel) forms its own group per design §2.5.2 (null == null in `by:`). For 1a, treat the sentinel value as just another integer key (it hashes to one group) — this matches "null==null grouping". Confirm against an existing single-key group test that this equals the old engine; if the old engine drops null keys differently, the differential test in Task 6 will catch it and we narrow the gate to reject HAS_NULLS keys in 1a.

- [ ] **Step 2: Test indirectly via the end-to-end differential** (Task 6). No standalone unit test for the static helper; it is exercised by `exec_group_v2`. (If you prefer, temporarily expose it for a unit test, then re-`static` it — optional.)

- [ ] **Step 3: Commit (checkpoint)**

```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 single-key hash grouping (dense gid, first-occurrence order)"
```

---

## Task 4: Drive accumulators over the groups (agg_engine core)

**Files:** Modify `src/ops/agg_engine.c`.

For each aggregate, resolve its vtable, allocate a dense SoA state array (`ngroups * state_size`), init every group's state, then one `update_batch` call over the whole column with the `gids` array.

- [ ] **Step 1: Write the driver**

```c
/* One finalized result column per aggregate, in group (=first-occurrence) order. */
static ray_t* agg_run_one(const agg_vtable_t* vt, ray_t* val_col,
                          const uint32_t* gids, int64_t nrows, int64_t ngroups) {
    char* states = calloc((size_t)ngroups, vt->state_size);
    if (!states) return ray_error("oom", NULL);
    for (int64_t gi = 0; gi < ngroups; gi++)
        vt->init(states + (size_t)gi * vt->state_size);

    /* COUNT has no typed input column; pass the key/any column for length only.
     * The vtable's update ignores vals for count. valid must reflect val_col. */
    ray_valid_t valid = { val_col ? ray_data(val_col) : NULL,
                          val_col ? val_col->type : RAY_I64,
                          val_col ? ((val_col->attrs & RAY_ATTR_HAS_NULLS) != 0) : false };
    const void* vals = val_col ? ray_data(val_col) : NULL;
    vt->update_batch(states, vt->state_size, gids, vals, &valid, nrows, NULL);

    /* finalize each group into an output column of vt->out_type */
    ray_t* out = ray_vec_new(vt->out_type, ngroups);
    if (!out || RAY_IS_ERR(out)) { free(states); return ray_error("oom", NULL); }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL);
        /* write cell's scalar into out[gi]; see note on the writer below */
        agg_out_put(out, gi, cell);
        ray_release(cell);
    }
    free(states);
    return out;
}
```
> CRITICAL: write `agg_out_put(out, gi, cell)` — store a finalized scalar `cell` into element `gi` of the typed result vector `out`. For I64/F64 this writes `((int64_t*)ray_data(out))[gi] = cell->i64` / `((double*)ray_data(out))[gi] = cell->f64`. For a typed-null cell, write the sentinel for `out_type` AND set `out->attrs |= RAY_ATTR_HAS_NULLS`. Mirror how the existing engine materializes aggregate columns (`emit_agg_columns`, group.c ~:6800-6873) — read it and reuse its cell→column store if there's a helper. Confirm the count aggregate: COUNT's `out_type` is RAY_I64 and its finalize returns `ray_i64(n)`.

- [ ] **Step 2: Exercised by Task 6 differential.** Commit:

```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 accumulator driver (SoA per-group state, finalize to columns)"
```

---

## Task 5: Output assembler — build the result table

**Files:** Modify `src/ops/agg_engine.c`.

Assemble `exec_group_v2`: gate already passed, so run grouping, build the key column from `groups.keys` (typed back to the key column's type), run each aggregate, and emit `{key col, agg cols...}` via `ray_table_new`/`ray_table_add_col`.

- [ ] **Step 1: Write exec_group_v2**

```c
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    int64_t nrows = ray_table_nrows(tbl);
    ray_op_t* key = ext->keys[0];
    ray_op_ext_t* kext = find_ext(g, key->id);
    ray_t* key_col = ray_table_get_col(tbl, kext->sym);

    agg_groups_t groups = {0};
    if (agg_group_keys_i(key_col, &groups) != 0) return ray_error("oom", NULL);

    ray_t* result = ray_table_new(1 + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) { free(groups.gids); free(groups.keys); return ray_error("oom", NULL); }

    /* key column, typed back to key_col->type, in first-occurrence order */
    ray_t* out_key = agg_build_key_col(key_col->type, groups.keys, groups.ngroups);
    result = ray_table_add_col(result, kext->sym, out_key);
    ray_release(out_key);

    for (uint8_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]->id);  /* agg input scan */
        ray_t* val_col = (ext->agg_ops[a] != OP_COUNT)
                         ? ray_table_get_col(tbl, ie->sym) : NULL;
        int8_t in_type = val_col ? val_col->type : RAY_I64;
        const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], in_type);
        ray_t* col = agg_run_one(vt, val_col, groups.gids, nrows, groups.ngroups);
        if (!col || RAY_IS_ERR(col)) { free(groups.gids); free(groups.keys); ray_release(result); return col; }
        int64_t agg_name = agg_result_col_name(ie->sym, ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, col);
        ray_release(col);
    }
    free(groups.gids); free(groups.keys);
    return result;
}
```
> **Naming — parity by construction.** Factor the old engine's name logic (group.c:4017-4054: `<input-col-name>` + per-op suffix `_sum`/`_count`/`_mean`/`_min`/`_max`, fallback to the input sym) into a shared helper `int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op)` and call it from BOTH `emit_agg_columns` and `exec_group_v2`. This guarantees v2 column names match the old engine rather than testing for it. The extraction is behavior-preserving for the old engine (same bytes out) — the existing suite verifies that. (If extraction proves messy, the 1a fallback is to replicate the ~15 lines inline; the Task 6 differential then catches any divergence.)
> **`agg_build_key_col(type, keys[], n)`:** build a vector of `type` and store each int-coded key back at native width (I64/TIMESTAMP direct; I32/DATE/TIME narrow to int32; I16; U8/BOOL; for RAY_SYM store the intern id and adopt the source column's domain — mirror how the old engine emits a SYM key column at group.c:6833 and `ray_sym_vec_adopt_domain` usage at :3900).

- [ ] **Step 2: Build clean** (no test yet exercises it until Task 6). `make 2>&1 | tail -5`. Commit:

```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 output assembler (key col + agg cols, first-occurrence order)"
```

---

## Task 6: End-to-end differential test vs the old engine

**Files:** Modify `test/test_agg_engine.c`.

Prove: for every supported single-key query shape, the v2 result table equals the old engine's result table — same column count, names, types, values, null positions, AND row order.

- [ ] **Step 1: Write the differential test**

Mirror `test/test_expr_null.c::diff_run` (toggle the knob) and `vec_expect_equal` (column compare). Structure:

```c
/* Run an OP_GROUP query both ways; compare result tables fully. */
static test_result_t diff_group(ray_t* tbl, group_builder_t build) {
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* old_r = ray_execute(g1, build(g1));
    if (ray_is_lazy(old_r)) old_r = ray_lazy_materialize(old_r);

    ray_agg_engine_v2 = true;
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* new_r = ray_execute(g2, build(g2));
    if (ray_is_lazy(new_r)) new_r = ray_lazy_materialize(new_r);
    ray_agg_engine_v2 = false;

    test_result_t res = table_expect_equal(old_r, new_r);  /* cols: count,name,type,len,values,nulls,order */
    ray_release(old_r); ray_release(new_r);
    ray_graph_free(g1); ray_graph_free(g2);
    return res;
}
```
Write `table_expect_equal` (compare ncols; per column: name sym, type, len, element values with F64 tolerance, null positions — reuse the element/null logic from `vec_expect_equal` in test_expr_null.c). Write `group_builder_t` builders for these cases, each over both a small table and an `N=70000` table (parallel threshold — the OLD engine goes parallel; v2 stays serial, results must still match):
1. single I64 key, `sum`
2. single I64 key, `count`
3. single I64 key, `min` and `max` (two aggs)
4. single I64 key, `sum`+`count`+`min`+`max` (four aggs, order varied)
5. single SYM key, `sum` over an I64 value
6. single I64 key with HAS_NULLS values, `sum`/`min`/`max` (null-skip parity)
7. single I64 key where the KEY column has nulls (verify null-key grouping parity; if it diverges, narrow the gate to reject HAS_NULLS keys in 1a and assert the gate defers — document it)

Register each as a `test_entry_t`.

- [ ] **Step 2: Run, expect PASS for all admitted shapes**

Run: `make test 2>&1 | tail -10 && ./rayforce.test -f agg_engine`
Expected: all `agg_engine/*` green. If any case diverges, that is a real finding — investigate (most likely: key-column naming, agg-column naming, SYM domain, or null-key grouping). Fix the v2 engine to match the old engine; if a shape is genuinely out of 1a scope, narrow `agg_v2_can_handle` to defer it and assert the gate returns false for it (document the deferral).

- [ ] **Step 3: Full suite + sanitizers clean**

Run: `make test 2>&1 | tail -8`
Expected: 0 failures; new `agg_engine/*` green; everything else unchanged (flag defaults off). ASAN/UBSan clean.

- [ ] **Step 4: Commit (checkpoint)**

```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 engine differential vs old engine (single-key simple aggs)"
```

---

## Phase 1a Done — exit criteria

- `ray_agg_engine_v2` routes admitted single-key + Phase-0-agg queries through `exec_group_v2`; everything else defers to the old engine unchanged.
- `exec_group_v2` produces result tables byte-equivalent to the old engine (columns, names, types, values, null positions, first-occurrence order) for all tested shapes, small and >65536 rows, with and without nulls.
- Flag defaults off → zero change to the shipping engine. Full suite green, ASAN/UBSan clean.

**Next (Phase 1b):** composite multi-key packing + parallel grouping (radix-style fork-join via `ray_pool_dispatch` + `scratch_calloc` per-worker state, merge), widening the gate to multi-key; then re-run the differential at scale.

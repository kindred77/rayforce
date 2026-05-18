# Sentinel-null migration — completion design

**Status:** Draft
**Date:** 2026-05-18
**Author:** Anton (with Claude)
**Branch:** `sentinel-migration-finish` (off master `717feba8`)
**Supersedes:** in-code phase plan documented at `include/rayforce.h:309–346`

---

## Goal

Make the per-type `NULL_*` sentinel the **sole source of truth** for null. Decommission the per-element bitmap arm of the 16-byte union and the parallel bitmap maintenance that Phase 2 / Phase 3a / Phase 3b kept alive as a dual-encoding bridge.

The vec-level `RAY_ATTR_HAS_NULLS` attribute **stays** as a check-free fast-path gate. The other arms of the 16-byte union (`slice_parent`/`slice_offset`, `sym_dict`, `str_pool`, `index`, `link_target`) stay unchanged.

When this design is implemented:

- `(v->attrs & RAY_ATTR_HAS_NULLS)` keeps working everywhere as a single-cycle "is there any null work to do?" gate. Kernels that branch on it pay zero per-element null cost when the vec is null-free.
- "Is element `i` null?" is answered by `payload[i] == NULL_T` (or `payload[i] != payload[i]` for F64), not by a bitmap lookup.
- `ray_vec_set_null(v, i)` writes the sentinel into the payload slot and sets `HAS_NULLS` on the vec. It no longer touches bitmap storage.
- The `nullmap[16]` arm and the `ext_nullmap` external allocation no longer exist as null-tracking storage; the arm gets a neutral name reflecting its remaining role as union scratch.

## Out of scope

- **Inline stats in the reclaimed arm.** A future feature, not part of this migration. The arm becomes reserved scratch for now.
- **Resolving the `INT_MIN` sentinel-collision hazard.** Already documented and accepted at Phase 3a (`include/rayforce.h:328–330`). Persists post-migration.
- **String / sym null representation.** Already sentinel-style (zero-length string; sym ID 0). The migration removes any parallel bitmap maintenance for these types but doesn't touch the underlying encoding.
- **Bool / u8 nullability.** Locked down as non-nullable at Phase 1; no work.

## Constraints / non-goals

- **No PRs to master until the migration is complete on the branch.** Per [[feedback-no-partial-state-to-master]].
- **No shims, no dual-encoding bridge during this work.** Greenfield rule per [[project-rayforce-greenfield]]. The branch may be incrementally broken during the migration; master is not affected because nothing lands there until completion.
- **Final perf must match or beat the dual-encoded baseline.** Losing the per-element bitmap removes a fast lookup but gains cache-line density (no separate bitmap to fetch). Net: expected neutral-to-positive on hot paths; benchmark suite must verify.

## End-state contract

```
Per-type encoding (unchanged):
  F64                          NaN with a specific bit pattern (NULL_F64)
  I16/I32/I64                  type-MIN sentinel (NULL_I16/I32/I64)
  DATE/TIME/TIMESTAMP          NULL_I32 or NULL_I64 based on storage width
  BOOL/U8                      non-nullable
  SYM                          sym ID 0
  STR                          empty string (length 0)

Vec-level dispatch (unchanged):
  attrs & RAY_ATTR_HAS_NULLS   set whenever the vec might contain any null
                               element; cleared only when the vec is
                               provably null-free
  attrs & RAY_ATTR_SLICE       unchanged; slices inherit the parent's
                               sentinel-bearing buffer
  ray_vec_has_any_nulls(v)     trivial inline accessor for the attr bit;
                               replaces ad-hoc (attrs & HAS_NULLS) reads
                               where useful for readability

Per-element queries (changed):
  ray_vec_is_null(v, i)        REMOVED. Callers compare the slot directly.
  ray_vec_set_null(v, i)       writes the type-correct sentinel into
                               payload[i] and ORs HAS_NULLS into v->attrs.
                               No longer touches bitmap storage.
  RAY_ATOM_IS_NULL(x)          checks the payload union field for the
                               type's sentinel value (and RAY_NULL_OBJ for
                               the untyped null singleton).
                               No longer reads nullmap[0] & 1.

Storage (changed):
  ray_t.nullmap[16]            RENAMED to ray_t.aux[16] (or equivalent
                               neutral name). No longer used for null
                               tracking; remains as union scratch for the
                               other arms.
  ext_nullmap pointer arm      REMOVED. The pointer-pair arm becomes
                               { sym_dict, _reserved } or similar — only
                               sym_dict survives from the original pair.
  ray_vec_nullmap_bytes()      REMOVED. No callers post-migration.
```

## Revised strategy (after first execution attempt — 2026-05-18)

**What the first attempt revealed.** The original Stage A plan flipped `ray_vec_is_null` to sentinel-based on the assumption that Phase 2 / 3a / 3a-13 had already closed all producer-side dual-encoding gaps (per the language in `include/rayforce.h:309-346`). The flip produced ~40 test failures + 1 ASAN SEGV across 9 test files and ~17 operator source files; reverted at commit `f8a2e9c0`. **The doc's claim was overstated** — many producers (cast_vec_copy_nulls, csv_write_cell consumer side, sort sentinel reorder, window lag/lead, str ops, etc.) still write the bitmap without writing the type-correct sentinel into the payload, or read the bitmap to drive subsequent sentinel-fill loops.

**Refined order:** instrument the consumer side first, run the suite under the instrumentation to enumerate the divergent producers, fix each producer one-at-a-time, then flip the source of truth.

### Stage 0 — Consistency-check instrumentation (NEW, completed 2026-05-18)

- `ray_vec_is_null` cross-checks the bitmap answer against `sentinel_is_null` when built with `-DRAYFORCE_NULL_AUDIT`. On divergence, the call site's return address is recorded and a one-shot stack trace is dumped to stderr (deduplicated by caller, max 128 unique sites).
- New `make audit` target builds + runs the full suite with the audit enabled.
- Baseline audit (master `717feba8` + branch through commit `45661964`): **142 unique divergent call sites** across `src/ops/{window,sort,string,idxop,join,expr,builtins,fused_topk,filter,linkop,query}.c`, `src/io/csv.c`, `src/table/dict.c`, `src/lang/{format,eval}.c`, plus test fixtures that exercise those operators. Distribution captured at audit time:
  ```
  18 src/ops/window.c          5 src/vec/vec.c          2 src/lang/internal.h
  18 test/test_window.c        5 src/ops/string.c        2 src/ops/internal.h
  13 test/test_index.c         5 src/ops/idxop.c         2 src/ops/fused_topk.c
   9 test/test_exec.c          4 src/ops/join.c          2 src/ops/filter.c
   8 test/test_store.c         4 src/ops/expr.c          2 src/table/dict.c
   8 test/test_sort.c          4 src/ops/builtins.c      2 src/ops/linkop.c
   6 src/ops/sort.c            3 test/test_str.c         1 src/ops/query.c
   6 test/test_vec.c           3 test/test_lang.c        1 src/lang/format.c
   5 test/test_fused_topk.c                              1 src/lang/eval.c
                                                        1 src/io/csv.c
                                                        1 test/test_partition_exec.c
                                                        1 test/test_link.c
  ```
- All divergences are `bitmap=1 sentinel=0` (bitmap claims null, sentinel disagrees). Direction confirms the gap is on the producer side: bitmap was set without a corresponding sentinel write.

### Stage 1 — Producer-gap closure (revised — runs BEFORE the flip)

For each `make audit` divergence: trace the offending consumer call back through the test scenario or production caller to identify the upstream producer that set the bitmap bit without writing the sentinel. Fix the producer to dual-write (sentinel + bitmap). Re-run `make audit`; the divergence count strictly decreases.

Order of attack (by leverage — fix producers that account for the most divergences first):
1. `cast_vec_copy_nulls` in `src/ops/builtins.c:748` — accounts for the `(as 'T [...])` cast paths.
2. Window kernels in `src/ops/window.c` (lag/lead/first/last/running aggregates) — 18 divergence sites concentrated here.
3. Sort sentinel reorder in `src/ops/sort.c` — null-position policies must write the dest sentinel.
4. Join window/asof null-key handling in `src/ops/join.c`.
5. Index attach paths in `src/ops/idxop.c` (zone_scan_int/float, attach_hash, attach_bloom).
6. String ops in `src/ops/string.c` and `src/ops/strop.c`.
7. Misc lower-volume sites in `expr.c`, `linkop.c`, `filter.c`, `fused_topk.c`, `dict.c`, `query.c`, `csv.c`, `format.c`, `eval.c`.

Each producer fix is one commit. The `test/rfl/null/sentinel_only_baseline` test plus any new per-producer regression test gates the fix. The Stage 1 exit gate is: `make audit` reports zero divergences across the full suite (including the existing `test/rfl/null/*` and the new `sentinel_only_baseline`).

### Stage 2 — Flip source of truth (formerly Stage A3/A4)

Once Stage 1 is clean, `ray_vec_is_null` and `RAY_ATOM_IS_NULL` switch their definitions to sentinel-based. The audit instrumentation can stay in place as a regression net for the remaining stages; remove it in Stage 5.

### Stage 3 — Drop bitmap writes (formerly Stage C)

### Stage 4 — Remove bitmap storage (formerly Stage D)

### Stage 5 — Cleanup (formerly Stage E)

Remove the audit instrumentation, the `make audit` target, and `RAYFORCE_NULL_AUDIT` references.

### Stage 6 — Verify + completion PR (formerly Stage F)

---

## Original work plan (high level, superseded by the Revised Strategy above for stage ordering)

All work happens on `sentinel-migration-finish`. Commits are structured for review; the final PR squashes/merges as a single completion against master.

### Stage 1 — Consumer audit & test baseline

1. **Catalog every reader** of `ray_vec_is_null`, `nullmap[0] & 1`, `nullmap[`*n*`]`, `RAY_ATOM_IS_NULL`, `ext_nullmap`. Group by file/operator. The catalog lives in this design doc (appendix, populated during Stage 1 of implementation).
2. **Run the full test suite** at the branch base, save the result, and add any thin-coverage regressions identified during the audit. Specifically: tests that exercise sentinel-only reads (no bitmap fallback) on every operator that currently reads the bitmap.

### Stage 2 — Consumer cutover

Operator by operator, convert per-element null queries to sentinel compares. For each conversion:

- Replace `ray_vec_is_null(v, i)` with the type-dispatched sentinel compare on `ray_data(v)[i]`.
- Replace `(x->nullmap[0] & 1)` atom checks with payload-union sentinel compare on `x`.
- Keep the surrounding `(attrs & HAS_NULLS)` gate intact — only the inner per-element query changes.
- Run the relevant test subset after each operator.

Operators expected in scope (from the audit): `collection.c` (count/sort/distinct/group entry paths), `strop.c` (string ops), `dict.c` (key handling), `morsel.c` (morsel-level null routing), `vec.c` (the helpers themselves), plus any operator-specific paths surfaced by the audit.

### Stage 3 — Producer cutover

1. Strip bitmap writes from `ray_vec_set_null` — it now writes only the sentinel and the `HAS_NULLS` attribute.
2. Strip bitmap maintenance from every other producer that currently dual-writes. The Phase 3a-13 / Phase 2g / Phase 2e sites already write the sentinel; this stage just removes the parallel bitmap write.
3. Remove `ext_nullmap` allocation in `ray_vec_new` / wherever the >128-element bitmap currently allocates.

### Stage 4 — Storage reclamation

1. Rename `ray_t.nullmap[16]` → `ray_t.aux[16]` (final name TBD during implementation — keep it short and neutral).
2. Remove the `ext_nullmap` member from the pointer-pair union arm; keep `sym_dict`. The arm becomes `{ sym_dict, _reserved }` or collapses if no other consumer needs the second pointer.
3. Update the union doc comment in `include/rayforce.h` to drop the per-element-null-bitmap arm description.
4. Remove `RAY_ATOM_IS_NULL`'s bitmap-bit fallback (it becomes a pure sentinel + `RAY_NULL_OBJ` check).

### Stage 5 — Doc + cleanup

1. Replace the multi-phase historical block in `include/rayforce.h` (lines ~309–346) with the final sentinel-only contract.
2. Update `.claude/skills/sentinel-null-conventions/SKILL.md` to drop the dual-encoding language and reflect the final state.
3. Remove dead code: `ray_vec_is_null`, `ray_vec_nullmap_bytes`, any `bitmap` helpers in `vec.c` with no remaining callers.
4. Final perf check against the benchmark suite.

### Stage 6 — Single completion PR

One PR against master, titled "Sentinel-null migration: complete cutover." Body summarises the end-state contract and links this design doc. No interim PRs.

## Test strategy

- **Regression coverage:** the existing `test/rfl/null/*` suite (including `f64_dual_encoding.rfl`, `integer_dual_encoding.rfl`, `grouped_agg_null_correctness.rfl`) must keep passing — these were written to detect *dual-encoding* divergence, but they also detect any "null produces wrong value" regression as a side effect.
- **New tests added in Stage 1:**
  - Per-operator "sentinel-only" tests: write a vec where the bitmap arm is deliberately wrong (or simulated absent), confirm the operator still gets the right answer via sentinel.
  - `HAS_NULLS=0` fast-path tests: write a vec with `HAS_NULLS` clear and confirm every operator takes the fast path with no per-element checks.
- **Sanitizer pass:** ASAN/UBSAN run on the branch after Stage 4 — the renamed union arm is the highest-risk change for stale pointer arithmetic. The `sanitizer-output-interpreter` agent can triage failures.
- **Perf pass:** benchmark suite (h2o + clickbench bottleneck) before merging. `perf-regression-reviewer` agent compares branch vs. master baseline.

## Risks

| Risk | Mitigation |
|---|---|
| Missed consumer still reads bitmap → silent wrong-result | Stage 1 audit must be exhaustive; sentinel-only tests in Stage 1 catch the rest |
| `HAS_NULLS` falsely cleared by a producer → sentinel slot read as a real value | Same producer rule has always existed; no new risk, but worth verifying every `attrs &= ~HAS_NULLS` site clears it only after a confirmed scan |
| `INT_MIN` user value collides with sentinel | Accepted hazard, documented at Phase 3a; persists |
| Slice over nullable parent loses null awareness | Slice shares buffer → sentinels visible through view; targeted test confirms |
| Perf regression from losing bitmap fast lookup | `HAS_NULLS` attribute survives; per-element lookup becomes a sentinel compare (single instruction). Measure to confirm |
| Branch lifetime causes merge conflicts with concurrent work | Migration touches ~700 sites; merge conflicts inevitable. Plan: rebase weekly off master; no avoidance |

## Appendix — Consumer catalog (populated in Stage 1)

*(To be filled during implementation Stage 1. Format: file:line → which API → which operator → conversion notes.)*

## Open questions

None at design time. Implementation may surface decisions (e.g. final name of the renamed union arm, whether `RAY_ATOM_IS_NULL` becomes an inline function or stays a macro); those are tactical and resolved on the branch.

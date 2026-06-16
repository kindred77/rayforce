# Aggregation Engine Redesign — Design

| | |
|---|---|
| **Date** | 2026-06-14 |
| **Status** | Design, approved for planning. Clean-room redesign of the group-by / aggregation subsystem. |
| **Scope** | Replace the per-benchmark "fast-path zoo" in the group-by engine with a composable, three-axis architecture. Internal contracts may break; conformance oracles will be re-baselined from the new engine after differential verification. |

## 1. Problem

The current group-by / aggregation engine welds three independent concerns into monolithic,
benchmark-shaped operators. Concrete evidence in the current tree:

- **Operator zoo.** ~8 opcodes / ~13 execution paths: generic hash, direct-array (low-card),
  radix v1, radix v2, SoA top-K candidate filter, `OP_FILTERED_GROUP` (count1 / multi), and six
  `OP_GROUP_*_ROWFORM` operators (`TOPK`, `BOTK`, `PEARSON`, `MAXMIN`, `MEDIAN_STDDEV`, `SUM_COUNT`).
  Anchor comments name the literal benchmark each serves (`h2o q6/q7/q9/q10`).
- **Perf cliffs from benchmark-shaped gates.** Gates match literal query form:
  `MAXMIN_ROWFORM` requires aggregates in exactly the order `(max x)` then `(min y)`
  (`query.c:7066`); `MEDIAN_STDDEV_ROWFORM` requires all aggregates reference the same column
  by `sym` (`query.c:7101-7114`). Writing the aggregates in another order silently falls to the
  generic path — an invisible, undiagnosable performance regression.
- **Double bookkeeping.** Each fast path's admission predicate is duplicated in the planner
  (`query.c:7060-7212`) and the executor (`group.c:11877`, `12453`, `13038`), kept in sync "by
  convention" (`group.c:12494`: *"Planner gates non-nullable; defensive guard here too."*).
- **No transparent fallback for this family (correction to prior review).** The rowform `root`
  runs through the common `ray_optimize` + `ray_execute` (`query.c:8041-8042`) and the result is
  **not** inspected for `nyi` and retried. If the executor's defensive guard fires (planner looser
  than executor), `ray_error("nyi", ...)` propagates to the user. Correctness today is preserved
  only because planner and executor read identical column metadata from the same table, so they
  cannot diverge per-query — the duplication is a maintenance hazard, not a runtime safety net.
- **Divergent null correctness across paths.** Null-skip is attr-gated (`group.c:181`,
  `RAY_ATTR_HAS_NULLS` selects a compile-time-specialized loop). `MEDIAN_STDDEV` and `SUM_COUNT`
  gate non-nullable in both planner and executor; `MAXMIN_ROWFORM` checks nulls in **neither**.
  The zoo replicates the null gate N times and the copies have already diverged.

Root cause: there is no composition along the natural axes. One "kitchen-sink" accumulator
(`reduce_acc_t`, holding every aggregate's state at once) plus a giant finalize switch
(`group.c:2046-2229`) does not scale, so each hot benchmark form gets a bespoke monolithic
operator fusing *aggregates × grouping strategy × output form*. Complexity grows as N×M×2.

## 2. Goals / Non-goals

**Goals**
- Dissolve the operator zoo into composition along three axes; complexity grows as N+M+2.
- Eliminate perf cliffs: grouping-strategy choice is independent of the aggregate set; queries
  are canonicalized before any matching; path choice is inspectable via `explain`.
- Eliminate double bookkeeping: a single strategy selector; the executor trusts the plan.
- Centralize null handling so correctness cannot diverge between paths.
- Preserve the performance-critical properties that made the rowform paths win
  (state density, monomorphic kernels, batch granularity, fusion, parallelism).

**Non-goals**
- Changing the on-disk format. Sentinel-null representation stays (see §6, option A).
- Query compilation / JIT. The chosen mechanism is a composable vectorized registry (no compiler).
- Redesigning join/window/sort. Only the group-by/aggregation subsystem is in scope.
- Bit-for-bit conformance with the current oracles. Oracles are re-baselined from the new engine
  after differential verification (§7).

## 3. Architecture — three axes, four modules

The monolithic operators are cut along their seams into four modules with narrow interfaces.

```
                    ┌─────────────────────────────────────────┐
   AggSpec[]  ──►   │  agg_planner   (single gate)             │
   (from select)    │  • is this a group-by? pick strategy     │
                    │  • resolve (agg_kind, in_type) → vtable   │
                    └───────────────┬─────────────────────────┘
                                    │ AggPlan {strategy_id, accs[]→vtable}
                                    ▼
        ┌──────────────────┐   gid[]   ┌──────────────────────┐
        │ grouping_driver  │ ────────► │  agg_engine          │
        │ (strategy vtable)│  per row  │  • per-group arena    │
        │  hash/radix/     │           │  • for each acc:      │
        │  direct/sort     │ ◄──────── │    update_batch()     │
        └──────────────────┘  values[] │  • merge / finalize   │
                                        └──────────┬───────────┘
                                                   │ finalized states
                                                   ▼
                                        ┌──────────────────────┐
                                        │  output_assembler    │
                                        │  one-pass build+order │
                                        └──────────────────────┘
```

| Module | Single responsibility | Depends on | Does NOT know |
|---|---|---|---|
| `grouping_driver` | row → group-id; parallel fork-join | key types | which aggregates run |
| `agg_registry` | `(agg_kind, in_type)` → accumulator vtable | types | grouping, output form |
| `agg_engine` | drive strategy + accumulators; own the state arena | both above | concrete aggregates (only via vtable) |
| `output_assembler` | finalized states → result columns, one pass, deterministic order | tracked group metadata | how it was computed |

**Zoo-killing invariant:** `agg_engine` references aggregates **only** through the vtable.
Adding `pearson` = registering one vtable, not writing an operator plus two gates.

**Agreed constraint:** the `grouping_driver` is fully aggregate-agnostic. It cannot peek at the
aggregate set to shortcut (e.g. "only max needed"). This is the price of composition and the
direct cure for the order-sensitivity perf cliff.

Code reorganization: `group.c` (~13K lines) splits into `grouping/` (strategies),
`agg/` (registry + engine), `agg_out.c` (assembler). The six `OP_GROUP_*_ROWFORM` opcodes and
their paired gates in `query.c` are deleted; their role is expressed by `AggPlan`.

## 4. Accumulator interface (axis: aggregates)

```c
typedef enum { ACC_STREAMING, ACC_BUFFERED } acc_kind_t;

typedef struct {
    uint16_t   state_size;   /* bytes of inline per-group state */
    acc_kind_t kind;
    int8_t     out_type;     /* result column type */
    void   (*init)        (void* state);
    void   (*update_batch)(void* states_base, size_t stride,
                           const uint32_t* gids, const void* vals,
                           const ray_valid_t* valid, int64_t n,
                           acc_arena_t* arena);   /* arena=NULL for STREAMING */
    void   (*merge)       (void* dst, const void* src, acc_arena_t* arena);
    ray_t* (*finalize)    (const void* state, acc_arena_t* arena);
} agg_vtable_t;
```

**1. Batch granularity, not per-row.** One indirect call per `(accumulator × morsel)`; the inner
loop is a tight monomorphic kernel. No vtable call per row, no JIT.

**2. Type specialization, no in-loop branching.** The registry is keyed by `(agg_kind, in_type)`
→ a monomorphic vtable (`sum_i64`, `sum_f64`, `max_i32`, ...). Type is resolved once at planning;
the loop is branch-free over the type. This recovers rowform-level throughput without a compiler;
state density is recovered because `state_size` covers only the requested aggregates.

**3. Universal buffered-eval accumulator = built-in oracle.** The registry contains one catch-all
`ACC_BUFFERED` accumulator that buffers a group's value slice and calls `ray_eval` — exactly what
today's eval-fallback does. Consequences:
- Any aggregable expression computes (no "no path → error"); generality is preserved.
- Specialized vtables are purely additive. The differential harness compares each `(agg, type)`
  vtable against buffered-eval. The "fallback is oracle" discipline is structural: the oracle lives
  in the same registry, not a parallel universe.

**4. State layout — `base + stride`, unified across strategies.** The strategy supplies the cursor;
the accumulator does not distinguish layouts:
- `direct-array` (dense gid): SoA — each accumulator owns one contiguous array, `stride = state_size`,
  `base` = that array. Vectorized, branch-free.
- `hash` / `radix` (gid from table): AoS row layout — per group a contiguous block of all states
  (one cache line per group), `stride = row_width`, `base` = the accumulator's offset in the row.

`update_batch` writes to `states_base + gids[i]*stride`; both layouts share one code path.

**5. Holistic vs streaming.**
- `ACC_STREAMING` (fixed small state): `sum`, `count`, `min`, `max`, `avg`, `stddev`, and
  `pearson` (6 fixed accumulators Σx, Σy, Σx², Σy², Σxy, n — not holistic).
- `ACC_BUFFERED` (per-group side buffer via `arena`): `median` (quickselect at finalize),
  `top-k` (bounded heap). The inline slot holds a handle into the arena.
- **Merge:** with radix partitioning a group lives entirely in one partition, so buffered state
  needs no cross-worker merge; for hash/direct without partitioning, buffered merge = buffer concat.

**6. Null — explicit validity, centralized (not attr-gated per kernel).** `update_batch` always
receives `ray_valid_t`; null-skip is uniform inside the accumulator. The engine derives the
validity view once per column. An accumulator physically cannot sum a sentinel as a number. The
fast (no-null) path is still available — the engine selects it from the column attribute — but the
decision is made in **one** place and honored by all accumulators, instead of being re-implemented
in ~15 paths and 6 operators (one of which, MAXMIN, forgot it). See §6 for the desync root cause.

## 5. Grouping driver and strategy selection (axis: grouping)

```c
typedef struct {
    strategy_id_t id;
    int (*assign)(grouping_ctx* ctx, const key_batch* keys, uint32_t* gids_out);
    int (*finish)(grouping_ctx* ctx, group_keys_out* out);
} grouping_strategy_t;
```

**Strategies — general physical methods, not benchmark forms:**

| Strategy | When | gid |
|---|---|---|
| `direct-array` | low-card int/temporal/SYM keys, product of ranges ≤ threshold | packed offset |
| `radix-hash` | general parallel default | per-partition open addressing |
| `sort-based` | keys already sorted / order required (optional, v2) | by run boundaries |

A single-partition `hash` is `radix-hash` with `P=1`; no separate strategy.

**Selector — the anti-cliff core. Four rules:**

1. **Choice depends only on data/key-type properties. The aggregate set never influences the
   strategy.** This alone kills the "wrong aggregate order → different path" class: aggregate
   order/composition is not an input to grouping.
2. **Canonicalize before any matching.** `AggSpec[]` is normalized (sort by `(kind, col)`, dedup);
   behavior is invariant to written order. Two semantically equal queries → identical plan.
3. **Single source of truth; the executor does not re-decide.** `select_strategy(key_descs, ranges)
   → strategy_id` is one function with a declarative threshold table. The planner runs it; the plan
   carries `strategy_id`; the executor takes it. No defensive re-gate, no `nyi` divergence. This
   removes the planner↔executor double bookkeeping and the "planner looser → error to user" failure
   mode.
4. **Sound choice, not speculative.** `direct-array` eligibility needs the real key min/max, so the
   selector does one cheap centralized range pre-scan. A chosen strategy is always executable → the
   executor never bails. (Future option: adaptive `direct-array` that promotes to `radix` on
   overflow; for v1 sound selection is simpler and also closes the failure mode.)

**Diagnosability.** Because the selector inputs are explicit, `explain` prints e.g.
`strategy=direct-array (2×i32, 4096 slots); radix rejected: cardinality within threshold`. The
cliff is no longer invisible.

## 6. Null handling decision (option A)

Sentinels stay; null derivation is centralized; the desync root cause is closed by validation,
not by a format change.

- **Representation:** unchanged (sentinels + `RAY_ATTR_HAS_NULLS`). On-disk format untouched.
- **Centralization:** the engine derives one `ray_valid_t` per column; all accumulators honor it
  (§4.6). The attr-gated fast no-null path remains, selected in one place.
- **Validator:** a debug-build assertion at subsystem boundaries (table constructors, gather/`at`
  output, IPC receive, storage load): *attribute clear ⟹ no sentinels physically present*. Runs
  across the conformance set.
- **Producer fixes:** fix producers that drop the attribute while emitting sentinels. First
  suspect to verify: `at`-gather result saved to a table (claimed by prior review; confirm against
  the live binary before asserting).
- **Bonus:** the "MAXMIN forgot the null check" hazard disappears structurally — there is no
  separate rowform operator; there is one path through `valid`.

Not in scope here (separate ADR if ever pursued): validity bitmap (option B), which would remove
the desync class structurally but changes the on-disk format.

## 7. Output assembler and group order (axis: result assembly)

The third axis is result *assembly*, not a columnar-vs-row user choice — a group-by result is
always a columnar table. The assembler's value is one-pass construction (no regather) and
strategy-independent deterministic order; the internal SoA/AoS state layout is fixed by the
chosen strategy (§4.4), not a separate plan field.

**Responsibility:** finalized states + key representatives → result-table columns, in **one pass,
no intermediate materialize+regather**. SoA: accumulator `k`'s array, compacted over occupied gids,
→ column `k`. AoS: gather field `k` across rows → column `k`. The columnar-intermediate + regather
that the current generic path performs is not built.

**Group order — deterministic and strategy-independent (the key fix).** Order is enforced by the
assembler over *any* strategy: choosing `direct-array` or `radix` yields the **same** output order.
Order is derived from explicitly tracked metadata (first-row index per group), not from how
partitions physically landed. This gives determinism by construction and removes the `BOOL`-reorder
post-process (`query.c:8067`) and the non-deterministic fused-filtered-group order as a class.

**Canonical order: first-occurrence** (groups in order of first key appearance in the input).
Cheap (track min row-index per group), no sort. Sorting by key, when needed downstream, is a
separate sort operator (`asc:`/`desc:`), not group-by's job — group-by does exactly one thing.

> **Correction (2026-06-15, found during Phase 1a).** The existing engine does NOT emit
> first-occurrence order: the dense single-key accumulator path emits **sorted ascending by key**,
> BOOL is reordered to first-occurrence (`query.c:8067`), and the rowform operators pin their own
> orders — i.e. the old engine's group order is *inconsistent across paths*. Therefore choosing
> first-occurrence is an intentional, **user-visible behavior change** at the eventual default-flip
> (kdb+ `by:` is sorted-by-key), not a no-op generalization of existing behavior. Decision
> **reaffirmed** knowing this: first-occurrence is the v2 canonical order. Consequence: the Phase-1a
> differential compares old-vs-v2 as **multisets** (sorts both sides by key before row compare),
> verifying identical groups+values while intentionally not asserting row order against the old
> engine. v2's own order determinism (across workers/strategies) is a separate gate (§9).

**Empty / all-null groups:** handled by `finalize`, which returns a typed null of the correct type
(not 0), centralized per aggregate.

## 8. Planner collapse

The whole `query.c:6897-7214` block (the `OP_GROUP`/`OP_FILTERED_GROUP` dispatch plus six rowform
gates) collapses into a linear, form-independent chain:

```
select → build canonical AggSpec[]  (sort by (kind, col), dedup)
       → select_strategy(key_descs, ranges)
       → emit ONE node OP_AGGREGATE { strategy_id, accs[]→vtable }
```

`exec.c` gains a single `case OP_AGGREGATE`. The six `OP_GROUP_*_ROWFORM` opcodes, their
executors, and their paired gates are deleted. One node replaces ten opcodes.

## 9. Conformance and testing

The "fallback is oracle" discipline is built in, not bolted on:

1. **buffered-eval accumulator = oracle.** Differential harness: every specialized `(agg, type)`
   vtable vs buffered-eval over random inputs, including null positions.
2. **Strategy equivalence.** `direct-array` ≡ `radix` on the same input — differential test.
3. **Determinism gate (real, not "design intent").** Result byte-identical across `{1,2,8,auto}`
   workers AND across strategies. The thing currently untested becomes a CI test.
4. **Null validator (option A)** runs in debug across the conformance set.
5. **Oracle re-baseline only after** the differential harness passes (vtable≡oracle,
   strategy≡strategy, order deterministic). Goldens encode verified-correct behavior, not "whatever
   came out."

## 10. Migration — phased, each phase independently verifiable

| Phase | What | Verification |
|---|---|---|
| 0 | `agg_registry` + buffered-eval oracle + differential harness, **beside** the old engine (not wired) | vtable ≡ current behavior |
| 1 | `agg_engine` + `radix` strategy + assembler, behind a flag | full conformance, diff vs old engine |
| 2 | `direct-array` strategy + selector; delete generic accumulator paths as coverage is matched | diff + perf gates |
| 3 | delete the six rowform operators + gates + the `OP_FILTERED_GROUP` special case | new engine subsumes them |
| 4 | re-baseline oracles, flip default, remove flag | perf gates G1/G2 |

## 11. Performance risk and the anti-recidivism escape hatch

Main open question: does composition match the hand-tuned rowform kernels? No structural reason for
regression — the design preserves state density, monomorphic kernels, batch granularity, fusion,
and parallelism — but it must be measured against the existing perf gates (G1/G2) per query.

**Principled escape hatch (so fusion never re-grows the zoo):** if profiling shows two accumulators
lose to a fused one (e.g. sum+count), register a *composite vtable* keyed by the canonical aggregate
set — through the **same** interface and the **same** oracle check — never a bespoke operator with
its own gates. Fusion, when needed, stays inside the composable frame.

## 12. Decisions log

- Scope: clean redesign of the subsystem (oracles re-baselined).
- Mechanism: composable vectorized registry (no JIT).
- Axes: all three redesigned from scratch (grouping driver, aggregate registry, output assembler).
- Driver is aggregate-agnostic.
- Null: option A (sentinels + centralized validity + debug validator + producer fixes).
- Executor trusts the plan's `strategy_id`; no defensive re-gate.
- Group order: first-occurrence; sorting is a separate operator.

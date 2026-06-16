# Aggregation Engine Redesign — Phase 1c Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit docs under `docs/design/`.

**Goal:** Parallelize v2 group-by — **both grouping and aggregation** — via two-phase hash aggregation (per-worker local tables → merge), behind the existing flag, preserving first-occurrence order and staying byte-equivalent (multiset) to the old engine. The serial path remains for inputs below the parallel threshold.

**Architecture:** Above `RAY_PARALLEL_THRESHOLD` (65536 rows), `exec_group_v2` runs:
- **Phase A (parallel, `ray_pool_dispatch` over rows):** each worker builds a LOCAL open-addressing hash table {key-tuple → local gid}, tracking per local group a min-row-index (`first_row`) and AoS-packed per-aggregate state (all aggs' states contiguous per group). Workers touch only their own per-worker structures → no shared mutation, no locks.
- **Phase B (serial merge):** fold all per-worker local tables into one GLOBAL table. Re-read each local group's keys at its `first_row` to find/create the global slot; merge AoS state blocks via each accumulator's `merge`; global `first_row = min`.
- **Phase C (order + emit):** sort global groups by `first_row` ascending (= first-occurrence order), then emit key columns (gather at `first_row`) + finalized aggregate columns.

Reuses Phase-0 accumulators (`init`/`update_batch`/`merge`/`finalize`) and Phase-1a/1b helpers (`agg_read_key_i64`, `agg_gather_key_col`, `agg_resolve`) unchanged.

**Tech Stack:** C11, `ray_pool` fork-join, in-repo harness. Builds on Phase 1b (branch `feat/agg-engine-phase0`, through `e8c2fdb2`).

**Reference (design):** `2026-06-14-...-design.md` §5 (parallel driver), §7 (first-occurrence order), §9 (determinism gate).

---

## Scope

**In scope (1c):** parallel two-phase hash aggregation for the cases v2 already admits (1–16 keys, Phase-0 agg set), gated at `RAY_PARALLEL_THRESHOLD`; serial path retained below it. Determinism: identical result (as a multiset; and identical first-occurrence order) regardless of worker count.

**NOT in scope:** the radix-partition optimization (Phase A here duplicates per-worker state → memory ≈ workers × cardinality; acceptable for now, no memory gate until flip — a radix strategy that avoids duplication is deferred to Phase 2's selector). No new aggregates, no `ACC_BUFFERED`. Flag still defaults off.

**Known limitation (document, don't fix here):** per-worker local tables duplicate group state across workers. For very high cardinality × many workers this is memory-heavy. Phase 2's strategy selector will add a radix/disjoint-partition path; until then 1c is correctness- and parallelism-complete but not memory-optimal.

---

## Parallel-safety notes (verified)
- `ray_pool_get()` → pool; `ray_pool_total_workers(pool)` → `nw`; `ray_pool_dispatch(pool, fn, &ctx, nrows)` calls `fn(ctx, worker_id, start, end)` for contiguous morsel chunks; a worker may get MULTIPLE chunks → accumulate into per-worker state keyed by `worker_id` (persist across that worker's chunks). Mirror the parallel-reduce pattern at `src/ops/group.c:2143-2162`.
- Per-worker structures are disjoint (indexed by `worker_id`) and only that worker writes them → no data race. Shared reads of `key_cols`/value-column data are read-only → safe. Accumulators allocate no `ray_t` during `update_batch`/`merge` (Phase-0 streaming accumulators are plain) → no refcount-sync concern; do NOT allocate/free `ray_t` inside the dispatched fn.
- `first_row` must be the **min** row index across a worker's (possibly out-of-order) chunks — track via `min`, not "first seen".

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_engine.c` | Modify | Per-worker local table type; Phase A dispatch fn; Phase B merge; Phase C order+emit; parallel/serial branch in `exec_group_v2`. |
| `src/ops/agg_engine.h` | Modify | Possibly expose a parallel entry for a direct unit test (or keep internal + validate via differential). |
| `test/test_agg_engine.c` | Modify | High-cardinality N=70000 differential shape; a determinism test (result identical across forced worker counts, if a knob exists; else parallel≡serial on the same input). |

**AoS state stride:** for the admitted aggregates, precompute `off[a]` = byte offset of agg `a`'s state within a group's block and `block = Σ state_size[a]`. A local/global group is one `block`-byte slab; agg `a`'s state for group `g` is at `base + g*block + off[a]`. `update_batch` for agg `a` is called with `states_base = base + off[a]`, `stride = block`.

---

## Task 1: Per-worker local table + Phase A (parallel group+accumulate)

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Define the per-worker structure and the parallel context**

```c
/* AoS per-group state: block = sum of agg state sizes; off[a] = agg a's offset. */
typedef struct {
    int32_t*  ht;        /* open-addressing: slot -> local gid, -1 empty */
    int64_t   htcap;     /* power of two */
    uint64_t  htmask;
    int64_t*  first_row; /* [cap] min row index per local group */
    char*     states;    /* [cap * block] AoS packed agg states */
    int64_t   ng;        /* local group count */
    int64_t   cap;       /* group capacity (grows) */
    int       oom;       /* set on allocation failure */
} agg_local_t;

typedef struct {
    ray_t**             key_cols;
    const void**        key_data;   /* [n_keys] */
    uint8_t             n_keys;
    uint8_t             n_aggs;
    const agg_vtable_t** vts;       /* [n_aggs] */
    const void**        val_data;   /* [n_aggs] value column data ptr (NULL for COUNT) */
    int8_t*             val_types;  /* [n_aggs] */
    uint8_t*            val_hasnull;/* [n_aggs] */
    uint16_t*           off;        /* [n_aggs] state offset within block */
    uint16_t            block;      /* total per-group state bytes */
    agg_local_t*        locals;     /* [nw] one per worker */
} agg_par_ctx_t;
```

- [ ] **Step 2: Phase A dispatch fn (per chunk: group rows, accumulate aggs)**

For each chunk `[start,end)` for `worker_id`:
1. Build a chunk-local `uint32_t cgid[end-start]` by probing/inserting each row's key tuple into `locals[worker_id]` (same tuple-hash + full-tuple equality-at-first_row as `agg_group_keys`, but into the worker-local table; grow `first_row`/`states` capacity as needed, `init`-ing new groups' state block for every agg via `vts[a]->init(states + g*block + off[a])`). Track `first_row[g] = min(first_row[g], r)`.
2. For each aggregate `a`, call `vts[a]->update_batch(states + off[a], block, cgid, val_a_chunk_base, &valid_a_chunk, end-start, NULL)` where `val_a_chunk_base` and the `valid` view are offset to `start` (so index 0 of the batch is row `start`). For COUNT, `val_a_chunk_base=NULL`, `valid.has_nulls=false`.

> The chunk-local `cgid` buffer: size ≤ morsel; allocate on the stack if morsel-bounded, else `malloc` per chunk (free at chunk end). Offsetting the value view: `valid = { (char*)val_data[a] + start*esz_a, val_types[a], val_hasnull[a] }` and pass `val_data[a] + start*esz_a` as `vals`; confirm `agg_read_key_i64`/`ray_valid_at` index from 0 over the offset base.

- [ ] **Step 3: Build the library** (`make 2>&1 | tail -5`). No test yet — exercised after Phase B/C wire up. Commit deferred to Task 4 (don't commit a non-functional partial). 

---

## Task 2: Phase B — merge per-worker locals into a global table

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Implement the merge**

Global table = same `agg_local_t` shape (or a dedicated global struct). For each worker `w`, for each local group `lg`:
- Re-read its key tuple at `locals[w].first_row[lg]` (keys are in the original columns, addressable by row index), probe the GLOBAL hash (tuple-hash + full-tuple equality verified at the global group's `first_row`).
- If new global group `gg`: allocate block, copy the local state block in (or `init` then `merge`), set `first_row[gg] = locals[w].first_row[lg]`.
- If existing `gg`: for each agg `a`, `vts[a]->merge(gstates + gg*block + off[a], locals[w].states + lg*block + off[a], NULL)`; set `first_row[gg] = min(first_row[gg], locals[w].first_row[lg])`.

> Merging into a fresh global group: simplest correct form is `init` the global block for all aggs, then `merge` the local block in (Phase-0 merges are associative w.r.t. an init'd identity — verify: sum 0+x, count 0+n, min INT64_MAX vs x, etc.). This avoids a separate "copy" path. Confirm each Phase-0 `merge` treats an init'd dst as identity (it does: min/max merge guard on `src->cnt`).

- [ ] **Step 2: Build.** Commit deferred to Task 4.

---

## Task 3: Phase C — order by first_row + emit

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Order + emit**

- Build `order[ngroups]` = group indices sorted by `first_row` ascending (first-occurrence order). Use a stable sort; `first_row` values are distinct across groups (each group's first row is unique) so order is total.
- Build a `first_row_ordered[ngroups]` array (the first rows in emit order) for key-column gather, and emit each KEY column via `agg_gather_key_col(key_cols[k], first_row_ordered, ngroups)`.
- For each aggregate `a`, build its result column of `vts[a]->out_type`, length `ngroups`: for `i in 0..ngroups`, `cell = vts[a]->finalize(gstates + order[i]*block + off[a], NULL)`; write into the column via the same cell→column store used by `agg_run_one` (factor that store into a shared helper `agg_put_cell(out, i, cell)` if not already, so parallel + serial share it).
- Assemble the result table `{key cols..., agg cols...}` exactly as serial `exec_group_v2` (names: key sym; agg `agg_result_col_name`).

- [ ] **Step 2: Build.** Commit deferred to Task 4.

---

## Task 4: Wire `exec_group_v2` to choose parallel vs serial + first green commit

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Branch**

In `exec_group_v2`, after resolving key columns + agg vtables:
```c
    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
        return exec_group_v2_parallel(g, op, tbl, key_cols, key_syms, ext, nrows, pool);
    /* else: existing serial path (agg_group_keys + agg_gather + agg_run_one) */
```
`exec_group_v2_parallel` runs Phase A (alloc `nw` locals, `ray_pool_dispatch`), Phase B (merge), Phase C (order+emit), freeing all per-worker + global scratch on every path.

- [ ] **Step 2: Full suite green (flag off) — nothing changes yet**

Run: `make test 2>&1 | tail -8`. Flag defaults off → suite unchanged (3485/3487). This confirms no build/integration breakage. ASAN/UBSan clean.

- [ ] **Step 3: Commit**

```bash
git add src/ops/agg_engine.c src/ops/agg_engine.h
git commit -m "feat(agg): v2 parallel two-phase group-by (per-worker local tables + merge)"
```

---

## Task 5: Validate — parallel ≡ serial ≡ old engine, and determinism

**Files:** Modify `test/test_agg_engine.c`.

The existing N=70000 differential shapes now route v2 through the PARALLEL path (nrows ≥ 65536) and are compared (multiset) against the old engine — so they already validate parallel correctness once Task 4 lands. Strengthen with high-cardinality + determinism:

- [ ] **Step 1: High-cardinality differential**

Add a shape where group COUNT is large at N=70000 (e.g. single I64 key = `row % 20000` → ~20000 groups; and a 2-key variant). This stresses the merge (many groups, many per-worker locals). Compare (multiset) v2-parallel vs old engine. Expect PASS.

- [ ] **Step 2: Determinism check**

Assert the v2 parallel result is identical (use `table_expect_equal` with the SAME table compared to itself across two runs; OR if a worker-count knob exists — grep `ray_pool_init`/a thread-count env/knob — run with worker counts {1, 2, 8} and assert identical results incl. ROW ORDER, since first-occurrence order must be worker-count-independent). If no runtime worker-count knob is reachable from a test, at minimum assert parallel(N=70000) == serial(same data forced below threshold by a second smaller-but-same-pattern table) for the multiset, and document that a CI worker-count matrix is the proper determinism gate (design §9 / GAPS).
> First-occurrence determinism is the subtle risk: `first_row` is a global row index, and `min` is order-independent, so the emit order (sorted by `first_row`) is worker-count-independent BY CONSTRUCTION. The test should confirm this empirically where possible.

- [ ] **Step 3: Full suite + sanitizers**

Run: `make test 2>&1 | tail -8`. 0 failures; new tests green; existing differential (now parallel) green; flag off. **Run under ASAN+TSan if available** for the parallel path — grep the Makefile for a TSan target; if none, ASAN+UBSan (the default test build) plus the determinism test is the bar. Report whether a data-race detector ran.

- [ ] **Step 4: Commit**

```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 parallel differential (high-cardinality) + determinism"
```

---

## Phase 1c Done — exit criteria

- `exec_group_v2` runs a parallel two-phase hash aggregation above `RAY_PARALLEL_THRESHOLD`, serial below; both produce identical results.
- Multiset differential vs the old engine passes for parallel v2 at N=70000, including high cardinality, multi-key, SYM, F64, nulls.
- First-occurrence order is worker-count-independent (by min-row-index construction); determinism confirmed empirically as far as the harness allows.
- Flag defaults off → zero production change. Full suite green, ASAN/UBSan clean (TSan if available).

**Next (Phase 2):** cost-based strategy selector + query canonicalization (sort aggregates by `(kind,col)` so `(min y)(max x)` ≡ `(max x)(min y)`) + `explain` — **kills the order-dependence perf cliff and collapses the planner/executor gate duplication into one selector** — and adds the radix/disjoint-partition strategy that removes 1c's per-worker memory duplication. Then Phase 3 deletes the six rowform operators.
```

# Aggregation Engine Redesign — Phase 2d Implementation Plan (top_n / bot_n)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit docs under `docs/design/`.

**Goal:** Add `top_n`/`bot_n` (OP_TOP_N/OP_BOT_N) to v2 — the LAST rowform aggregate. Each group's result cell is a **vector of up to K values** (native input type; descending for top, ascending for bot), so the output column is a **LIST column**. After 2d, v2 covers every rowform aggregate → Phase 3 can delete the six rowform operators.

**Architecture:** Reuses `ACC_BUFFERED` + `destroy` (2c) but with a **native-typed** per-group buffer (top_n preserves the input type, unlike median's F64). Adds (1) a **K parameter** to `finalize` (from `ext->agg_k[a]`), (2) a **LIST-column assembler path** (when `out_type == RAY_LIST`, collect finalized vector cells into a `ray_list_new` column), (3) gate admission of `agg_k` for top/bot. Value/order parity via the old engine's own `topk_take_vec` (reused in finalize). It stays a uniform accumulator — no OP_TOP_N special path in the engine beyond the generic LIST-output branch.

**Tech Stack:** C11, in-repo harness. Builds on Phase 2c (branch `feat/agg-engine-phase0`, through `e18f4293`).

**Reference:** user-visible shape (rfl): `(top v 2) by k` → one row/group, LIST column, first cell `[30 20]`, `type 'I64` (test/rfl/agg/rowform_topk.rfl:25-33). `topk_take_vec(ray_t* v, int64_t k, uint8_t desc)` (src/ops/sort.c:3214, currently `static` — expose it). `ray_list_new(cap)` / `ray_list_set(list, idx, item)` (src/vec/list.c:42,123). `OP_TOP_N=89`, `OP_BOT_N=90`. K is in `ext->agg_k[a]`.

**Known limitation:** I64+F64 inputs in 2d (narrow ints defer to old engine); per-group native buffers (same memory profile as 2c). Documented, not fixed.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_acc.h` | Modify | `finalize` signature += `int64_t param` (K; 0/ignored for non-holistic). |
| `src/ops/sort.c` / a header | Modify | expose `topk_take_vec` (de-`static` + declare). |
| `src/ops/agg_stream.c` | Modify | top_n/bot_n accumulators (native buffer I64+F64; finalize via `topk_take_vec` → vector cell; `out_type = RAY_LIST`); update ALL existing finalizes for the new param; register. |
| `src/ops/agg_engine.c` | Modify | gate admits `agg_k` for top/bot; pass `agg_k[a]` to finalize; LIST-column assembler path (serial + parallel). |
| `test/test_agg_registry.c` | Modify | direct unit test (single group, vs `topk_take_vec`). |
| `test/test_agg_engine.c` | Modify | LIST-cell support in `table_expect_equal`; top/bot differential. |

---

## Task 1: `finalize` K-parameter (mechanical, all accumulators)

**Files:** Modify `src/ops/agg_acc.h`, `src/ops/agg_stream.c`, `src/ops/agg_engine.c`.

- [ ] **Step 1: Change the finalize signature (agg_acc.h)**

```c
    /* param: per-aggregate integer parameter (K for top_n/bot_n via ext->agg_k[a];
     * 0 and ignored for all other aggregates). */
    ray_t* (*finalize)(const void* state, acc_arena_t* arena, int64_t param);
```

- [ ] **Step 2: Update EVERY existing finalize in agg_stream.c**

Add `int64_t param` (and `(void)param;`) to every `*_final` / `fin_*` / `*_finalize` function (sum/count/min/max/avg/var family/stddev family/pearson/median — all of them). Mechanical. The vtables already reference these by name (designated init) so no vtable-literal change needed beyond the function signatures matching.

- [ ] **Step 3: Update the engine's finalize call sites (agg_engine.c)**

Everywhere finalize is invoked — `agg_run_one`, `agg_run_one_bin`, and the parallel Phase C — pass the K param. Thread `agg_k[a]` (0 if `ext->agg_k` is NULL) to the per-aggregate driver so it can pass it: `vt->finalize(state, NULL, kparam)`. For the existing unary/binary drivers, add a `kparam` argument (default 0).

- [ ] **Step 4: Build green; commit**

`make test 2>&1 | tail -8` — 0 failures, suite unchanged (3506/3508), -Werror clean (every finalize now takes the param; all call sites pass it).
```bash
git add src/ops/agg_acc.h src/ops/agg_stream.c src/ops/agg_engine.c
git commit -m "refactor(agg): finalize takes a per-aggregate int param (K); all accumulators updated"
```

---

## Task 2: Expose `topk_take_vec` + top_n/bot_n accumulators

**Files:** Modify `src/ops/sort.c` (+ a header), `src/ops/agg_stream.c`; test in `test/test_agg_registry.c`.

- [ ] **Step 1: Expose `topk_take_vec`**

De-`static` `topk_take_vec` in src/ops/sort.c; declare `ray_t* topk_take_vec(ray_t* v, int64_t k, uint8_t desc);` in a header agg_stream.c can include (src/ops/internal.h or sort.h — match where other sort helpers are declared). Confirm it returns a NEW vector of v's type with min(k, len) elements, ordered (desc=1 → largest-first; desc=0 → smallest-first). Build to confirm no link/duplicate issue.

- [ ] **Step 2: Failing unit test (test_agg_registry.c)**

Single-group harness for a LIST-returning agg: build I64 col {30,10,20,40}, resolve `agg_resolve(OP_TOP_N, RAY_I64)`, run the accumulator (gids 0), `finalize(state, NULL, /*K=*/2)` → a vector cell; assert it equals `topk_take_vec(col, 2, 1)` (compare element-wise: `[40 30]`) and `type == RAY_I64`. A bot_n case → `[10 20]`. A K>len case (K=10) → all 4 sorted. Call `vt->destroy` after. Expect FAIL (not registered).

- [ ] **Step 3: Implement top_n/bot_n (agg_stream.c)**

Native-typed buffer (reuse the median ACC_BUFFERED pattern but store native values). Per type (I64, F64):
```c
typedef struct { int64_t* buf; int64_t len; int64_t cap; } topk_i64_state;   /* native */
/* init/push(grow)/update_i64 (append d[i])/merge(concat)/destroy(free) — mirror median but int64 */
static ray_t* topk_i64_final(const void* s, acc_arena_t* a, int64_t k){ (void)a;
    const topk_i64_state* st=s;
    ray_t* v = ray_vec_new(RAY_I64, st->len); v->len=st->len;
    memcpy(ray_data(v), st->buf, (size_t)st->len*sizeof(int64_t));
    ray_t* out = topk_take_vec(v, k, /*desc=*/1);   /* top: largest-first */
    ray_release(v); return out; }                   /* out is the LIST cell (a vector) */
static ray_t* botk_i64_final(...){ ... topk_take_vec(v, k, /*desc=*/0); }   /* bot: smallest-first */
```
out_type = `RAY_LIST` (the marker the assembler branches on). kind = ACC_BUFFERED, destroy = the buffer-free. Analogous F64 variants (double buffer, `ray_vec_new(RAY_F64,...)`). Register: `(OP_TOP_N, RAY_I64)→&TOPK_I64`, `(OP_BOT_N, RAY_I64)→&BOTK_I64`, F64 likewise.
> Empty group (len==0): return an empty vector (`ray_vec_new(type,0)`) — confirm that's what the old engine emits for an empty group (a 0-length cell). topk_take_vec on a 0-len vec should yield 0-len.

- [ ] **Step 4: Run unit test → PASS; commit**

```bash
git add src/ops/sort.c src/ops/internal.h src/ops/agg_stream.c test/test_agg_registry.c
git commit -m "feat(agg): top_n/bot_n accumulators (native buffer, reuse topk_take_vec, LIST cell)"
```

---

## Task 3: Gate admits agg_k + LIST-column assembler (serial + parallel)

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Gate — admit top/bot with K**

In `agg_v2_can_handle`'s per-agg loop, replace `if (ext->agg_k && ext->agg_k[a]) return false;` with: allow `agg_k[a]` ONLY when `agg_ops[a]` is `OP_TOP_N`/`OP_BOT_N`, the input is an OP_SCAN of a resolvable type, and `agg_resolve(agg_ops[a], in_type)` is non-NULL; else still reject. (Keep the binary/pearson handling; top/bot are unary with a K param.) Also require K ≥ 1 (`ext->agg_k[a] >= 1`).

- [ ] **Step 2: Pass K + LIST-column assembly — serial (`agg_run_one`)**

`agg_run_one` already takes the gids/ngroups. Add a `kparam` argument (the engine passes `ext->agg_k ? ext->agg_k[a] : 0`). In the finalize loop, pass `kparam` to `vt->finalize`. Then:
- If `vt->out_type == RAY_LIST`: build the result column as a LIST — `ray_list_new(ngroups)`, and for each group set the finalized vector cell via `ray_list_set(list, gi, cell)` (which retains/owns it — confirm ownership; release the local cell ref after set if ray_list_set retains). Return the LIST column.
- Else: the existing flat typed-column path (`agg_put_cell`).
`destroy` still called per group after finalize (buffered).

- [ ] **Step 3: Pass K + LIST-column assembly — parallel (Phase C)**

In `exec_group_v2_parallel` Phase C, thread `agg_k[a]` to the finalize call, and when `vts[a]->out_type == RAY_LIST` build a LIST column (ray_list_new(ngroups) + ray_list_set per group) instead of the flat typed column. The grouping/merge (Phase A/B) is unchanged — top/bot are buffered like median; only the finalize+assemble differs by out_type.

- [ ] **Step 4: Build green; commit**

`make test 2>&1 | tail -8` — suite unchanged (flag off). Commit:
```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 gate admits top/bot K + LIST-column assembler (serial + parallel)"
```

---

## Task 4: LIST-cell comparator + top/bot differential

**Files:** Modify `test/test_agg_engine.c`.

- [ ] **Step 1: Extend `table_expect_equal` / `cell_equal` for LIST columns**

When a result column's type is `RAY_LIST`, compare cell-by-cell: each cell is a vector — compare its type, length, and elements (the existing scalar `cell_equal` logic applied element-wise, or recurse). The composite-sort key columns are still the leading n_keys (scalar) columns, so the row-permutation is built as before; only the per-column compare gains a LIST branch.
> The old engine's top/bot output column is a LIST of native-typed vectors, ordered desc(top)/asc(bot); v2 produces the same via `topk_take_vec`. Element order within each cell matters and must match — both use `topk_take_vec`, so they agree.

- [ ] **Step 2: Add top/bot differential shapes**

`diff_group` runner. Build value data with varied group sizes (some groups smaller than K, some larger). Use `ray_group` — but TOP_N/BOT_N nodes need `agg_k` set: check how `ray_group`/`ray_group2` set `agg_k`, or how the rowform-topk rfl/test builds the node (grep `agg_k` / OP_TOP_N construction; there may be a builder variant or you set `find_ext(...)->agg_k[a]` post-build). Resolve this before writing all shapes. Shapes (each small + N=70000 parallel):
1. single I64 key, `(top v 2)` 
2. single I64 key, `(bot v 3)` over F64
3. two I64 keys, `(top v 2)`
4. K larger than every group (K=100) — cells = full sorted group
5. heterogeneous: single I64 key, `sum` + `(top v 2)` + `count` (LIST col beside scalar cols)
Pass correct n_keys. Match the old engine (multiset over key columns; LIST cells compared element-wise).

- [ ] **Step 3: Run, iterate to green; full suite + ASAN**

`make test 2>&1 | tail -8 && ./rayforce.test -f diff_group`. All shapes match. ASAN clean (top/bot are buffered → exercise the destroy lifecycle + the LIST cells are owned by the result list). If mismatch: K-node construction, cell order (both via topk_take_vec), empty/short-group cell length, or LIST ownership (ray_list_set retain semantics → double-free or leak; ASAN catches).
```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 top/bot differential vs old engine (LIST cells, serial + parallel)"
```

---

## Phase 2d Done — exit criteria

- v2 computes `top_n`/`bot_n` over I64+F64 (1–16 keys, serial + parallel) equal to the old engine: LIST-valued output column, native-typed K-element cells, correct desc/asc order, K>group-size handled, ASAN-clean (buffered lifecycle + LIST ownership).
- The accumulator interface now carries a finalize K-param; the assembler has a generic LIST-output path. No OP_TOP_N special-case in the engine.
- Flag defaults off → zero production change. Full suite green, ASAN/UBSan clean.

**Coverage complete:** v2 now subsumes ALL six rowform operators (MAXMIN, SUM_COUNT, PEARSON, MEDIAN_STDDEV, TOPK, BOTK). **Next: Phase 3** — delete the six `OP_GROUP_*_ROWFORM` operators + their planner/executor gates, route their queries through v2's single order-agnostic path (where the order-dependence cliff + double-bookkeeping die), guarded by the differential. Then the careful flag-default-flip (with the recorded behavior changes: group order, pearson r²→r).
```

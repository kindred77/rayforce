# Aggregation Engine Redesign — Phase 2c Implementation Plan (median / ACC_BUFFERED)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit docs under `docs/design/`.

**Goal:** Add `median` (OP_MEDIAN) to v2 — the first `ACC_BUFFERED` (holistic) aggregate, which holds unbounded per-group state (a buffer of values). Introduces a `destroy` vtable hook for per-group state cleanup and wires it through the serial + parallel engine lifecycles. Output F64, value-parity via the old engine's own `ray_median_dbl_inplace`.

**Architecture:** A buffered accumulator's inline state holds a growable `double` buffer handle `{double* buf; int64 len; int64 cap}`. `update_batch` appends each (valid) value (widened to double) to its group's buffer; `merge` concatenates; `finalize` runs `ray_median_dbl_inplace` → F64 (does NOT free); `destroy` frees the buffer. The engine gains a uniform cleanup rule: for any accumulator with a non-NULL `destroy`, call it on every init'd per-group state once it will no longer be used (after finalize in the serial/global paths; after merge for per-worker local states in the parallel path; on all error paths). This keeps the engine median-agnostic — no special OP_MEDIAN path (no zoo recidivism); the `destroy` hook is reused by 2d (top_n/bot_n).

**Tech Stack:** C11, in-repo harness. Builds on Phase 2b (branch `feat/agg-engine-phase0`, through `3d5f12ef`).

**Reference:** `double ray_median_dbl_inplace(double* a, int64_t n)` (src/lang/internal.h:425); `med_read_as_f64` per-type widening (src/ops/group.c:1570); scalar oracle `ray_med_fn` (src/ops/agg.c:567 — median is F64, empty→typed-null F64); `OP_MEDIAN=88`.

**Known limitation (document, don't fix):** per-group buffers (one growable malloc per group; in the parallel path also per-worker) — memory ≈ values buffered × (1 + duplication across workers). Acceptable for now (no memory gate until flip); a bucket-scatter/arena optimization is future work.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_acc.h` | Modify | vtable += `void (*destroy)(void* state)` (NULL for streaming). |
| `src/ops/agg_stream.c` | Modify | median accumulator (ACC_BUFFERED, I64+F64); register `(OP_MEDIAN, RAY_I64/RAY_F64)`. |
| `src/ops/agg_engine.c` | Modify | call `destroy` on init'd states across serial + parallel lifecycles (after-finalize, after-merge-for-locals, all error paths). |
| `test/test_agg_registry.c` | Modify | direct unit test vs `ray_med_fn`. |
| `test/test_agg_engine.c` | Modify | median differential (single+multi key, I64+F64, even/odd group sizes, nulls, small+N=70000). |

---

## Task 1: `destroy` hook + median accumulator + register

**Files:** Modify `src/ops/agg_acc.h`, `src/ops/agg_stream.c`; test in `test/test_agg_registry.c`.

- [ ] **Step 1: Add the destroy hook (agg_acc.h)**

After `finalize` in the vtable:
```c
    /* Release any heap state owned by a per-group state slab. NULL for
     * ACC_STREAMING (fixed inline state needs no cleanup). The engine calls
     * this on every init'd state once it will not be used again. */
    void   (*destroy)(void* state);
```
Existing designated-init vtables default it NULL — verify the suite still builds after just this addition.

- [ ] **Step 2: Failing unit test (test_agg_registry.c)**

Single-group harness (`run_single_group` already calls finalize; extend it to call `vt->destroy(state)` after reading the result if `vt->destroy` is set, so buffered tests don't leak). Cases vs `ray_med_fn` (materialized, `->f64`, 1e-9):
- I64 {1,3,2,5,4} (odd, 5 elems) → median 3.0
- I64 {1,2,3,4} (even) → median 2.5
- F64 {1.5, 2.5, 0.5} → median 1.5
Assert v2 == `ray_med_fn`. Expect FAIL (not registered).

- [ ] **Step 3: Implement median (agg_stream.c)**

```c
#include "lang/internal.h"   /* ray_median_dbl_inplace — confirm include path */
typedef struct { double* buf; int64_t len; int64_t cap; } median_state;
static void median_init(void* s){ median_state* st=s; st->buf=NULL; st->len=0; st->cap=0; }
static inline void median_push(median_state* st, double v){
    if (st->len == st->cap){ int64_t nc = st->cap ? st->cap*2 : 8;
        double* nb = realloc(st->buf, (size_t)nc*sizeof(double)); /* assume success (2c) */
        st->buf = nb; st->cap = nc; }
    st->buf[st->len++] = v;
}
static void median_update_i64(void* base, size_t stride, const uint32_t* gids,
                              const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const int64_t* d=vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        median_push((median_state*)((char*)base+(size_t)gids[i]*stride), (double)d[i]); }
}
static void median_update_f64(void* base, size_t stride, const uint32_t* gids,
                              const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const double* d=vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        median_push((median_state*)((char*)base+(size_t)gids[i]*stride), d[i]); }
}
static void median_merge(void* dd, const void* ss, acc_arena_t* a){ (void)a;
    median_state* d=dd; const median_state* s=ss;
    for (int64_t i=0;i<s->len;i++) median_push(d, s->buf[i]); }
static ray_t* median_final(const void* s, acc_arena_t* a){ (void)a; median_state* st=(median_state*)s;
    if (st->len==0) return ray_typed_null(-RAY_F64);
    return ray_f64(ray_median_dbl_inplace(st->buf, st->len)); }   /* reuses old routine → value parity */
static void median_destroy(void* s){ median_state* st=s; free(st->buf); st->buf=NULL; st->len=st->cap=0; }
static const agg_vtable_t MEDIAN_I64 = { .state_size=sizeof(median_state), .kind=ACC_BUFFERED, .out_type=RAY_F64,
    .init=median_init, .update_batch=median_update_i64, .merge=median_merge, .finalize=median_final, .destroy=median_destroy };
static const agg_vtable_t MEDIAN_F64 = { .state_size=sizeof(median_state), .kind=ACC_BUFFERED, .out_type=RAY_F64,
    .init=median_init, .update_batch=median_update_f64, .merge=median_merge, .finalize=median_final, .destroy=median_destroy };
```
Register: `if (agg_kind==OP_MEDIAN && in_type==RAY_I64) return &MEDIAN_I64; if (agg_kind==OP_MEDIAN && in_type==RAY_F64) return &MEDIAN_F64;`
> `ray_median_dbl_inplace` mutates its input (partitions it) — fine, the buffer is owned by the state and freed by destroy. `finalize` is `const void*` but median mutates the buffer; cast away const (the buffer contents are scratch). CONFIRM the include for `ray_median_dbl_inplace` (lang/internal.h) compiles in agg_stream.c.

- [ ] **Step 4: Run unit test → PASS; commit**

```bash
git add src/ops/agg_acc.h src/ops/agg_stream.c test/test_agg_registry.c
git commit -m "feat(agg): ACC_BUFFERED destroy hook + median accumulator (I64+F64)"
```

---

## Task 2: Engine lifecycle — call `destroy` on init'd states (serial + parallel)

**Files:** Modify `src/ops/agg_engine.c`.

This is the cross-cutting part: every init'd per-group state of a `destroy`-bearing accumulator must be destroyed exactly once.

- [ ] **Step 1: Serial path (`agg_run_one`)**

`agg_run_one` inits all `ngroups` states, runs update_batch, then loops finalize per group. Add:
- After `agg_put_cell(out, gi, cell)` (and `ray_release(cell)`) in the finalize loop, if `vt->destroy` call `vt->destroy(states + gi*state_size)`.
- On the error path where `out` allocation fails AFTER init (so states are init'd but never finalized): if `vt->destroy`, loop all `ngroups` and destroy each before `free(states)`.
Apply the same to `agg_run_one_bin` (pearson has no destroy, but be uniform — guard on `vt->destroy`).

- [ ] **Step 2: Parallel path — per-worker local states (after merge)**

In `exec_group_v2_parallel`, the per-worker `locals[w]` states are merged into `gt` in Phase B, then the local slabs are freed. BEFORE freeing each local slab, for every buffered agg (vts[a]->destroy != NULL), destroy each local group's state slice: for `lg` in `[0, locals[w].ng)`, for each agg `a` with destroy, `vts[a]->destroy(locals[w].states + lg*block + off[a])`. Add this to the local-cleanup routine (it currently just frees ht/first_row/states). It needs `vts`/`off`/`block`/`n_aggs` — pass them to the cleanup or inline the loop in `exec_group_v2_parallel` where those are in scope.
> Merge copies values (median_merge appends src into dst), so after merge the local buffers are redundant and must be freed via destroy. The global `gt` group buffers now own all values.

- [ ] **Step 3: Parallel path — global states (after finalize)**

In Phase C, after finalizing each global group into the output columns, destroy the global state slices for buffered aggs: after the finalize loop for agg `a`, or interleaved — for each global group `gg`, after finalize, `if (vts[a]->destroy) vts[a]->destroy(gt.states + gg*block + off[a])`. Then free the `gt` slab. On ALL error paths in `exec_group_v2_parallel` (after gt groups have been created with buffers), destroy gt's buffered states before freeing gt — and destroy any not-yet-cleaned local states.
> Be careful: a global buffered state is created during Phase B merge (init then merge). If an error occurs between Phase B and Phase C finalize, those gt buffers leak unless destroyed. Ensure every post-Phase-B error path destroys gt's buffered states.

- [ ] **Step 4: Build + suite green (flag off)**

`make test 2>&1 | tail -8`. Flag off → suite unchanged (3500/3502). Median only runs flag-on (Task 3). ASAN/UBSan clean (ASAN will catch any leak/double-free in the destroy wiring even before Task 3 if any flag-on test exists; Task 3 is the real exercise). Commit:
```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 destroy lifecycle for buffered accumulators (serial + parallel)"
```

---

## Task 3: median differential vs the old engine (+ leak check)

**Files:** Modify `test/test_agg_engine.c`.

- [ ] **Step 1: Add differential shapes**

`diff_group` runner + `table_expect_equal(.., n_keys)`. Build value columns so groups have varied sizes (even AND odd) to exercise the even-count averaging. Shapes (each small + N=70000 parallel):
1. single I64 key, `median` over an I64 value
2. single I64 key, `median` over an F64 value
3. two I64 keys, `median` over an I64 value
4. heterogeneous: single I64 key, `sum` + `median` + `count` (mixed streaming + buffered)
5. single I64 key, `median` over an I64 value WITH HAS_NULLS (null-skip parity)
Use deterministic data; ensure some groups have even and some odd counts. Pass correct n_keys.
> DISCOVER the old grouped-median OUTPUT TYPE: `ray_med_fn` is F64, but the grouped emit path may differ. If a shape's column-type check fails (old engine emits I64 for I64-input median), match it: set v2 MEDIAN_I64 out_type accordingly. Most likely F64 (matching the scalar). The differential pins it.

- [ ] **Step 2: Run, iterate to green**

`make test 2>&1 | tail -8 && ./rayforce.test -f diff_group`. Match the old engine (multiset; values via the shared `ray_median_dbl_inplace` should be bit-identical). If mismatch: output type (above), even-count rule (both use ray_median_dbl_inplace so identical), or null-skip (both skip nulls). 

- [ ] **Step 3: Full suite + LEAK CHECK + commit**

`make test 2>&1 | tail -8` — 0 failures; new shapes green; flag off; **ASAN clean is the critical signal here** — it proves the destroy lifecycle frees every per-group/per-worker buffer with no leak or double-free (the median shapes at N=70000 exercise per-worker locals + merge + global finalize). Confirm no ASAN leak report.
```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 median differential vs old engine (serial + parallel)"
```

---

## Phase 2c Done — exit criteria

- v2 computes `median` over I64+F64 (1–16 keys, serial + parallel) equal to the old engine; the `ACC_BUFFERED` mechanism + `destroy` lifecycle works with no leaks/double-frees (ASAN-confirmed at N=70000).
- The engine stays median-agnostic (uniform accumulator interface; no OP_MEDIAN special path).
- Flag defaults off → zero production change. Full suite green, ASAN/UBSan clean.

**Next:** Phase 2d — `top_n`/`bot_n` (OP_TOP_N=89/OP_BOT_N=90): reuses ACC_BUFFERED + destroy, ADDS the K param (`ext->agg_k[a]`, which the gate currently rejects) and **LIST-valued output** (each group's cell is K values → the result column is a LIST, not a flat typed vector). After 2d, v2 covers every rowform aggregate → **Phase 3 deletes the six rowform operators**.
```

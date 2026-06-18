# Aggregation Engine Redesign — Phase 2b Implementation Plan (pearson)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit docs under `docs/design/`.

**Goal:** Add `pearson` (OP_PEARSON_CORR) to v2 — the first BINARY aggregate (two value columns). Extends the accumulator interface with an optional binary `update_batch2`, wires it through both the serial and parallel drivers and the gate, and proves it equal to the old engine. F64 inputs in 2b; integer-input pearson deferred (gate doesn't admit → old engine handles).

**Architecture:** vtable gains `update_batch2` (binary: x, y, valid_x, valid_y); unary accumulators leave it NULL (designated-init → no change to existing accumulators), pearson sets it (and leaves `update_batch` NULL). State `{sx, sy, sxx, syy, sxy, n}` (5 doubles + count), merges additively (associative → parallel path works). Finalize matches the old formula exactly. The serial and parallel drivers branch: if `vt->update_batch2`, fetch the second column (`agg_ins2[a]`) and call it; else the existing unary path. The gate is extended to admit pearson specifically (still rejects other binary/holistic aggs).

**Tech Stack:** C11, in-repo harness. Builds on Phase 2a (branch `feat/agg-engine-phase0`, through `1805cdbf`).

**Reference:** old formula `src/ops/group.c:3378-3382`; `OP_PEARSON_CORR=79`; ext binary input is `ext->agg_ins2[a]` (ops.h:386). Differential oracle: the old engine's own pearson for the query (flag off).

---

## The exact formula (match the old engine)
```
per group, over rows where BOTH x and y are non-null:
  n = count ; sx = Σx ; sy = Σy ; sxx = Σx² ; syy = Σy² ; sxy = Σxy
  num = n*sxy - sx*sy ; dx = n*sxx - sx*sx ; dy = n*syy - sy*sy
  r   = num / sqrt(dx * dy)          (signed correlation; out_type RAY_F64)
```
The old engine does NOT guard degenerate groups: n<2 or dx*dy<=0 yields NaN/inf from the division/sqrt. v2 must reproduce this (do NOT add guards) so the differential matches — `cell_equal` treats NaN==NaN as equal. A row is counted only if BOTH x and y are non-null (skip if either null).

> DISCOVER-AND-MATCH (do this early): build a perfectly anti-correlated group (x={1,2,3,4}, y={4,3,2,1}) and run the OLD engine (`(pearson 'x 'y) by 'k`, flag off). If it returns ≈ -1.0, the old engine produces signed r (expected) → v2 matches with the formula above. If it returns ≈ +1.0, the old engine produces r² → square the result. Make v2 match whatever the old engine actually outputs; report which.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_acc.h` | Modify | vtable += `update_batch2` (binary) fn pointer. |
| `src/ops/agg_stream.c` | Modify | pearson accumulator (F64); register `(OP_PEARSON_CORR, RAY_F64)`. |
| `src/ops/agg_engine.c` | Modify | gate admits pearson; serial driver (`exec_group_v2` agg loop) + parallel driver (`agg_phaseA_fn` + ctx) binary branch. |
| `test/test_agg_registry.c` | Modify | direct unit test (single group, known r). |
| `test/test_agg_engine.c` | Modify | pearson differential (single + multi key, small + N=70000). |

---

## Task 1: Interface extension + pearson accumulator + register

**Files:** Modify `src/ops/agg_acc.h`, `src/ops/agg_stream.c`; test in `test/test_agg_registry.c`.

- [ ] **Step 1: Extend the vtable (agg_acc.h)**

Add, after `update_batch`:
```c
    /* Binary aggregates (e.g. pearson): two value columns. NULL for unary
     * accumulators. The engine calls update_batch2 instead of update_batch
     * when it is non-NULL. A row contributes only if BOTH x and y are valid. */
    void   (*update_batch2)(void* states_base, size_t stride, const uint32_t* gids,
                            const void* vals_x, const void* vals_y,
                            const ray_valid_t* valid_x, const ray_valid_t* valid_y,
                            int64_t n, acc_arena_t* arena);
```
Place it so existing designated-initializer vtables (which don't set it) default it to NULL. (Designated init guarantees NULL for omitted fields — verify the existing vtables use `.field=` style; they do.)

- [ ] **Step 2: Write a failing unit test (test_agg_registry.c)**

A single-group binary harness (adapt `run_single_group`): build x={1,2,3,4} (F64) and y={2,4,6,8} (F64) (perfectly correlated → r=1.0); resolve `agg_resolve(OP_PEARSON_CORR, RAY_F64)`; run the accumulator (gids all 0) via `update_batch2`; finalize; assert `->f64 ≈ 1.0` (TEST_ASSERT_EQ_F, 1e-9). Add an anti-correlated case y={8,6,4,2}-style → expect ≈ -1.0 (this also confirms signed-r vs r²). Expect FAIL (not registered).

- [ ] **Step 3: Implement pearson + register (agg_stream.c)**

```c
typedef struct { double sx, sy, sxx, syy, sxy; int64_t n; } pearson_state;
static void pearson_init(void* s){ pearson_state* st=s; st->sx=st->sy=st->sxx=st->syy=st->sxy=0; st->n=0; }
static void pearson_update2(void* base, size_t stride, const uint32_t* gids,
                            const void* vx, const void* vy,
                            const ray_valid_t* valx, const ray_valid_t* valy,
                            int64_t n, acc_arena_t* a){
    (void)a; const double* x=(const double*)vx; const double* y=(const double*)vy;
    for (int64_t i=0;i<n;i++){
        if(!ray_valid_at(valx,i) || !ray_valid_at(valy,i)) continue;
        pearson_state* st=(pearson_state*)((char*)base+(size_t)gids[i]*stride);
        double xi=x[i], yi=y[i];
        st->sx+=xi; st->sy+=yi; st->sxx+=xi*xi; st->syy+=yi*yi; st->sxy+=xi*yi; st->n++;
    }
}
static void pearson_merge(void* dd, const void* ss, acc_arena_t* a){ (void)a;
    pearson_state* d=dd; const pearson_state* s=ss;
    d->sx+=s->sx; d->sy+=s->sy; d->sxx+=s->sxx; d->syy+=s->syy; d->sxy+=s->sxy; d->n+=s->n; }
static ray_t* pearson_final(const void* s, acc_arena_t* a){ (void)a; const pearson_state* st=s;
    double dn=(double)st->n;
    double num = dn*st->sxy - st->sx*st->sy;
    double dx  = dn*st->sxx - st->sx*st->sx;
    double dy  = dn*st->syy - st->sy*st->sy;
    return ray_f64(num / sqrt(dx*dy)); }   /* matches old engine incl. NaN on degenerate */
static const agg_vtable_t PEARSON_F64 = { sizeof(pearson_state), ACC_STREAMING, RAY_F64,
    pearson_init, /*update_batch*/ NULL, pearson_merge, pearson_final };
```
> IMPORTANT: this vtable leaves `update_batch` NULL and must set `update_batch2 = pearson_update2`. Use designated initializers: `{ .state_size=..., .kind=ACC_STREAMING, .out_type=RAY_F64, .init=pearson_init, .update_batch2=pearson_update2, .merge=pearson_merge, .finalize=pearson_final }`. Match the field order/style of existing vtables.

Register: `if (agg_kind==OP_PEARSON_CORR && in_type==RAY_F64) return &PEARSON_F64;`

- [ ] **Step 4: Run unit test → PASS (apply DISCOVER-AND-MATCH if anti-correlated ≠ -1); commit**

```bash
git add src/ops/agg_acc.h src/ops/agg_stream.c test/test_agg_registry.c
git commit -m "feat(agg): binary update_batch2 interface + pearson accumulator (F64)"
```

---

## Task 2: Gate admits pearson + serial & parallel drivers (binary branch)

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Gate — admit pearson, keep rejecting other binary/holistic**

Replace the blanket binary rejection. For each agg a:
```c
    if (ext->agg_k && ext->agg_k[a]) return false;     /* holistic K still deferred */
    if (ext->agg_ins2 && ext->agg_ins2[a]) {
        /* only pearson is an admitted binary agg in 2b */
        if (ext->agg_ops[a] != OP_PEARSON_CORR) return false;
        ray_op_t* xin = ext->agg_ins[a]; ray_op_t* yin = ext->agg_ins2[a];
        if (!xin || xin->opcode != OP_SCAN || !yin || yin->opcode != OP_SCAN) return false;
        ray_op_ext_t* xe = find_ext(g, xin->id); ray_op_ext_t* ye = find_ext(g, yin->id);
        ray_t* xc = xe ? ray_table_get_col(tbl, xe->sym) : NULL;
        ray_t* yc = ye ? ray_table_get_col(tbl, ye->sym) : NULL;
        if (!xc || !yc) return false;
        if (xc->type != RAY_F64 || yc->type != RAY_F64) return false;  /* 2b: F64 only */
        if (!agg_resolve(OP_PEARSON_CORR, RAY_F64)) return false;
        continue;   /* admitted */
    }
    /* ... existing unary per-agg checks (COUNT / OP_SCAN + agg_resolve) ... */
```

- [ ] **Step 2: Serial driver binary branch (exec_group_v2 / agg_run_one)**

The serial agg loop currently fetches one `val_col` and calls `agg_run_one`. For pearson, it must fetch x (`agg_ins[a]`) and y (`agg_ins2[a]`) and call the binary update. Add a binary variant `agg_run_one_bin(vt, x_col, y_col, gids, nrows, ngroups)` mirroring `agg_run_one` but calling `vt->update_batch2(states, vt->state_size, gids, ray_data(x_col), ray_data(y_col), &valid_x, &valid_y, nrows, NULL)`. In the loop: `if (vt->update_batch2) col = agg_run_one_bin(vt, x_col, y_col, ...); else col = agg_run_one(vt, val_col, ...);`. Result column name: pearson's name — check how the old engine names it (the suffix map at group.c:4037-4051 has no pearson entry → it likely falls back to the input sym or a different suffix; CONFIRM by reading the naming path for OP_PEARSON_CORR and match it in `agg_result_col_name`, extending that helper if needed).

- [ ] **Step 3: Parallel driver binary branch (agg_phaseA_fn + ctx)**

Extend the parallel context with per-agg second-input arrays: `const void** val2_data; int8_t* val2_types; uint8_t* val2_hasnull;` (NULL/0 for unary aggs). In `agg_phaseA_fn`, when accumulating agg a: `if (vts[a]->update_batch2) vts[a]->update_batch2(states+off[a], block, cgid, val_data[a]+start*esz, val2_data[a]+start*esz2, &valid_x, &valid_y, end-start, NULL); else vts[a]->update_batch(... existing ...)`. Populate val2_data[a] etc. from `agg_ins2[a]` in the parallel setup. Phase B merge (`vts[a]->merge`) and Phase C finalize are unchanged (binary state is just a state).

- [ ] **Step 4: Build + suite green (flag off)**

`make test 2>&1 | tail -8` — builds; suite unchanged (flag off → no behavior change; pearson only runs when flag on, which only the new differential does). Commit:
```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 gate + serial/parallel drivers admit binary pearson"
```

---

## Task 3: pearson differential vs the old engine

**Files:** Modify `test/test_agg_engine.c`.

- [ ] **Step 1: Add differential shapes**

Using the `diff_group` runner + `table_expect_equal(.., n_keys)`. Build x and y F64 value columns with deterministic small data (e.g. `x=(i%89)*1.0`, `y=(i%53)*1.0` for varied correlation; ensure groups have ≥2 distinct rows so dx,dy≠0 and r is finite — OR accept NaN parity if a group is degenerate, since cell_equal treats NaN==NaN equal). `ray_group` with the pearson agg: agg_ops[a]=OP_PEARSON_CORR, agg_ins[a]=scan(x), and you must also set agg_ins2[a]=scan(y) — CHECK how ray_group accepts the second input (grep ray_group / how the rowform-pearson test or query.c builds a binary agg node; the OP_GROUP ext has agg_ins2 — confirm ray_group's signature populates it, or there's a builder variant). 
Shapes (each small + N=70000 parallel):
1. single I64 key, pearson(x,y) F64
2. two I64 keys, pearson(x,y) F64
3. heterogeneous: single I64 key, sum(x) + pearson(x,y) + count
Pass correct n_keys.

> If `ray_group` cannot set `agg_ins2`, find the API that builds a binary-agg group node (grep tests/query.c for OP_PEARSON_CORR construction) and use it. This is the one likely friction point — resolve it before writing all shapes.

- [ ] **Step 2: Run, iterate to green**

`make test 2>&1 | tail -8 && ./rayforce.test -f diff_group`. Match the old engine (multiset, incl. NaN parity for degenerate groups). If mismatch: the r-vs-r² question (apply DISCOVER-AND-MATCH from Task 1), the both-null-skip rule, or the column name. If the old engine for this query uses a path that produces a DIFFERENT pearson value than the generic formula (e.g. rowform produces r² while generic produces r — a known invariant-16.1-style divergence), MATCH whatever the flag-off old engine produces for THIS query shape (that's the differential's definition of correct) and document the finding.

- [ ] **Step 3: Full suite + sanitizers + commit**

`make test 2>&1 | tail -8` — 0 failures; new shapes green; existing green; flag off; ASAN/UBSan clean.
```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 pearson differential vs old engine (serial + parallel)"
```

---

## Phase 2b Done — exit criteria

- The accumulator interface supports binary aggregates (`update_batch2`); unary accumulators unaffected.
- v2 computes `pearson` over F64 inputs (1–16 keys, serial + parallel) equal to the old engine, including NaN parity on degenerate groups.
- Flag defaults off → zero production change. Full suite green, ASAN/UBSan clean.

**Next:** Phase 2c — `median` (OP_MEDIAN): the `ACC_BUFFERED` mechanism (per-group value buffers via the `acc_arena` stubbed in agg_acc.h + quickselect at finalize). Then 2d `top_n`/`bot_n` (arena + K param `agg_k[a]` + LIST-valued output). After full aggregate coverage, **Phase 3 deletes the six rowform operators**. (Integer-input pearson can be widened later by reading x/y via a per-type widening; 2b is F64-only.)
```

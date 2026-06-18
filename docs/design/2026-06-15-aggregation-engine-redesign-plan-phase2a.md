# Aggregation Engine Redesign — Phase 2a Implementation Plan (var/stddev family)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit docs under `docs/design/`.

**Goal:** Add `var`/`var_pop`/`stddev`/`stddev_pop` to the v2 aggregate registry (streaming, fit the existing interface — no new mechanism), so v2 covers these aggregates byte-equivalently to the old engine, in both the serial and parallel paths. This is one slice of "aggregate coverage" — the prerequisite to deleting the rowform zoo (Phase 3). Pearson (2b, binary input), median (2c, arena), top_n/bot_n (2d, arena+LIST) follow.

**Architecture:** Register accumulators keyed `(OP_VAR|OP_VAR_POP|OP_STDDEV|OP_STDDEV_POP, RAY_I64|RAY_F64)`. State `{sum, sumsq, cnt}` (sumsq int64 for I64 inputs, double for F64 — matching the old engine). `merge` adds the three fields (associative → the Phase-1c parallel merge handles it unchanged). `finalize` replicates the old engine's variance formula exactly. The gate auto-admits these once registered (it admits any `agg_resolve`-resolvable OP_SCAN input). No gate, engine, or parallel-path change.

**Tech Stack:** C11, in-repo harness. Builds on Phase 1c (branch `feat/agg-engine-phase0`, through `367e2583`).

**Reference:** old formula at `src/ops/group.c:2190-2204` (ungrouped) and `:2230-2242`; per-op enum values `OP_STDDEV=59`, `OP_STDDEV_POP=73`, `OP_VAR=74`, `OP_VAR_POP=75` (ops.h). Differential oracle functions: `ray_var_fn`/`ray_var_pop_fn`/`ray_stddev_fn`/`ray_stddev_pop_fn` (dispatched group.c:2058-2061).

---

## The exact formula (must match — within the differential's F64 tolerance)
```
cnt = number of non-null values in the group
insufficient: (VAR || STDDEV) → cnt <= 1 ;  (VAR_POP || STDDEV_POP) → cnt <= 0   → result = typed null F64
mean    = sum / cnt
var_pop = sumsq / cnt - mean*mean        (then clamp var_pop = max(var_pop, 0))
VAR_POP     = var_pop
VAR         = var_pop * cnt / (cnt - 1)
STDDEV_POP  = sqrt(var_pop)
STDDEV      = sqrt(var_pop * cnt / (cnt - 1))
```
For I64 input the old engine accumulates `sumsq` as int64 (unsigned-wrap, like SUM at group.c:186) then casts to double in the formula; for F64 it accumulates `sumsq` as double. Match per input type.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_stream.c` | Modify | 8 accumulators (4 ops × {I64,F64}); register in `agg_resolve`. |
| `test/test_agg_engine.c` | Modify | Differential shapes for var/stddev (single + multi key, I64 + F64, small + N=70000). |
| `test/test_agg_registry.c` | Modify | Optional: direct unit test of one accumulator vs a hand-computed value. |

No changes to `agg_engine.c` (gate auto-widens; serial + parallel paths use `agg_resolve`/`merge` already).

---

## Task 1: var/stddev accumulators + register

**Files:** Modify `src/ops/agg_stream.c`; optional direct test in `test/test_agg_registry.c`.

- [ ] **Step 1: Write a failing direct unit test (in test_agg_registry.c)**

Mirror the existing single-group harness (`run_single_group`, `vec_i64`/`vec_f64`). Resolve `agg_resolve(OP_STDDEV_POP, RAY_I64)` etc. and compare against the old oracle `ray_stddev_pop_fn(col)` (lazy → `ray_lazy_materialize`, read `->f64`, `TEST_ASSERT_EQ_F(... , 1e-9)`), over a known column e.g. `{2,4,4,4,5,5,7,9}` (population stddev = 2.0). Cover all four ops over I64 and a couple over F64. Expect FAIL (not registered yet).

- [ ] **Step 2: Implement the accumulators**

```c
/* ---- variance family: shared state {sum, sumsq, cnt} ----------------- */
/* I64 input: sumsq accumulated as int64 (unsigned wrap), matching the old engine. */
typedef struct { double sum; int64_t sumsq; int64_t cnt; } var_i64_state;
static void var_i64_init(void* s){ var_i64_state* st=s; st->sum=0; st->sumsq=0; st->cnt=0; }
static void var_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const int64_t* d=(const int64_t*)vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        var_i64_state* st=(var_i64_state*)((char*)base+(size_t)gids[i]*stride);
        int64_t v=d[i]; st->sum+=(double)v;
        st->sumsq=(int64_t)((uint64_t)st->sumsq+(uint64_t)v*(uint64_t)v); st->cnt++; }
}
static void var_i64_merge(void* dd, const void* ss, acc_arena_t* a){ (void)a;
    var_i64_state* d=dd; const var_i64_state* s=ss;
    d->sum+=s->sum; d->sumsq=(int64_t)((uint64_t)d->sumsq+(uint64_t)s->sumsq); d->cnt+=s->cnt; }
/* finalize: one per op (share a var_pop helper). insufficient → typed null F64. */
static inline double var_i64_varpop(const var_i64_state* st){
    double mean=st->sum/(double)st->cnt;
    double vp=(double)st->sumsq/(double)st->cnt - mean*mean; return vp<0?0:vp; }
static ray_t* fin_var_pop_i64(const void* s, acc_arena_t* a){ (void)a; const var_i64_state* st=s;
    if(st->cnt<=0) return ray_typed_null(-RAY_F64); return ray_f64(var_i64_varpop(st)); }
static ray_t* fin_var_i64(const void* s, acc_arena_t* a){ (void)a; const var_i64_state* st=s;
    if(st->cnt<=1) return ray_typed_null(-RAY_F64);
    return ray_f64(var_i64_varpop(st)*(double)st->cnt/((double)st->cnt-1.0)); }
static ray_t* fin_stddev_pop_i64(const void* s, acc_arena_t* a){ (void)a; const var_i64_state* st=s;
    if(st->cnt<=0) return ray_typed_null(-RAY_F64); return ray_f64(sqrt(var_i64_varpop(st))); }
static ray_t* fin_stddev_i64(const void* s, acc_arena_t* a){ (void)a; const var_i64_state* st=s;
    if(st->cnt<=1) return ray_typed_null(-RAY_F64);
    return ray_f64(sqrt(var_i64_varpop(st)*(double)st->cnt/((double)st->cnt-1.0))); }
static const agg_vtable_t VAR_POP_I64 = { sizeof(var_i64_state), ACC_STREAMING, RAY_F64,
    var_i64_init, var_i64_update, var_i64_merge, fin_var_pop_i64 };
static const agg_vtable_t VAR_I64 = { sizeof(var_i64_state), ACC_STREAMING, RAY_F64,
    var_i64_init, var_i64_update, var_i64_merge, fin_var_i64 };
static const agg_vtable_t STDDEV_POP_I64 = { sizeof(var_i64_state), ACC_STREAMING, RAY_F64,
    var_i64_init, var_i64_update, var_i64_merge, fin_stddev_pop_i64 };
static const agg_vtable_t STDDEV_I64 = { sizeof(var_i64_state), ACC_STREAMING, RAY_F64,
    var_i64_init, var_i64_update, var_i64_merge, fin_stddev_i64 };
```
Then the F64 analogues (`var_f64_state { double sum; double sumsq; int64_t cnt; }`, `sumsq += v*v` as double, finalize uses `sumsq/cnt - mean*mean`). 4 more vtables: `VAR_POP_F64`/`VAR_F64`/`STDDEV_POP_F64`/`STDDEV_F64`.

Register in `agg_resolve` (before `return NULL`):
```c
    if (in_type == RAY_I64) {
        if (agg_kind==OP_VAR)        return &VAR_I64;
        if (agg_kind==OP_VAR_POP)    return &VAR_POP_I64;
        if (agg_kind==OP_STDDEV)     return &STDDEV_I64;
        if (agg_kind==OP_STDDEV_POP) return &STDDEV_POP_I64;
    } else if (in_type == RAY_F64) {
        if (agg_kind==OP_VAR)        return &VAR_F64;
        if (agg_kind==OP_VAR_POP)    return &VAR_POP_F64;
        if (agg_kind==OP_STDDEV)     return &STDDEV_F64;
        if (agg_kind==OP_STDDEV_POP) return &STDDEV_POP_F64;
    }
```
`<math.h>` is already included (used by F64 min/max). The vtable initializer order must match `agg_vtable_t` field order (`state_size, kind, out_type, init, update_batch, merge, finalize`) — use designated initializers if the existing accumulators do, else positional as shown.

- [ ] **Step 3: Run unit test → PASS; commit**

`make test 2>&1 | tail -8 && ./rayforce.test -f agg_registry`. Then:
```bash
git add src/ops/agg_stream.c test/test_agg_registry.c
git commit -m "feat(agg): v2 var/var_pop/stddev/stddev_pop accumulators (I64+F64)"
```

---

## Task 2: var/stddev differential vs the old engine (serial + parallel)

**Files:** Modify `test/test_agg_engine.c`.

- [ ] **Step 1: Add differential shapes**

Using the existing `diff_group` runner + `table_expect_equal(.., n_keys)`. The old engine computes var/stddev via its own accumulator; v2 now via the registry. Each over a small table AND N=70000 (parallel path):
1. single I64 key, `stddev` over an I64 value
2. single I64 key, `var` + `var_pop` + `stddev` + `stddev_pop` over an I64 value (all four, order varied)
3. single I64 key, F64 value, `stddev`
4. two I64 keys, `stddev_pop` over an I64 value
5. one shape mixing `sum` + `stddev` + `count` (heterogeneous agg set) over an I64 value

> The F64 tolerance: `table_expect_equal`/`cell_equal` already compares F64 with a tolerance — confirm it (read the comparator). If var/stddev values are large, a relative tolerance may be needed; if the comparator uses a tight absolute epsilon and a shape's variance is large, scale the test data (use small values like 0..100) so absolute differences stay within tolerance. Matching the old engine's exact formula keeps the difference at rounding level.

Build the value column with deterministic small-range data (e.g. `v = (i % 97)`), so cnt≥2 per group (avoid the all-null/singleton null paths here — those are covered by formula, and a separate shape can test a singleton group → null if easy).

- [ ] **Step 2: Run, iterate to green**

`make test 2>&1 | tail -8 && ./rayforce.test -f diff_group`. All shapes match (multiset). If a shape mismatches: check the insufficient-cnt null convention (cnt≤1 sample vs cnt≤0 pop), the sample-vs-pop factor `cnt/(cnt-1)`, and the I64-sumsq-as-int64 detail. If mismatch is pure float rounding beyond tolerance, confirm v2 uses the identical formula and the same sumsq accumulation type as the old engine; only then loosen the test data range.

- [ ] **Step 3: Full suite + sanitizers + commit**

`make test 2>&1 | tail -8` — 0 failures; new shapes green; flag defaults off; ASAN/UBSan clean.
```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 var/stddev differential vs old engine (serial + parallel)"
```

---

## Phase 2a Done — exit criteria

- `agg_resolve` returns accumulators for `var`/`var_pop`/`stddev`/`stddev_pop` over I64 and F64; the gate auto-admits them; v2 computes them in both serial and parallel paths, byte-equivalent (within F64 tolerance) to the old engine.
- Flag defaults off → zero production change. Full suite green, ASAN/UBSan clean.

**Next:** Phase 2b — `pearson` (OP_PEARSON_CORR): binary aggregate needing TWO value columns (`agg_ins[a]` and `agg_ins2[a]`). This requires extending the accumulator interface for a second input (or a dedicated binary-accumulator path) — the first new mechanism since Phase 0. Then 2c `median` (ACC_BUFFERED + per-group arena), 2d `top_n`/`bot_n` (arena + K param `agg_k[a]` + LIST-valued output column). After the registry covers all rowform aggregates, **Phase 3 deletes the six rowform operators** and routes their queries through v2's single order-agnostic path — which is where the order-dependence cliff and the planner/executor gate duplication finally die.
```

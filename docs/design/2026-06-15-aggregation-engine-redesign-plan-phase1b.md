# Aggregation Engine Redesign — Phase 1b Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit the design/plan docs under `docs/design/` (standing user preference).

**Goal:** Extend the v2 aggregation engine (behind `ray_agg_engine_v2`) from single-key to **multi-key** (composite) group-by, still single-threaded, and prove it byte-equivalent (as a multiset) to the old engine for multi-key shapes.

**Architecture:** Replace single-key grouping with a **tuple-hash** grouping over N key columns (N ≤ 16) that records, per group, the **first row** at which it appeared (`first_row[gid]`). Build each result key column by **gathering** the source key column at `first_row[]` (type-exact, preserves SYM ids + domain). Aggregate driving and result assembly from Phase 1a are reused unchanged (they are already key-count-agnostic — they operate on `gids`/`ngroups`). Widen the gate to admit ≤16 supported-type `OP_SCAN` keys.

**Tech Stack:** C11, in-repo test harness. Builds on Phase 1a (branch `feat/agg-engine-phase0`, commits through `fc53fada`).

**Reference (design):** `docs/design/2026-06-14-aggregation-engine-redesign-design.md` §5 (driver), §7 (assembler + first-occurrence order; see the 2026-06-15 correction — order parity with the old engine is checked as a **multiset**, not row order).

---

## Scope

**In scope (1b):** multi-key grouping (1 ≤ n_keys ≤ 16), each key an `OP_SCAN` of a supported type (I64/I32/I16/U8/BOOL/DATE/TIME/TIMESTAMP/SYM); the Phase-0 aggregate set unchanged; single-threaded; differential vs the old engine (multiset compare over all key columns).

**NOT in scope:** parallelism (separate later phase — v2 stays serial; the N=70000 differential still runs the *old* engine parallel and must match), `ACC_BUFFERED` aggregates, the cost-based strategy selector / canonicalization (Phase 2), deleting the rowform operators (Phase 3).

**Note on parallelism deferral:** v2-serial is correctness-complete — the Phase-1a differential already compares old-parallel (N>65536) vs v2-serial and matches. Parallelism is a perf concern with no gate until the flag flips, so it is deferred.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/ops/agg_engine.h` | Modify | `agg_groups_t` gains `int64_t* first_row` (replaces single `keys`); new `agg_group_keys(ray_t** key_cols, uint8_t n_keys, int64_t nrows, agg_groups_t*)`. |
| `src/ops/agg_engine.c` | Modify | Multi-key tuple-hash grouping; gather-based `agg_build_key_col`; multi-key emit in `exec_group_v2`; widen the gate. |
| `test/test_agg_engine.c` | Modify | Update the 1a grouping/driver tests to the new `first_row` contract; extend `table_expect_equal` to sort by ALL keys; add multi-key differential shapes. |

**Confirmed facts (verified):**
- Max keys 16 (consistent cap: `TOPK_MAX_KEYS=16`, ext arrays `[16]`).
- Reuse helpers (already used by 1a's `agg_build_key_col`): `col_vec_new(src, n)`, `ray_sym_vec_adopt_domain(out, sym_domain_rep(src))`, element size `col_elem_size(col)` (src/ops/internal.h:206), `agg_read_key_i64(col, data, row)` (added in 1a, reads any supported key type widened to int64; SYM via `ray_read_sym`).
- Old multi-key engine emits each key column named by its own scan sym (loop, group.c:8897-region) and agg columns via `agg_result_col_name` (now shared). Row order differs (sorted vs first-occurrence) → multiset compare.
- `exec_group_v2` (1a) and `agg_run_one` (1a) already use `gids`/`ngroups` only — no change needed for the aggregate side.

---

## Task 1: Multi-key tuple-hash grouping (with `first_row`)

**Files:** Modify `src/ops/agg_engine.h`, `src/ops/agg_engine.c`, `test/test_agg_engine.c`.

- [ ] **Step 1: Change the grouping contract in `agg_engine.h`**

```c
typedef struct {
    uint32_t* gids;       /* len = nrows; gid per row */
    int64_t*  first_row;  /* len = ngroups; row index where each group first appeared */
    int64_t   ngroups;
} agg_groups_t;

/* Multi-key grouping over n_keys (1..16) columns, first-occurrence gid order.
 * Returns 0 on success (caller free()s gids + first_row), -1 on OOM. */
int agg_group_keys(ray_t** key_cols, uint8_t n_keys, int64_t nrows, agg_groups_t* out);
```
Remove the old `int64_t* keys` field and the single-key `agg_group_keys_i` declaration (it is replaced; update its callers in Step 3 and the tests in Step 4).

- [ ] **Step 2: Implement `agg_group_keys` in `agg_engine.c`**

Tuple-hash open addressing. Hash combines all keys; collision check verifies every key equals the group's first-occurrence row.
```c
int agg_group_keys(ray_t** key_cols, uint8_t n_keys, int64_t nrows, agg_groups_t* out) {
    const void* data[16];
    for (uint8_t k = 0; k < n_keys; k++) data[k] = ray_data(key_cols[k]);

    int64_t cap = 16; while (cap < nrows * 2) cap <<= 1;
    uint64_t mask = (uint64_t)cap - 1;
    int32_t* ht_gid = malloc((size_t)cap * sizeof(int32_t));
    out->gids      = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(uint32_t));
    out->first_row = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(int64_t)); /* <= nrows groups */
    if (!ht_gid || !out->gids || !out->first_row) {
        free(ht_gid); free(out->gids); free(out->first_row);
        out->gids = NULL; out->first_row = NULL; return -1;
    }
    for (int64_t i = 0; i < cap; i++) ht_gid[i] = -1;

    int64_t ngroups = 0;
    for (int64_t r = 0; r < nrows; r++) {
        uint64_t h = 1469598103934665603ULL;  /* FNV-ish seed */
        for (uint8_t k = 0; k < n_keys; k++) {
            int64_t v = agg_read_key_i64(key_cols[k], data[k], r);
            h ^= (uint64_t)v; h *= 1099511628211ULL;
        }
        uint64_t slot = h & mask;
        for (;;) {
            int32_t gptr = ht_gid[slot];
            if (gptr < 0) {                               /* new group */
                ht_gid[slot] = (int32_t)ngroups;
                out->first_row[ngroups] = r;
                out->gids[r] = (uint32_t)ngroups;
                ngroups++;
                break;
            }
            /* candidate: verify ALL keys match the group's first row */
            int64_t fr = out->first_row[gptr];
            int eq = 1;
            for (uint8_t k = 0; k < n_keys; k++) {
                if (agg_read_key_i64(key_cols[k], data[k], r) !=
                    agg_read_key_i64(key_cols[k], data[k], fr)) { eq = 0; break; }
            }
            if (eq) { out->gids[r] = (uint32_t)gptr; break; }
            slot = (slot + 1) & mask;                     /* linear probe */
        }
    }
    out->ngroups = ngroups;
    free(ht_gid);
    return 0;
}
```
(int32 gid is fine — group counts here are well under 2^31. `agg_read_key_i64` already exists from 1a.)

- [ ] **Step 3: Build the library, fix 1a callers**

`exec_group_v2` and 1a's grouping/driver unit tests call the old `agg_group_keys_i` / read `groups.keys`. Step 4 (exec_group_v2) and the test updates fix them. For now ensure the header compiles: `make 2>&1 | tail -5` will fail to link until callers are updated — that is expected; proceed to update them in Tasks 2-3 and the tests here.

- [ ] **Step 4: Update the 1a grouping unit test + add a multi-key one**

In `test/test_agg_engine.c`:
- Update `group_keys_i_first_occurrence` (and the I32 case) to call `agg_group_keys(&col, 1, nrows, &out)` and assert via `first_row`: for `{5,3,5,3,5,7}`, `out.ngroups==3`, `out.first_row=={0,1,5}`, `out.gids=={0,1,0,1,0,2}`. Free `out.gids`/`out.first_row`.
- Add `group_keys_multi`: two I64 key columns `kA={1,1,2,1}`, `kB={9,9,8,8}` → tuples `(1,9),(1,9),(2,8),(1,8)` → `ngroups==3`, `gids=={0,0,1,2}`, `first_row=={0,2,3}`.
- The `agg_run_one_i64` test (1a) groups `{1,1,2}` via the old helper — update it to `agg_group_keys(&keycol,1,3,&out)` and use `out.gids`/`out.ngroups` (the aggregate assertions are unchanged: SUM{30,30}, etc.).

- [ ] **Step 5: Run + commit** (after Tasks 2-3 make it build; if you implement in this order, defer the commit until the suite builds). Once green:

```bash
git add src/ops/agg_engine.h src/ops/agg_engine.c test/test_agg_engine.c
git commit -m "feat(agg): v2 multi-key tuple-hash grouping (first_row tracking)"
```

> Implementation order tip: it is cleaner to do Tasks 1+2+3 as one editing pass (grouping + gather + gate/emit) THEN build, since they are mutually dependent, committing once the suite is green. Keep the commits split by concern if convenient, but do not commit a non-building tree.

---

## Task 2: Gather-based key column build (all types, exact)

**Files:** Modify `src/ops/agg_engine.c`.

Replace the int-coded `agg_build_key_col(src, keys[], n)` with a **gather**: copy each group's first-occurrence element from the source key column. Type-exact (memcpy by element size), preserves SYM ids; adopt the SYM domain.

- [ ] **Step 1: Implement the gather**

```c
/* Build a result key column by gathering src_col at the per-group first rows.
 * Exact copy (element-size memcpy) preserves all widths incl. SYM ids. */
static ray_t* agg_gather_key_col(ray_t* src_col, const int64_t* first_row, int64_t n) {
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    size_t esz = col_elem_size(src_col);            /* internal.h:206 */
    const char* src = (const char*)ray_data(src_col);
    char* dst = (char*)ray_data(out);
    for (int64_t gi = 0; gi < n; gi++)
        memcpy(dst + (size_t)gi * esz, src + (size_t)first_row[gi] * esz, esz);
    /* if the source column carries nulls, a gathered sentinel is already the
     * correct null byte-pattern; propagate the attr so printing/aggregation
     * downstream treats it as null. */
    if (src_col->attrs & RAY_ATTR_HAS_NULLS) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}
```
> CONFIRM `col_elem_size` is the right element-size helper for ALL supported key types (it delegates to `ray_sym_elem_size(col->type, col->attrs)` — verify it returns the correct fixed byte width for I64/I32/I16/U8/BOOL/DATE/TIME/TIMESTAMP and the SYM adaptive width). Confirm `col_vec_new(src, n)` creates a same-type (and same SYM-width) empty vector. Remove the now-unused `write_col_i64`-based body and the `keys[]` parameter.

---

## Task 3: Widen the gate + multi-key emit in `exec_group_v2`

**Files:** Modify `src/ops/agg_engine.c`.

- [ ] **Step 1: Widen the gate**

In `agg_v2_can_handle`, change the single-key check to admit 1..16 keys, each a supported-type `OP_SCAN`:
```c
    if (ext->n_keys < 1 || ext->n_keys > 16) return false;   /* was: != 1 */
    ...
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_t* key = ext->keys[k];
        if (!key || key->opcode != OP_SCAN) return false;
        ray_op_ext_t* kext = find_ext(g, key->id);
        ray_t* kc = kext ? ray_table_get_col(tbl, kext->sym) : NULL;
        if (!kc) return false;
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
    }
```
(Keep all the existing per-agg checks unchanged.)

- [ ] **Step 2: Multi-key grouping + emit in `exec_group_v2`**

```c
    /* resolve key columns */
    ray_t* key_cols[16];
    int64_t key_syms[16];
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]->id);
        key_cols[k] = ray_table_get_col(tbl, kext->sym);
        key_syms[k] = kext->sym;
    }

    agg_groups_t groups = {0};
    if (agg_group_keys(key_cols, ext->n_keys, nrows, &groups) != 0)
        return ray_error("oom", NULL);

    ray_t* result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) { free(groups.gids); free(groups.first_row); return ray_error("oom", NULL); }

    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], groups.first_row, groups.ngroups);
        if (!kc || RAY_IS_ERR(kc)) { free(groups.gids); free(groups.first_row); ray_release(result); return kc ? kc : ray_error("oom", NULL); }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }
    /* aggregate columns: UNCHANGED from 1a — uses groups.gids / groups.ngroups */
    for (uint8_t a = 0; a < ext->n_aggs; a++) { /* ... existing loop ... */ }

    free(groups.gids); free(groups.first_row);
    return result;
```
Update the existing agg loop's cleanup `free(groups.keys)` → `free(groups.first_row)` everywhere (all early-return paths too).

- [ ] **Step 3: Build + suite green (flag off) + commit**

`make test 2>&1 | tail -8` — must build clean and pass at the prior count (3478/3480) with the flag still defaulting off; the new grouping unit tests (Task 1 Step 4) pass.
```bash
git add src/ops/agg_engine.c
git commit -m "feat(agg): v2 multi-key gate + gather emit (exec_group_v2)"
```

---

## Task 4: Multi-key differential vs the old engine

**Files:** Modify `test/test_agg_engine.c`.

- [ ] **Step 1: Extend `table_expect_equal` to canonicalize by ALL key columns**

The 1a comparator sorts both tables by key column 0 only. For multi-key, two groups can share key0 — sort by the **composite of all key columns** (the leading `n_keys` columns) so the multiset canonicalization is unambiguous.
> Determine `n_keys` for the comparator: the result table is `{key cols..., agg cols...}`; pass `n_keys` into `table_expect_equal` (the test knows it per shape) and build the sort permutation from a composite comparison across columns `0..n_keys-1` (compare column 0, tie-break by 1, …). Reuse the existing per-cell compare for tie-breaks. Keep applying the SAME row permutation across all columns (sound — preserves key↔agg association, as in 1a).

- [ ] **Step 2: Add multi-key differential shapes**

Each over a small table AND N=70000 (old engine parallel, v2 serial). Build via the existing builders; `ray_group(g, key_ops, n_keys, agg_ops, agg_ins, n_aggs)` with n_keys>1.
1. two I64 keys, SUM
2. two I64 keys, SUM+COUNT+MIN+MAX
3. three keys (I64, I32, SYM), COUNT
4. SYM key + I64 key, SUM over an I64 value
5. two I64 keys, F64 value, AVG
6. two I64 keys with HAS_NULLS in one key column (null-key grouping parity — null==null forms one group; confirm parity, else narrow gate to reject HAS_NULLS keys and assert the gate defers, documenting it)

Pass the correct `n_keys` to `table_expect_equal` per shape. Register each as a `test_entry_t`.

- [ ] **Step 3: Run, iterate to green**

`make test 2>&1 | tail -8 && ./rayforce.test -f diff_group` (or your multi-key prefix). All shapes match (multiset). For any mismatch, debug grouping/gather/gate. Likely culprits: composite hash collision handling, SYM domain on a gathered SYM key, null-key tuple grouping vs the old engine. If null-key multi-key grouping genuinely diverges from the old engine, narrow the gate to reject HAS_NULLS keys for 1b and assert the gate defers (document the deferral).

- [ ] **Step 4: Full suite + sanitizers clean + commit**

`make test 2>&1 | tail -8` — 0 failures; new multi-key tests green; everything else unchanged (flag off). ASAN/UBSan clean.
```bash
git add test/test_agg_engine.c
git commit -m "test(agg): v2 multi-key differential vs old engine (multiset over all keys)"
```

---

## Phase 1b Done — exit criteria

- The gate admits 1..16 supported-type keys; `exec_group_v2` groups via tuple hash, emits all key columns (gathered, type-exact) + agg columns.
- Multiset differential (canonicalized over all key columns) matches the old engine for multi-key shapes, small and >65536 rows, with/without nulls, mixed key types incl. SYM.
- Flag defaults off → zero production change. Full suite green, ASAN/UBSan clean.

**Next (Phase 1c / Phase 2):** Phase 1c — parallelize grouping (per-worker partial hash tables + merge, or radix partitioning via `ray_pool_dispatch` + `scratch_calloc`), keeping the multiset differential. Phase 2 — the cost-based strategy selector + query canonicalization (sort aggregates by `(kind,col)`) + `explain`; **this is where the order-dependence perf cliff dies** and the planner/executor gate duplication collapses to one selector.

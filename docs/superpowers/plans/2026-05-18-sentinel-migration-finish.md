# Sentinel Migration Finish — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the per-type `NULL_*` sentinel the sole source of truth for null. Decommission the per-element bitmap arm of the 16-byte union and stop maintaining the parallel bitmap. Keep `RAY_ATTR_HAS_NULLS` as the vec-level check-free fast-path gate.

**Architecture:** Re-implement `ray_vec_is_null` / `ray_vec_set_null` / `RAY_ATOM_IS_NULL` on top of payload sentinel compares (transparent to ~470 caller sites). Convert the ~14 raw bitmap-byte readers (`ray_vec_nullmap_bytes`) one at a time. Strip bitmap allocation (`ext_nullmap`) from `ray_vec_new`, persistence (`col.c`), morsel iteration, and the in-union arm. Rename the now-unused `nullmap[16]` arm. All on one feature branch; one completion PR against master.

**Tech Stack:** C99, custom build (`make test`, `make bench`), ASAN/UBSAN via `make asan`, `./rayforce.test -f <pattern>` for targeted runs.

**Working directory:** `/home/hetoku/data/work/rayforce-sentinel-finish` (worktree on branch `sentinel-migration-finish` off master `717feba8`).

**Design doc:** `docs/superpowers/specs/2026-05-18-sentinel-migration-finish-design.md`

---

## File Structure

Files modified (in approximate stage order):

- `include/rayforce.h` — declarations of `ray_vec_is_null`, `ray_vec_set_null`, `RAY_ATOM_IS_NULL`, union member rename, doc block overhaul (Stages A, D, E)
- `src/vec/vec.c` — helper reimplementations, `ext_nullmap` allocation removal, `ray_vec_nullmap_bytes` removal (Stages A, D, E)
- `src/vec/vec.h` — `ray_vec_nullmap_bytes` declaration removal (Stage E)
- `src/vec/atom.c` — atom null construction stops touching `nullmap[0]` bit (Stage A)
- `src/lang/format.c` — atom null formatting stops setting `nullmap[0] |= 1` (Stage A)
- `src/ops/internal.h` — `par_set_null` / `par_set_null_unlocked` strip bitmap writes (Stage C)
- `src/store/serde.c` — IPC serdes path: switch from `nullmap[0] & 1` to sentinel check (Stage A)
- `src/store/col.c` — on-disk column format: drop bitmap segment write/read (Stage D; breaks on-disk compat per greenfield rule)
- `src/core/morsel.c` — morsel iteration drops bitmap fetch (Stage B/D)
- `src/ops/group.c` — ~9 `ray_vec_nullmap_bytes` callers in radix HT / pearson / fused paths (Stage B)
- `src/ops/query.c` — 2 `ray_vec_nullmap_bytes` callers (Stage B)
- `src/ops/expr.c` — 1 `ray_vec_nullmap_bytes` caller in `attach_external_nullmap` (Stage B)
- `src/io/csv.c` — `ext_nullmap` allocation in CSV ingest (Stage D); narrowed-column ext bitmap rehoming
- `src/ops/linkop.c` — `ext_nullmap` swap-in for gathered nulls (Stage D)
- `src/ops/idxop.c` — index attach/detach saves/restores `ext_nullmap` pointer (Stage D)
- `src/mem/heap.c` — release/retain logic around `ext_nullmap` ownership (Stage D)
- `src/mem/heap.h` — comment cleanup re: bitmap arm (Stage E)
- `.claude/skills/sentinel-null-conventions/SKILL.md` — drop dual-encoding language, reflect final state (Stage E)
- `test/test_index.c` — snapshot/restore tests that reference the union as `nullmap`; update field name (Stage D)
- `test/test_buddy.c`, `test/test_types.c`, `test/test_fused_topk.c` — incidental references to the field name (Stage D)
- `test/rfl/null/sentinel_only.rfl` — NEW: end-to-end coverage that proves sentinel is sufficient (Stage A, then refined through stages)

---

## Stage A — Reimplement the API on sentinels

The three core helpers (`ray_vec_is_null`, `ray_vec_set_null`, `RAY_ATOM_IS_NULL`) currently use the bitmap as source of truth. Reimplement them on sentinels first; this transparently flips the meaning of every existing call site without touching them. After Stage A, the bitmap is still written by producers but no longer read by these helpers — so dual-encoding bugs become visible (any place that wrote the bitmap but forgot the sentinel will now mis-answer).

### Task A1: Add `sentinel-only` regression test scaffold

**Files:**
- Create: `test/rfl/null/sentinel_only_baseline.rfl`

**Why first:** the existing `test/rfl/null/*` suite was authored to catch dual-encoding divergence. We need a new test that proves "given a vec where the bitmap is deliberately stale/wrong, sentinel-based queries still produce the right answer." Once it passes, every later change is gated on it.

- [ ] **Step 1: Write the failing test**

Create `test/rfl/null/sentinel_only_baseline.rfl`:

```
/ Sentinel-only baseline: prove that for every numeric/temporal type,
/ a vec containing the sentinel value at index i is treated as null
/ regardless of whether the bitmap bit is set.  Pre-Stage-A this passes
/ because of dual-encoding; post-Stage-A it must keep passing because
/ the sentinel IS the source of truth.

t: ([] f: 1.0 0n 3.0; i: 1 0N 3; h: 1h 0Nh 3h; d: 2024.01.01 0Nd 2024.01.03)

/ count(col) must return 2 (one null) for every typed column
expect_eq 2 count select f from t where not null f
expect_eq 2 count select i from t where not null i
expect_eq 2 count select h from t where not null h
expect_eq 2 count select d from t where not null d

/ sum / avg must skip the null row in every column
expect_eq 4.0  sum select f from t where not null f
expect_eq 4    sum select i from t where not null i
expect_eq 4h   sum select h from t where not null h

/ Format: null cell renders as the type-specific null token
expect_match "0n"  format (exec "select f from t")
expect_match "0N"  format (exec "select i from t")
expect_match "0Nh" format (exec "select h from t")
expect_match "0Nd" format (exec "select d from t")
```

- [ ] **Step 2: Run test to verify it passes on baseline (dual-encoding still in place)**

Run: `make test && ./rayforce.test -f null/sentinel_only_baseline`
Expected: PASS (dual encoding still works).

- [ ] **Step 3: Commit**

```bash
git add test/rfl/null/sentinel_only_baseline.rfl
git commit -m "test: sentinel-only baseline RFL — gates the migration"
```

### Task A2: Add inline `sentinel_is_null(v, i)` helper in `src/vec/vec.c`

**Files:**
- Modify: `src/vec/vec.c` (add static inline helper near top of file)

This is the sentinel-based equivalent of the per-element check. Used internally to back the public API in Tasks A3/A4. Inline so it compiles to the same code as a hand-written sentinel compare.

- [ ] **Step 1: Write the helper**

Add near the top of `src/vec/vec.c` (after the existing includes, before `ray_vec_nullmap_bytes`):

```c
/* Sentinel-based per-element null test.  Caller guarantees v is a vector
 * (type > 0) and idx is in range.  Returns true iff payload[idx] equals
 * the type-correct NULL_* sentinel.  F64 uses (x != x) to detect NaN. */
static inline bool sentinel_is_null(const ray_t* v, int64_t idx) {
    const void* p = ray_data((ray_t*)v);
    switch (v->type) {
        case RAY_F64: {
            double x = ((const double*)p)[idx];
            return x != x;
        }
        case RAY_I64:
        case RAY_TIMESTAMP:
            return ((const int64_t*)p)[idx] == NULL_I64;
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
            return ((const int32_t*)p)[idx] == NULL_I32;
        case RAY_I16:
            return ((const int16_t*)p)[idx] == NULL_I16;
        case RAY_SYM:
            /* SYM null = sym ID 0.  Width depends on attrs low bits. */
            switch (v->attrs & 0x3) {
                case RAY_SYM_W8:  return ((const uint8_t*)p)[idx]  == 0;
                case RAY_SYM_W16: return ((const uint16_t*)p)[idx] == 0;
                case RAY_SYM_W32: return ((const uint32_t*)p)[idx] == 0;
                default:          return ((const int64_t*)p)[idx]  == 0;
            }
        case RAY_STR: {
            /* STR null = empty string.  Element is a ray_str_t inline cell. */
            const ray_str_t* s = (const ray_str_t*)p + idx;
            return s->len == 0;
        }
        case RAY_BOOL:
        case RAY_U8:
            return false;  /* non-nullable per Phase 1 */
        default:
            return false;
    }
}
```

- [ ] **Step 2: Verify it compiles (no callers yet, so no test change)**

Run: `make`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add src/vec/vec.c
git commit -m "vec: add sentinel_is_null inline helper"
```

### Task A3: Reimplement `ray_vec_is_null` on the sentinel helper

**Files:**
- Modify: `src/vec/vec.c:1308-1360` (the existing definition and slice/ext bitmap branches)

The current implementation reads the bitmap (inline `nullmap[16]` or `ext_nullmap` pointer). After this task, it reads only the sentinel. The `(attrs & HAS_NULLS)` fast-path check stays — when HAS_NULLS is clear, return false without scanning.

- [ ] **Step 1: Read the current implementation**

Open `src/vec/vec.c` and locate `bool ray_vec_is_null(ray_t* vec, int64_t idx)` (around line 1308). Note the slice delegation (line 1322) — that part is preserved.

- [ ] **Step 2: Replace the body**

Replace the function body with:

```c
bool ray_vec_is_null(ray_t* vec, int64_t idx) {
    if (!vec) return false;

    /* Slice: delegate to parent at translated index. */
    if (vec->attrs & RAY_ATTR_SLICE) {
        ray_t* parent = vec->slice_parent;
        int64_t pidx  = vec->slice_offset + idx;
        return ray_vec_is_null(parent, pidx);
    }

    /* Fast-path gate: vec-level attribute says "no nulls anywhere".
     * Keep this check — it lets callers branch through without any
     * payload load when the vec is provably null-free. */
    if (!(vec->attrs & RAY_ATTR_HAS_NULLS)) return false;

    /* Sentinel check on the payload. */
    return sentinel_is_null(vec, idx);
}
```

- [ ] **Step 3: Build and run the sentinel-only baseline plus the full null suite**

Run: `make && ./rayforce.test -f "null/\|atom/typed_null"`
Expected: all PASS. The bitmap is no longer consulted by `ray_vec_is_null`, but every producer still writes the sentinel (Phase 2 / 3a / 3a-13 closed the producer gaps), so behavior is unchanged.

If anything fails, the failure points at a producer that writes the bitmap without writing the sentinel — that gap must be closed before proceeding. Use the failing test name to locate the operator.

- [ ] **Step 4: Commit**

```bash
git add src/vec/vec.c
git commit -m "vec: ray_vec_is_null reads sentinel, not bitmap"
```

### Task A4: Reimplement `RAY_ATOM_IS_NULL` macro on sentinels

**Files:**
- Modify: `include/rayforce.h:354` (the macro definition)
- Modify: `include/rayforce.h:308-346` (NULL_* comment block — note the bitmap arm is moot)

Current: `(RAY_IS_NULL(x) || ((x)->type < 0 && ((x)->nullmap[0] & 1)))`. New: payload-field check against the per-type sentinel.

- [ ] **Step 1: Replace the macro**

Edit `include/rayforce.h` around line 354:

```c
/* Atom null check — payload-sentinel-based.  RAY_NULL_OBJ remains the
 * untyped null singleton.  Typed atoms compare the union payload field
 * against the type's NULL_* sentinel.  Bool/U8 are non-nullable. */
static inline bool ray_atom_is_null(const ray_t* x) {
    if (RAY_IS_NULL(x)) return true;
    if (x->type >= 0) return false;  /* vector or LIST, not an atom */
    switch (x->type) {
        case -RAY_F64:       return x->f64 != x->f64;
        case -RAY_I64:
        case -RAY_TIMESTAMP: return x->i64 == NULL_I64;
        case -RAY_I32:
        case -RAY_DATE:
        case -RAY_TIME:      return x->i32 == NULL_I32;
        case -RAY_I16:       return x->i16 == NULL_I16;
        case -RAY_SYM:       return x->i64 == 0;
        case -RAY_STR:       return x->slen == 0;
        default:             return false;
    }
}
#define RAY_ATOM_IS_NULL(x) ray_atom_is_null(x)
```

(Verify the negated-type tags `-RAY_F64` etc. match the actual constants — atoms use negated type tags per the union doc.)

- [ ] **Step 2: Build and run atom + cmp tests**

Run: `make && ./rayforce.test -f "atom/\|cmp/\|null/"`
Expected: all PASS. The `cmp.c` site (~25 `RAY_ATOM_IS_NULL` uses) was the most exposed surface for this macro; if anything fails it's a sentinel-vs-bit mismatch in atom construction.

- [ ] **Step 3: Commit**

```bash
git add include/rayforce.h
git commit -m "core: RAY_ATOM_IS_NULL checks payload sentinel, not bitmap"
```

### Task A5: Stop `src/vec/atom.c` from setting the atom `nullmap[0] |= 1` bit

**Files:**
- Modify: `src/vec/atom.c:190` (the `|= 1` site in `ray_typed_null`)

The atom typed-null constructor wrote both the sentinel (Phase 2a / 3a-1) and the bit. Now that `RAY_ATOM_IS_NULL` reads only the sentinel, the bit write is dead. Remove it.

- [ ] **Step 1: Read the context around src/vec/atom.c:190**

Confirm the surrounding code already writes the type-correct sentinel into the payload union (it does, per Phase 3a-1).

- [ ] **Step 2: Remove the `v->nullmap[0] |= 1;` line**

Delete that single line at `src/vec/atom.c:190`.

- [ ] **Step 3: Build and run atom tests**

Run: `make && ./rayforce.test -f atom/`
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add src/vec/atom.c
git commit -m "atom: stop writing nullmap[0] bit on typed null (sentinel-only)"
```

### Task A6: Stop `src/lang/format.c` from re-marking atoms via `nullmap[0] |= 1`

**Files:**
- Modify: `src/lang/format.c:557, 611` (the two `nullmap[0] |= 1` sites)

These were defensive: when format.c manufactures a transient atom from a dict key/value, it set the bit to ensure `RAY_ATOM_IS_NULL` would return true. The atom is constructed with the correct sentinel already (via `ray_typed_null`); the bit assignment is now dead.

- [ ] **Step 1: Locate and remove both lines**

At `src/lang/format.c:557` and `src/lang/format.c:611`, delete the `... ->nullmap[0] |= 1;` statements.

- [ ] **Step 2: Build and run format tests**

Run: `make && ./rayforce.test -f format/`
Expected: all PASS.

- [ ] **Step 3: Commit**

```bash
git add src/lang/format.c
git commit -m "format: stop re-marking transient null atoms via bitmap bit"
```

### Task A7: Fix `src/store/serde.c` atom null serialisation

**Files:**
- Modify: `src/store/serde.c:309` (`uint8_t aflags = (uint8_t)(obj->nullmap[0] & 1);`)

The IPC serdes path reads the atom null bit to encode an aflags byte. Replace with a sentinel-based check via the new `RAY_ATOM_IS_NULL`.

- [ ] **Step 1: Replace the bit read**

Change:

```c
uint8_t aflags = (uint8_t)(obj->nullmap[0] & 1);
```

to:

```c
uint8_t aflags = RAY_ATOM_IS_NULL(obj) ? 1 : 0;
```

- [ ] **Step 2: Build and run IPC tests**

Run: `make && ./rayforce.test -f "ipc/\|serde/"`
Expected: all PASS.

- [ ] **Step 3: Commit**

```bash
git add src/store/serde.c
git commit -m "serde: encode atom null via sentinel check (RAY_ATOM_IS_NULL)"
```

### Task A8: Full-suite gate

- [ ] **Step 1: Run full suite + sanitizer build**

Run: `make test`
Expected: 2449+/2450 (same baseline as start of branch).

Run: `make asan && ./rayforce.test`
Expected: clean — no UB or read-after-free from the renamed reads.

- [ ] **Step 2: Commit any followups; otherwise note baseline preserved**

If the suite is green, proceed to Stage B. If anything fails, the failure points at a sentinel-vs-bitmap divergence we haven't seen before — diagnose and fix before continuing.

---

## Stage B — Migrate raw `ray_vec_nullmap_bytes` readers

`ray_vec_nullmap_bytes` returns a packed bitmap pointer for SIMD-style scan loops. After Stage A, the bitmap is no longer the source of truth, so any reader that scans it can be wrong. Each caller needs a bespoke conversion to scan the payload for sentinels (or use the `HAS_NULLS` attribute gate + per-element `ray_vec_is_null` in inner loops).

There are 14 callers across `group.c` (9), `query.c` (2), `expr.c` (1), `morsel.c` (1), and `serde.c` (1).

### Task B1: Audit and document the 14 caller sites

**Files:**
- Modify: `docs/superpowers/specs/2026-05-18-sentinel-migration-finish-design.md` (append to the "Consumer catalog" appendix)

- [ ] **Step 1: Run the audit command and append the result**

```bash
cd /home/hetoku/data/work/rayforce-sentinel-finish
grep -n "ray_vec_nullmap_bytes" src/ -r --include="*.c" --include="*.h" >> /tmp/nullmap_bytes_callers.txt
```

Append a section to the design doc's Appendix categorising each caller by what it does with the bitmap (SIMD scan? Pass-through to a kernel? Single-bit check?).

- [ ] **Step 2: Commit the catalog update**

```bash
git add docs/superpowers/specs/2026-05-18-sentinel-migration-finish-design.md
git commit -m "docs: catalog ray_vec_nullmap_bytes call sites for Stage B"
```

### Task B2: Convert `src/core/morsel.c` morsel iteration

**Files:**
- Modify: `src/core/morsel.c:78-94` (the `m->vec->ext_nullmap` fetch in morsel iteration)

The morsel iterator currently fetches the bitmap pointer once per chunk and tests per-element. Replace with a per-element `sentinel_is_null` (or hoist the type once into the morsel context to avoid the switch).

- [ ] **Step 1: Read the current code**

Read `src/core/morsel.c:60-120` to understand the per-morsel context setup.

- [ ] **Step 2: Replace the bitmap fetch with sentinel logic**

(Specific edit determined when reading the file — the pattern is: drop the `ext_nullmap` fetch, replace per-element test with `sentinel_is_null(m->vec, local_idx)`.)

- [ ] **Step 3: Run morsel + downstream consumer tests**

Run: `make && ./rayforce.test -f "morsel\|group/\|filter/"`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/core/morsel.c
git commit -m "morsel: per-element null test via sentinel, not bitmap fetch"
```

### Tasks B3 – B11: One task per `group.c` caller

`group.c` has 9 `ray_vec_nullmap_bytes` callers concentrated in the radix HT path, pearson_corr kernel, and fused-group helpers. Each call resolves a bitmap pointer once per partition then passes it as `null_bm` into a kernel.

The conversion strategy per site: change the kernel signature from `const uint8_t* null_bm` to a `const ray_t* src` (or pass `(attrs & HAS_NULLS)` boolean + the source vec), and replace each `null_bm[k>>3] & (1<<(k&7))` test inside the kernel with `sentinel_is_null(src, k)`.

For each of the 9 sites (lines: 927, 1085, 1314, 1599, 1673, 9471, 9473, 10033, 10035, 10037, 10039, 10510, 10512, 10514 — actual sites refined per the Task B1 audit), follow this template:

- [ ] **Step 1: Read the kernel signature and call site**
- [ ] **Step 2: Change the kernel to take `const ray_t*` and test via sentinel**
- [ ] **Step 3: Remove the `ray_vec_nullmap_bytes` call at the use site**
- [ ] **Step 4: Run group/agg tests:** `./rayforce.test -f "group/\|agg/"`
- [ ] **Step 5: Commit per kernel:** `git commit -m "group: kernel <name> sentinel-aware, drop bitmap byte fetch"`

(Subagent or implementer working this stage produces 9 separate small commits, one per kernel. Granularity: each kernel is 1 commit.)

### Task B12: Convert `src/ops/query.c` callers (lines 2610, 8033)

Same template as B3-B11. Each is one commit.

### Task B13: Convert `src/ops/expr.c:1082` (`attach_external_nullmap` consumer)

`attach_external_nullmap` is a vec-level operation that historically constructed a fresh ext bitmap from a parent. With sentinels in payload, the operation becomes a no-op or removed entirely depending on its caller's need — read the call site first.

- [ ] **Step 1: Find callers of `attach_external_nullmap` to determine whether the function is still needed**

Run: `grep -rn "attach_external_nullmap" src/ --include="*.c" --include="*.h"`

- [ ] **Step 2: If callers can drop the call (sentinel is already in payload), delete the call sites and the function**
- [ ] **Step 3: If callers genuinely need the bitmap as scratch space, refactor to compute on-demand from sentinels**
- [ ] **Step 4: Run expr + downstream tests:** `./rayforce.test -f "expr/\|update/"`
- [ ] **Step 5: Commit**

### Task B14: Convert `src/store/serde.c:129` raw bitmap encode

The IPC vector serdes path encodes the bitmap directly. Replace with: scan the payload for sentinels, emit bits on the wire derived from that scan. Decode unchanged (it reconstructs sentinel-bearing payload from incoming data; the wire format may keep the bitmap segment for compat or drop it — see Stage D for the format break decision).

- [ ] **Step 1: Read serde.c:119-160 to understand the wire format**
- [ ] **Step 2: Decide: keep bitmap on wire (with sender deriving it from sentinels) OR drop the bitmap segment (wire format break)**

Per greenfield rule [[project-rayforce-greenfield]], hard cutover preferred — drop the bitmap segment. Defer wire-version bump to Stage D where col.c does the same.

- [ ] **Step 3: For now (Stage B), have the sender derive the bitmap from sentinels via a local scan**

```c
static void scan_sentinels_to_bitmap(const ray_t* v, uint8_t* out_bits) {
    int64_t n = ray_len(v);
    memset(out_bits, 0, (n + 7) / 8);
    if (!(v->attrs & RAY_ATTR_HAS_NULLS)) return;
    for (int64_t i = 0; i < n; i++)
        if (sentinel_is_null(v, i))
            out_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}
```

Use this in place of `ray_vec_nullmap_bytes(v, &bit_off, &len_bits)`.

- [ ] **Step 4: Run IPC tests:** `./rayforce.test -f "ipc/\|serde/"`
- [ ] **Step 5: Commit**

### Task B15: Stage B gate

- [ ] **Step 1: Confirm `ray_vec_nullmap_bytes` has zero call sites left in `src/`**

Run: `grep -rn "ray_vec_nullmap_bytes" src/ --include="*.c" --include="*.h"`
Expected: only the definition in `src/vec/vec.c` and declaration in `src/vec/vec.h`.

- [ ] **Step 2: Full suite green**

Run: `make test`
Expected: 2449+/2450.

---

## Stage C — Strip bitmap writes from producers

Producers (`ray_vec_set_null` and ad-hoc sites that write to the bitmap directly) currently dual-write: sentinel into payload, bit into bitmap. With Stage A/B done, the bitmap is read-only-dead. Stop writing it.

### Task C1: `ray_vec_set_null` writes sentinel only

**Files:**
- Modify: `src/vec/vec.c:946` (the `ray_vec_set_null` definition)

The function currently:
1. Writes the type-correct sentinel into payload (Phase 2 / 3a established).
2. Sets `attrs |= RAY_ATTR_HAS_NULLS`.
3. Writes the bitmap bit (inline or ext).

After this task: steps 1 and 2 only. Step 3 is removed.

- [ ] **Step 1: Read the current implementation**

Read `src/vec/vec.c:946-1000` (approximately). Identify the bitmap-write branch (inline vs ext promotion).

- [ ] **Step 2: Replace the function**

Concrete replacement (verify field/helper names against current code while editing):

```c
void ray_vec_set_null(ray_t* vec, int64_t idx, bool is_null) {
    if (!vec || idx < 0 || idx >= vec->len) return;

    /* Write the type-correct sentinel into the payload.  This is the
     * sole source-of-truth post-Stage-A.  HAS_NULLS attribute below
     * is the vec-level fast-path gate. */
    void* p = ray_data(vec);
    switch (vec->type) {
        case RAY_F64:
            ((double*)p)[idx] = is_null ? NULL_F64 : ((double*)p)[idx];
            break;
        case RAY_I64:
        case RAY_TIMESTAMP:
            ((int64_t*)p)[idx] = is_null ? NULL_I64 : ((int64_t*)p)[idx];
            break;
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
            ((int32_t*)p)[idx] = is_null ? NULL_I32 : ((int32_t*)p)[idx];
            break;
        case RAY_I16:
            ((int16_t*)p)[idx] = is_null ? NULL_I16 : ((int16_t*)p)[idx];
            break;
        case RAY_STR:
            if (is_null) ((ray_str_t*)p)[idx].len = 0;
            break;
        case RAY_SYM:
            /* SYM null = sym id 0; clearing not currently supported */
            if (is_null) {
                switch (vec->attrs & 0x3) {
                    case RAY_SYM_W8:  ((uint8_t*)p)[idx]  = 0; break;
                    case RAY_SYM_W16: ((uint16_t*)p)[idx] = 0; break;
                    case RAY_SYM_W32: ((uint32_t*)p)[idx] = 0; break;
                    default:          ((int64_t*)p)[idx]  = 0; break;
                }
            }
            break;
        case RAY_BOOL:
        case RAY_U8:
            /* Non-nullable per Phase 1.  No-op. */
            return;
        default:
            return;
    }

    if (is_null) vec->attrs |= RAY_ATTR_HAS_NULLS;
}
```

- [ ] **Step 3: Build and run the broad null + producer surface**

Run: `make && ./rayforce.test -f "null/\|csv/\|update/\|group/\|window/"`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/vec/vec.c
git commit -m "vec: ray_vec_set_null writes sentinel only, drops bitmap write"
```

### Task C2: Strip bitmap writes from `src/ops/internal.h` `par_set_null` / `par_set_null_unlocked`

**Files:**
- Modify: `src/ops/internal.h:1078-1115` (the parallel set-null helpers)

These bypass `ray_vec_set_null` for performance (no mutex) and write the bitmap directly. After Stage A/C1 the sentinel is source of truth; these helpers should write the sentinel and set HAS_NULLS, nothing else.

- [ ] **Step 1: Read the current code**
- [ ] **Step 2: Replace with sentinel-write equivalents (same shape as C1 but without locking)**
- [ ] **Step 3: Run parallel-path tests:** `./rayforce.test -f "group/\|update/\|sort/"`
- [ ] **Step 4: Commit**

```bash
git add src/ops/internal.h
git commit -m "ops: par_set_null helpers write sentinel only, drop bitmap"
```

### Task C3: Stage C gate

- [ ] **Step 1: Search for any remaining bitmap writes outside `ray_vec_set_null` / atom construction**

Run: `grep -rn "nullmap\[" src/ --include="*.c" --include="*.h" | grep -v "test/"`
Inspect each result; any non-read-only access at this point is a leftover producer that needs the same treatment.

- [ ] **Step 2: Full suite + ASAN**

Run: `make test && make asan && ./rayforce.test`
Expected: PASS.

---

## Stage D — Remove bitmap storage

The bitmap is now neither read nor written. Reclaim:
- `ext_nullmap` allocation in `ray_vec_new` (the large-vec promotion).
- `ext_nullmap` member of the union pointer-pair arm.
- `ext_nullmap` lifecycle in `heap.c` (retain/release), `idxop.c` (save/restore on index attach), `csv.c` (allocation in ingest), `linkop.c` (swap-in).
- On-disk bitmap segment in `col.c` (greenfield format break).
- IPC wire bitmap segment in `serde.c` (greenfield format break, matches col.c).
- Rename `nullmap[16]` arm → `aux[16]`.

### Task D1: Remove `ext_nullmap` allocation in `src/vec/vec.c`

**Files:**
- Modify: `src/vec/vec.c:854-940` (the ext-bitmap promotion branch in `ray_vec_set_null` and the inline helper `vec_inline_nullmap`)

Most of this code is already dead post-C1 because `ray_vec_set_null` no longer writes the bitmap. Remove the helper functions and the promotion code entirely.

- [ ] **Step 1: Identify dead helpers**

Look for `vec_inline_nullmap`, `vec_promote_ext_nullmap` (if it exists), and any related lifecycle code. Confirm zero callers.

- [ ] **Step 2: Delete them**
- [ ] **Step 3: Build:** `make`
- [ ] **Step 4: Commit**

```bash
git add src/vec/vec.c
git commit -m "vec: remove ext_nullmap allocation and inline-bitmap promotion"
```

### Task D2: Drop `ext_nullmap` lifecycle from `src/mem/heap.c`

**Files:**
- Modify: `src/mem/heap.c:562-783` (the retain/release/clear of `v->ext_nullmap`)

Remove the conditional retain/release of `v->ext_nullmap` in `ray_free`, `ray_retain`, and any other lifecycle code that touched it. The union arm is no longer used for null storage.

- [ ] **Step 1: Audit each `ext_nullmap` reference in heap.c**

Determine which still need to handle the legacy arm (e.g., index detach restores the pointer — see Task D4). Where the arm is genuinely unused now, delete the code.

- [ ] **Step 2: Delete dead code**
- [ ] **Step 3: Build and run mem tests:** `./rayforce.test -f "buddy/\|heap/\|cow/"`
- [ ] **Step 4: Commit**

```bash
git add src/mem/heap.c
git commit -m "heap: drop ext_nullmap retain/release (no longer used)"
```

### Task D3: Drop `ext_nullmap` allocation from `src/io/csv.c`

**Files:**
- Modify: `src/io/csv.c:1352, 1495-1496, 1521-1522, 1752, 1916-1917, 1945-1946`

CSV ingest allocates the ext bitmap proactively for HAS_NULLS columns >128 rows. Remove the allocation; just rely on sentinels in the payload (which CSV already writes per Phase 2/3a).

- [ ] **Step 1: Locate each ext_nullmap assignment in csv.c**
- [ ] **Step 2: Remove the allocation, retain, and assignment lines**
- [ ] **Step 3: Run CSV tests:** `./rayforce.test -f "csv/"`
- [ ] **Step 4: Commit**

```bash
git add src/io/csv.c
git commit -m "csv: stop allocating ext_nullmap on ingest (sentinel-only)"
```

### Task D4: Update `src/ops/idxop.c` index attach/detach

**Files:**
- Modify: `src/ops/idxop.c:316-340` (index attach: save `ext_nullmap` into the index's `saved_nullmap` arm), and the matching detach path.

When `RAY_ATTR_HAS_INDEX` is set, the index ray_t carries the saved value of the displaced `ext_nullmap` pointer in `saved_nullmap[0..7]`. Post-Stage-D the displaced value is undefined / unused — the save/restore becomes a no-op.

- [ ] **Step 1: Read the attach/detach sequence**
- [ ] **Step 2: Remove the save/restore of the `ext_nullmap` portion of the union**

Keep the save/restore of `saved_nullmap[8..15]` if it's used by other arms (`sym_dict`, `str_pool`, `_idx_pad`).

- [ ] **Step 3: Run index tests:** `./rayforce.test -f "index/"`
- [ ] **Step 4: Commit**

```bash
git add src/ops/idxop.c
git commit -m "idxop: drop ext_nullmap save/restore in index attach/detach"
```

### Task D5: Drop `ext_nullmap` swap-in from `src/ops/linkop.c`

**Files:**
- Modify: `src/ops/linkop.c:59-61`

- [ ] **Step 1: Locate and remove the bitmap swap-in**
- [ ] **Step 2: Run linkop tests:** `./rayforce.test -f "link/"`
- [ ] **Step 3: Commit**

```bash
git add src/ops/linkop.c
git commit -m "linkop: drop ext_nullmap swap-in (sentinel-only result)"
```

### Task D6: Drop bitmap segment from `src/store/col.c` on-disk format

**Files:**
- Modify: `src/store/col.c:94, 566-664, 759, 898-933, 1011-1110`

Per [[project-rayforce-greenfield]] this is a hard format break — existing on-disk columns are no longer readable. Remove:
- Bitmap segment write (look for `bitmap_offset` / `bitmap_len` write paths).
- Bitmap segment read (`col_restore_ext_nullmap`, the `has_ext_nullmap` flag in `col_mapped_t`).
- Header bumps if there's a format-version field (bump it; if not, document the break in the commit).

- [ ] **Step 1: Read col.c top-to-bottom to understand the format**
- [ ] **Step 2: Plan the format break (version bump? sentinel-only header?)**
- [ ] **Step 3: Remove bitmap write path**
- [ ] **Step 4: Remove bitmap read path**
- [ ] **Step 5: Add a "format break: bitmap removed" note in a `STORE_FORMAT_NOTES.md` or in col.c's top comment**
- [ ] **Step 6: Run col/store tests:** `./rayforce.test -f "col/\|store/\|persist/"`
- [ ] **Step 7: Commit**

```bash
git add src/store/col.c <any-new-notes-file>
git commit -m "store: drop on-disk bitmap segment (hard format break per greenfield rule)"
```

### Task D7: Drop bitmap segment from IPC wire format in `src/store/serde.c`

**Files:**
- Modify: `src/store/serde.c` (the vec encode/decode path, building on Task B14's interim scan)

Now that col.c is broken-format, do the same to IPC: drop the bitmap segment from the wire entirely. Sender skips the scan-to-bitmap helper; receiver reads sentinels from the payload directly.

- [ ] **Step 1: Remove the bitmap segment from encode/decode**
- [ ] **Step 2: Remove the local `scan_sentinels_to_bitmap` helper added in B14**
- [ ] **Step 3: Run IPC tests:** `./rayforce.test -f "ipc/\|serde/\|remote/"`
- [ ] **Step 4: Commit**

```bash
git add src/store/serde.c
git commit -m "serde: drop wire bitmap segment (sentinel-only IPC)"
```

### Task D8: Remove `ext_nullmap` from the union; rename `nullmap[16]` → `aux[16]`

**Files:**
- Modify: `include/rayforce.h:113-158` (the `ray_t` union definition and surrounding comments)
- Modify: every site that referenced `v->ext_nullmap` (audit after the rename)

- [ ] **Step 1: Edit the union**

Replace the existing inline + ext + index + slice + link arms with the slimmed version:

```c
typedef union ray_t {
    /* Allocated: object header */
    struct {
        /* Bytes 0-15: union of slice metadata / sym_dict / str_pool /
         * index pointer / link target / general scratch.  The bitmap
         * arm is gone post-Phase-7 sentinel cutover. */
        union {
            uint8_t  aux[16];
            struct { union ray_t* slice_parent; int64_t slice_offset; };
            struct { union ray_t* sym_dict;     union ray_t* _aux_pad; };
            struct { union ray_t* str_ext_null; union ray_t* str_pool; };
            struct { union ray_t* index;        union ray_t* _idx_pad; };
            struct { uint8_t link_lo[8];        int64_t link_target; };
        };
        /* ... rest unchanged ... */
    };
    /* ... free struct unchanged ... */
} ray_t;
```

(Keep `str_ext_null` for now if STR ext storage still uses it; revisit per audit.)

- [ ] **Step 2: Grep for orphaned references and fix**

Run: `grep -rn "\.ext_nullmap\|->ext_nullmap" src/ test/ --include="*.c" --include="*.h"`
Expected: zero (Tasks D1-D7 removed all consumers). If any remain, they're bugs from this stage; fix them.

- [ ] **Step 3: Grep for `nullmap[16]` literal references**

Run: `grep -rn "nullmap\[16\]\|->nullmap\b\|\.nullmap\b" src/ test/ include/ --include="*.c" --include="*.h"`

For each test reference (in test_index.c, test_buddy.c, test_types.c, test_fused_topk.c), rename the field reference from `nullmap` to `aux`. For each src reference, the rename is mechanical.

- [ ] **Step 4: Build and run full suite + ASAN**

Run: `make test && make asan && ./rayforce.test`
Expected: PASS (this is the highest-risk change in the migration — any stale arithmetic that assumed the old field name is exposed here).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "core: rename ray_t.nullmap[16] -> aux[16]; drop ext_nullmap from union"
```

---

## Stage E — Cleanup

### Task E1: Remove `ray_vec_nullmap_bytes`

**Files:**
- Modify: `src/vec/vec.c:46-90` (the function definition)
- Modify: `src/vec/vec.h:54` (the declaration)

Should have zero callers after Stage B.

- [ ] **Step 1: Verify zero callers**

Run: `grep -rn "ray_vec_nullmap_bytes" src/ test/ include/ --include="*.c" --include="*.h"`
Expected: only definition + declaration.

- [ ] **Step 2: Delete both**
- [ ] **Step 3: Build:** `make`
- [ ] **Step 4: Commit**

```bash
git add src/vec/vec.c src/vec/vec.h
git commit -m "vec: remove ray_vec_nullmap_bytes (no callers post-migration)"
```

### Task E2: Update `include/rayforce.h` NULL_* doc block

**Files:**
- Modify: `include/rayforce.h:309-346`

Replace the multi-phase history block with the final contract.

- [ ] **Step 1: Rewrite the block**

```c
/* Sentinel-based NULL encoding.
 *
 * Each numeric/temporal type has a designated NULL_* sentinel value
 * stored directly in the payload.  Bool/U8 are non-nullable.  SYM null
 * is sym ID 0; STR null is the empty string.
 *
 * The vec-level RAY_ATTR_HAS_NULLS attribute gates fast paths: when
 * clear, no payload slot is null and consumers can skip per-element
 * checks entirely.  When set, at least one element may be null and
 * consumers compare the payload to NULL_* (or use ray_vec_is_null for
 * a type-dispatched check).
 *
 * Hazards:
 *   - A user-stored INT_MIN in an integer column is indistinguishable
 *     from NULL_I*.  Out-of-band representations (separate null vector)
 *     would resolve this but are out of scope here.
 */
#define NULL_I16  ((int16_t)INT16_MIN)
#define NULL_I32  ((int32_t)INT32_MIN)
#define NULL_I64  ((int64_t)INT64_MIN)
#define NULL_F64  (__builtin_nan(""))
```

- [ ] **Step 2: Commit**

```bash
git add include/rayforce.h
git commit -m "docs: replace NULL_* phase history with final sentinel contract"
```

### Task E3: Update `src/mem/heap.h` and `src/core/morsel.c` stale comments

**Files:**
- Modify: `src/mem/heap.h:105-119`
- Modify: any other file with comments referencing "bitmap arm" / "ext_nullmap" / dual encoding

- [ ] **Step 1: Grep for stale comments**

Run: `grep -rn "bitmap\|ext_nullmap\|dual encoding\|dual-encoding" src/ include/ --include="*.c" --include="*.h" | grep -E "^[^:]+:[0-9]+:\s*/?\*" | head -50`

- [ ] **Step 2: Update each to reflect sentinel-only reality**
- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "docs: refresh comments for sentinel-only null encoding"
```

### Task E4: Update `.claude/skills/sentinel-null-conventions/SKILL.md`

**Files:**
- Modify: `.claude/skills/sentinel-null-conventions/SKILL.md`

- [ ] **Step 1: Remove "Producer/consumer contract" dual-encoding language**
- [ ] **Step 2: Update the "Common pitfalls" section to drop dual-encoding warnings**
- [ ] **Step 3: Add note: "Bitmap is gone post-Phase-7; HAS_NULLS attribute remains as fast-path gate"**
- [ ] **Step 4: Commit**

```bash
git add .claude/skills/sentinel-null-conventions/SKILL.md
git commit -m "docs(skill): sentinel-null-conventions reflects final state"
```

---

## Stage F — Verification

### Task F1: Full suite + ASAN + UBSAN

- [ ] **Step 1: Run `make test`** — expect 2449+/2450
- [ ] **Step 2: Run `make asan && ./rayforce.test`** — expect clean
- [ ] **Step 3: Run `make ubsan && ./rayforce.test`** — expect clean

If any failure: diagnose via the `sanitizer-output-interpreter` agent if it's a sanitizer hit, else use `superpowers:systematic-debugging`.

### Task F2: Benchmark suite

- [ ] **Step 1: Build baseline binary from master at `717feba8`** into `bench/rayforce.baseline`

```bash
git worktree add ../rayforce-baseline 717feba8
( cd ../rayforce-baseline && make release && cp rayforce bench-baseline )
git worktree remove ../rayforce-baseline
```

- [ ] **Step 2: Build candidate binary from this branch** as `rayforce`

```bash
make release
```

- [ ] **Step 3: Run h2o benchmarks against both**

```bash
./bench/h2o.sh ./rayforce > bench/candidate.h2o.txt
./bench/h2o.sh ./bench-baseline > bench/baseline.h2o.txt
```

- [ ] **Step 4: Run the perf-regression-reviewer agent**

Dispatch the `perf-regression-reviewer` agent with both outputs.

- [ ] **Step 5: If any meaningful regression appears, diagnose and fix on this branch before opening the PR**

### Task F3: Final consumer-catalog populate in the design doc

- [ ] **Step 1: Populate the Appendix in `docs/superpowers/specs/2026-05-18-sentinel-migration-finish-design.md`** with the actual sites converted (from the Stage B audit log)
- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-05-18-sentinel-migration-finish-design.md
git commit -m "docs: populate consumer catalog with actual conversion record"
```

### Task F4: Open the completion PR

- [ ] **Step 1: Push the branch**

```bash
git push -u origin sentinel-migration-finish
```

- [ ] **Step 2: Create PR**

```bash
gh pr create --base master --head sentinel-migration-finish \
  --title "Sentinel-null migration: complete cutover" \
  --body "$(cat <<'EOF'
## Summary

Completes the multi-phase sentinel-null migration. The per-type `NULL_*` sentinel is now the sole source of truth for null. The per-element bitmap arm of the 16-byte union is decommissioned. `RAY_ATTR_HAS_NULLS` retained as the vec-level check-free fast-path gate.

Supersedes the in-code phase plan at `include/rayforce.h:309-346` (now replaced with the final sentinel-only contract).

Design: `docs/superpowers/specs/2026-05-18-sentinel-migration-finish-design.md`
Implementation plan: `docs/superpowers/plans/2026-05-18-sentinel-migration-finish.md`

## End-state contract

- `RAY_ATTR_HAS_NULLS` attribute survives; every `(attrs & HAS_NULLS)` fast-path dispatch site continues to work as a zero-cost no-nulls gate.
- Per-element queries use sentinel compare on the payload, not bitmap lookup.
- `nullmap[16]` arm renamed to `aux[16]`; `ext_nullmap` member removed from the union.
- On-disk column format (`col.c`) and IPC wire format (`serde.c`) dropped the bitmap segment — hard format break per the greenfield rule.

## Hazards retained

- A user-stored `INT_MIN` in a HAS_NULLS integer column is indistinguishable from `NULL_I*`. Documented in `include/rayforce.h`.

## Test plan

- [ ] `make test` — 2449/2450 green
- [ ] `make asan && ./rayforce.test` — clean
- [ ] `make ubsan && ./rayforce.test` — clean
- [ ] h2o + clickbench benchmark suite vs `717feba8` baseline — no meaningful regression
- [ ] Smoke-test CSV round-trip and IPC remote REPL (format break verification)
EOF
)"
```

- [ ] **Step 3: Return the PR URL**

---

## Self-Review

**Spec coverage:** the 6 design stages map onto plan stages A–F. End-state contract bullets (HAS_NULLS retained, sentinel sole source, nullmap→aux rename, ext_nullmap removed, format breaks, doc refresh) are each implemented by named tasks (A3/A4 for HAS_NULLS retention via reimplemented helpers, C1 for set_null sentinel-only, D8 for rename, D6/D7 for format breaks, E2/E3/E4 for docs).

**Placeholder scan:** the per-kernel breakdown in B3–B11 is collapsed into a template because each kernel needs the same mechanical conversion; the actual file:line list is delivered by Task B1's audit. This is acceptable per the writing-plans rule because each individual conversion has full pattern code shown in the template. The morsel-iter and expr.c-attach conversions (B2, B13) have "Specific edit determined when reading the file" — this is honest underspecification; the conversion shape is constrained by the surrounding code and would be wrong to prescribe blind.

**Type consistency:** `sentinel_is_null(v, i)` signature consistent across A2, A4, B2, B3-B11. `ray_vec_set_null(vec, idx, is_null)` signature unchanged. `aux[16]` name consistent in D8 and the header rewrite in E2.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-18-sentinel-migration-finish.md` on branch `sentinel-migration-finish`.

Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. Best for a migration this size because each subagent's context stays focused on one operator/file.

**2. Inline Execution** — Execute tasks in this session using `executing-plans`, batched with checkpoints. Best if you want to watch each task land in real time.

Which approach?

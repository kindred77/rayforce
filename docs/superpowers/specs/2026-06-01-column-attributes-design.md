# Column Attributes — `sorted` / `unique` / `grouped` / `parted`

**Date:** 2026-06-01
**Status:** Approved design

## Problem

Rayforce tracks no *semantic* properties of a column's contents. The `attrs`
byte on `ray_t` carries only runtime metadata (nulls present, slice, has-index,
linked column, arena). There is no first-class notion that a column is sorted,
distinct, grouped, or partitioned.

Two consequences:

1. **Order-aware operators cannot trust order.** Sortedness is detected
   dynamically (`detect_sortedness()` in `src/ops/sort.c`) but never persisted,
   so nothing downstream can rely on it.
2. **asof join always pays a full sort.** `exec_window_join`
   (`src/ops/join.c`) re-sorts *both* inputs by `(eq-keys, time)` on every call
   — `O(n log n + m log m)` — even when a table is already ordered or
   partitioned. There is no way to tell the engine "this table is already laid
   out for asof," so the per-join sort is unavoidable. This is the immediate
   pain motivating the work.

An accelerator-index layer already exists (`.idx.zone/.hash/.sort/.bloom`,
backed by a discriminated `ray_index_t` in a child `RAY_INDEX` block) but
operators do not yet consult it ("optimizer routing not yet wired").

## Goal

Add first-class, persisted column attributes that order-aware operators can
trust, and wire the asof executor to exploit them. Attributes are settable,
readable, droppable, and verified at set time. Build on the existing index
layer rather than introducing a parallel mechanism.

Non-goal (this spec): active attribute *maintenance* across transforming
operators (see Propagation §4 — we ship conservative invalidation only).

## Attribute model

Four attributes in two categories:

| Attribute | Category       | Meaning                                                     | Backing                       |
|-----------|----------------|-------------------------------------------------------------|-------------------------------|
| `sorted`  | marker         | column is physically ascending                              | none (flag)                   |
| `unique`  | marker         | all values distinct                                         | none (flag)                   |
| `grouped` | backing index  | value → row-id list                                         | reuses **hash index**         |
| `parted`  | backing index  | value → contiguous `[start,end)` range; data laid out as contiguous, ascending value-blocks | new **part index** kind       |

### Combinability rules

- The two markers (`sorted`, `unique`) are independent and may coexist — e.g.
  a sorted distinct key carries both.
- A column has **at most one backing index** at a time. The backing-index
  attributes are `grouped`, `parted`, and the index flavor of `sorted` (the
  existing sort permutation, `RAY_IDX_SORT`). Setting one replaces any other.
- Markers may coexist with a backing index (e.g. a `parted` column whose values
  are also globally ascending could also carry `sorted`).

### The two forms of `sorted`

`sorted` has two distinct, both-useful forms:

- **sorted-as-marker** — the data is physically ascending. Cheap (a flag).
  Enables in-place binary search and lets asof join skip its sort. This is the
  form the asof fast-path consumes.
- **sorted-as-index** — the data is *not* reordered, but a sort permutation
  exists (today's `RAY_IDX_SORT`). Gives ordered access without moving the
  column. This is the backing-index flavor and is mutually exclusive with
  `grouped`/`parted`.

`(.attr.set 'sorted v)` sets the **marker** form. The index form is reached via
the existing `(.idx.sort v)`.

## Surface syntax

A new `.attr.*` builtin family, parallel to `.idx.*`. Keeping the namespaces
separate preserves the distinction: `.idx.*` builds physical accelerators,
`.attr.*` asserts semantic properties (even though `grouped`/`parted` share
storage with the index layer underneath).

```
(.attr.set 'sorted  v)   ; verify ascending, stamp marker; error on violation
(.attr.set 'unique  v)   ; verify distinct, stamp marker; error on duplicate
(.attr.set 'grouped v)   ; build hash-backed group index
(.attr.set 'parted  v)   ; verify value-block layout, build part index; error otherwise
(.attr.get  v)           ; → list of attribute symbols currently held, or null
(.attr.drop v)           ; → strip all attributes / backing index, return v
```

`.attr.get` reports semantic attributes; `.idx.info` continues to report the
physical accelerator. A `grouped`/`parted` column shows up in both.

## Set semantics — strict verify

Setting an attribute verifies the property and **errors on violation**, so a
stamped attribute can never lie and consumers trust it unconditionally.

- `sorted`: O(n) ascending scan; non-ascending → error.
- `unique`: O(n) distinctness check (via a hash index); duplicate → error.
- `grouped`: build the value→row-id structure (reuse `RAY_IDX_HASH`).
- `parted`: verify the column is already laid out as contiguous, ascending
  value-blocks; if not, **error** (verify-only — the caller orders the data
  first; `.attr.set` never reorders). On success, build the part index
  (value → `[start,end)`).

A trusted/unsafe assert variant is explicitly out of scope for this spec; it can
be added later for bulk loaders that already guarantee the property.

## Propagation — conservative invalidate

Attributes survive only operators that *trivially* preserve them; any
uncertainty drops them. The result is correctness-safe and small.

- **Preserve:** refcount / COW copy, pure slices that do not reorder, rename,
  no-op reshapes.
- **Drop:** append / mutate, filter, reorder, arithmetic, join output, take with
  reordering, and anything else not on the preserve list. The result carries no
  attributes unless re-asserted with `.attr.set`.

Active maintenance (proving that, e.g., a filtered sorted column stays sorted)
is a deliberate future phase and is **not** in this spec.

## The asof-join payoff

`exec_window_join` gains a pre-check before its mandatory sort. The fast paths
are pure opt-in acceleration; absent the attributes it falls back to today's
sort-merge unchanged, so there is no correctness risk.

- **Un-partitioned** `(asof-join [Time] L R)`: if `L.Time` and `R.Time` both
  carry `sorted` → skip both merge-sorts, go straight to the two-pointer merge.
  `O(n+m)`.
- **Partitioned** `(asof-join [Sym Time] L R)`: if a side is `parted` on `Sym`
  **and** its `Time` is sorted within each part → skip that side's sort. When
  both sides qualify → full `O(n+m)`. (A side's "Time sorted within parts" is
  established by the column being laid out as `parted` value-blocks with `Time`
  ascending inside each block; the executor checks the part index plus a sorted
  marker on `Time`.)
- **Otherwise:** today's sort-merge path, unchanged.

## Representation — open implementation item

The marker bits need a home. The `attrs` byte is bit-tight: across all vector
types only `0x20` is cleanly free (SYM vectors use `0x01–0x03` for symbol
width), giving room for ~1 marker bit, not 2. Candidates to resolve during
implementation planning (after reading the full `ray_t` struct in
`include/rayforce.h`):

- Use `attrs` `0x20` for one marker and the spare `order` byte for the other (or
  pack both markers into `order`).
- Record markers in the index block's existing `saved_attrs` field where a
  backing index is present, and a small flag elsewhere where it is not.

The backing-index attributes (`grouped`, `parted`, `sorted`-as-index) cost **no**
`attrs` bits beyond the existing `RAY_ATTR_HAS_INDEX (0x08)` — they are
distinguished by a new/existing `ray_idx_kind_t`:

- `grouped` → `RAY_IDX_HASH` (existing).
- `parted`  → new `RAY_IDX_PART` kind, payload `{ keys, starts, lens }` mapping
  each distinct value to its contiguous range.
- `sorted`-as-index → `RAY_IDX_SORT` (existing).

## Components & boundaries

- **`src/ops/idxop.h` / index builder** — add `RAY_IDX_PART`, its payload, and a
  builder; reuse the hash builder for `grouped`. Owns backing-structure
  construction and verification.
- **`.attr.*` builtins** (registration alongside `.idx.*` in `src/lang/eval.c`,
  implementation near the index fns in `src/ops/query.c`) — parse the attribute
  symbol, dispatch to verify+stamp / build, expose get/drop. Depends on the
  index builder and the marker representation.
- **Marker representation** — a small accessor pair (`ray_attr_has(v, kind)` /
  `ray_attr_set/clear`) hiding where the bits live, so the storage decision is
  changeable without touching call sites.
- **Invalidation hooks** — call `ray_attr_clear` (or simply do not copy markers)
  in the operators on the Drop list. Conservative default: markers/backing index
  are dropped unless an operator is explicitly on the preserve list.
- **asof fast-path** in `exec_window_join` — read-only consumer of the
  attributes; checks markers/part index and selects skip-sort vs. fall-back.

## Testing

- **Set/verify:** `.attr.set 'sorted` on ascending vs. non-ascending (error);
  `'unique` on distinct vs. dup (error); `'parted` on value-blocked vs. unordered
  (error); `.attr.get` round-trips; `.attr.drop` clears.
- **Combinability:** `sorted`+`unique` coexist; setting a second backing index
  replaces the first; `.idx.info` and `.attr.get` agree.
- **Propagation:** preserve-list operators keep attributes; drop-list operators
  (filter, append, arithmetic, reordering take) clear them.
- **asof equivalence:** for each fast-path (un-partitioned both-sorted;
  partitioned parted+sorted one side; both sides), assert the result is
  **identical** to the sort-merge fallback on the same inputs without attributes.
  Reuse `test/rfl/integration/joins.rfl` fixtures.
- **Performance sanity:** a pre-sorted asof avoids the sort (observable via the
  fast-path being taken; assert correctness, not wall-clock, in CI).

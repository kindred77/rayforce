# Accelerator Indexes

Per-column accelerator structures — `zone`, `hash`, `sort`, `bloom` — attached directly to numeric vectors.  Build once at user request; survive copy / refcount semantics; ride alongside the column through the pipeline.

!!! note "v1 status"
    The four index kinds, their build kernels, and the `(.idx.*)` Rayfall surface are shipped.  *Optimizer integration* — rewriting `filter (= col const)`, `in`, `find`, `distinct`, and join build sides to consult the index instead of a linear scan — is not yet wired.  Today's value is the index-as-data-structure: build it, inspect it, share it across views.  Auto-routed fast paths land in a follow-up.

This page is the **reference** for the `.idx.*` family. For the broader index landscape (HNSW, linked columns, partition pruning, CSR), see [Indexes Overview](indexes-overview.md). For decision walk-throughs and worked workflows, see the [Indexes Guide](../guides/indexes.md).

## Quick Pick

Match the shape of your *query*, not the shape of the data. The **Will enable** column describes the cost the structure unlocks *once optimizer routing lands* — today none of these kinds is consulted by `filter` / `in` / `find` / `distinct` / SIP, so the table below is for forward-planning, not a list of wins you'll see today.

| Query shape | Kind | Will enable (after routing) |
|---|---|---|
| Constant predicate may fall outside the column's value range | `.idx.zone` | O(1) range-check before any scan |
| Repeated `=` / `in` / `find` / `distinct` | `.idx.hash` | O(1) lookup; chained on duplicates |
| Range queries, sorted output, top-N | `.idx.sort` | O(log n) binary search via row-id permutation |
| Cheap probabilistic membership rejection | `.idx.bloom` | O(k) probe; small footprint, no false negatives |

## Why Per-Column Indexes

Rayforce's hot path is morsel-driven columnar execution — every operator scans 1024-element chunks of contiguous values.  That's already fast for full-column work, but it's still O(n) for needle-in-haystack queries: `filter (= col 42)` visits every row even when only one matches, `(in col big-set)` rebuilds a fresh hash table per call, `(find col k)` linear-scans.

An **accelerator index** is a precomputed data structure attached to the column itself.  The user pays the build cost once; once the optimizer routing pass lands, subsequent queries against that column will read the index instead of the raw values. Today the four kinds below all build correctly and are inspectable through `(.idx.info)`, but no query operator consults them — `filter` / `in` / `find` / `distinct` / SIP all still scan linearly.  The structures described below are *what each kind will enable*, not what they accelerate today:

- `zone` — min/max plus null count (~32 bytes per column).  Will let the optimizer prune whole columns / segments when a constant predicate falls outside the range.
- `hash` — chained open-addressing table.  Will support O(1) eq / in / find / distinct; stays useful for non-unique columns via the chain.
- `sort` — ascending row-id permutation.  Will support binary search for range predicates and `limit` queries; pre-grouped distinct.
- `bloom` — m-bit probabilistic membership filter (k=3 hashes).  Cheap, small, no false negatives — will be wired for SIP into joins and quick rejection of `in` probes.

## Constructing an Index

Each kind has a one-arg attach builtin.  Attaching is idempotent — if a different kind is already attached, it's dropped first; mutation invalidates and clears it implicitly.

### Signature

`(.idx.zone v)   (.idx.hash v)   (.idx.sort v)   (.idx.bloom v)`

Attach an index of the named kind to a numeric vector `v`.  Returns the column with the new index attached (the underlying values are unchanged).  Type errors out for non-numeric columns — `RAY_BOOL`, `RAY_U8`, `RAY_I16`, `RAY_I32`, `RAY_I64`, `RAY_F32`, `RAY_F64`, `RAY_DATE`, `RAY_TIME`, `RAY_TIMESTAMP`.  `RAY_SYM` / `RAY_STR` targets are deferred to v2.

### Examples

```lisp
(set v [5 1 9 3 7])

; Zonemap: min, max, null count.
(set vz (.idx.zone v))
(.idx.info vz)
; ⇒ {kind:zone length:5 parent_type:5 saved_attrs:0 min:1 max:9 n_nulls:0}

; Hash: chained table with the linked-list of duplicates.
(set vh (.idx.hash v))
(.idx.info vh)
; ⇒ {kind:hash length:5 parent_type:5 saved_attrs:0 capacity:16 n_keys:5}

; Sort: ascending row-id permutation.
(set vs (.idx.sort v))
(.idx.info vs)
; ⇒ {kind:sort length:5 parent_type:5 saved_attrs:0 perm_len:5}

; Bloom: m bits, k hashes, n_keys non-null rows added.
(set vb (.idx.bloom v))
(.idx.info vb)
; ⇒ {kind:bloom length:5 parent_type:5 saved_attrs:0 m_bits:64 k:3 n_keys:5}
```

Indexed columns participate in normal operations transparently.  `(sum vh)`, `(at vz 0)`, `(count vb)` all work identically to the un-indexed source.  Whatever was true of `v` — nullability, slicing, arithmetic — is true of every `vz`, `vh`, `vs`, `vb`.

## Introspection & Lifecycle

| Function | Returns | Notes |
|---|---|---|
| `(.idx.has? v)` | boolean | True iff any index kind is currently attached. |
| `(.idx.info v)` | dict or null | Per-kind metadata: kind, length, parent_type, saved_attrs, plus kind-specific stats (min/max, capacity, perm_len, m_bits / k / n_keys).  Null when no index is attached. |
| `(.idx.drop v)` | v with index removed | Detaches whichever kind is attached; restores the underlying nullmap state byte-for-byte.  No-op if no index is attached. |

### Mutation Drops the Index

The Rayfall in-place mutators — `(insert 'v val)` for append and `(alter 'v set i val)` / `(alter 'v concat val)` for set / append — invalidate the attached index.  The mutator paths drop the index transparently, restore the original nullmap bytes, and proceed with the write.  Subsequent `(.idx.has?)` calls return false; reattach the index after the write if you want the structure available again (today it's still only inspectable through `(.idx.info)`; query consumers land with the routing pass).

```lisp
(set v [5 1 9 3 7])
(set vz (.idx.zone v))
(.idx.has? vz)              ; ⇒ true
(alter 'vz set 0 100)
(.idx.has? vz)              ; ⇒ false
```

### Coexistence with Linked Columns

An accelerator index uses bytes 0–7 of the column's [nullmap union](https://github.com/RayforceDB/rayforce/blob/master/include/rayforce.h); a [linked column](links.md) uses bytes 8–15.  The two flags — `RAY_ATTR_HAS_INDEX = 0x08` and `RAY_ATTR_HAS_LINK = 0x04` — are independent and a column can carry both.  Dereferencing through a link still works on an indexed column; dropping the index leaves the link intact and vice versa.

## Common Workflows

### Hot column, repeated point lookups

```lisp
(set v (* 7 (til 1000)))   ; 0, 7, 14, ..., 6993
(set v (.idx.hash v))
(in 700 v)                   ; ⇒ true   (700 = 7 × 100)
(in 701 v)                   ; ⇒ false
(set v (.idx.drop v))         ; optional early release; mutators auto-drop
```

### Range scans on a stable column

```lisp
(set prices [102 98 105 101 99 103])
(set prices (.idx.sort prices))
(.idx.info prices)
; ⇒ {kind:sort length:6 parent_type:5 saved_attrs:0 perm_len:6}
```

The original column is unchanged; the sort lives in a row-id permutation alongside it. Once the optimizer routing pass lands, range queries against this column will be able to read that permutation instead of full-scanning — today it's just inspectable structure (`(.idx.info)`), and range queries still scan linearly.

### Whole-column pruning

```lisp
(set ages [21 34 29 42 37])
(set ages (.idx.zone ages))
(.idx.info ages)
; ⇒ {kind:zone length:5 parent_type:5 saved_attrs:0 min:21 max:42 n_nulls:0}
```

A predicate like `(= ages 17)` falls outside `[21, 42]`; once optimizer routing lands, the executor will prune the column without scanning. Today the zone is inspectable via `.idx.info`.

## Performance Characteristics

| Kind | Build | Space | Query |
|---|---|---|---|
| `.idx.zone` | O(n) one pass | O(1) — min, max, n_nulls | O(1) range check |
| `.idx.hash` | O(n) | O(n) table + chain | O(1) average; chain-walk on collisions |
| `.idx.sort` | O(n log n) | O(n) row-id permutation | O(log n) binary search |
| `.idx.bloom` | O(n · k), k = 3 | O(m) bits, m = 64 default | O(k) probe with false-positive rate |

Zone, hash, and bloom build in a single pass over the column; sort needs O(n log n) for the underlying ordering. Space is bounded by the column length, with bloom the only kind that doesn't scale with *n*. The query columns above describe the cost the structure *enables* — until optimizer routing lands, the executor still scans linearly for `filter` / `in` / `find` / `distinct`. See [Caveats](#caveats-and-limits) for the exact wired/unwired surface today.

## Storage and Lifetime

An accelerator index is a child `ray_t*` of type `RAY_INDEX` whose `data[]` payload holds a `ray_index_t` struct — the kind tag, kind-specific child references (the hash table + chain vec, the sort permutation vec, the bloom bit vec), and a 16-byte snapshot of the parent's pre-attach nullmap union.  When the index is attached, the parent's nullmap union holds an owning pointer to the index ray_t.  Detach memcpy's the snapshot back; refcounting handles shared cases (a COW-shared index is cloned-on-detach so neither holder's view breaks).

This design has three useful properties:

- **Free for non-indexed columns.**  Vectors without `RAY_ATTR_HAS_INDEX` set pay no extra cost — the same 16 bytes that hold the index pointer hold the original inline nullmap or external pointer, which they would have anyway.
- **Survives copies and slices.**  `ray_alloc_copy` retains the index along with the parent; slicing creates a fresh slice header that doesn't itself carry `HAS_INDEX`, so the slice is index-free until you attach one to it.  The parent and any other holder still see the original index.
- **Dropped, not orphaned, on mutation.**  Indexes are transient by design.  Persistence is intentionally not supported (use `(.col.link)` for persistent column metadata) — `ray_col_save` serializes a clean column with no index, the on-disk format never carries the bit.

## Caveats and Limits

- **Numeric types only (v1).**  The four kinds accept `RAY_BOOL`, `RAY_U8`, `RAY_I16`, `RAY_I32`, `RAY_I64`, `RAY_F32`, `RAY_F64`, `RAY_DATE`, `RAY_TIME`, `RAY_TIMESTAMP`.  `RAY_SYM` / `RAY_STR` are not yet supported — their nullmap-union byte 8–15 collisions with the `sym_dict` / `str_pool` pointers haven't been swept.
- **One slot per column.**  A column carries at most one index kind at a time.  Calling a different kind's attach replaces the existing one.  Multiple indexes per column are deferred — the slot can point to a list-of-indexes in v2 if real workloads want both eq and range fast paths.
- **In-memory only.**  Indexes do not persist across `ray_col_save` / `ray_col_load`; users rebuild explicitly after a load.  Unlike the HNSW handle (see [Vector Search](../graph/vector-search.md)), which has dedicated `hnsw-save` / `hnsw-load` builtins for explicit persistence, `.idx.*` has no on-disk format at all — rebuild is the only option.
- **No optimizer routing yet.**  Building a hash index doesn't currently make `filter (= col const)` faster — the executor still scans linearly.  Wire-up of consumer paths is the next phase.  Until then, indexes are inspectable metadata you can build and query via `(.idx.info)`; they don't auto-accelerate queries.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `.idx.zone` | `(.idx.zone v)` | Attach a min/max + null-count zonemap |
| `.idx.hash` | `(.idx.hash v)` | Attach a chained hash table for eq lookups |
| `.idx.sort` | `(.idx.sort v)` | Attach an ascending row-id permutation |
| `.idx.bloom` | `(.idx.bloom v)` | Attach an *m*-bit Bloom filter |
| `.idx.drop` | `(.idx.drop v)` | Remove any attached index |
| `.idx.has?` | `(.idx.has? v)` | True iff `v` carries an index |
| `.idx.info` | `(.idx.info v)` | Dict of kind + length + per-kind stats, or null |

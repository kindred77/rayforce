# Indexes Guide

This guide is the procedural companion to [Indexes Overview](../queries/indexes-overview.md) (the map of what exists) and [Accelerator Indexes](../queries/indexes.md) (the per-function reference). Read those first if you haven't; this page assumes you know *what* the kinds are and focuses on *when* and *how*.

## 1. When to bother building an index { #when }

Rayforce's hot path already scans columnar data fast: each operator processes 1024-element morsels, and full-column work is cache-friendly and SIMD-amenable out of the box. An index pays off only when the build cost is amortized across many queries that exploit it.

Three rules of thumb to start with. Note: `.idx.*` indexes are *built* today, but the executor does not yet consult them — observable per-query speedups arrive once the optimizer routing pass lands. Until then, the rules below describe when an index will *structurally* help, not what's faster right now. [Status of each index kind](../queries/indexes-overview.md#status).

- **One-shot scans:** don't build an index. The build is itself a full pass; you'd pay it twice for a single query, with no payoff today and no payoff after routing lands either.
- **Repeated point lookups against the same column:** a hash index is what the routing pass will consult to take `=` / `in` / `find` from O(n) to O(1) average. Build it now if you're staging for that — just don't expect the speedup until routing lands.
- **Range or sorted-output queries against a stable column:** a sort index gives the routing pass an O(log n) binary-search path instead of a full filter. Same caveat — structural fit, not yet observable.

A useful lower bound: if you are not planning to run at least a handful of queries against the indexed column before mutating it, the index loses on round-trip cost.

## 2. Choosing an accelerator kind { #choose }

The four `.idx.*` kinds occupy the same per-column slot today — you pick one. **Important framing:** the four subsections below describe the query shape each kind is *structurally suited* to accelerate. None of them is consulted by query operators today — `filter` / `in` / `find` / `distinct` still scan linearly, and the SIP pass does not consume bloom. Treat the "suited for" phrasings as a guide to which kind to build now in preparation for the routing pass; the observable speedup arrives with that pass, not with the build.

### Equality probes — `.idx.hash`

Structurally suited to "does this value exist in the column" / "which row(s) hold this value" queries. Build cost O(n); space O(n). Handles duplicates via chained open-addressing — equally happy on a unique-key column or one with heavy repetition. Today: built and inspectable; not yet consulted by any query.

### Range queries / sorted access — `.idx.sort`

Structurally suited to "values between A and B", "top-N", and ordered-output queries. Build cost O(n log n); space O(n) for the row-id permutation. The original column stays in original order — the sort lives in the permutation, not the values. Today: built and inspectable; not yet consulted by any query.

### Whole-column / segment pruning — `.idx.zone`

Structurally suited to filter predicates that may fall outside the column's value range. Cheap to build, cheap to keep — effectively three numbers (min, max, null count) per column. Conceptually a column-level analogue of the optimizer's separate [partition-pruning pass](../architecture/pipeline.md); the two are not yet integrated. Today: built and inspectable; not yet consulted by any query.

### Membership rejection — `.idx.bloom`

Structurally suited to cheap "definitely not in this set" rejection, accepting some false positives that fall back to the real check. The 64-bit default is sized for small-to-mid columns; build cost O(n · k) with k = 3 hashes. Once consumed by the SIP pass, it would serve as a sideways-information-passing bitmap into a join's probe side — that integration is not yet wired. Today: built and inspectable; not yet consulted by any query.

## 3. Workflow: hot column, repeated lookups { #workflow-analytics }

The most common use of `.idx.hash`. You have a column that gets queried many times during a session, the values are stable for that session, and each probe today is a linear scan.

```lisp
; Build column.
(set v (* 7 (til 1000)))   ; 0, 7, 14, ..., 6993

; Attach the hash index once.  This builds the structure but does not
; yet change query latency — (in) still scans linearly today.  The
; build is preparation for the upcoming optimizer routing pass.
(set v (.idx.hash v))
(.idx.has? v)                  ; ⇒ true
(.idx.info v)                  ; ⇒ {kind:hash length:1000 ...}  (inspect the structure)

; Membership probes return the right answer either way.
(in 700  v)                    ; ⇒ true   (700 = 7 × 100)
(in 701  v)                    ; ⇒ false  (not a multiple of 7)

; Explicit drop — only needed if you want to release the structure early.
; If you're about to mutate, you can skip this: the in-place mutators
; (insert 'v ...) / (alter 'v ...) drop the index automatically.
(set v (.idx.drop v))
(.idx.has? v)                  ; ⇒ false
```

`.idx.drop` is for *explicit release of the structure* — you call it when you want the per-index memory back before the column itself goes out of scope. You do **not** need to call it before a mutation (the in-place mutators `(insert 'v val)`, `(alter 'v set i val)`, and `(alter 'v concat vals)` all drop any attached index transparently as part of the write path) and you do **not** need it to switch to a different kind: calling another `.idx.*` attach on a column that already has one drops the existing kind first as part of the attach.

## 4. Workflow: ANN over embeddings { #workflow-ann }

HNSW is the only kind today that is both built and consulted at query time. For cosine-similarity nearest-neighbor over a list of float vectors:

```lisp
; A list of three 4-dimensional embeddings.
(set vecs (list
  [0.1 0.2 0.3 0.4]
  [0.9 0.8 0.7 0.6]
  [0.5 0.5 0.5 0.5]))

; Build the index.  metric ∈ {cosine, l2, ip}.
(set h (hnsw-build vecs 'cosine 16 200))

; Query: top-2 nearest neighbors of [0.1 0.2 0.3 0.4].
(ann h [0.1 0.2 0.3 0.4] 2 50)
; ⇒ table { _rowid: I64, _dist: F64 } sorted ascending by _dist

; Optional: release early if you want the memory back before scope exit.
; Otherwise, the handle auto-frees when refcounting drops it.
(hnsw-free h)
```

For one-off queries, brute-force `(knn vecs query k)` needs no index and is fine on small vector sets. Switch to HNSW once you have many queries against the same vector set.

See [Vector Search & HNSW](../graph/vector-search.md) for the full reference, including persistence (`hnsw-save` / `hnsw-load`) and metric trade-offs.

## 5. Workflow: cross-table reference via linked columns { #workflow-link }

A *linked column* stores row-id references into another table; dereferencing follows the link and pulls the target row at query time. Useful when you have a large fact column whose values are dictionary-style references into a smaller dimension table.

The full surface and worked examples live on the [Linked Columns](../queries/links.md) page. From an indexing perspective, the link *is* the cross-table index: there is no separate structure to build, query, or invalidate. With parted facts, the link points at a non-parted dim — per-segment `HAS_LINK` is preserved through `ray_read_parted` and segment streaming, so deref works inside streamed queries without extra plumbing. Parted-target dims are rejected at attach time; see [Linked Columns: Parted-Table Interaction](../queries/links.md#parted).

## 6. Workflow: partition pruning on parted tables { #workflow-parted }

For very large datasets, partition by the discriminator that filter predicates target most often:

- **Date partitioning** — the canonical choice for time-series. Filters of the form `where: (> Date 2024.01.01)` let the optimizer load only the matching partition directories.
- **Integer partitioning** — for bucketed numeric data (e.g. user-id ranges, region codes).
- **Symbol partitioning** — for categorical data with stable, low-cardinality discriminators.

The directory layout drives partition selection — see [Storage Guide: partitioned tables](storage.md#partitioned-tables) for the on-disk shape and [Block Offloading](../architecture/offloading.md) for how the optimizer streams across partitions without loading them all at once.

## 7. Lifecycle gotchas { #lifecycle }

Five things that bite first-time users.

- **Mutation drops the index.** The quoted-symbol forms `(insert 'v val)` and `(alter 'v set i val)` / `(alter 'v concat vals)` mutate `v` in place and invalidate the attached structure. The mutator paths handle the drop transparently — there's no error, just a silently un-indexed column afterward. Rebuild explicitly after the write. The non-quoted form `(insert v val)` returns a fresh value and leaves the original (still indexed) untouched.
- **Slices can't carry an index.** Internally, slicing a column produces a fresh slice header without `RAY_ATTR_HAS_INDEX`; `(.idx.*)` attach refuses to operate on a slice ("cannot index a slice; materialize first"). The Rayfall surface for slices today is C-API-only — this is mostly relevant when calling Rayforce from your own C code.
- **One slot per column, one kind at a time.** Calling `.idx.hash` on a column that already has `.idx.zone` drops the zone first. Multiple coexisting kinds per column is a v2 feature.
- **No persistence for `.idx.*`.** The on-disk column format never carries an index; `ray_col_save` serializes a clean column. Rebuild after a load. HNSW handles can be persisted explicitly with `hnsw-save` / `hnsw-load`.
- **Numeric only (v1).** Internally `.idx.*` accept boolean and numeric element types through `RAY_TIMESTAMP`. From Rayfall, integer / float / date / time / timestamp vectors are the practical reach. Symbol or string columns are explicitly rejected with `error: nyi: only numeric vectors supported in v1` — their nullmap-union layout collides with the `sym_dict` / `str_pool` pointers and the displacement sweep is pending.

## 8. Performance characteristics { #perf }

| Kind | Build cost | Space | Query cost (structural) | Used by today |
|---|---|---|---|---|
| `.idx.zone` | O(n) one pass | O(1) — min/max/null-count | O(1) range check | Inspectable via `.idx.info` only |
| `.idx.hash` | O(n) one pass | O(n) — table + chain | O(1) average; chain-walk on collisions | Inspectable via `.idx.info` only |
| `.idx.sort` | O(n log n) | O(n) — row-id permutation | O(log n) binary search | Inspectable via `.idx.info` only |
| `.idx.bloom` | O(n · k), k = 3 | O(m) bits, m default 64 | O(k) probe with false-positive rate | Inspectable via `.idx.info` only |
| HNSW | O(n log n) typical | O(n · M) graph edges | O(log n) approximate | **Consulted directly** by `(ann)` |
| Linked column | O(n) one-time bind | O(n) row-id vector | O(1) deref | **Consulted directly** on column dereference |
| Partition pruning | None — layout-driven | None | O(p) partition count | **Optimizer pass** rewrites filters to skip non-matching partitions |

The query column above is the cost the structure *enables*. The four `.idx.*` kinds build correctly today, but the executor does not yet rewrite `filter` / `in` / `find` / `distinct` to consult them — queries scan linearly until that routing pass lands. HNSW and linked columns are consulted as soon as you call `(ann)` or dereference the link; partition pruning is the only one that goes through an optimizer pass. Plan accordingly: build a `.idx.*` for repeated queries, but expect the observable speedup to arrive with the routing pass, not before.

## Next steps

- [**Indexes Overview**](../queries/indexes-overview.md) — the full landscape and decision matrix.
- [**Accelerator Indexes**](../queries/indexes.md) — per-function reference for `.idx.*`.
- [**Vector Search & HNSW**](../graph/vector-search.md) — ANN reference, persistence, metrics.
- [**Linked Columns**](../queries/links.md) — cross-table references.
- [**Storage Guide**](storage.md) — partitioned-table layout and recommended directory shapes.

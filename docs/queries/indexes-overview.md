# Indexes Overview

A map of every index-like structure Rayforce ships — per-column accelerators, vector ANN indexes, linked columns, partition pruning, and graph indices. One mental model, then a decision matrix that points you at the right tool.

## What an "index" means in Rayforce

An **index** in Rayforce is a precomputed, optional structure that *rides alongside* the data it's built for. It is not a separate database object: it lives on the column or table it indexes, survives copy / refcount semantics, and travels with the data through the query pipeline. Whether queries actually *consult* that structure varies by kind — HNSW, linked columns, partition pruning, and CSR are read by their query paths today; the four `.idx.*` accelerators are built and inspectable but not yet consumed by any operator. See the [status section](#whats-wired-today-whats-not) below.

Three properties hold for every kind of index documented on this page:

- **Opt-in.** Indexes are built explicitly when you decide the build cost is worth it. The system never builds one behind your back.
- **Mutation-aware.** Mutating the underlying data drops or invalidates the index by design — a stale index is a wrong-answer bug, so the runtime refuses to keep one. Rebuild after a write.
- **Transient by default.** Per-column accelerators live in memory only; HNSW handles can be persisted to disk explicitly. The on-disk file format for ordinary tables never carries an index. After loading, rebuild whatever you want indexed.

## The five index-like structures

### 1. Per-column accelerators — `.idx.zone` / `.idx.hash` / `.idx.sort` / `.idx.bloom`

Attach one of four kinds to a numeric vector. Each kind builds a structure suited to a different query shape: hash for equality lookups, sort for binary search and ordered access, zone for column-level min/max/null pruning, bloom for cheap probabilistic membership rejection. All four occupy the same per-column slot — one kind at a time today.

**Today's status:** all four *build correctly* and are *inspectable* via `(.idx.info)`, but no query operator consults them. Building one does not change `filter` / `in` / `find` / `distinct` / SIP behavior; the optimizer routing pass that wires the consumers up is the next phase. See the [status section](#whats-wired-today-whats-not) below.

**Surface:** `(.idx.zone v)`, `(.idx.hash v)`, `(.idx.sort v)`, `(.idx.bloom v)`, `(.idx.drop v)`, `(.idx.has? v)`, `(.idx.info v)`. Numeric only in v1 (`RAY_BOOL` through `RAY_TIMESTAMP` at the C level; integer / float / date / time / timestamp vectors are the practical reach from Rayfall); `RAY_SYM` / `RAY_STR` are deferred.

**See:** [Accelerator Indexes (reference)](indexes.md) · [Indexes Guide: choosing a kind](../guides/indexes.md#choose).

### 2. Vector ANN index — HNSW

Hierarchical Navigable Small World multi-layer proximity graph for approximate nearest neighbor search over float embedding vectors. Three distance metrics — cosine, L2, inner product. Built once with `hnsw-build`, queried with `ann`, optionally persisted to a directory with `hnsw-save` / `hnsw-load`.

**Surface:** `(hnsw-build col [metric] [M] [ef_c])`, `(ann handle query k [ef_search])`, `(knn col query k [metric])`, `(hnsw-save handle dir)`, `(hnsw-load dir)`, `(hnsw-free handle)`, `(hnsw-info handle)`. Brute-force `knn` needs no index and exists alongside.

**See:** [Vector Search & HNSW](../graph/vector-search.md) · [Indexes Guide: ANN workflow](../guides/indexes.md#workflow-ann).

### 3. Linked columns

A column whose values are row-id references into another table. Functions as a row-level index: dereferencing follows the link and resolves the target row at query time, similar in spirit to a foreign-key relationship but maintained at the column level.

**Surface:** `(.col.link col target-table)`, `(.col.unlink col)`, `(.col.link? col)`, `(.col.target col)`.

**Parted-table interaction:** a parted fact can carry a linked column targeting a non-parted dim (in-memory or splayed); per-segment `HAS_LINK` is preserved through `ray_read_parted` and segment streaming. Targets with any parted column are rejected at attach time. See [Linked Columns: Parted-Table Interaction](links.md#parted-table-interaction).

**See:** [Linked Columns](links.md).

### 4. Partition pruning

A storage layout, not a column-level index, but it functions as a coarse zone-map at the table level: the partition discriminator (date, integer, or symbol) selects whole sub-tables to load. Filters that target the partition column let the optimizer skip entire partitions before any scan begins.

**Surface:** implicit — the directory layout under your database root drives partition selection. The C API loader (`ray_part_load`) infers the partition type (`date` / `int` / `sym`) from the directory names.

**See:** [Columnar Storage](../storage/index.md) · [Storage Guide: partitioned tables](../guides/storage.md#partitioned-tables) · [Block Offloading](../architecture/offloading.md).

### 5. CSR graph index

A double-indexed Compressed Sparse Row adjacency structure (forward + reverse) attached to graph relationships. Used transparently by every graph opcode — `OP_EXPAND`, `OP_VAR_EXPAND`, `OP_SHORTEST_PATH`, `OP_WCO_JOIN` — and by Leapfrog Triejoin for worst-case optimal joins.

**Surface:** none directly — the CSR is built when a relationship is loaded and consulted automatically by graph queries. There is no `(.csr.*)` Rayfall surface today.

**See:** [Graph Storage](../graph/storage.md) · [Graph Algorithms](../graph/algorithms.md).

## Pick the right kind

Match the shape of your query to the structure that fits it. Read the **Active today** column carefully — the four `.idx.*` kinds are *built and inspectable* today but no query operator consults them yet, so they don't change observable query latency until the optimizer routing pass lands. HNSW, linked columns, partition pruning, and CSR are all consumed by their respective query paths today.

| Want to… | Structure | Active today? |
|---|---|---|
| Skip whole columns or segments where a predicate constant lies outside the value range | `.idx.zone` — min/max plus null count | No — structure built; `.idx.info` only |
| Make repeated `=` / `in` / `find` / `distinct` over a numeric column O(1) instead of O(n) | `.idx.hash` — chained open-addressing table | No — structure built; `.idx.info` only |
| Binary-search a numeric column for ranges, sorted scans, or `limit` queries | `.idx.sort` — ascending row-id permutation | No — structure built; `.idx.info` only |
| Cheaply reject "definitely not in this set" probes — e.g. for SIP into a join | `.idx.bloom` — m-bit probabilistic filter | No — structure built; `.idx.info` only (the SIP pass does not yet consult bloom) |
| Find the k nearest neighbors of an embedding vector by cosine, L2, or inner product | HNSW — `(hnsw-build)` + `(ann)` | Yes — `(ann)` consults the index |
| Resolve a cross-table reference at query time without a materialized join | Linked column — `(.col.link)` | Yes — column dereference resolves through the link |
| Skip whole sub-tables in a parted dataset based on the partition discriminator | Partition pruning — date / int / sym partitioning | Yes — optimizer pass rewrites filters |
| Traverse a graph — BFS, shortest path, betweenness, MST | CSR — transparent under graph opcodes | Yes — every graph opcode reads CSR directly |

## What's wired today, what's not

Rayforce is honest about phasing. The structures above all *build* correctly; integration with the optimizer is staged.

- **Per-column accelerators** — build kernels and the `(.idx.*)` surface are shipped and tested. Auto-routing — rewriting `filter (= col const)`, `in`, `find`, `distinct`, and join build sides to consult the index instead of a linear scan — is the next phase. Until then, treat indexes as inspectable metadata; they don't yet auto-accelerate queries. See [caveats](indexes.md#caveats-and-limits).
- **HNSW** — fully wired. `(ann)` consults the index immediately.
- **Linked columns** — fully wired. Dereference resolves through the link at query time.
- **Partition pruning** — the optimizer's pruning pass skips partitions whose discriminator falls outside a filter predicate's range. See [Pipeline & Optimizer](../architecture/pipeline.md).
- **CSR** — fully wired and used by every graph opcode.

## Where to go next

- [**Indexes Guide**](../guides/indexes.md) — procedural walk-through: when to build, how to choose, common workflows, lifecycle gotchas.
- [**Accelerator Indexes**](indexes.md) — full reference for the `.idx.*` family.
- [**Vector Search & HNSW**](../graph/vector-search.md) — ANN reference and worked examples.
- [**Linked Columns**](links.md) — cross-table reference reference.
- [**Columnar Storage**](../storage/index.md) — partitioning and on-disk layout.

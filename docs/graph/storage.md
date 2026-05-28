# Graph Storage

Compressed Sparse Row (CSR) format, double-indexed relationships, persistence to disk, and memory layout.

Rayforce offers two approaches to graph work. The **C API** gives direct control over CSR construction, persistence, and DAG-based graph algorithms — use it for performance-critical workloads, weighted shortest paths, PageRank, and HNSW vector search. **Rayfall** models graphs as EAV triples in the built-in Datalog engine — use it for interactive exploration, pattern matching, and recursive reachability queries from the REPL. See the [Graph Queries Tutorial](../tutorials/graph.md) for a complete Rayfall walkthrough.

## CSR Format

Rayforce stores graph edges in **Compressed Sparse Row (CSR)** format — a compact, cache-friendly representation that enables O(1) neighbor lookups and sequential scan of adjacency lists. CSR is the standard format for high-performance graph engines because it stores edges contiguously in memory, maximizing prefetch efficiency.

A CSR index consists of two arrays:

- **`offsets`** — an `I64` vector of length `n_nodes + 1`. For node `i`, its neighbors are stored at indices `offsets[i]` through `offsets[i+1] - 1` in the targets array.
- **`targets`** — an `I64` vector of length `n_edges`. Contains the destination node IDs, packed contiguously by source node.

!!! note "O(1) degree lookup"
    The degree of any node is simply `offsets[i+1] - offsets[i]`. No iteration required.

### Visual Layout

Consider a graph with 4 nodes and 5 edges:

```lisp
; Edges: 0->1, 0->2, 1->2, 2->3, 3->0

offsets:  [0, 2, 3, 4, 5]      ; 5 entries (n_nodes + 1)
targets:  [1, 2, 2, 3, 0]      ; 5 entries (n_edges)

Node 0: targets[0..2) = [1, 2]  ; neighbors of node 0
Node 1: targets[2..3) = [2]     ; neighbors of node 1
Node 2: targets[3..4) = [3]     ; neighbors of node 2
Node 3: targets[4..5) = [0]     ; neighbors of node 3
```

### Sorted Adjacency Lists

When `sorted == true`, the targets within each adjacency list are stored in ascending order. This is **required** for `OP_WCO_JOIN` (Leapfrog TrieJoin), which relies on sorted lists for its O(N^{1/2}) intersection algorithm. Sorting is enabled by passing `sort_targets = true` when building the relationship.

## The ray_csr_t Structure

The internal CSR representation is defined as:

```c
typedef struct ray_csr {
    ray_t*    offsets;      /* I64 vec, length = n_nodes + 1                 */
    ray_t*    targets;      /* I64 vec, length = n_edges                     */
    ray_t*    rowmap;       /* I64 vec, length = n_edges (CSR pos -> prop row)*/
    ray_t*    props;        /* optional edge property table (ray_t RAY_TABLE) */
    int64_t  n_nodes;
    int64_t  n_edges;
    bool     sorted;       /* targets sorted per adjacency list             */
} ray_csr_t;
```

Key fields:

- **`offsets`** and **`targets`** are standard `ray_t` vectors, managed by the same allocator and COW ref-counting as all other Rayforce data.
- **`rowmap`** maps each CSR edge position back to the original edge property table row, enabling efficient property lookups during traversal (e.g., edge weights for Dijkstra).
- **`props`** is an optional `RAY_TABLE` holding edge properties (weight, label, timestamp, etc.).

## Double-Indexed CSR: ray_rel_t

A **relationship** (`ray_rel_t`) wraps two CSR indices — one for forward traversal (src → dst) and one for reverse (dst → src):

```c
typedef struct ray_rel {
    uint16_t    from_table;   /* source table ID                */
    uint16_t    to_table;     /* destination table ID            */
    int64_t     name_sym;     /* relationship name (symbol ID)  */
    ray_csr_t    fwd;          /* src -> dst                     */
    ray_csr_t    rev;          /* dst -> src                     */
} ray_rel_t;
```

Having both directions pre-built allows the query engine to traverse edges in either direction without re-indexing at query time. The `direction` parameter on graph opcodes selects which CSR to use:

- `0` — forward (fwd CSR)
- `1` — reverse (rev CSR)
- `2` — both directions (union of forward and reverse neighbors)

## Building a Graph from Edge Lists

The primary API for constructing a relationship from a table of edges:

```c
ray_rel_t* ray_rel_from_edges(
    ray_t*       edge_table,     /* table with src and dst columns */
    const char* src_col,        /* name of source column          */
    const char* dst_col,        /* name of destination column     */
    int64_t     n_src_nodes,    /* number of source nodes         */
    int64_t     n_dst_nodes,    /* number of destination nodes    */
    bool        sort_targets    /* sort adjacency lists?          */
);
```

There is also `ray_rel_build` for constructing from a foreign-key column:

```c
ray_rel_t* ray_rel_build(
    ray_t*       from_table,     /* table containing foreign key   */
    const char* fk_col,         /* foreign key column name        */
    int64_t     n_target_nodes, /* number of target nodes         */
    bool        sort_targets    /* sort adjacency lists?          */
);
```

### Complete C Example

Build a small social graph with 4 users and query neighbors:

```c
/* Build an edge table: Alice(0)->Bob(1), Alice(0)->Carol(2),
 * Bob(1)->Carol(2), Carol(2)->Dave(3) */
ray_t* edges = ray_table_new(2);

/* Source column */
ray_t* src = ray_vec_new(RAY_I64, 4);
int64_t src_vals[] = {0, 0, 1, 2};
for (int i = 0; i < 4; i++)
    src = ray_vec_append(src, &src_vals[i]);

/* Destination column */
ray_t* dst = ray_vec_new(RAY_I64, 4);
int64_t dst_vals[] = {1, 2, 2, 3};
for (int i = 0; i < 4; i++)
    dst = ray_vec_append(dst, &dst_vals[i]);

/* Add columns to table */
int64_t src_sym = ray_sym_intern("src", 3);
int64_t dst_sym = ray_sym_intern("dst", 3);
edges = ray_table_add_col(edges, src_sym, src);
edges = ray_table_add_col(edges, dst_sym, dst);

/* Build double-indexed CSR with sorted adjacency lists */
ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst",
                                    4, 4, true);

/* Query neighbors of node 0 (Alice) in the forward direction */
int64_t count;
const int64_t* nbrs = ray_rel_neighbors(rel, 0, 0, &count);
/* nbrs = [1, 2], count = 2 (Bob and Carol) */

/* Query reverse neighbors of node 2 (Carol): who points to Carol? */
const int64_t* rev_nbrs = ray_rel_neighbors(rel, 2, 1, &count);
/* rev_nbrs = [0, 1], count = 2 (Alice and Bob) */

/* Cleanup */
ray_rel_free(rel);
ray_release(edges);
```

#### Rayfall

In Rayfall, graphs are built by asserting EAV triples into a datoms store. Each edge becomes a fact with the source entity, an attribute name, and the destination entity:

```lisp
; Create an empty graph database
(set db (datoms))

; Add edges: Alice(0)->Bob(1), Alice(0)->Carol(2),
; Bob(1)->Carol(2), Carol(2)->Dave(3)
(set db (assert-fact db 0 'follows 1))
(set db (assert-fact db 0 'follows 2))
(set db (assert-fact db 1 'follows 2))
(set db (assert-fact db 2 'follows 3))

; List all edges
(scan-eav db 'follows)
; => table with columns (e, v) showing all follow edges
```

Query neighbors of a specific node using a Datalog query with a constant in the entity position:

```lisp
; Forward neighbors of node 0 (Alice)
(query db (find ?dst)
  (where (0 :follows ?dst)))
; => ?dst: 1, 2 (Bob and Carol)

; Reverse: who follows Carol (node 2)?
(query db (find ?src)
  (where (?src :follows 2)))
; => ?src: 0, 1 (Alice and Bob)
```

## Neighbor Lookup API

Two inline functions provide fast neighbor access without going through the DAG pipeline:

### ray_csr_degree

```c
static inline int64_t ray_csr_degree(ray_csr_t* csr, int64_t node);
```

Returns the degree (number of outgoing edges) of the given node in O(1) time.

### ray_csr_neighbors

```c
static inline int64_t* ray_csr_neighbors(
    ray_csr_t* csr, int64_t node, int64_t* out_count);
```

Returns a pointer directly into the targets array for the given node's adjacency list. The pointer is valid as long as the CSR is alive. Writes the neighbor count to `out_count`.

### ray_rel_neighbors

```c
const int64_t* ray_rel_neighbors(
    ray_rel_t* rel, int64_t node,
    uint8_t direction, int64_t* out_count);
```

Higher-level API that selects the forward or reverse CSR based on `direction` (0=forward, 1=reverse).

## Edge Properties

Edge properties (weights, labels, timestamps) are stored in a standard `RAY_TABLE` attached to the CSR via `ray_rel_set_props`:

```c
ray_t* props = ray_table_new(1);
/* ... add a "weight" column with F64 values ... */
ray_rel_set_props(rel, props);
```

The `rowmap` vector in each CSR maps edge positions back to property table rows, so graph algorithms like Dijkstra and A* can look up edge weights during traversal.

## Persistence

Relationships can be saved to disk and loaded back, supporting both eager loading and memory-mapped I/O.

### Save

```c
ray_err_t ray_rel_save(ray_rel_t* rel, const char* dir);
```

Writes the forward and reverse CSR arrays (offsets, targets, rowmap) as column files in the given directory. Returns `RAY_OK` on success.

### Load (eager)

```c
ray_rel_t* ray_rel_load(const char* dir);
```

Reads the column files into memory and reconstructs the `ray_rel_t`. The loaded vectors use the standard Rayforce allocator and participate in COW ref counting.

### Memory-mapped Load

```c
ray_rel_t* ray_rel_mmap(const char* dir);
```

Maps the column files directly into the address space without copying. The vectors are marked as `mmod=1` (file-mmap), making them read-only. This is ideal for large graphs that exceed available RAM — the OS page cache handles eviction transparently.

!!! note "mmap vs. eager load"
    Use `ray_rel_mmap` for graphs that are larger than available memory or when startup time matters. Use `ray_rel_load` when you need to mutate the graph or want predictable query latency without page faults.

### Cleanup

```c
void ray_rel_free(ray_rel_t* rel);
```

Releases all vectors in both the forward and reverse CSR, then frees the `ray_rel_t` itself.

## Memory Layout

Since CSR arrays are standard `ray_t` vectors, they benefit from the same infrastructure as all other Rayforce data:

- **Buddy allocator** — vectors are allocated from thread-local heaps with order-based block sizing
- **COW ref counting** — multiple graphs can share the same CSR vectors without copying; mutations trigger copy-on-write
- **Null bitmaps** — the 16-byte nullmap in each `ray_t` header is unused for CSR vectors (edge indices are never null)
- **mmap support** — persisted vectors can be memory-mapped with the `mmod` flag set

The total memory footprint of a CSR with N nodes and E edges is:

```text
offsets:  (N + 1) * 8 bytes  +  32 bytes header
targets:  E * 8 bytes        +  32 bytes header
rowmap:   E * 8 bytes        +  32 bytes header  (if properties exist)

Total (without props): ~16E + 8N + 96 bytes
```

## Integration with the DAG Pipeline

Once a `ray_rel_t` is built, it feeds into the operation graph for query execution. Graph opcodes like `OP_EXPAND`, `OP_DIJKSTRA`, and `OP_PAGERANK` receive the relationship as a parameter and operate directly on the CSR arrays during morsel-driven execution.

```c
/* Build a DAG that expands neighbors of filtered nodes */
ray_graph_t* g = ray_graph_new(nodes_table);
ray_op_t* id_col    = ray_scan(g, "id");
ray_op_t* filter     = ray_filter(g, id_col, ray_lt(g, id_col, ray_const_i64(g, 100)));
ray_op_t* neighbors  = ray_expand(g, filter, rel, 0);  /* forward */
ray_t*    result     = ray_execute(g, neighbors);
ray_graph_free(g);
```

See [Graph Algorithms](algorithms.md) for the full catalog of graph opcodes that operate on CSR storage.

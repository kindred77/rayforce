# Pipeline & Optimizer

## Three-Phase Pipeline

Every Rayforce query goes through three phases:

1. **Build** — Construct a lazy DAG of operations using the C API or Rayfall `select`/`update` builtins. No data is processed at this stage.
2. **Optimize** — Transform the DAG through multiple optimization passes that rewrite, reorder, and fuse operations.
3. **Execute** — Walk the optimized DAG and process data in 1024-element morsels using fused bytecode with computed-goto dispatch.

```c
/* Phase 1: Build the DAG (lazy, no data movement) */
ray_graph_t* g    = ray_graph_new(table);
ray_op_t*    x    = ray_scan(g, "x");
ray_op_t*    y    = ray_scan(g, "y");
ray_op_t*    sum  = ray_add(g, x, y);
ray_op_t*    pred = ray_gt(g, sum, ray_const_i64(g, 100));
ray_op_t*    filt = ray_filter(g, sum, pred);

/* Phase 2 + 3: Optimize and execute (called internally) */
ray_t* result = ray_execute(g, filt);

ray_graph_free(g);
```

## DAG Node Structure

### ray_op_t — Base Node (32 bytes)

Every operation in the DAG is represented by a 32-byte `ray_op_t` that fits in a single cache line:

```c
typedef struct ray_op {
    uint16_t       opcode;     /* OP_ADD, OP_SCAN, OP_FILTER, etc.  */
    uint8_t        arity;      /* 0, 1, or 2                        */
    uint8_t        flags;      /* OP_FLAG_FUSED, OP_FLAG_DEAD       */
    int8_t         out_type;   /* inferred output type               */
    uint8_t        pad[3];
    uint32_t       id;         /* unique node ID                    */
    uint32_t       est_rows;   /* estimated row count                */
    struct ray_op*  inputs[2];  /* NULL if unused                    */
} ray_op_t;
```

Key design decisions:

- **32-byte alignment** — nodes are allocated from a contiguous array in the `ray_graph_t`, keeping the DAG hot in L1 cache during optimization walks.
- **At most 2 inputs** — binary operations use both; unary ops use `inputs[0]` only; sources have both NULL.
- **Flags** — `OP_FLAG_FUSED` marks nodes absorbed by fusion; `OP_FLAG_DEAD` marks nodes removed by DCE.
- **est_rows** — populated by the type inference pass and used by filter reorder and join planning.

### ray_op_ext_t — Extended Node

Operations that need more state (GROUP, SORT, JOIN, WINDOW, PIVOT, and all graph ops) use an extended node that embeds `ray_op_t` as its first field, followed by a union of operation-specific data:

- **GROUP**: key columns, aggregation ops and inputs, key/agg counts
- **SORT**: column array, descending flags, nulls-first flags
- **JOIN**: left/right key columns, join type (inner/left/full)
- **WINDOW**: partition keys, order keys, function kinds, frame specification
- **PIVOT**: index columns, pivot column, value column, aggregation op
- **Graph ops**: relationship pointer, SIP selection bitmap, direction, depth limits, damping factor, weight column, coordinate columns
- **WCO**: relationship array, variable count
- **Vector similarity**: query vector, dimension, k, ef_search (HNSW)

### ray_graph_t — Operation Graph

```c
typedef struct ray_graph {
    ray_op_t*       nodes;       /* contiguous array of op nodes       */
    uint32_t       node_count;  /* number of nodes                   */
    uint32_t       node_cap;    /* allocated capacity                */
    ray_t*          table;       /* bound table (column source)       */
    ray_t**         tables;      /* multi-table registry               */
    uint16_t       n_tables;    /* number of registered tables        */
    ray_op_ext_t**  ext_nodes;   /* tracked extended nodes for cleanup */
    uint32_t       ext_count;
    uint32_t       ext_cap;
    ray_t*          selection;   /* RAY_SEL bitmap (lazy filter)       */
} ray_graph_t;
```

## Optimizer Passes

The optimizer runs multiple passes in a fixed order. Each pass walks the DAG and rewrites it in-place.

### 1. Type Inference

Bottom-up pass that propagates types from leaf nodes (SCAN, CONST) through the DAG. Determines the `out_type` of every node and populates `est_rows` estimates. This information drives all subsequent passes.

### 2. Constant Folding

Evaluates operations on constant inputs at optimization time. For example, `ray_add(const(2), const(3))` is replaced with `const(5)`. Applies to all element-wise arithmetic, comparisons, and logical operations.

### 3. Sideways Information Passing (SIP)

Propagates selection bitmaps backward through `OP_EXPAND` chains. When a filter follows a graph expansion, SIP creates a `RAY_SEL` bitmap on the source side that allows the executor to skip source nodes whose neighbors would all fail the downstream filter. See [SIP Optimization](../graph/algorithms.md#sip) for details.

### 4. Factorize

Marks multi-hop graph expansions for factorized execution. Sets the `factorized` flag on EXPAND/VAR_EXPAND nodes, enabling output as `ray_fvec_t` / `ray_ftable_t` instead of flat vectors. Avoids cross-product materialization in multi-join graph patterns.

### 5. Predicate Pushdown

Pushes filter predicates as close to the data source as possible. A filter above a join is split into components that can be evaluated on individual join inputs, reducing the number of rows entering the join. Filters above scans are pushed directly onto the scan, eliminating rows before any computation.

### 6. Filter Reorder

When multiple filter predicates are chained, reorders them by estimated selectivity (most selective first). Uses the `est_rows` from type inference to estimate selectivity. More selective filters run first, reducing the number of rows processed by subsequent filters.

### 7. Projection Pushdown

Identifies which columns are actually needed by the query and pushes projection below joins and group-by operations. Columns that are never referenced after a certain point are dropped early, reducing memory bandwidth and intermediate result sizes.

### 8. Partition Pruning

For partitioned tables, analyzes filter predicates to determine which partitions can be skipped entirely. A predicate like `date > '2024-01-01'` on a date-partitioned table eliminates all partition directories before that date without scanning them. The pruning pass produces a `seg_mask` bitmap that the executor uses during [block offloading](offloading.md) to skip pruned segments entirely.

### 9. Fusion

The most impactful optimization. Merges chains of element-wise operations (arithmetic, comparisons, logical ops, string ops) into a single fused pass. A chain like `SCAN → ADD → MUL → GT → FILTER` becomes a single bytecode program that processes each morsel in one pass without materializing intermediate vectors.

!!! note "Fusion boundaries"
    Pipeline breakers (GROUP, SORT, JOIN, WINDOW, graph ops) cannot be fused. They materialize their inputs and produce new vectors for the downstream fused chain.

Fused nodes are marked with `OP_FLAG_FUSED`. The lead node (the last in the chain) consumes all fused predecessors and executes them as inlined bytecode.

### 10. Dead Code Elimination (DCE)

Removes nodes marked `OP_FLAG_DEAD` or `OP_FLAG_FUSED` (absorbed by fusion) that have no live consumers. Cleans up the DAG after all other passes.

## Morsel-Driven Execution

### Morsel Processing

All vector processing in Rayforce is chunked into **morsels** of 1024 elements (`RAY_MORSEL_ELEMS`). This constant is tuned to fit in L1 cache across all supported architectures.

```c
typedef struct {
    ray_t*    vec;          /* source vector                     */
    int64_t  offset;       /* current position (element index)  */
    int64_t  len;          /* total length of vector            */
    uint32_t elem_size;    /* bytes per element                 */
    int64_t  morsel_len;   /* elements in current morsel        */
    void*    morsel_ptr;   /* pointer to current morsel data    */
    uint8_t* null_bits;    /* current morsel null bitmap        */
} ray_morsel_t;
```

The morsel iterator API:

```c
ray_morsel_init(&m, vec);             /* initialize iterator       */
while (ray_morsel_next(&m)) {         /* advance to next morsel    */
    /* process m.morsel_ptr[0..m.morsel_len) */
}
```

### Computed-Goto Dispatch

Fused operation chains compile to a bytecode program that the executor runs using computed-goto dispatch (GCC/Clang `__attribute__((computed_goto))`). Each opcode is a label in a jump table, eliminating branch prediction overhead from a traditional switch/case loop. The executor processes one morsel at a time through the entire fused chain before advancing.

### Register Slots

During execution, intermediate morsel results are stored in register slots on the executor's pipeline (`ray_pipe_t`). Each fused node reads from and writes to specific register slots, avoiding heap allocation for intermediate results within a morsel.

```c
typedef struct ray_pipe {
    ray_op_t*          op;            /* operation node                */
    struct ray_pipe*   inputs[2];     /* upstream pipes                */
    ray_morsel_t       state;         /* current morsel state          */
    ray_t*             materialized;  /* materialized intermediate     */
    int               spill_fd;      /* spill file descriptor (-1)    */
} ray_pipe_t;
```

## Selection Bitmaps

Filters in Rayforce produce `RAY_SEL` bitmaps instead of materializing filtered vectors. A selection bitmap is a compact bit-vector with per-morsel metadata that enables three-tier processing:

| Segment Flag | Meaning | Processing |
|---|---|---|
| `RAY_SEL_NONE (0)` | All bits zero | Skip entire morsel — zero cost |
| `RAY_SEL_ALL (1)` | All bits one | Process without bitmap check |
| `RAY_SEL_MIX (2)` | Mixed bits | Check per-row bitmap |

The selection bitmap layout in memory:

```c
/* RAY_SEL block layout (at ray_data offset 0): */
ray_sel_meta_t  meta;          /* total_pass, n_segs (16 bytes) */
uint8_t        seg_flags[];   /* NONE/ALL/MIX per morsel       */
uint16_t       seg_popcnt[];  /* passing row count per morsel   */
uint64_t       bits[];        /* 1024 rows / 64 bits = 16 words per morsel */
```

This three-tier scheme means that fully-passing or fully-failing morsels (which are common in practice) incur no per-row overhead. Only the `MIX` segments require bitmap checking.

## Parallelism

### Thread Pool

Rayforce uses a global thread pool (`ray_pool_t`) for parallel execution. Queries with more than `RAY_PARALLEL_THRESHOLD` (64 * 1024 = 65,536) elements are dispatched across worker threads. Each thread processes `RAY_DISPATCH_MORSELS` (8) morsels at a time to amortize scheduling overhead.

### Radix-Partitioned Hash Join

Hash joins use adaptive radix partitioning to ensure each partition's hash table fits in L2 cache. The number of radix bits adapts between `RAY_JOIN_MIN_RADIX` (2, producing 4 partitions) and `RAY_JOIN_MAX_RADIX` (14, producing 16K partitions) based on input size, targeting a per-partition working set of `RAY_JOIN_L2_TARGET` (256 KB).

The join pipeline:

1. **Partition** — Radix-partition both inputs by hash key bits
2. **Build** — Build per-partition hash tables (each fits in L2)
3. **Probe** — Probe partitions in parallel across worker threads

### Per-Thread Heaps

Each worker thread has its own heap (`heap_id` in the pool header). Vectors allocated during parallel execution are tagged with their owning heap via the pool they reside in. Cross-heap frees are deferred to a lock-free LIFO and reclaimed when the owning heap flushes. See [Memory Model](memory.md) for details.

## Full Pipeline Example

A complete example showing all three phases for a filtered aggregation query:

```c
/* Table: trades with columns price (F64), qty (I64), sym (SYM) */
ray_graph_t* g = ray_graph_new(trades);

/* Phase 1: Build DAG */
ray_op_t* price = ray_scan(g, "price");
ray_op_t* qty   = ray_scan(g, "qty");
ray_op_t* sym   = ray_scan(g, "sym");

/* Compute notional = price * qty */
ray_op_t* notional = ray_mul(g, price, qty);

/* Filter: price > 50.0 */
ray_op_t* pred = ray_gt(g, price, ray_const_f64(g, 50.0));
ray_op_t* filt = ray_filter(g, notional, pred);

/* Group by sym, sum notional */
ray_op_t* keys[] = {sym};
uint16_t agg_ops[] = {OP_SUM};
ray_op_t* agg_ins[] = {filt};
ray_op_t* grouped = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

/* Phase 2+3: Optimize and execute */
ray_t* result = ray_execute(g, grouped);

/*
 * What the optimizer does:
 *  1. Type inference: price=F64, qty=I64, notional=F64, pred=BOOL
 *  2. Constant folding: const 50.0 stays (already constant)
 *  3. Predicate pushdown: filter moves below the multiply
 *  4. Fusion: SCAN(price) -> GT(const) -> FILTER fused into one pass
 *  5. DCE: removes any dead intermediate nodes
 */

ray_graph_free(g);
```

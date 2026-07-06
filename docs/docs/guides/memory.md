# Memory & Monitoring

## 1. Memory Architecture Overview

Rayforce uses a custom memory subsystem — no calls to `malloc` or `free` ever reach the C library. Every allocation flows through one of these layers:

- **Buddy allocator with thread-local heaps** — Each VM thread gets its own heap (identified by a `heap_id`). Allocations are fast, lock-free within a thread. Cross-heap frees are deferred to a lock-free queue and reclaimed lazily.
- **Slab cache** — Small allocations (common for atoms and short vectors) are served from pre-sized slab pools, avoiding buddy-tree overhead.
- **COW ref counting** — Vectors use copy-on-write semantics via `ray_retain`/`ray_release`. Shared vectors are only copied when mutated. Note that `ray_retain`/`ray_release`/`ray_cow` are no-ops on `RAY_ERROR` objects, so an error block must be reclaimed with `ray_error_free()` rather than `ray_release()`.
- **Arena allocator** — For bulk short-lived blocks (e.g., intermediate query results). Arena objects carry an `RAY_ATTR_ARENA` flag that makes retain/release no-ops. The entire arena is freed at once when work completes.
- **Out-of-core spill** — There is no enforced memory ceiling. When an anonymous `mmap` for a new pool is *refused* by the OS, the heap falls back to a file-backed mapping on disk, so the working set spills rather than the allocation failing. Note this depends on the OS actually refusing the mapping: under Linux's default memory overcommit (`vm.overcommit_memory=0`), a single allocation larger than RAM+swap is typically *accepted* and then killed by the OOM killer as its pages fault in — spill only saves you when the kernel declines the mapping up front. Total physical RAM is detected at startup for informational reporting only (see `.sys.info` → `total-mem`).

For a deep dive into the allocator internals, see [Memory Model](../architecture/memory.md).

## 2. The `.sys.mem` Function

Call `(.sys.mem 0)` to get a snapshot of the current heap's allocation statistics. It returns a dictionary with the following fields:

| Field | Type | Description |
|---|---|---|
| `alloc-count` | i64 | Total number of allocations performed since init |
| `bytes-allocated` | i64 | Live bytes in buddy-pool blocks (sub-32 MB objects) |
| `direct-bytes` | i64 | Live bytes in direct mmaps (objects ≥ 32 MB, mapped at exact size) |
| `peak-bytes` | i64 | High-water mark of bytes-allocated |
| `slab-hits` | i64 | Number of allocations served from the slab cache |
| `sys-current` | i64 | Committed RAM: every anonymous mapping (buddy pools, sys allocations, swap-fallback pool) |
| `sys-mapped` | i64 | File-backed bytes currently mapped (splayed columns, symbol file, parse buffers) |
| `sys-mapped-peak` | i64 | High-water mark of `sys-mapped` |

### Basic Usage

```lisp
;; Check current memory stats
(.sys.mem)
```

```text
alloc-count     | 14523
bytes-allocated | 2621440
peak-bytes      | 5242880
slab-hits       | 9870
sys-current     | 8388608
```

### Checking Memory Before and After an Operation

```lisp
;; Snapshot before
(set before (.sys.mem))

;; Load a large CSV
(set trades (.csv.read "trades-10M.csv"))

;; Snapshot after
(set after (.sys.mem))

;; See how much memory the load consumed
(- (after 'bytes-allocated) (before 'bytes-allocated))
```

```text
838860800
```

In this example, loading 10 million rows consumed roughly 800 MB of heap memory.

## 3. The `.sys.gc` Function

Call `(.sys.gc 0)` to signal that the runtime should reclaim unused memory. Currently this is a lightweight hook that returns `0` — Rayforce uses deterministic ref counting and eager page release via `madvise` during buddy coalescing, so most memory is reclaimed automatically when references are dropped.

```lisp
(.sys.gc)  ; 0
```

!!! note "Note"

    Because Rayforce uses deterministic ref counting (not tracing GC), memory is freed immediately when the last reference is released. The buddy allocator coalesces blocks and releases pages back to the OS automatically. `(.sys.gc 0)` exists as a hook for future use.

## 4. The `.sys.info` Function

Call `(.sys.info 0)` to see system-level information about the Rayforce runtime:

```lisp
(.sys.info)
```

```text
cores     | 16
page-size | 4096
total-mem | 16777216000
```

| Field | Description |
|---|---|
| `cores` | Number of logical CPU cores available |
| `page-size` | OS page size in bytes |
| `total-mem` | Total physical RAM in bytes |

!!! note "Note"

    On Windows, only `cores` is currently reported.

## 5. Progress Monitoring

Long-running queries display a progress bar automatically in the REPL. The bar appears after approximately 2 seconds of execution and shows real-time feedback.

### Progress Bar Format

```text
[████████░░░░░░░░]  52% · group: hash · 3.2s · 1.2G/12.8G
```

The bar displays:

- **Percentage** — Estimated completion based on rows processed
- **Operation name** — The current phase (e.g., `group: hash`, `sort: merge`, `join: probe`)
- **Elapsed time** — Wall-clock time since query start
- **Memory** — Live object footprint (`used / total`) against total physical RAM, so you can watch a large query climb toward the point where it no longer fits

### Example Session

```lisp
;; This query processes 50 million rows -- progress bar appears automatically
(select {from: trades
         by: {sym: sym}
         total: (sum price)
                n: (count price)
                hi: (max price)})
```

```text
[████████████████]  100% · group: merge · 4.1s · 3.8G/12.8G
┌──────┬───────────────┬──────────┬──────────┐
│  sym │     total     │    n     │    hi    │
│  SYM │      F64      │   I64    │   F64    │
├──────┼───────────────┼──────────┼──────────┤
│ AAPL │ 8825431692.50 │ 50120832 │   502.39 │
│ GOOG │ 6129847201.75 │ 49879168 │   501.97 │
├──────┴───────────────┴──────────┴──────────┤
│ 2 rows                        4 columns    │
└────────────────────────────────────────────┘
```

The progress bar is cleared automatically when the query completes and the result is displayed. In non-interactive mode (file execution), progress output is suppressed.

## 6. The `timeit` Function

Wrap any expression in `(timeit ...)` to measure its execution time. It evaluates the expression, discards the result, and **returns the elapsed time in milliseconds** as an f64 value.

```lisp
;; Time a simple vector operation
(timeit (sum (til 10000000)))
```

```text
12.4
```

```lisp
;; Time a grouped aggregation
(timeit (select {from: trades
                 by: {sym: sym}
                 n: (count price)}))
```

```text
291.0
```

!!! note "Note"

    `timeit` returns only the elapsed milliseconds — it does not return the expression's result. To see the result and the timing, evaluate the expression separately and use `timeit` for benchmarking.

## 7. Profiling with `:t`

The REPL command `:t` (or `:timeit`) toggles profiling mode. When active, every expression displays a detailed timing span tree showing where time was spent.

### Example Session

```lisp
;; Toggle profiling on
:t
```

```text
. Timeit is on.
```

```lisp
;; Now every expression shows timing spans
(sum (til 1000000))
```

```text
499999500000
── top-level ─────────── 1.8ms
```

```lisp
;; Toggle profiling off
:t
```

```text
. Timeit is off.
```

This is the fastest way to identify slow expressions in an interactive session. Combine it with `.sys.mem` snapshots to see both time and memory costs.

## 8. Total RAM and Out-of-Core Spill

Rayforce imposes **no enforced memory ceiling**. Total physical RAM is detected
at startup purely for reporting; there is no budget threshold that rejects work
or throttles it.

When the OS *refuses* a new pool mapping, the heap spills that pool to a
file-backed mapping on disk and the query keeps running (more slowly). This is a
genuine safety net under strict overcommit (`vm.overcommit_memory=2`) or when
address space is bounded. Under Linux's **default** heuristic overcommit,
however, a single result larger than RAM+swap is usually *accepted* by the
kernel and then killed by the OOM killer as its pages fault in — so a query like
`(til 10000000000)` (a 74 GiB vector) on a machine with less RAM than that will
be terminated, not spilled. Watch the progress bar's `used / total` figure to
see the footprint approach total RAM and cancel with Ctrl-C before it is killed.

### Checking total RAM

```lisp
;; The total-mem field shows total physical RAM
(.sys.info)
```

```text
cores     | 16
page-size | 4096
total-mem | 16777216000
```

### Gauging headroom

To see how close a workload is to spilling to disk, compare the live object
footprint (`bytes-allocated + direct-bytes` from `(.sys.mem)`) against
`total-mem` from `(.sys.info)`:

```lisp
;; Live footprint as a percentage of physical RAM
(set stats (.sys.mem))
(set info (.sys.info))
(* 100.0 (/ (+ (stats bytes-allocated) (stats direct-bytes)) (info total-mem)))
```

```text
29.4
```

This shows 29.4% of physical RAM is in use — plenty of headroom before the heap
begins spilling to disk.

## 9. Practical Patterns

### Pattern 1: Monitor Memory During CSV Loading

```lisp
;; Check baseline
(set m0 (.sys.mem))

;; Load data
(set trades (.csv.read "trades-50M.csv"))

;; Check cost
(set m1 (.sys.mem))
(println (format "Loaded: {} bytes, peak: {} bytes"
  (- (m1 bytes-allocated) (m0 bytes-allocated))
  (m1 peak-bytes)))
```

```text
Loaded: 4194304000 bytes, peak: 4831838208 bytes
```

### Pattern 2: GC Between Independent Queries

```lisp
;; First analysis pass
(set result1 (select {from: trades
                       by: {date: date}
                       vol: (sum qty)})
(.csv.write result1 "daily-volume.csv")
(set result1 0)

;; Free intermediates before the next heavy query
(.sys.gc)

;; Second analysis pass with maximum headroom
(set result2 (select {from: trades
                       by: {sym: sym}
                       vwap: (/ (sum (* price qty)) (sum qty))}))
```

### Pattern 3: Profile a Slow Query

```lisp
;; Enable REPL profiling
:timeit

;; Break the query into parts to find the bottleneck
(set filtered (select {from: trades where: (> price 100.0)}))
```

```text
elapsed: 42.1ms
```

```lisp
(set grouped (select {from: filtered
                        by: {sym: sym}
                        avg_price: (avg price)
                               n: (count price)}))
```

```text
elapsed: 203.7ms
```

```lisp
(set sorted (select {from: grouped by: {n: (desc n)}}))
```

```text
elapsed: 0.8ms
```

```lisp
;; The group-by at 204ms is the bottleneck, not the filter (42ms) or sort (<1ms)
:timeit
```

### Pattern 4: Check Peak Memory After a Join

```lisp
;; Reset peak tracking with .sys.gc
(.sys.gc)
(set before-peak ((.sys.mem 0) peak-bytes))

;; Run a memory-intensive join
(set joined (select {from: trades join: quotes on: [sym time]}))

;; Check the peak
(set after-peak ((.sys.mem 0) peak-bytes))
(println (format "Join peak overhead: {} bytes"
  (- after-peak before-peak)))
```

```text
Join peak overhead: 1073741824 bytes
```

This tells you the join needed about 1 GB of temporary memory beyond what was already allocated.

## Summary

| Tool | What It Does | When to Use |
|---|---|---|
| `(.sys.mem 0)` | Returns heap allocation statistics | Monitor memory usage, detect leaks |
| `(.sys.gc 0)` | Flushes caches, releases pages | Between heavy queries, before benchmarks |
| `(.sys.info 0)` | Shows system and runtime info | Check total RAM, CPU count, OS details |
| `(timeit expr)` | Measures execution time of one expression | Benchmark a specific operation |
| `:timeit` | Toggles profiling for all REPL expressions | Interactive performance exploration |
| Progress bar | Automatic during long queries | Visual feedback on query progress |

## Next Steps

- [**Memory Model**](../architecture/memory.md) — Deep dive into the buddy allocator, arenas, COW, and per-VM heaps
- [**Block Offloading**](../architecture/offloading.md) — How Rayforce handles datasets larger than RAM
- [**Pipeline & Optimizer**](../architecture/pipeline.md) — How queries are planned, optimized, and executed
- [**REPL Reference**](../language/repl.md) — All REPL commands and configuration

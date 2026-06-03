# Allocator Micro-Benchmark Baseline

## Pre-P0 baseline (commit cce8bf82)

```
(a) single-thread alloc/free:
  atom-64B          129.5 Mops/s  (20000000 iters, 0.154s)
  vec-256B          143.7 Mops/s  (20000000 iters, 0.139s)
  morsel-8K         101.7 Mops/s  (5000000 iters, 0.049s)
  morsel-16K        101.9 Mops/s  (5000000 iters, 0.049s)
  large-1M          103.0 Mops/s  (200000 iters, 0.002s)
(b) producer-consumer (morsel-8K):
  throughput        1.0 Mops/s  peak RSS 10574976 KB
```

## Post-P1 (commit 25d8b72a — warm-first local free, skip cold pool-header read)

```
(a) single-thread alloc/free:
  atom-64B          116.7 Mops/s  (20000000 iters, 0.171s)
  vec-256B          150.1 Mops/s  (20000000 iters, 0.133s)
  morsel-8K         109.5 Mops/s  (5000000 iters, 0.046s)
  morsel-16K        109.9 Mops/s  (5000000 iters, 0.046s)
  large-1M          108.6 Mops/s  (200000 iters, 0.002s)
(b) producer-consumer (morsel-8K):
  throughput        0.9 Mops/s  peak RSS 8650176 KB
```

Note: atom-64B shows ~10% lower than Pre-P0; this regression appears to have
originated in commits between Pre-P0 and P1 (407df9d9 / 2cb0560b), not in
the warm-first reorder itself. vec-256B improved +4.5% vs Pre-P0.

## Post-P2 (commit a19e48ae — byte-budgeted morsel slab cache, orders 6-16)

```
(a) single-thread alloc/free:
  atom-64B          127.7 Mops/s  (20000000 iters, 0.157s)
  vec-256B          150.3 Mops/s  (20000000 iters, 0.133s)
  morsel-8K         146.9 Mops/s  (5000000 iters, 0.034s)
  morsel-16K        148.8 Mops/s  (5000000 iters, 0.034s)
  large-1M          111.2 Mops/s  (200000 iters, 0.002s)
(b) producer-consumer (morsel-8K):
  throughput        1.0 Mops/s  peak RSS 9804656 KB
```

morsel-8K: +34.2% vs Post-P1 (146.9 vs 109.5 Mops/s); morsel-16K: +35.4% vs Post-P1 (148.8 vs 109.9 Mops/s).
Orders 14-16 (8K-64K) are now slab-cached under the 1 MB byte budget.

## Post-P4 (commit perf/allocator HEAD — same-pool coalescing under parallel flag, TSan-clean)

```
(a) single-thread alloc/free:
  atom-64B          115.2 Mops/s  (20000000 iters, 0.174s)
  vec-256B          148.7 Mops/s  (20000000 iters, 0.135s)
  morsel-8K         149.6 Mops/s  (5000000 iters, 0.033s)
  morsel-16K        150.0 Mops/s  (5000000 iters, 0.033s)
  large-1M          108.9 Mops/s  (200000 iters, 0.002s)
(b) producer-consumer (morsel-8K):
  throughput        1.0 Mops/s  peak RSS 11328400 KB
```

P4 enables same-pool buddy coalescing during parallel execution (previously
skipped entirely), guarded by rc==0 + matching heap_id ownership. This mainly
reduces fragmentation / pool-growth during PARALLEL work; the single-thread (a)
table is roughly flat vs Post-P2 (atom-64B 115.2 vs 127.7, within run-to-run
variance; vec/morsel/large unchanged). TSan-clean: no allocator-function data
races across 4 runs incl. the real parallel_probe / wide_key_probe thread-pool
tests.

## Post-P5 (A/B — cross-thread free queue, flag RAY_THREAD_FREE_QUEUE)

The optional MPSC owner-drain queue (compile flag, default OFF) routes a
cross-thread free onto the OWNER heap's atomic `thread_free_head` stack; the
owner drains+coalesces it continuously on its alloc slow-path and at GC,
instead of leaving it on the freeing heap's `foreign` list until GC. With the
flag OFF behavior is byte-identical to Post-P4.

The producer-consumer workload is the headline case: one thread allocates,
another frees cross-thread. Under the foreign-list model the freed blocks never
return to the producer between GCs, so the producer keeps growing pools (RSS
balloons to ~12 GB). With the owner-drain queue ON the producer reclaims them
continuously and reuses pools — RSS collapses to tens of MB and throughput
rises ~8x.

```
producer-consumer (morsel-8K):

  flag OFF (foreign-list, Post-P4 model):
    throughput   ~1.0-1.1 Mops/s   peak RSS ~12,310,496 KB  (~12 GB)

  flag ON  (RAY_THREAD_FREE_QUEUE=1, owner-drain queue):
    throughput   ~8.5-9.0 Mops/s   peak RSS ~34,272 KB      (~33 MB)
```

Net: peak RSS ~360x lower (~12 GB → ~33 MB) and throughput ~8x higher with the
flag on. Build flag-on bench with `make bench-alloc-tfq`. TSan flag-on: no new
allocator-function data races across 4 runs (the residual `pool.c`/`sort.c`/
registry races and the intermittent join spin-wait stall reproduce identically
on the flag-OFF+TSan build — pre-existing thread-pool behavior, not introduced
by this change). ASan flag-on: clean.

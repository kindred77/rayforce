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

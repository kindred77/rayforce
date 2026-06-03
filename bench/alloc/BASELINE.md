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

## Post-P1 (commit TBD — warm-first local free, skip cold pool-header read)

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

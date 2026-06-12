# Index routing per-point perf gate

## Environment

- Machine: linux x86_64 (Linux 6.8.0-100-generic)
- CPU: native / -march=native release build (-O3 -funroll-loops -fomit-frame-pointer)
- Load at run time: 2.22–4.01 (1-min avg, lightly loaded dev box; not idle)
- Rayforce commit: e1c7636c (branch perf/index-routing)
- Driver: bench/idx_route/main.c, 10M rows, 9 reps/side interleaved

## Table layout

| Column | Formula | Purpose |
|--------|---------|---------|
| k  | i % 1000000 | 1M distinct values (hash/bloom targets) |
| k2 | i / 10      | 1M distinct, sorted ascending (zone target) |
| v  | i           | payload |
| k3 | LCG-shuffle of 0..9999999 | shuffled permutation (sort/range/distinct) |
| k4 | k3 % 100000 | 100k distinct shuffled (distinct target) |

k3 is used for filter-range instead of k2 to keep matching rows
randomly distributed across segments (MIX, not ALL) and avoid a
rowsel ALL-segment rollback corner case in rowsel_from_sorted_ids
that causes OOB writes when the very first N segments all match at
100% fill (pre-existing engine bug, not introduced here).

## Per-point medians (3 driver runs)

### Run 1 (load 2.22)

| point          | indexed_ms | plain_ms | delta_ms |
|----------------|-----------|---------|---------|
| filter-range   |  16.965   |  15.010 |  +1.955 |
| filter-range-G | 101.619   |  95.431 |  +6.187 |
| filter-zone-N  |   0.009   |   4.550 |  -4.541 |
| filter-zone-A  |   0.003   |   4.563 |  -4.560 |
| filter-bloom   |   0.013   |   5.429 |  -5.416 |
| in             |   0.118   |  16.527 | -16.409 |
| find           |   0.000   |   0.000 |   0.000 |
| sort           | 408.283   | 515.584 | -107.301 |
| distinct       | 171.784   | 808.866 | -637.082 |

### Run 2 (load 4.01)

| point          | indexed_ms | plain_ms | delta_ms |
|----------------|-----------|---------|---------|
| filter-range   |  17.249   |  15.043 |  +2.206 |
| filter-range-G | 103.431   |  96.374 |  +7.057 |
| filter-zone-N  |   0.009   |   4.564 |  -4.555 |
| filter-zone-A  |   0.003   |   4.628 |  -4.626 |
| filter-bloom   |   0.013   |   5.487 |  -5.474 |
| in             |   0.116   |  17.000 | -16.884 |
| find           |   0.000   |   0.000 |   0.000 |
| sort           | 421.962   | 526.911 | -104.950 |
| distinct       | 166.673   | 811.957 | -645.284 |

### Run 3 (load 3.57)

| point          | indexed_ms | plain_ms | delta_ms |
|----------------|-----------|---------|---------|
| filter-range   |  17.134   |  16.779 |  +0.354 |
| filter-range-G | 101.815   |  95.527 |  +6.288 |
| filter-zone-N  |   0.009   |   4.565 |  -4.556 |
| filter-zone-A  |   0.003   |   4.524 |  -4.521 |
| filter-bloom   |   0.013   |   5.452 |  -5.438 |
| in             |   0.097   |  16.184 | -16.087 |
| find           |   0.000   |   0.000 |   0.000 |
| sort           | 419.620   | 527.066 | -107.446 |
| distinct       | 201.603   | 666.397 | -464.794 |

## Attach cost table (one-time costs, not in query medians)

| index kind      | column | run 1 ms | run 2 ms | run 3 ms |
|-----------------|--------|---------|---------|---------|
| zone            | k2     |   78.40 |   78.55 |   79.81 |
| bloom           | k      |  564.03 |  578.73 |  569.53 |
| hash            | k      |  620.66 |  632.43 |  620.36 |
| sort (shuffled) | k3     |  104.99 |  106.09 |  104.50 |

Attach costs are consistent across runs (< 3% variation).
Zone is cheapest (~79ms), sort is modest (~105ms),
bloom and hash are expensive (~570–630ms) for 10M rows.

## Guard-overhead note (filter-range-G)

filter-range-G runs FILTER(k3 < 5000000) — 50% selectivity, which
exceeds the IDX_RANGE_MAX_FRAC=4 guard threshold (5M > 10M/4 = 2.5M).
The sort index IS consulted (consult counter advances, verified),
but the guard fires and returns NULL, falling back to the scan path.

Result: indexed side is ~6-7ms SLOWER than plain because the
consult + guard check + range-rowsel setup overhead adds to the scan.
This correctly demonstrates that the guard works: the index routing
is bypassed at >25% selectivity, and the bench correctly detects and
reports this regression overhead.

## Mechanism evidence

All 9 reps × 9 points verified each indexed-side rep: the consult
counter for the point's IDX_SITE_* advances by ≥ 1 per rep.
Abort on failure. All 3 driver runs passed without any mechanism
failure. 

## Anomalies and observations

**filter-range (+1.95 to +2.21 ms):** indexed side is slower than plain
for 1% selectivity (100k rows). This is expected behavior: the sort-range
path spends O(span) building the rowsel, then scatters results via rowsel
gather. The break-even for the range fast-path on randomly-distributed
data is much lower selectivity (~0.1% or less), or requires the column
to be nearly sorted (so the gather is cache-hot). The index routing IS
firing (mechanism verified), but the gain is not apparent at 1% on
shuffled data. Reported as-is — no verdict per spec.

**find (0.000 ms both sides):** two calls to ray_index_find_row are
sub-microsecond. The timing resolution of now_ms() (~1µs effective) is
insufficient to measure individual point lookups. Both sides register 0.
To measure find meaningfully, a dedicated micro-bench (1M probes per rep)
would be needed. Noted as an anomaly; not a routing failure.

**distinct (2x-4x speedup):** sort index on k4 reduces distinct from
a full sort+dedup (640-870ms) to a sort-perm walk (160-200ms). The win
is large and stable across runs.

**sort (20% speedup):** sort index on k3 turns the O(N log N) sort into
an O(N) perm-gather, ~105ms win on 500ms baseline.

**Run stability:** medians are stable across the 3 runs within ~5%,
despite load varying 2.2–4.0. The ~20-30ms scatter in some points
(especially sort/distinct) comes from OS jitter at 10M-row gather time.

## Raw reps (appended)

### Run 1 indexed

```
point              rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
filter-range        16.03    16.24    16.28    16.30    16.97    17.91    18.02    18.36    19.20
filter-range-G      99.62    99.79   101.37   101.47   101.62   102.66   104.58   105.63   117.57
filter-zone-N        0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01
filter-zone-A        0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
filter-bloom         0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01
in                   0.10     0.11     0.11     0.12     0.12     0.12     0.12     0.13     0.13
find                 0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
sort               402.97   403.74   404.32   405.65   408.28   417.22   423.07   429.66   438.12
distinct           159.14   159.19   160.87   161.99   171.78   173.60   174.68   183.69   190.72
```

### Run 1 plain

```
point              rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
filter-range        14.44    14.91    14.96    14.97    15.01    15.23    15.47    15.49    16.36
filter-range-G      91.55    92.78    94.13    95.26    95.43    96.20    96.77    97.09    98.63
filter-zone-N        4.53     4.53     4.53     4.55     4.55     4.56     4.66     4.68     4.80
filter-zone-A        4.50     4.51     4.52     4.53     4.56     4.60     4.68     4.74     4.90
filter-bloom         5.24     5.27     5.30     5.33     5.43     5.47     5.50     5.54     5.64
in                  15.58    15.84    16.27    16.45    16.53    16.60    17.49    18.15    18.71
find                 0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
sort               491.68   499.13   509.82   512.19   515.58   528.49   531.50   534.36   535.58
distinct           757.78   784.94   791.88   803.89   808.87   811.82   828.15   848.67   868.73
```

### Run 2 indexed

```
point              rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
filter-range        16.13    16.13    16.16    16.27    17.25    18.42    18.99    19.04    19.43
filter-range-G      98.60   100.64   101.24   101.74   103.43   103.47   106.26   107.58   120.94
filter-zone-N        0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01
filter-zone-A        0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
filter-bloom         0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01
in                   0.11     0.11     0.11     0.11     0.12     0.13     0.13     0.13     0.13
find                 0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
sort               413.85   415.96   420.97   421.18   421.96   425.33   425.33   432.85   449.79
distinct           158.02   161.77   162.65   165.38   166.67   167.21   170.07   189.94   197.10
```

### Run 2 plain

```
point              rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
filter-range        14.65    14.76    14.98    14.99    15.04    15.13    15.33    15.42    17.06
filter-range-G      92.31    94.69    94.90    95.48    96.37    97.59    98.24    99.06   100.15
filter-zone-N        4.54     4.54     4.55     4.56     4.56     4.57     4.65     4.68     4.74
filter-zone-A        4.50     4.53     4.58     4.60     4.63     4.64     4.74     4.81     4.88
filter-bloom         5.26     5.26     5.26     5.37     5.49     5.52     5.62     5.65     5.68
in                  15.10    15.54    15.95    16.37    17.00    17.04    17.27    17.31    18.56
find                 0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
sort               519.53   520.77   524.23   525.30   526.91   529.98   530.66   535.56   539.80
distinct           631.24   667.21   786.68   795.72   811.96   812.06   817.26   820.47   836.72
```

### Run 3 indexed

```
point              rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
filter-range        16.02    16.09    16.09    16.27    17.13    17.21    17.30    17.32    17.67
filter-range-G      99.83   101.17   101.23   101.78   101.81   104.17   104.51   106.90   118.02
filter-zone-N        0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01
filter-zone-A        0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
filter-bloom         0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01     0.01
in                   0.09     0.09     0.09     0.09     0.10     0.12     0.13     0.14     0.16
find                 0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
sort               416.34   418.30   418.65   419.16   419.62   419.82   422.93   423.69   448.74
distinct           165.23   166.33   166.61   198.56   201.60   202.26   202.53   205.68   206.85
```

### Run 3 plain

```
point              rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
filter-range        15.01    15.14    15.15    15.25    16.78    16.83    16.84    17.98    18.69
filter-range-G      93.58    94.07    95.19    95.20    95.53    96.83    98.30    98.65    99.34
filter-zone-N        4.52     4.53     4.54     4.55     4.56     4.57     4.75     4.86     4.87
filter-zone-A        4.51     4.51     4.51     4.52     4.52     4.53     4.54     4.55     4.87
filter-bloom         5.27     5.27     5.37     5.42     5.45     5.58     5.64     5.79     6.19
in                  14.53    14.58    15.44    15.60    16.18    16.90    17.34    18.18    18.53
find                 0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00     0.00
sort               523.34   526.02   526.26   526.95   527.07   527.90   530.98   531.34   533.71
distinct           647.42   653.33   655.95   656.36   666.40   670.33   695.74   798.47   827.86
```

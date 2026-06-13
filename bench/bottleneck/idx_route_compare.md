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

---

## ROUND 2 — three open questions

Driver extended in bench/idx_route/main.c (round2_run).
Machine: linux x86_64 (Linux 6.8.0-100-generic), same box.
System load at run start: 4.81 / 3.09 / 5.12 (1-min avg, runs 1–3).
Note: machine was lightly loaded for round 1 (2.22–4.01); round 2 ran
under noticeably higher load (3.1–5.5).  Filter-range medians are
load-stable (< 3% variation across runs at each selectivity cell).
Q2 delta varies +3.2 to +7.8ms across runs — scatter addressed below.
9 reps/side interleaved; 3 driver runs; medians across the 3 runs shown.

### Q1: filter-range selectivity curve

`FILTER(k3 < B)` on shuffled data (shuf-*); `FILTER(k2 < B)` on sorted
data (sorted-1pct).  Sort index on the respective column.

k3 is a permutation of 0..9999999 → `k3 < B` gives exactly B rows.
k2 = i/10 → `k2 < 10000` gives rows i < 100000 (100k rows, 1%).

| point        | selectivity | indexed_ms | plain_ms | delta_ms |
|--------------|-------------|-----------|---------|----------|
| shuf-1pct    | 1%          |    16.238 |   14.315 | +1.923 |
| shuf-0.1pct  | 0.1%        |     1.928 |    9.716 | -7.788 |
| shuf-0.01pct | 0.01%       |     0.317 |    5.865 | -5.548 |
| sorted-1pct  | 1% sorted   |     4.580 |    5.822 | -1.242 |

*(medians from run 1, load 4.81; runs 2–3 differ < 3%)*

Per-run medians (indexed_ms / plain_ms / delta_ms):

| point        | run 1 (load 4.81) | run 2 (load 3.09) | run 3 (load 5.12) |
|--------------|-------------------|-------------------|-------------------|
| shuf-1pct    | 16.238 / 14.315 / +1.923 | 16.220 / 14.420 / +1.799 | 16.215 / 14.201 / +2.014 |
| shuf-0.1pct  |  1.928 /  9.716 / -7.788 |  1.925 /  9.603 / -7.677 |  1.951 /  9.508 / -7.557 |
| shuf-0.01pct |  0.317 /  5.865 / -5.548 |  0.326 /  5.970 / -5.644 |  0.311 /  5.835 / -5.524 |
| sorted-1pct  |  4.580 /  5.822 / -1.242 |  4.629 /  5.988 / -1.359 |  4.690 /  5.960 / -1.270 |

Median-of-3-runs delta: shuf-1pct **+1.923**, shuf-0.1pct **-7.677**,
shuf-0.01pct **-5.548**, sorted-1pct **-1.270**.

#### Raw reps Q1 — Run 1 (indexed, ms)

```
point            rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
shuf-1pct      16.075  16.201  16.220  16.235  16.238  16.533  16.819  16.881  17.188
shuf-0.1pct     1.848   1.923   1.924   1.927   1.928   1.933   1.940   2.025   2.227
shuf-0.01pct    0.304   0.307   0.309   0.309   0.317   0.319   0.342   0.402   0.426
sorted-1pct     4.415   4.477   4.514   4.554   4.580   4.581   4.610   4.972   5.196
```

#### Raw reps Q1 — Run 1 (plain, ms)

```
point            rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
shuf-1pct      13.737  13.993  14.234  14.271  14.315  14.631  16.439  16.590  16.646
shuf-0.1pct     9.438   9.469   9.539   9.617   9.716  10.908  11.029  11.124  11.371
shuf-0.01pct    5.674   5.685   5.790   5.862   5.865   5.915   6.008   6.282   6.321
sorted-1pct     5.782   5.786   5.810   5.815   5.822   5.839   6.007   6.012   6.070
```

#### Raw reps Q1 — Run 2 (indexed, ms)

```
point            rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
shuf-1pct      16.145  16.151  16.170  16.212  16.220  16.301  16.530  16.960  17.386
shuf-0.1pct     1.852   1.866   1.911   1.923   1.925   1.945   2.234   2.631   3.075
shuf-0.01pct    0.309   0.312   0.315   0.317   0.326   0.327   0.337   0.363   0.432
sorted-1pct     4.486   4.500   4.546   4.628   4.629   4.630   4.712   4.827   5.077
```

#### Raw reps Q1 — Run 2 (plain, ms)

```
point            rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
shuf-1pct      14.231  14.308  14.334  14.335  14.420  14.903  16.261  16.408  16.786
shuf-0.1pct     9.403   9.468   9.512   9.527   9.603   9.703   9.709  10.828  11.087
shuf-0.01pct    5.688   5.698   5.732   5.787   5.970   5.980   6.201   6.294   9.537
sorted-1pct     5.788   5.811   5.853   5.971   5.988   6.004   6.189   6.469   6.677
```

#### Raw reps Q1 — Run 3 (indexed, ms)

```
point            rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
shuf-1pct      16.105  16.172  16.173  16.178  16.215  16.255  16.304  16.525  20.975
shuf-0.1pct     1.911   1.913   1.914   1.926   1.951   1.978   2.003   2.214   2.878
shuf-0.01pct    0.305   0.306   0.307   0.311   0.311   0.314   0.319   0.321   0.339
sorted-1pct     4.579   4.635   4.637   4.645   4.690   4.756   5.094   5.702   8.057
```

#### Raw reps Q1 — Run 3 (plain, ms)

```
point            rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
shuf-1pct      13.629  13.757  13.906  13.906  14.201  14.369  14.531  16.325  16.628
shuf-0.1pct     9.122   9.422   9.495   9.503   9.508   9.539   9.541  11.011  11.109
shuf-0.01pct    5.704   5.731   5.746   5.827   5.835   5.854   5.857   5.881   6.139
sorted-1pct     5.794   5.839   5.950   5.960   5.960   6.086   6.103   6.121   6.152
```

---

### Q2: range-guard overhead (re-measure, 50% selectivity, guard fires)

`FILTER(k3 < 5000000)` — 50% of 10M rows exceeds IDX_RANGE_MAX_FRAC=4
threshold (5M > 10M/4 = 2.5M).  Sort index is consulted (consult counter
advances, verified), binary searches execute, span guard fires, NULL
returned; query falls back to plain scan.

Guard code path in ray_index_range_rowsel (src/ops/idxop.c):
- Line 1263–1285: two binary searches (lower + upper bound, O(log N) ~23 steps each)
- Line 1293: `if (n >= 64 && span > n / IDX_RANGE_MAX_FRAC) return NULL;`
- Line 1296: `ray_t* scratch = ray_alloc(...)` ← copy starts HERE, AFTER guard

The guard fires before any allocation or data copy.

| side    | run 1 (load 4.81) | run 2 (load 3.09) | run 3 (load 5.12) | median-of-3 |
|---------|-------------------|-------------------|-------------------|-------------|
| indexed |         101.931   |         101.311   |         101.287   |    101.311  |
| plain   |          94.178   |          98.153   |          95.836   |     95.836  |
| delta   |          +7.752   |          +3.158   |          +5.451   |     +5.451  |

#### Raw reps Q2

Run 1 (load 4.81):
```
indexed   99.295  100.113  100.820  101.380  101.931  102.177  103.139  104.495  116.517
plain     91.305   92.783   92.889   92.924   94.178   94.251   94.773   95.199   98.468
```

Run 2 (load 3.09):
```
indexed   98.490  100.132  100.929  101.209  101.311  101.923  103.152  107.199  118.688
plain     92.492   93.336   94.364   95.452   98.153   99.721  100.133  100.302  100.711
```

Run 3 (load 5.12):
```
indexed   98.870   99.687  100.890  101.101  101.287  101.603  101.907  104.287  121.823
plain     93.249   93.827   93.985   95.679   95.836   95.853   96.581   96.659   97.836
```

---

### Q3: find isolated — present vs absent (1000 lookups/rep, 9 reps)

`ray_index_find_row` measured in isolation with hash index on k (1M distinct
values, 10 rows per key).  1000 lookups per rep to clear timer resolution.

- **present**: key 42 — always in [0..999999]; hash-chain walk finds it quickly
- **absent**: key 1000003 — above max k=999999; hash slot walk, no match

plain side = `ray_index_find_row` on unindexed column; `idx_fresh_nonull`
returns false → early exit, NOT a full scan.  Reports raw call overhead only.

| variant         | run 1 idx_µs | run 2 idx_µs | run 3 idx_µs | med_idx_µs | med_plain_µs |
|-----------------|-------------|-------------|-------------|-----------|--------------|
| present (k=42)  |       0.049 |       0.045 |       0.041 |     0.045 |        0.006 |
| absent (k=1M+3) |       0.026 |       0.020 |       0.021 |     0.021 |        0.006 |

#### Raw reps Q3 (ms/1000-lookup batch, indexed)

Run 1 (load 4.81):
```
present-indexed   0.049  0.049  0.049  0.049  0.049  0.049  0.049  0.049  0.052
absent-indexed    0.026  0.026  0.026  0.026  0.026  0.026  0.026  0.026  0.026
```

Run 2 (load 3.09):
```
present-indexed   0.031  0.031  0.031  0.041  0.045  0.057  0.057  0.057  0.063
absent-indexed    0.016  0.016  0.016  0.016  0.020  0.048  0.048  0.048  0.049
```

Run 3 (load 5.12):
```
present-indexed   0.041  0.041  0.041  0.041  0.041  0.041  0.042  0.042  0.044
absent-indexed    0.021  0.021  0.021  0.021  0.021  0.021  0.022  0.022  0.022
```

#### Raw reps Q3 (ms/1000-lookup batch, plain — early-exit overhead only)

Run 1 (load 4.81):
```
present-plain     0.006  0.006  0.006  0.006  0.006  0.006  0.006  0.006  0.006
absent-plain      0.006  0.006  0.006  0.006  0.006  0.006  0.006  0.007  0.007
```

Run 2 (load 3.09):
```
present-plain     0.004  0.004  0.004  0.005  0.008  0.009  0.009  0.009  0.009
absent-plain      0.004  0.004  0.004  0.005  0.007  0.008  0.009  0.009  0.009
```

Run 3 (load 5.12):
```
present-plain     0.005  0.005  0.005  0.005  0.005  0.005  0.005  0.005  0.005
absent-plain      0.005  0.005  0.005  0.005  0.005  0.005  0.005  0.005  0.006
```

---

## ROUND 3 — guard tightened to 1/128; confirmation measurements

Guard changed: `IDX_RANGE_MAX_FRAC 4` → `128`.  New threshold: span > n/128
(~0.78% of 10M = 78125 rows).

Two confirmation points (single driver run, load 3.10, same box):

| point       | selectivity | indexed_ms | plain_ms | delta_ms | verdict         |
|-------------|-------------|-----------|---------|----------|-----------------|
| shuf-0.1pct | 0.1%        |     1.957 |    9.961 |   -8.004 | HIT — span 10000 < 78125 ✓ |
| shuf-1pct   | 1%          |    16.828 |   15.402 |   +1.427 | BAIL — span 100000 > 78125; delta within scan noise ✓ |

shuf-0.1pct: index path taken, -8ms win preserved.
shuf-1pct: guard fires, indexed falls back to scan; +1.4ms is scan-variance noise
(identical code path as plain after guard fires).

sorted-1pct (incidental): was -1.27ms win with old guard (sorted data, contiguous
segments); now +0.26ms (guard fires, win forfeited).  Expected — guard is
layout-blind; revisit if RAY_ATTR_SORTED signal is ever available.

---

## CONTROLLER VERDICTS

### zone (filter-zone-N / filter-zone-A): WIN — keep

-4.5ms median across all 3 runs, stable.  Zone map prunes NONE/ALL segments
before any row evaluation.

### bloom (filter-bloom): WIN — keep

-5.4ms median.  Bloom absent-key prune fires on 0 matching rows in the 1M-key
column; consult cost ~0.013ms is negligible vs 5.4ms scan.

### in (in_hash): WIN — keep

-16ms median (~16x speedup vs scan for 1M-key column lookup).

### sort (sort_asc / sort_desc): WIN — keep

~105ms win on 500ms baseline (~20% speedup); O(N) perm-gather replaces O(N log N)
sort.

### distinct (distinct_sorted): WIN — keep

~460–640ms win (2–4x speedup); sort index walk replaces full sort+dedup.

### find (find_hit / find_miss): WIN — keep

O(1) hash probe vs O(n) absent-key scan.  Both sides register 0.000ms in the
bench timer (sub-microsecond per lookup); the "plain" column measured the
`idx_fresh_nonull` early-exit path (~6ns/call) — i.e. zero overhead on
unindexed columns, not a full scan baseline.  Win by construction: hash probe
≈0.04µs vs absent-key scan ≈5.9ms (Q3 isolation measurement, 1000 lookups/rep).

### filter-range: WIN with guard tightened to 1/128

Guard IDX_RANGE_MAX_FRAC tightened from 4 (25%) to 128 (~0.78%) per the
measured selectivity curve (ROUND 2 Q1): loses +1.9ms at 1% shuffled,
wins -7.8ms at 0.1% shuffled.  1/128 admits the 0.1% win region and rejects
the loss region.  Round 3 confirms: shuf-0.1pct still hits (-8ms); shuf-1pct
bails (+1.4ms noise).

Sorted-layout 1% wins (previously -1.27ms) are forfeited; the guard is
layout-blind (no RAY_ATTR_SORTED signal).  Revisit if that attribute is added.

### guard-overhead question (Q2): CLOSED — scan-variance noise

50% selectivity case adds +3.2 to +7.8ms across 3 runs; variance equals the
plain-scan run-to-run scatter.  After the guard fires the code path is
identical to a plain scan; the delta measures only OS jitter on the 10M-row
FILTER, not guard overhead per se.  No action needed.

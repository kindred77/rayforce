# Join build-side selection perf gate

## Environment

**CPU**: Intel Core i7-6700 @ 3.40GHz (8 logical cores, 4C/8T)  
**RAM**: 62 GiB (24 GiB free at run time)  
**OS**: Linux 6.8.0-100-generic (Ubuntu 24.04)  
**Compiler**: gcc 13.3.0  
**Build flags**: `-O3 -march=native -funroll-loops -fomit-frame-pointer -fno-math-errno -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math -std=c17`  
**Sanitizer-free proof**: `nm bench-join-buildside | grep -ci asan` → **0**  

**System load at run time (all three runs)**:  
- Run 1: load avg 4.81 / 2.60 / 2.45 (1-min / 5-min / 15-min)  
- Run 2: load avg 5.49 / 3.16 / 2.65  
- Run 3: load avg 5.94 / 4.18 / 3.08  

Note: load was elevated above idle throughout. 1-minute load rose from 4.8 → 5.9 across the three runs. This adds noise especially to the WIN case where the absolute times are small (70–85 ms). Results should be interpreted with this in mind.

---

## Case definitions

| case | right | left | right key | left key | swap expected |
|------|-------|------|-----------|----------|---------------|
| WIN | 10,000,000 | 10,000 | `i % 1000000` | `i % 1000000` | YES |
| CONTROL | 10,000,000 | 10,000,000 | `i % 1000000` | `i % 1000000` | NO (equal sizes) |
| MANY-TO-MANY | 10,000,000 | 100,000 | `i % 100000` | `i % 100000` | YES |

`RAY_PARALLEL_THRESHOLD = 65536`. All right tables exceed this threshold so the radix path is taken.

Timing: `CLOCK_MONOTONIC` around `ray_execute` only; tables built once outside the timed loop; graph rebuilt per rep. 9 reps, interleaved swap/legacy per rep.

---

## Per-case medians table

### Run 1 (load 1-min=4.81)

| case | side | median_ms | delta_ms | rows_out |
|------|------|-----------|----------|----------|
| WIN | swap | 78.925 | | 100,000 |
| WIN | legacy | 82.453 | -3.529 | 100,000 |
| CONTROL | swap | 1984.066 | | 100,000,000 |
| CONTROL | legacy | 1990.880 | -6.815 | 100,000,000 |
| MANY-TO-MANY | swap | 155.775 | | 10,000,000 |
| MANY-TO-MANY | legacy | 325.641 | **-169.866** | 10,000,000 |

### Run 2 (load 1-min=5.49)

| case | side | median_ms | delta_ms | rows_out |
|------|------|-----------|----------|----------|
| WIN | swap | 78.570 | | 100,000 |
| WIN | legacy | 71.417 | **+7.153** | 100,000 |
| CONTROL | swap | 2171.522 | | 100,000,000 |
| CONTROL | legacy | 2063.529 | +107.993 | 100,000,000 |
| MANY-TO-MANY | swap | 161.459 | | 10,000,000 |
| MANY-TO-MANY | legacy | 320.383 | **-158.924** | 10,000,000 |

### Run 3 (load 1-min=5.94)

| case | side | median_ms | delta_ms | rows_out |
|------|------|-----------|----------|----------|
| WIN | swap | 76.861 | | 100,000 |
| WIN | legacy | 68.201 | **+8.660** | 100,000 |
| CONTROL | swap | 2197.371 | | 100,000,000 |
| CONTROL | legacy | 2069.016 | +128.356 | 100,000,000 |
| MANY-TO-MANY | swap | 158.962 | | 10,000,000 |
| MANY-TO-MANY | legacy | 343.601 | **-184.639** | 10,000,000 |

*(delta = swap_ms − legacy_ms; negative = swap wins)*

---

## WIN-case delta

Run 1: swap 78.9 ms, legacy 82.5 ms → swap wins by **3.5 ms (~4%)**  
Run 2: swap 78.6 ms, legacy 71.4 ms → **legacy wins by 7.2 ms (~10%)**  
Run 3: swap 76.9 ms, legacy 68.2 ms → **legacy wins by 8.7 ms (~13%)**  

The WIN-case delta is **unstable and reverses sign across runs**. Run 1 showed the expected win; runs 2 and 3 showed legacy faster. The absolute times are in the 68–93 ms range (high noise sensitivity at this load level). The WIN case does not produce a reliable positive result under these conditions.

---

## Near-equal control delta

| run | swap median_ms | legacy median_ms | delta_ms |
|-----|---------------|-----------------|----------|
| 1 | 1984.1 | 1990.9 | -6.8 |
| 2 | 2171.5 | 2063.5 | +108.0 |
| 3 | 2197.4 | 2069.0 | +128.4 |

Control delta is noisy at this load level (±108–128 ms on a ~2000 ms operation = ±5%). The swap counter correctly did NOT advance for this case in all three runs, confirming the mechanism is correct. The large absolute variation is attributable to system load; both sides are doing identical work (no swap fires).

---

## Mechanism counter evidence

Per-run swap counter log:

**Run 1**:
- WIN: before=0, after=9, fired=**YES** (9 reps × 1 swap each)
- CONTROL: before=9, after=9, fired=**NO**
- MANY-TO-MANY: before=9, after=18, fired=**YES**

**Run 2** and **Run 3**: identical pattern (counters reset per process, pattern the same).

`ray_join_build_swaps` increments exactly once per swap per rep. No abort was triggered in any run. The knob (`ray_join_no_build_swap`) correctly prevented swapping on the CONTROL case.

---

## Many-to-many actual fan-out and output size

- Right table: 10,000,000 rows, key `i % 100000` → 100,000 distinct keys, ~100 rows/key  
- Left table: 100,000 rows, key `i % 100000` → 100,000 distinct keys, ~1 row/key  
- Per-key output: 100 right × 1 left = 100 rows  
- Total output: 100,000 keys × 100 = **10,000,000 rows** (confirmed: actual output exactly 10,000,000 in all reps of all 3 runs)

**Many-to-many delta across runs**: swap wins by 158–185 ms (~2.0–2.1× speedup).

| run | swap median_ms | legacy median_ms | speedup |
|-----|---------------|-----------------|---------|
| 1 | 155.8 | 325.6 | 2.09× |
| 2 | 161.5 | 320.4 | 1.98× |
| 3 | 159.0 | 343.6 | 2.16× |

This is the strongest and most stable signal: building the 100K hash (swap) vs the 10M hash (legacy) is consistently ~2× faster.

---

## Stability across 3 runs

| case | swap medians (ms) | legacy medians (ms) | stability |
|------|-------------------|---------------------|-----------|
| WIN | 76.9 – 78.9 | 68.2 – 82.5 | POOR — delta reverses |
| CONTROL | 1984 – 2197 | 1991 – 2069 | POOR — ±10% abs variation; load-driven |
| MANY-TO-MANY swap | 155.8 – 161.5 | 320.4 – 343.6 | GOOD — delta stable 158–185 ms |

---

## Anomalies

1. **WIN case reversal**: In runs 2 and 3, legacy was faster than swap. Likely causes: (a) the timed interval is short (70–85 ms) and system load variation (load avg 4.8–5.9) creates per-rep jitter larger than the expected delta; (b) the 10K build-side hash may not fit cleanly in L3 at this load level vs. the 10M hash's access pattern benefiting from hardware prefetch at steady-state. The 10K case exercises a very different access pattern (10K-bucket HT + 10M probe sweeps) vs. legacy (10M-bucket HT + 10K probe). Under high load the 10M HT approach may have prefetch advantages. This needs lower-load re-measurement.

2. **CONTROL case variation**: absolute times varied 1984–2197 ms across runs (±10%). This is load noise, not a bug. The swap counter correctly stayed at zero.

3. **First rep is always slower** (cold cache): WIN rep1 is ~90 ms vs steady-state ~76 ms; MANY-TO-MANY rep1 is ~226 ms vs ~155 ms. This is expected warm-up; the median of 9 reps absorbs it.

---

## Raw per-rep numbers

### Run 1

```
case              side       rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
WIN               swap       90.337   77.418   79.329   78.586   82.898   78.925   76.803   75.918   80.366
                  legacy     95.237   86.188   83.359   85.808   81.690   82.453   79.599   82.114   82.258
CONTROL           swap      3049.514  2010.508  2013.847  2016.586  1973.166  1968.752  1984.066  1974.845  1975.436
                  legacy    2380.562  2035.458  2014.158  2074.368  1967.412  1985.715  1973.760  1990.880  1971.053
MANY-TO-MANY      swap      226.784  155.775  149.075  148.459  152.110  156.972  159.776  151.737  170.155
                  legacy    320.990  323.409  308.825  316.318  325.823  327.102  331.992  328.973  325.641
```

### Run 2

```
case              side       rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
WIN               swap       92.826   80.433   78.522   81.035   72.911   81.389   79.018   75.440   80.181
                  legacy     82.181   77.179   73.365   68.858   70.239   67.686   67.243   74.222   71.417
CONTROL           swap      2535.336  2155.234  2159.188  2150.322  2186.951  1885.090  1858.770  1906.131  2169.749
                  legacy    1954.351  1898.128  2063.529  2286.899  2140.902  2173.863  2187.598  1919.678  1965.690
MANY-TO-MANY      swap      227.553  161.045  153.593  148.513  146.755  152.025  157.489  147.254  148.327
                  legacy    315.530  319.077  325.443  315.324  337.729  346.070  320.383  319.075  349.046
```

### Run 3

```
case              side       rep1   rep2   rep3   rep4   rep5   rep6   rep7   rep8   rep9
WIN               swap       83.680   75.620   82.262   76.861   80.400   76.344   73.883   78.162   76.573
                  legacy     82.253   72.625   69.432   67.441   67.136   67.669   69.596   67.028   68.201
CONTROL           swap      2593.339  1925.137  1929.210  1901.532  1927.473  2197.371  2348.809  2326.468  2262.956
                  legacy    1918.641  1963.724  1928.151  1919.931  2069.016  2188.931  2339.301  2251.812  2334.737
MANY-TO-MANY      swap      245.060  147.458  175.081  158.200  160.899  165.038  158.962  153.615  157.189
                  legacy    343.601  325.009  348.166  335.863  347.791  341.597  345.932  347.162  338.319
```

---

## Summary for controller

- **MANY-TO-MANY wins cleanly and stably**: ~2× speedup (155–161 ms swap vs 320–343 ms legacy), stable across all 3 runs.  
- **WIN case is inconclusive**: delta reverses sign across runs (−3.5 ms in run 1, +7–9 ms in runs 2–3). System load too high for a reliable sub-10% measurement.  
- **CONTROL mechanism is correct**: swap counter never advanced; no abort; both sides similar within load noise.  
- **Verdict input**: the optimization delivers a clear 2× benefit on the many-to-many case. The WIN case (10K build vs 10M build) requires a quieter box or more reps to confirm the expected gain; under the current load it is not distinguishable from noise.

---

## ROUND 2 — Quiet-box re-measure + duplication-scaling probe

**System load at measurement**: 1-min 1.70 / 5-min 1.87 / 15-min 2.36 (significantly quieter than Round 1: 4.8–5.9).  
**NREPS**: 15 (up from 9). Swap-counter assertions passed on all four cases.

### Case definitions (round 2)

| case | right | left | right key | left key | dup/key (right) | swap expected |
|------|-------|------|-----------|----------|-----------------|---------------|
| WIN | 10,000,000 | 10,000 | `i % 1000000` | `i % 1000000` | 10 | YES |
| HEAVY-DUP-WIN | 10,000,000 | 10,000 | `i % 1000` | `i % 1000` | 10,000 | YES |
| CONTROL | 10,000,000 | 10,000,000 | `i % 1000000` | `i % 1000000` | 10 | NO |
| MANY-TO-MANY | 10,000,000 | 100,000 | `i % 100000` | `i % 100000` | 100 | YES |

### Medians and minimums table

| case | side | median_ms | min_ms | delta_med_ms | delta_min_ms | rows_out |
|------|------|-----------|--------|--------------|--------------|----------|
| WIN | swap | 78.439 | 70.047 | | | 100,000 |
| WIN | legacy | 68.470 | 67.292 | +9.969 | +2.755 | 100,000 |
| HEAVY-DUP-WIN | swap | 1,218.672 | 1,088.182 | | | 100,000,000 |
| HEAVY-DUP-WIN | legacy | 9,835.726 | 9,750.034 | **-8,617.055** | **-8,661.851** | 100,000,000 |
| CONTROL | swap | 1,948.671 | 1,841.534 | | | 100,000,000 |
| CONTROL | legacy | 1,932.271 | 1,867.267 | +16.400 | -25.733 | 100,000,000 |
| MANY-TO-MANY | swap | 163.139 | 150.634 | | | 10,000,000 |
| MANY-TO-MANY | legacy | 327.220 | 310.879 | **-164.081** | **-160.245** | 10,000,000 |

*(delta = swap_ms − legacy_ms; negative = swap wins)*

### WIN case (round 2)

At load 1.70 (vs 4.8–5.9 in round 1), legacy is still faster: swap median 78.4 ms, legacy median 68.5 ms, delta_med = +10.0 ms (legacy wins ~15%). Minimum also goes to legacy: swap min 70.0 ms, legacy min 67.3 ms, delta_min = +2.8 ms. This is a stable result under quiet conditions: with 10 dup/key on the right (10M) side, building the large-side hash is faster despite its size, because the probe loop against the 10K-hash accesses each of 10M right-side rows sequentially while the 10K-hash has high collision density (10K rows distributed across ~10K buckets = chains of average length 1).

### HEAVY-DUP-WIN case (round 2)

With 10,000 dup/key on the right side: swap median 1,218.7 ms, legacy median 9,835.7 ms, delta_med = **-8,617 ms** (swap wins ~8.1×). Minimum: swap 1,088.2 ms, legacy 9,750.0 ms, delta_min = **-8,662 ms**. Output is 100M rows (1,000 keys × 10,000 right/key × 10 left/key). The output fan-out is very large; the 8× gap reflects both hash-build cost (10K vs 10M) and probe-chain traversal: legacy must follow 10,000-row chains in the 10M-bucket hash per output row.

### Duplication-scaling observation

The swap win scales strongly with large-side key duplication: at 10 dup/key (WIN) swap loses (legacy faster by ~10 ms); at 100 dup/key (MANY-TO-MANY) swap wins by ~164 ms (~2×); at 10,000 dup/key (HEAVY-DUP-WIN) swap wins by ~8,617 ms (~8×).

### Raw per-rep numbers (round 2)

```
case              side       rep01   rep02   rep03   rep04   rep05   rep06   rep07   rep08   rep09   rep10   rep11   rep12   rep13   rep14   rep15
WIN               swap       88.049   82.236   79.295   81.535   80.839   70.047   78.439   82.525   73.389   72.177   73.635   79.715   72.920   76.083   74.270
                  legacy     81.540   71.117   69.885   71.010   67.753   68.299   70.257   68.180   69.817   68.470   68.381   68.195   68.258   74.429   67.292
HEAVY-DUP-WIN     swap     1853.694  1218.672  1388.087  1398.513  1088.947  1215.236  1088.182  1116.155  1225.546  1127.843  1111.761  1303.884  1186.649  1632.247  1229.656
                  legacy   9858.120  10233.416  10847.602  9750.034  9865.900  9831.022  9778.619  10957.208  9795.598  9820.259  9789.746  9814.713  10699.711  10324.484  9835.726
CONTROL           swap     1865.826  1841.534  1948.671  1854.722  1942.966  1951.175  1903.590  1962.794  2291.110  2220.735  2177.393  2180.941  1900.926  1982.935  1899.523
                  legacy   1867.267  1895.258  1907.535  1937.255  1927.432  1932.271  1936.678  2057.345  2207.303  2183.425  2183.529  2205.904  1891.102  1893.791  1920.141
MANY-TO-MANY      swap      168.174  150.634  165.381  163.139  190.703  165.451  152.290  164.149  153.455  159.366  222.164  167.555  158.498  156.277  152.885
                  legacy    369.834  310.879  319.009  314.401  314.928  348.581  318.818  311.451  331.450  328.876  361.020  316.547  327.220  328.117  344.521
```

## CONTROLLER VERDICT: KEEP — net win with a small, bounded regression

The size-only swap rule (`INNER && left_rows < right_rows`, radix path) is **kept**.
Rationale, from the measured duplication-scaling curve:

- **Win where it matters:** 2× at 100 rows/key, **8× at 10,000 rows/key** — the
  fact×dimension join shape (many fact rows per dimension key) that dominates
  analytic workloads. The 8× case goes 9.8s → 1.2s.
- **Regression is small and bounded:** near-unique-key joins (~10 rows/key,
  e.g. primary-key joins) lose ~4% best-case / ~15% median — a few ms on a
  ~70ms join. Never catastrophic.
- **No regression on near-equal sizes:** the swap correctly does not fire when
  `left_rows >= right_rows` (CONTROL: counter unchanged, deltas in noise).

**Why size alone can't separate win from loss:** radix partitioning already
makes per-partition hash builds cache-resident, muting the classic
"smaller-hash-fits-cache" benefit. The real win is avoiding an O(n×dup) build
on a heavily-duplicated large side (long open-addressing collision chains).
The *size ratio* doesn't predict duplication — both the 8× win and the 4% loss
occur at the same 1000:1 ratio — and the large side's distinct-key count is not
cheaply known before the join. So a refined predictor (decide-after-partition
on partition skew, or fixing the O(n×dup) build degeneracy) is recorded as
future work; the size heuristic ships as a strong net positive.

Suite green under ASan+UBSan; differential multiset equality holds across all
edge fixtures (m:n, nulls, multi-key, no-match, all-match).

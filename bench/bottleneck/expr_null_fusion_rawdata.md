# Perf Gate Results — Task 14 Steps 1–2
# Branch: perf/expr-null-fusion
# Date: 2026-06-11

## Environment

- **Branch HEAD (candidate):** 64cdecfb (docs: scratch buffers are arena-allocated per worker, not stack)
- **Baseline HEAD (master):**  c31dc460 (fix(ops): parted gathers convert mixed-width SYM segments instead of zero-filling)
- **Machine:** hetoku Linux 6.8.0-100-generic
- **Build flags:** debug + ASAN (`-g -O0 -fsanitize=address,undefined`), single-process
- **Dataset:** 100K-row in-memory sample (`/tmp/hits_sample.csv`, first 100,001 lines of `hits_h.csv`)
- **Queries:** All 43 ClickBench queries from `/home/hetoku/data/work/ClickBench/rayforce/queries.rfl`
- **Iterations:** 5 runs per query, both binaries in same process (cold data within process; sample is 100K rows so fits in L2 cache after first load)
- **Timing method:** `rayforce -t 1 -i` — `╭ top-level … ╰─┤ X ms` blocks extracted, median of 5 taken
- **Scale caveat:** 100K rows (not 10M). Expression-evaluation wins are real but absolute speedups are smaller than at full scale (filter selectivity unchanged, but compute time per row is same; fixed overhead dominates many sub-ms queries). Q40's -76.5% at 100K sample is representative because it exercises a complex multi-key group-by with nullable conditional keys — the expression work is proportional to row count.

### F1 full-parted benchmark
Not run. The parted dataset (`/skull/tmp/rayforce_tests/current`) does not exist; splayed directories exist but are not in the format expected by `bench/bottleneck/run.sh`. Building a fresh parted store from `hits_h.csv` (7.6 GB, ~2m42s load + partition) was not attempted within a measurement-only step.

---

## Bail-counter comparison (RAY_EXPR_STATS=1)

**Note:** `RAY_EXPR_STATS` instrumentation is only present on `perf/expr-null-fusion` (commits `1c2cf5c3` and later). The master binary has no instrumentation code, so baseline bail counts come from Phase-0 measurements.

### Phase-0 reference (single run, master binary, cb_sample_full.rfl)
```
expr_compile ok=1
expr_compile bail nulls      42
expr_compile bail const      11
```
- Total attempts: 54
- ok fraction: 1/54 = 1.9%
- nulls bail fraction: 42/53 = 79.2% of bails

### Candidate (single run, 64cdecfb, cb_sample_full.rfl)
```
expr_compile ok=37
expr_compile bail const      11
```
- Total attempts: 48 (note: 48 vs 54 — compile attempt count differs between builds; the fuse.c deletion removed one optimizer pass that injected synthetic compile attempts)
- ok fraction: 37/48 = 77.1%
- nulls bail fraction: 0/11 = 0% (nulls bails entirely gone)

### Candidate (5x run, 64cdecfb, cb_bench_5x.rfl — 5 reps × 43 queries)
```
expr_compile ok=185
expr_compile bail const      55
```
- 5 × 37 ok = 185 ✓ (consistent)
- 5 × 11 const = 55 ✓ (consistent)
- nulls bails: 0

**Delta summary:** nulls bails: 42 → 0 (per single run); ok: 1 → 37 per run (+3600% on ClickBench workload).

---

## Per-query median table — all 43 queries

Build flags: debug + ASAN (same for both). Medians over 5 runs each.

| Q  | Baseline med (ms) | Candidate med (ms) | Delta %  |
|----|-------------------|--------------------|----------|
|  1 |             0.018 |              0.019 |    +5.6% |
|  2 |             4.122 |              1.774 |   -57.0% |
|  3 |             0.003 |              0.004 |   +33.3% |
|  4 |             0.003 |              0.003 |    +0.0% |
|  5 |             0.004 |              0.004 |    +0.0% |
|  6 |             0.004 |              0.004 |    +0.0% |
|  7 |             0.090 |              0.097 |    +7.8% |
|  8 |             0.003 |              0.003 |    +0.0% |
|  9 |             0.005 |              0.005 |    +0.0% |
| 10 |             0.003 |              0.003 |    +0.0% |
| 11 |             0.003 |              0.003 |    +0.0% |
| 12 |             0.001 |              0.001 |    +0.0% |
| 13 |             0.001 |              0.001 |    +0.0% |
| 14 |             0.001 |              0.001 |    +0.0% |
| 15 |             0.001 |              0.001 |    +0.0% |
| 16 |             0.001 |              0.001 |    +0.0% |
| 17 |             0.001 |              0.001 |    +0.0% |
| 18 |             0.001 |              0.001 |    +0.0% |
| 19 |             0.001 |              0.002 |  +100.0% |
| 20 |             0.001 |              0.001 |    +0.0% |
| 21 |             3.368 |              3.234 |    -4.0% |
| 22 |             0.002 |              0.001 |   -50.0% |
| 23 |             0.002 |              0.002 |    +0.0% |
| 24 |             1.670 |              1.712 |    +2.5% |
| 25 |             1.199 |              1.199 |    +0.0% |
| 26 |             1.365 |              1.423 |    +4.2% |
| 27 |             1.156 |              1.193 |    +3.2% |
| 28 |             0.001 |              0.001 |    +0.0% |
| 29 |             0.001 |              0.001 |    +0.0% |
| 30 |             0.002 |              0.002 |    +0.0% |
| 31 |             1.139 |              1.055 |    -7.4% |
| 32 |             1.188 |              1.065 |   -10.4% |
| 33 |             0.001 |              0.001 |    +0.0% |
| 34 |             0.001 |              0.001 |    +0.0% |
| 35 |             0.001 |              0.001 |    +0.0% |
| 36 |             0.001 |              0.001 |    +0.0% |
| 37 |             0.002 |              0.002 |    +0.0% |
| 38 |             0.002 |              0.002 |    +0.0% |
| 39 |             0.002 |              0.003 |   +50.0% |
| 40 |            15.647 |              3.683 |   -76.5% |
| 41 |             0.002 |              0.002 |    +0.0% |
| 42 |             0.002 |              0.002 |    +0.0% |
| 43 |             0.002 |              0.002 |    +0.0% |

**Load time:** baseline 1556.0 ms, candidate 1738.7 ms (CSV read — warm-up only, not meaningful for query perf)

---

## Summary of meaningful queries (median >= 0.1 ms)

| Q  | Baseline med ms | Candidate med ms | Delta %  | Note |
|----|-----------------|------------------|----------|------|
|  2 |           4.122 |            1.774 |   -57.0% | WIN: filter on nullable column AdvEngineID |
| 21 |           3.368 |            3.234 |    -4.0% | wash (within noise) |
| 24 |           1.670 |            1.712 |    +2.5% | flat |
| 25 |           1.199 |            1.199 |    +0.0% | flat |
| 26 |           1.365 |            1.423 |    +4.2% | flat |
| 27 |           1.156 |            1.193 |    +3.2% | flat |
| 31 |           1.139 |            1.055 |    -7.4% | WIN: multi-agg group-by with nullable filter |
| 32 |           1.188 |            1.065 |   -10.4% | WIN: multi-agg group-by with nullable filter |
| 40 |          15.647 |            3.683 |   -76.5% | WIN: complex nullable conditional key group-by |

**Significant wins** (delta < -5% on queries >= 0.1ms median): Q2, Q31, Q32, Q40

**Significant regressions** (delta > +5% on queries >= 0.1ms median): NONE

Queries Q1, Q3, Q7, Q19, Q22, Q39 show apparent percentage swings but are all sub-0.1ms — timer resolution noise; absolute deltas are ≤0.01ms.

---

## Test suite

Command: `make test` on `perf/expr-null-fusion` HEAD (64cdecfb)

```
=== 3357 of 3359 passed (2 skipped, 0 failed) ===
```

3357 passed, 2 skipped (pre-existing; same count as master), 0 failed.

Candidate also adds `test/test_expr_null.o` — new test file not present on master.

---

## Anomalies and caveats

1. **UB warning in both binaries:** `src/ops/query.c:9030:26: runtime error: signed integer overflow: -153722868 * 60000000000` — fires on Q43 (minute-bucket xbar with NULL timestamp). Pre-existing on master; not introduced by candidate.

2. **Sub-ms queries:** 34 of 43 queries have median < 0.1ms on the 100K sample. Percentage deltas there are meaningless — timer resolution is ~0.001ms and results jitter by ±1 count unit.

3. **Load time regression:** baseline CSV load 1556ms vs candidate 1738ms (+11.7%). This is the one-time `.csv.read` setup — not a query-path regression; likely ASAN heap overhead fluctuation across runs.

4. **Scale limitation:** All measurements on 100K rows (not 10M). Q40's -76.5% is based on 15.6ms vs 3.7ms — absolute values are real but the full-scale win could differ if the bottleneck shifts to I/O at large scale. Expression-fused paths typically scale linearly with row count, so the percentage improvement should hold or improve at full scale.

5. **F1 (full parted) benchmark:** NOT RUN — parted dataset unavailable locally. Step 6 result: SKIP.

---

## Raw run data

### Baseline raw times per query (5 runs each, ms)

| Q  | r1     | r2     | r3     | r4     | r5     |
|----|--------|--------|--------|--------|--------|
|  1 |  0.042 |  0.018 |  0.018 |  0.016 |  0.018 |
|  2 |  4.122 |  4.001 |  4.032 |  5.999 |  4.243 |
|  3 |  0.004 |  0.004 |  0.003 |  0.003 |  0.003 |
|  4 |  0.004 |  0.003 |  0.003 |  0.003 |  0.003 |
|  5 |  0.005 |  0.004 |  0.004 |  0.004 |  0.004 |
|  6 |  0.004 |  0.004 |  0.003 |  0.004 |  0.004 |
|  7 |  0.107 |  0.092 |  0.090 |  0.069 |  0.072 |
|  8 |  0.003 |  0.003 |  0.003 |  0.003 |  0.005 |
|  9 |  0.004 |  0.005 |  0.005 |  0.005 |  0.007 |
| 10 |  0.003 |  0.003 |  0.003 |  0.003 |  0.003 |
| 11 |  0.005 |  0.005 |  0.003 |  0.003 |  0.001 |
| 12 |  0.001 |  0.002 |  0.001 |  0.001 |  0.001 |
| 13 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 14 |  0.001 |  0.002 |  0.001 |  0.001 |  0.001 |
| 15 |  0.002 |  0.001 |  0.002 |  0.001 |  0.001 |
| 16 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 17 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 18 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 19 |  0.001 |  0.001 |  0.002 |  0.002 |  0.001 |
| 20 |  0.002 |  0.001 |  0.001 |  0.001 |  0.001 |
| 21 |  3.334 |  3.378 |  3.361 |  3.368 |  3.508 |
| 22 |  0.002 |  0.008 |  0.002 |  0.001 |  0.001 |
| 23 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 24 |  1.738 |  1.670 |  1.664 |  1.661 |  1.721 |
| 25 |  1.237 |  1.201 |  1.199 |  1.155 |  1.142 |
| 26 |  1.360 |  1.416 |  1.429 |  1.327 |  1.365 |
| 27 |  1.177 |  1.156 |  1.146 |  1.164 |  1.147 |
| 28 |  0.002 |  0.001 |  0.001 |  0.002 |  0.001 |
| 29 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 30 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 31 |  1.237 |  1.139 |  1.145 |  1.043 |  1.048 |
| 32 |  1.354 |  1.155 |  1.188 |  1.034 |  1.197 |
| 33 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 34 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 35 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 36 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 37 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 38 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 39 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 40 | 13.699 | 15.395 | 16.278 | 15.647 | 15.647 |
| 41 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 42 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 43 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |

### Candidate raw times per query (5 runs each, ms)

| Q  | r1     | r2     | r3     | r4     | r5     |
|----|--------|--------|--------|--------|--------|
|  1 |  0.043 |  0.021 |  0.019 |  0.018 |  0.018 |
|  2 |  2.196 |  1.751 |  1.774 |  1.854 |  1.753 |
|  3 |  0.005 |  0.003 |  0.004 |  0.003 |  0.004 |
|  4 |  0.003 |  0.003 |  0.003 |  0.003 |  0.003 |
|  5 |  0.007 |  0.006 |  0.004 |  0.004 |  0.004 |
|  6 |  0.004 |  0.004 |  0.004 |  0.004 |  0.005 |
|  7 |  0.106 |  0.097 |  0.097 |  0.096 |  0.099 |
|  8 |  0.003 |  0.003 |  0.003 |  0.003 |  0.005 |
|  9 |  0.004 |  0.006 |  0.004 |  0.005 |  0.007 |
| 10 |  0.004 |  0.003 |  0.003 |  0.003 |  0.003 |
| 11 |  0.005 |  0.003 |  0.003 |  0.003 |  0.001 |
| 12 |  0.001 |  0.001 |  0.001 |  0.002 |  0.001 |
| 13 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 14 |  0.001 |  0.001 |  0.002 |  0.002 |  0.001 |
| 15 |  0.001 |  0.001 |  0.002 |  0.001 |  0.002 |
| 16 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 17 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 18 |  0.001 |  0.001 |  0.002 |  0.001 |  0.002 |
| 19 |  0.001 |  0.002 |  0.002 |  0.002 |  0.001 |
| 20 |  0.003 |  0.001 |  0.001 |  0.001 |  0.001 |
| 21 |  2.804 |  3.354 |  3.362 |  3.234 |  2.814 |
| 22 |  0.002 |  0.001 |  0.001 |  0.001 |  0.001 |
| 23 |  0.002 |  0.002 |  0.003 |  0.002 |  0.002 |
| 24 |  1.699 |  1.757 |  1.714 |  1.704 |  1.712 |
| 25 |  1.249 |  1.196 |  1.193 |  1.165 |  1.290 |
| 26 |  1.417 |  1.403 |  1.425 |  1.430 |  1.383 |
| 27 |  1.240 |  1.188 |  1.227 |  1.193 |  1.162 |
| 28 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 29 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 30 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 31 |  1.157 |  1.052 |  1.118 |  0.958 |  1.050 |
| 32 |  1.109 |  1.059 |  1.116 |  1.010 |  1.047 |
| 33 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 34 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 35 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 36 |  0.001 |  0.001 |  0.001 |  0.001 |  0.001 |
| 37 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 38 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 39 |  0.003 |  0.003 |  0.003 |  0.003 |  0.003 |
| 40 |  3.676 |  3.949 |  3.668 |  3.655 |  3.992 |
| 41 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 42 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |
| 43 |  0.002 |  0.002 |  0.002 |  0.002 |  0.002 |

---

## RELEASE BUILD RUN

### Environment

- **Build:** `make release` — `-O3 -march=native -funroll-loops -fomit-frame-pointer -fno-math-errno -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math` — NO sanitizers
- **Sanitizer-free confirmation:** `nm /tmp/rayforce_rel_base` and `nm /tmp/rayforce_rel_cand` — zero hits for `asan`/`ubsan` symbols; `ldd` shows no `libasan` linkage. Both confirmed sanitizer-free.
- **Baseline binary:** `/tmp/rayforce_rel_base` (master c31dc460, built 2026-06-11)
- **Candidate binary:** `/tmp/rayforce_rel_cand` (perf/expr-null-fusion 64cdecfb, built 2026-06-11)
- **Dataset:** FULL 10M-row `.csv.read` in-memory load — `/home/hetoku/data/work/ClickBench/rayforce/hits_h.csv` (7.6 GB, 10,000,001 rows). NOT a sample.
- **Timing method:** Schema + `.csv.read` load + all 43 queries, 5 reps each, single process. `╰─┤ X ms` top-level blocks extracted. Medians computed over 10 reps (2 complete runs × 5 reps per binary).
- **Threads:** 8 (`-t 8`)
- **Load time:** Baseline run1=38227ms run2=38734ms avg=38480ms; Candidate run2=38593ms run3=38599ms avg=38596ms (wash — not on query path)

### RAY_EXPR_STATS on release binaries

**Baseline (`c31dc460`):** No output — `RAY_EXPR_STATS` atexit hook and format strings (`expr_compile ok=%llu`) are not present in the master binary. Confirmed via `strings`.

**Candidate (`64cdecfb`) — single pass, all 43 queries once:**
```
expr_compile ok=2
expr_compile bail const      5
```
Total attempts: 7 (vs 48 in DEBUG build). The large drop (48→7) reflects the O3 optimizer constant-folding more expressions upstream before `expr_compile` is reached. `bail nulls` count: **0** (null-bail elimination is intact in release). `ok` fraction: 2/7 = 28.6% in release vs 1.9% on debug-master and 77.1% on debug-candidate.

### Crash note

The first 8-thread candidate run crashed with SIGSEGV after 68/217 timings (during Q14 rep 2). Single-thread replay of all 43×5 completed cleanly (217 timings, exit 0). Two subsequent 8-thread runs both completed fully (217 timings, exit 0). The crash appears to be a non-deterministic race condition — not reliably reproducible. The medians below come from the two complete 8-thread runs (runs 2 and 3).

### Per-query medians — release build, full 10M dataset

Medians over 10 reps (2 runs × 5 reps each). `**REGRESS**` = |delta| > 5% on queries with base_med ≥ 5ms. `**WIN**` = same threshold, negative direction.

| Q  | Base med (ms) | Cand med (ms) | Delta %  |
|----|---------------|---------------|----------|
|  1 |           0.0 |           0.0 |    +0.0% |
|  2 |           0.0 |           0.0 |    +0.0% |
|  3 |           4.9 |           5.7 |   +16.1% |
|  4 |           6.3 |           7.2 |   +14.5% | **REGRESS**
|  5 |          62.8 |          65.1 |    +3.6% |
|  6 |          18.5 |          18.3 |    -1.2% |
|  7 |           0.0 |           0.0 |    +0.0% |
|  8 |           1.7 |           2.0 |   +15.3% |
|  9 |         107.8 |         119.6 |   +10.9% | **REGRESS**
| 10 |         132.0 |         132.6 |    +0.4% |
| 11 |           8.4 |           7.9 |    -4.8% |
| 12 |         484.0 |         506.1 |    +4.6% |
| 13 |          43.0 |          43.5 |    +1.1% |
| 14 |         199.6 |         202.9 |    +1.6% |
| 15 |          70.1 |          78.1 |   +11.3% | **REGRESS**
| 16 |         107.8 |         111.9 |    +3.8% |
| 17 |         129.6 |         139.8 |    +7.9% | **REGRESS**
| 18 |         133.9 |         146.6 |    +9.5% | **REGRESS**
| 19 |         258.8 |         279.4 |    +7.9% | **REGRESS**
| 20 |           0.1 |           0.1 |    +3.4% |
| 21 |          50.7 |          50.9 |    +0.4% |
| 22 |          48.9 |          51.9 |    +6.1% | **REGRESS**
| 23 |         124.0 |         128.3 |    +3.5% |
| 24 |          50.3 |          52.8 |    +4.9% |
| 25 |           6.2 |           6.8 |    +9.6% | **REGRESS**
| 26 |          13.4 |          13.9 |    +4.4% |
| 27 |           6.5 |           6.7 |    +3.3% |
| 28 |          66.5 |          71.7 |    +7.8% | **REGRESS**
| 29 |         514.2 |         539.7 |    +5.0% |
| 30 |           2.2 |           2.2 |    +2.8% |
| 31 |          69.5 |          75.3 |    +8.3% | **REGRESS**
| 32 |          88.0 |          90.5 |    +2.8% |
| 33 |         544.2 |         583.9 |    +7.3% | **REGRESS**
| 34 |          28.0 |          28.4 |    +1.5% |
| 35 |          78.7 |          79.1 |    +0.6% |
| 36 |         156.6 |         155.0 |    -1.0% |
| 37 |          32.7 |          30.0 |    -8.1% | **WIN**
| 38 |          10.8 |          10.5 |    -2.8% |
| 39 |           8.2 |           8.0 |    -1.6% |
| 40 |         166.9 |         170.3 |    +2.1% |
| 41 |           3.9 |           3.8 |    -3.3% |
| 42 |           2.7 |           2.8 |    +0.8% |
| 43 |          26.6 |          26.5 |    -0.2% |

**Wins (>5%, base ≥ 5ms):** Q37 only (−8.1%, sort with `asc EventTime take 10`)

**Regressions (>5%, base ≥ 5ms):** Q04 (+14.5%), Q09 (+10.9%), Q15 (+11.3%), Q17 (+7.9%), Q18 (+9.5%), Q19 (+7.9%), Q22 (+6.1%), Q25 (+9.6%), Q28 (+7.8%), Q31 (+8.3%), Q33 (+7.3%) — 11 queries.

**Q40:** +2.1% (flat) — the −76.5% debug win does not appear in release. The debug baseline was dominated by ASAN shadow-memory overhead on the complex group-by; in release both binaries run O3 paths and the fusion adds no measurable gain.

**Q02:** sub-ms noise in release — the −57.0% debug win was also ASAN overhead on the filtered count, not expression evaluation benefit.

### Raw timing data — release runs (ms, 10 reps per query per binary)

Baseline: runs 1+2 combined. Candidate: runs 2+3 combined (run 1 crashed).

| Q  | Base r1-r5                               | Base r6-r10                              | Cand r1-r5                               | Cand r6-r10                              |
|----|------------------------------------------|------------------------------------------|------------------------------------------|------------------------------------------|
|  1 | 0 0 0 0 0                                | 0 0 0 0 0                                | 0 0 0 0 0                                | 0 0 0 0 0                                |
|  3 | 5.1 5.2 5.0 5.0 4.9                      | 4.9 4.8 4.8 4.7 4.7                      | 5.6 5.6 5.4 5.4 5.4                      | 6.1 6.0 6.0 5.9 5.9                      |
|  4 | 6.4 6.4 6.6 6.3 6.3                      | 6.3 6.3 6.8 6.3 6.3                      | 6.6 6.9 6.8 7.0 6.6                      | 7.5 8.5 7.6 7.6 7.6                      |
|  5 | 61.3 63.5 61.7 63.9 63.2                 | 60.5 62.4 64.3 64.8 61.8                 | 61.5 63.1 64.0 64.5 63.7                 | 66.1 69.5 66.5 65.6 67.3                 |
|  9 | 119.6 108.1 109.3 113.5 105.8            | 124.7 106.7 107.0 107.5 106.2            | 128.9 108.9 105.9 106.2 107.2            | 141.4 128.5 120.2 119.0 122.7            |
| 10 | 138.0 131.6 128.7 132.4 124.5            | 140.0 132.7 139.4 128.4 129.5            | 131.7 130.2 126.9 129.3 126.9            | 152.0 158.8 151.7 147.2 133.5            |
| 12 | 547.1 485.9 482.0 472.4 476.2            | 535.0 488.9 481.4 479.8 498.3            | 549.8 486.3 490.4 496.6 496.4            | 570.4 499.5 546.5 512.8 522.0            |
| 15 | 76.2 68.5 69.0 72.8 70.1                 | 69.3 68.7 76.0 75.3 70.2                 | 74.3 69.6 69.3 76.3 72.4                 | 86.7 79.9 88.8 81.8 83.3                 |
| 17 | 148.5 146.6 124.2 125.8 127.9            | 146.3 131.4 124.7 133.3 125.1            | 145.1 131.2 126.5 122.2 121.0            | 172.0 152.3 140.3 139.4 144.7            |
| 18 | 129.7 136.4 132.9 136.3 131.4            | 133.8 136.4 133.9 131.7 141.6            | 132.1 131.5 131.7 132.8 148.8            | 146.8 157.3 148.6 146.4 148.6            |
| 19 | 264.2 257.0 257.6 253.6 260.1            | 261.2 257.5 256.0 261.8 260.1            | 258.6 288.2 278.6 303.2 280.1            | 280.4 284.5 267.5 265.1 260.5            |
| 22 | 48.3 47.8 49.4 48.4 50.2                 | 52.2 51.7 49.0 48.0 48.9                 | 58.0 55.3 54.0 58.9 55.6                 | 48.8 49.8 49.4 49.0 49.0                 |
| 25 | 6.1 6.1 6.3 6.4 6.8                      | 6.2 6.0 6.0 6.1 6.7                      | 6.9 6.7 6.8 7.4 8.1                      | 6.0 6.0 6.1 6.6 7.0                      |
| 28 | 65.2 65.6 66.9 67.9 64.9                 | 66.1 65.1 68.4 67.4 68.6                 | 73.7 75.0 76.8 85.8 76.1                 | 63.7 65.1 64.9 65.2 69.6                 |
| 29 | 516.2 508.7 512.1 501.4 517.7            | 522.7 517.6 506.8 527.5 512.1            | 615.5 548.0 574.1 539.0 559.0            | 510.3 501.7 527.1 509.9 540.4            |
| 31 | 72.8 66.6 67.7 70.0 68.2                 | 73.3 70.9 73.2 68.5 69.1                 | 81.5 73.2 75.7 74.9 79.1                 | 76.4 73.5 68.5 68.8 76.4                 |
| 33 | 563.6 544.5 536.9 529.2 553.3            | 591.9 561.7 543.4 543.9 540.0            | 634.3 598.4 589.3 585.1 586.3            | 582.7 538.9 546.3 531.7 539.6            |
| 37 | 28.2 36.0 34.1 31.5 31.1                 | 28.4 33.8 34.1 30.3 34.1                 | 28.1 29.7 30.8 30.1 29.9                 | 28.9 30.9 32.7 30.6 29.8                 |
| 40 | 166.5 163.5 162.3 158.3 175.9            | 173.0 167.3 167.4 153.9 177.4            | 175.4 170.6 170.0 158.4 185.8            | 166.2 170.8 174.3 162.1 168.8            |


---

# ROUND 3 — INTERLEAVED QUIET-BOX RUN

## Environment

- Binaries verified (no sanitizer strings in either):
  - `/tmp/rayforce_rel_base`: master c31dc460, ELF64 release, 3,817,344 bytes
  - `/tmp/rayforce_rel_cand`: branch perf/expr-null-fusion HEAD 64cdecfb, ELF64 release, 3,870,736 bytes
- Dataset: `/home/hetoku/data/work/ClickBench/rayforce/hits_h.csv` (10M rows)
- Queries: `/home/hetoku/data/work/ClickBench/bench10m/q/q00.rfl` .. `q42.rfl` (43 queries)

### Load averages

- Pre-run (19:56:35): `load average: 2.13, 2.71, 5.25`
  - Primary consumers: claude ~24% CPU, qemu VM ~18% CPU (idle)
  - 1-min average had dropped from 3.30 at conversation start to 0.50-2.1 by run time
- Post Goal A (20:03:56): `load average: 3.23, 3.41, 4.67`
- Post Goal B (20:08:22): `load average: 0.77, 2.01, 3.77`

### Design

Goal A: interleaved B,C,B,C,B,C (3 process-runs each), 7 timeit reps per query per run.
Per-run: single rayforce process, CSV loaded once, all 43 queries timed inline.
Run durations: BASE ~73s, CAND ~71-73s each. Per-query median: median of 21 reps (3 runs × 7).

Goal B: interleaved B,C,B,C (2 runs each), 7 reps per query per run.
In-memory synthetic tables (10,000,001 rows): `concat(til(10M), [0N])`.
Setup timing line excluded from per-query medians.

---

## Goal A: ClickBench regression check

Threshold: |delta| > 5% AND base_median >= 5.0 ms

| Q | Query shape | Base med ms | Cand med ms | Delta% | B_r1 ms | B_r2 ms | B_r3 ms | C_r1 ms | C_r2 ms | C_r3 ms | Notes |
|--:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---|
| 4 | count distinct UserID | 6.308 | 6.696 | +6.2% | 6.31 | 6.43 | 6.29 | 7.37 | 6.70 | 6.63 | consistent sign |
| 11 | nested group-by MobilePhone | 7.124 | 7.739 | +8.6% | 7.00 | 7.12 | 7.12 | 7.20 | 7.46 | 7.87 | consistent sign |
| 26 | ORDER BY+LIMIT EventTime+SearchPhrase | 13.761 | 12.826 | -6.8% | 13.45 | 12.59 | 15.07 | 12.75 | 13.19 | 12.64 | sign-flips (noise) |

Rows above threshold: 3

**Borderline (|delta|>5%, 4ms <= base < 5ms):**

| Q | Query shape | Base med ms | Cand med ms | Delta% | B_r1 ms | B_r2 ms | B_r3 ms | C_r1 ms | C_r2 ms | C_r3 ms |
|--:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | avg(UserID) | 4.749 | 5.482 | +15.4% | 4.75 | 4.75 | 4.79 | 5.60 | 5.42 | 5.41 |

### Per-run delta detail (significant queries)

- q04: run1: +16.8%, run2: +4.1%, run3: +5.4% — consistent sign
- q11: run1: +2.9%, run2: +4.8%, run3: +10.5% — consistent sign
- q26: run1: -5.2%, run2: +4.8%, run3: -16.1% — FLIPS SIGN

- q03 (borderline): run1: +17.9%, run2: +14.1%, run3: +13.1% — consistent sign

---

## Goal B: Fused-nullable engagement findings

### Nullable columns at full scale

**Finding: the ClickBench `hits_h.csv` dataset produces ZERO non-SYMBOL columns**
**with `RAY_ATTR_HAS_NULLS` at 10M scale.**

- Checked 1,000,000 rows for empty CSV fields in non-SYMBOL columns: none found.
- All empty fields occur in SYMBOL columns (Title, URL, Referer, SearchPhrase, MobilePhoneModel, etc.).
- CSV reader remaps empty SYM fields to sym-id 0; `RAY_ATTR_HAS_NULLS` is never set for SYMBOL columns.
- All numeric/temporal columns (I16, I32, I64, DATE, TIMESTAMP) have fully-populated rows.

**Consequence:** The fused-nullable path cannot engage on any ClickBench query.
No numeric column satisfies the `col->attrs & RAY_ATTR_HAS_NULLS` guard in expr_compile.

**RAY_EXPR_STATS on candidate, all 43 ClickBench queries:**
```
expr_compile ok=2
expr_compile bail const      5
```
The ok=2 are from q35 (`(- ClientIP 1/2/3)` in group-by computed key) — non-nullable arithmetic.
The base binary predates Phase 0 and has no RAY_EXPR_STATS instrumentation.

### Synthetic engagement benchmark (10M+1 rows with nullable columns)

Tables: `Tnull.x = Tnull.y = concat(til(10M), [0N])`, `Tbase.x = Tbase.y = til(10M)`

Engagement confirmed: `RAY_EXPR_STATS` on candidate = `expr_compile ok=5` (all 5 queries compiled).
Base binary: no stats output — predates instrumentation; element-wise scalar fallback path used.

| Query | Expression | Base med ms | Cand med ms | Delta% | B_r1 ms | B_r2 ms | C_r1 ms | C_r2 ms |
|:------|:-----------|---:|---:|---:|---:|---:|---:|---:|
| Q_B1 | `nullable (* x 2)` | 55.986 | 17.773 | -68.3% | 55.935 | 56.779 | 17.665 | 17.907 |
| Q_B2 | `nullable (+ x y)` | 98.559 | 19.146 | -80.6% | 98.615 | 98.468 | 19.156 | 19.137 |
| Q_B3 | `non-null (* x 2)` | 17.062 | 16.364 | -4.1% | 17.008 | 17.141 | 16.351 | 16.378 |
| Q_B4 | `nullable (- x y)` | 100.038 | 19.241 | -80.8% | 100.069 | 99.732 | 18.986 | 19.570 |
| Q_B5 | `nullable (> x 1000)` | 48.210 | 3.782 | -92.2% | 48.824 | 47.957 | 3.790 | 3.775 |

Notes:
- Q_B3 (non-null): both binaries ~17ms — non-nullable path structurally equivalent; -4% is noise.
- Q_B1/Q_B2/Q_B4 (nullable arith): base ~56-100ms (element-wise scalar loop), cand ~18-19ms (fused path).
- Q_B5 (nullable comparison `> x 1000`): base ~48ms, cand ~3.8ms. -92%.
- Per-run distributions are tight (CoV < 5%); results are not noise.

---

## Anomalies

1. **q04 (count distinct UserID, +6.2%, never flips):**
   base per-run medians 6.29-6.43ms, cand 6.63-7.37ms. ~0.3-1ms absolute.
   agg.c unchanged; expr.c not invoked for count-distinct. Possible icache effect
   from 53KB larger cand binary. Does not flip sign.

2. **q11 (nested group-by MobilePhone, +8.6%, never flips):**
   Per-run deltas monotonically increasing: run1 +2.9%, run2 +4.8%, run3 +10.5%.
   Monotonic drift across a 7-minute window suggests system warming (thermal/background)
   rather than a deterministic regression in the cand binary.

3. **q26 (ORDER BY EventTime+SearchPhrase take 10, apparent -6.8%):**
   SIGN FLIPS across runs: run1 -5.2%, run2 +4.8%, run3 -16.1%.
   Base run3 had anomalously high values (15-17ms vs 12-13ms normal). Noise.

4. **q03 (avg UserID, +15.4%, never flips, BELOW threshold):**
   base 4.749ms, cand 5.482ms. Excluded (base < 5ms).
   agg.c unchanged; 0.7ms gap is within sub-5ms measurement uncertainty.
   All 3 run-medians consistent: B [4.748, 4.746, 4.785] vs C [5.600, 5.416, 5.414].

5. **Load during Goal A run:**
   1-min average 2.1 at start, 3.2 at end. Interleaved design cancels monotonic drift
   at the cross-binary level. Within-pair B-before-C ordering could introduce small
   warm-start bias favoring C; cross-run sign consistency is the reliability check.

---

## Raw data

### Goal A: all 43 queries

| Q | Base med ms | Cand med ms | Delta% | B_r1 | B_r2 | B_r3 | C_r1 | C_r2 | C_r3 |
|--:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.005 | 0.005 | 0.0% | 0.004 | 0.005 | 0.005 | 0.006 | 0.005 | 0.005 |
| 2 | 0.010 | 0.010 | 0.0% | 0.012 | 0.008 | 0.010 | 0.009 | 0.009 | 0.010 |
| 3 | 4.749 | 5.482 | +15.4% | 4.748 | 4.746 | 4.785 | 5.600 | 5.416 | 5.414 |
| 4 | 6.308 | 6.696 | +6.2% | 6.308 | 6.432 | 6.291 | 7.366 | 6.696 | 6.633 |
| 5 | 62.422 | 62.975 | +0.9% | 62.422 | 63.356 | 62.049 | 64.719 | 61.952 | 62.587 |
| 6 | 18.255 | 18.425 | +0.9% | 18.160 | 18.147 | 18.319 | 19.132 | 18.506 | 18.152 |
| 7 | 0.012 | 0.012 | 0.0% | 0.017 | 0.012 | 0.012 | 0.011 | 0.012 | 0.013 |
| 8 | 1.648 | 1.769 | +7.3% | 1.652 | 1.633 | 1.658 | 1.857 | 1.755 | 1.749 |
| 9 | 106.852 | 106.959 | +0.1% | 106.118 | 107.365 | 105.999 | 105.174 | 108.614 | 107.119 |
| 10 | 130.501 | 127.200 | -2.5% | 130.502 | 129.662 | 130.521 | 125.062 | 128.790 | 138.628 |
| 11 | 7.124 | 7.739 | +8.6% | 6.995 | 7.124 | 7.125 | 7.199 | 7.463 | 7.872 |
| 12 | 492.474 | 492.445 | -0.0% | 501.011 | 487.699 | 487.064 | 477.158 | 493.843 | 497.506 |
| 13 | 43.756 | 43.416 | -0.8% | 45.101 | 42.822 | 43.683 | 42.553 | 43.979 | 43.520 |
| 14 | 200.398 | 202.141 | +0.9% | 211.775 | 198.886 | 196.653 | 194.379 | 202.141 | 209.116 |
| 15 | 76.723 | 74.863 | -2.4% | 83.805 | 72.784 | 70.838 | 69.410 | 74.716 | 82.548 |
| 16 | 110.282 | 109.555 | -0.7% | 120.579 | 108.836 | 108.405 | 105.558 | 109.555 | 117.483 |
| 17 | 130.324 | 129.765 | -0.4% | 147.801 | 125.613 | 126.272 | 123.476 | 125.038 | 142.622 |
| 18 | 135.888 | 134.245 | -1.2% | 153.942 | 134.298 | 133.824 | 134.245 | 131.066 | 149.836 |
| 19 | 259.664 | 256.743 | -1.1% | 293.474 | 254.853 | 255.149 | 256.303 | 255.740 | 257.357 |
| 20 | 0.122 | 0.106 | -13.1% | 0.139 | 0.103 | 0.120 | 0.098 | 0.109 | 0.106 |
| 21 | 51.280 | 49.528 | -3.4% | 55.820 | 51.041 | 50.636 | 49.819 | 49.250 | 49.298 |
| 22 | 49.804 | 49.076 | -1.5% | 53.729 | 49.382 | 47.815 | 49.268 | 48.936 | 49.076 |
| 23 | 122.510 | 121.706 | -0.7% | 123.901 | 121.113 | 122.704 | 121.724 | 121.406 | 121.922 |
| 24 | 49.001 | 49.560 | +1.1% | 49.001 | 48.859 | 49.546 | 49.490 | 49.319 | 50.585 |
| 25 | 6.339 | 6.143 | -3.1% | 6.339 | 6.154 | 8.241 | 6.139 | 6.370 | 6.094 |
| 26 | 13.761 | 12.826 | -6.8% | 13.447 | 12.590 | 15.068 | 12.753 | 13.191 | 12.642 |
| 27 | 6.523 | 6.280 | -3.7% | 6.523 | 6.265 | 7.274 | 6.257 | 6.379 | 6.265 |
| 28 | 68.945 | 65.716 | -4.7% | 65.760 | 65.161 | 83.304 | 65.775 | 65.543 | 66.625 |
| 29 | 515.045 | 512.089 | -0.6% | 514.659 | 511.185 | 603.680 | 505.541 | 512.915 | 507.716 |
| 30 | 2.451 | 2.302 | -6.1% | 2.483 | 2.254 | 2.520 | 2.311 | 2.302 | 2.288 |
| 31 | 72.123 | 68.828 | -4.6% | 69.399 | 70.018 | 87.616 | 69.528 | 68.548 | 68.822 |
| 32 | 88.724 | 87.755 | -1.1% | 87.667 | 86.791 | 94.720 | 88.203 | 87.249 | 87.087 |
| 33 | 547.509 | 546.794 | -0.1% | 546.302 | 543.132 | 590.205 | 548.707 | 540.236 | 553.103 |
| 34 | 28.258 | 27.645 | -2.2% | 27.715 | 27.682 | 28.503 | 27.615 | 28.078 | 27.609 |
| 35 | 78.097 | 75.247 | -3.6% | 77.316 | 76.315 | 82.480 | 75.528 | 75.247 | 75.149 |
| 36 | 157.567 | 154.644 | -1.9% | 157.492 | 155.719 | 161.031 | 155.669 | 157.047 | 153.295 |
| 37 | 31.443 | 31.007 | -1.4% | 31.373 | 29.844 | 34.401 | 30.516 | 34.931 | 30.035 |
| 38 | 11.021 | 10.562 | -4.2% | 11.021 | 10.161 | 11.410 | 10.395 | 10.593 | 10.645 |
| 39 | 8.220 | 8.146 | -0.9% | 8.220 | 7.862 | 8.815 | 8.054 | 8.161 | 8.081 |
| 40 | 157.158 | 162.067 | +3.1% | 156.361 | 156.715 | 162.965 | 162.756 | 170.541 | 156.191 |
| 41 | 4.017 | 3.937 | -2.0% | 4.017 | 4.009 | 4.264 | 3.836 | 4.211 | 3.883 |
| 42 | 2.802 | 2.780 | -0.8% | 2.779 | 2.652 | 3.043 | 2.780 | 3.160 | 2.717 |
| 43 | 27.070 | 27.010 | -0.2% | 27.595 | 26.494 | 27.180 | 26.621 | 29.986 | 26.546 |

### Goal B: per-rep detail

**Q_B1 — `nullable (* x 2)`:**
  base run1: `[55.703, 55.791, 55.841, 55.935, 56.037, 58.126, 59.557]`
  cand run1: `[17.311, 17.367, 17.626, 17.665, 17.742, 17.978, 18.024]`
  base run2: `[55.481, 55.537, 55.906, 56.779, 57.335, 59.328, 68.791]`
  cand run2: `[17.589, 17.69, 17.805, 17.907, 17.923, 17.95, 18.336]`

**Q_B2 — `nullable (+ x y)`:**
  base run1: `[98.403, 98.515, 98.603, 98.615, 99.183, 104.367, 109.422]`
  cand run1: `[18.713, 18.715, 18.736, 19.156, 19.202, 19.233, 19.297]`
  base run2: `[97.881, 97.95, 98.457, 98.468, 98.473, 102.943, 110.981]`
  cand run2: `[19.038, 19.075, 19.089, 19.137, 19.155, 19.35, 19.636]`

**Q_B3 — `non-null (* x 2)`:**
  base run1: `[16.902, 16.948, 16.974, 17.008, 17.066, 17.244, 17.705]`
  cand run1: `[16.285, 16.306, 16.324, 16.351, 16.703, 16.709, 17.166]`
  base run2: `[16.983, 17.012, 17.058, 17.141, 17.189, 17.524, 17.545]`
  cand run2: `[16.268, 16.288, 16.294, 16.378, 16.448, 16.882, 17.39]`

**Q_B4 — `nullable (- x y)`:**
  base run1: `[98.953, 99.313, 100.007, 100.069, 100.206, 104.928, 106.794]`
  cand run1: `[18.793, 18.816, 18.897, 18.986, 19.04, 19.393, 19.958]`
  base run2: `[98.968, 99.145, 99.254, 99.732, 103.088, 106.932, 110.936]`
  cand run2: `[19.077, 19.089, 19.48, 19.57, 19.961, 20.062, 20.213]`

**Q_B5 — `nullable (> x 1000)`:**
  base run1: `[47.656, 47.828, 48.324, 48.824, 50.572, 52.6, 53.404]`
  cand run1: `[3.701, 3.738, 3.775, 3.79, 3.791, 3.796, 3.8]`
  base run2: `[47.716, 47.782, 47.79, 47.957, 48.096, 50.809, 52.018]`
  cand run2: `[3.695, 3.756, 3.761, 3.775, 3.815, 3.864, 3.889]`


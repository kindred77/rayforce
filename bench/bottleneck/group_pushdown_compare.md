# Group Predicate Pushdown — Perf Gate

## Redesign Note

The original plan assumed a language-layer FILTER(GROUP) shape. The language layer
evaluates nested selects eagerly, so the pushable shape exists only when the DAG is
built via the C graph API directly.  `query-layer where+by` is already served by
`OP_FILTERED_GROUP` / lazy selection, a separate fast path.  This driver constructs
the FILTER(GROUP) DAG via the C API, mirrors the optimizer arm's rewrite, and
measures the executor benefit in-process with the `ray_opt_no_group_pushdown` knob.

## Environment

- CPU: Intel(R) Core(TM) i7-6700 @ 3.40GHz (x86_64)
- OS: Linux 6.8.0-100-generic, up 37 days
- Load at bench time: 1-min load 2.08 (was 2.82 at build start; multiple runs taken)
- Build flags: `-O3 -march=native -funroll-loops -fomit-frame-pointer -fno-math-errno -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math -std=c17`
- Sanitizer-free: `nm bench-group-pushdown | grep -ci asan` → **0**

## Query Shapes

| Shape | Predicate | Expected groups |
|-------|-----------|----------------|
| WIN | `k < 10000` (1% selectivity) | 10,000 |
| CONTROL | `k >= 0` (pass-all) | 1,000,000 |

Table: 10M rows, `k = i % 1000000` (1M distinct keys, 10 rows each), `v = i`.
Query: `FILTER(pred, GROUP(sum v by k))` → after push: `GROUP(FILTER(pred, table))`.

## Medians (9 reps each, exec and optimize timed separately)

| shape | side | exec_med_ms | opt_med_ms | rows |
|-------|------|-------------|-----------|------|
| WIN (k<10000) | pushed | 42.8 | 0.009 | 10,000 |
| WIN (k<10000) | unpushed | 992.0 | 0.004 | 10,000 |
| CONTROL (k>=0) | pushed | 1014.9 | 0.009 | 1,000,000 |
| CONTROL (k>=0) | unpushed | 974.5 | 0.004 | 1,000,000 |

WIN speedup (pushed / unpushed exec): **~23x** (992 / 42.8).
CONTROL pushed vs unpushed: within noise (~4%), confirming the overhead is only the
filter evaluation cost, which is negligible vs the full-hash-build cost saved by the
selectivity reduction.

## Mechanism Evidence

Each rep: after `ray_optimize`, WIN+pushed shape is asserted to be
`OP_GROUP(OP_FILTER(...))` (root opcode and `inputs[0]->opcode` checked; aborts on
failure).  All 27 WIN+pushed executions across 3 runs passed the assertion silently.

## Run-to-Run Stability (3 independent runs)

| run | WIN pushed med ms | WIN unpushed med ms |
|-----|-------------------|---------------------|
| 1 | 42.8 | 992.0 |
| 2 | 41.9 | 969.4 |
| 3 | 40.6 | 987.0 |

Spread: WIN pushed ±5.3%, WIN unpushed ±2.3%.  Stable across the three runs.

Note: system load was 2.08–2.82 during measurement (not an idle box).  The pushed
side shows more variance because 40ms is small relative to OS scheduling jitter; the
absolute gap (~950ms) is large enough that the sign cannot flip.

## Raw Per-Rep Numbers

### Run 1 (exec_ms)

| shape | side | rep1 | rep2 | rep3 | rep4 | rep5 | rep6 | rep7 | rep8 | rep9 |
|-------|------|------|------|------|------|------|------|------|------|------|
| WIN (k<10000) | pushed | 39.6 | 40.7 | 41.3 | 41.3 | 42.8 | 44.6 | 46.1 | 46.4 | 47.8 |
| WIN (k<10000) | unpushed | 942.7 | 943.0 | 950.6 | 969.5 | 992.0 | 1001.6 | 1016.7 | 1025.0 | 1049.6 |
| CONTROL (k>=0) | pushed | 1001.1 | 1001.1 | 1004.1 | 1010.1 | 1014.9 | 1018.1 | 1033.7 | 1122.4 | 1176.5 |
| CONTROL (k>=0) | unpushed | 952.9 | 955.4 | 960.4 | 962.9 | 974.5 | 993.3 | 1009.4 | 1026.6 | 1032.5 |

### Run 2 (exec_ms)

| shape | side | rep1 | rep2 | rep3 | rep4 | rep5 | rep6 | rep7 | rep8 | rep9 |
|-------|------|------|------|------|------|------|------|------|------|------|
| WIN (k<10000) | pushed | 39.9 | 40.0 | 40.0 | 41.4 | 41.9 | 42.3 | 46.0 | 49.8 | 52.3 |
| WIN (k<10000) | unpushed | 949.1 | 949.4 | 953.4 | 957.8 | 969.4 | 994.1 | 1000.8 | 1024.9 | 1037.9 |
| CONTROL (k>=0) | pushed | 945.1 | 957.0 | 960.6 | 964.5 | 971.3 | 973.6 | 1006.1 | 1042.4 | 1047.8 |
| CONTROL (k>=0) | unpushed | 952.3 | 952.4 | 955.6 | 963.5 | 965.7 | 976.4 | 995.4 | 1051.3 | 1073.8 |

### Run 3 (exec_ms)

| shape | side | rep1 | rep2 | rep3 | rep4 | rep5 | rep6 | rep7 | rep8 | rep9 |
|-------|------|------|------|------|------|------|------|------|------|------|
| WIN (k<10000) | pushed | 39.1 | 39.4 | 39.7 | 40.1 | 40.6 | 40.7 | 41.6 | 43.0 | 44.7 |
| WIN (k<10000) | unpushed | 973.9 | 979.8 | 982.8 | 986.1 | 987.0 | 988.6 | 1006.4 | 1020.6 | 1025.5 |
| CONTROL (k>=0) | pushed | 942.9 | 953.2 | 956.3 | 959.3 | 959.5 | 960.1 | 963.4 | 965.0 | 971.4 |
| CONTROL (k>=0) | unpushed | 984.8 | 985.9 | 987.3 | 989.7 | 990.8 | 992.4 | 992.9 | 996.6 | 997.5 |

### Run 1 (opt_ms)

| shape | side | rep1 | rep2 | rep3 | rep4 | rep5 | rep6 | rep7 | rep8 | rep9 |
|-------|------|------|------|------|------|------|------|------|------|------|
| WIN (k<10000) | pushed | 0.009 | 0.009 | 0.009 | 0.009 | 0.009 | 0.009 | 0.010 | 0.011 | 0.025 |
| WIN (k<10000) | unpushed | 0.003 | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | 0.005 |
| CONTROL (k>=0) | pushed | 0.008 | 0.009 | 0.009 | 0.009 | 0.009 | 0.009 | 0.009 | 0.010 | 0.012 |
| CONTROL (k>=0) | unpushed | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | 0.005 | 0.005 | 0.005 |

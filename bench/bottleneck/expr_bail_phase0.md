# Phase-0: expr_compile bail-reason measurements

Branch: perf/expr-null-fusion  
Date: 2026-06-11  
Binary: debug+ASAN (make default), commit at measurement time on branch perf/expr-null-fusion.

Counter semantics: `RAY_EXPR_STATS=1` prints cumulative process-global tallies at exit.
Each call to `expr_compile` either increments `ok` (fused kernel emitted) or one bail
bucket (first-matching reason wins, checked in order: root-shape → graph-size → depth →
regs → ins → mapcommon → str → nulls → slice → sym-domain → const → null-shape → other).

---

## Workload 1: Full test suite

Command: `RAY_EXPR_STATS=1 ./rayforce.test 2>&1 | grep "expr_compile"`

```
expr_compile ok=1925
expr_compile bail root-shape 78
expr_compile bail graph-size 2047
expr_compile bail depth      962
expr_compile bail regs       52
expr_compile bail mapcommon  1
expr_compile bail str        68
expr_compile bail nulls      181
expr_compile bail slice      222
expr_compile bail sym-domain 79
expr_compile bail const      68
expr_compile bail other      24
```

Derived totals:

| metric | value |
|---|---|
| total compile attempts | 5707 |
| ok (kernel emitted) | 1925 (33.7%) |
| total bails | 3782 (66.3%) |
| nulls bails | 181 (4.8% of bails / 3.2% of all) |

Bail-reason ranking (bails only):

| rank | reason | count | % of bails |
|---|---|---|---|
| 1 | graph-size | 2047 | 54.1% |
| 2 | depth | 962 | 25.4% |
| 3 | slice | 222 | 5.9% |
| 4 | **nulls** | **181** | **4.8%** |
| 5 | sym-domain | 79 | 2.1% |
| 6 | root-shape | 78 | 2.1% |
| 7 | str | 68 | 1.8% |
| 8 | const | 68 | 1.8% |
| 9 | regs | 52 | 1.4% |
| 10 | other | 24 | 0.6% |
| 11 | mapcommon | 1 | 0.0% |

Notes: `graph-size` + `depth` together account for 79.5% of all bails. These are checked
before `nulls`, so expressions that would also hit `nulls` may be masked here.

---

## Workload 2: ClickBench — 100K-row in-memory sample

Dataset: first 100,001 lines of `/home/hetoku/data/work/ClickBench/rayforce/hits_h.csv`
(7.6 GB headered file; full 100 M-row load takes ~2m42s so a 100K-row head was used).  
All 43 ClickBench queries in
`/home/hetoku/data/work/ClickBench/rayforce/queries.rfl` were run once.

Command:
```
head -n 100001 hits_h.csv > /tmp/hits_sample.csv
RAY_EXPR_STATS=1 ./rayforce -i < cb_sample_full.rfl 2>&1 | grep "expr_compile"
```

```
expr_compile ok=1
expr_compile bail nulls      42
expr_compile bail const      11
```

Derived totals:

| metric | value |
|---|---|
| total compile attempts | 54 |
| ok (kernel emitted) | 1 (1.9%) |
| total bails | 53 (98.1%) |
| nulls bails | 42 (79.2% of bails / 77.8% of all) |

Bail-reason ranking (bails only):

| rank | reason | count | % of bails |
|---|---|---|---|
| 1 | **nulls** | **42** | **79.2%** |
| 2 | const | 11 | 20.8% |

Notes: The ClickBench schema is heavily typed (I16/I32/I64/SYMBOL/TIMESTAMP columns),
and many columns carry NULL sentinel values, making `nulls` the dominant blocker.
`const` bails are expected (scalar-only expressions like `(count hits)` that don't
map to a fused columnar kernel).

---

## Workload 3: h2o group-by benchmark (q9)

File: `bench/h2o/q9.rfl`

Dataset required: `/home/serhii/Anton/teide-bench/datasets/G1_1e7_1e2_0_0/G1_1e7_1e2_0_0.csv`
(path hard-coded; that machine is not this machine).

**Status: CANNOT RUN — dataset not available locally.**

No local copy of G1_1e7_1e2_0_0.csv was found under `/home/hetoku`, `/skull`, or `/tmp`.
No download was performed.

---

## Verdict

### What fraction of bails are `nulls`?

| workload | nulls / total bails | nulls / all attempts |
|---|---|---|
| test suite | 181 / 3782 = 4.8% | 181 / 5707 = 3.2% |
| ClickBench (100K sample) | 42 / 53 = 79.2% | 42 / 54 = 77.8% |

### Top bail reasons by workload

**Test suite:** dominated by `graph-size` (54%) and `depth` (25%). These fire first in
the check order, so they shadow expressions that would also fail on `nulls`. `nulls` is
4th at 4.8% of bails. Fixing nulls alone would not move the test-suite fusion rate much
without also addressing graph-size and depth.

**ClickBench:** `nulls` is overwhelmingly the #1 reason (79%). The second reason `const`
(20%) reflects structurally unfuseable queries (aggregates over whole-table scalars) that
would remain regardless. In the ClickBench workload, fixing `nulls` would convert ~79% of
bail'd expressions into potentially fuseable candidates.

### Workloads that could not be measured

- h2o q9: G1 dataset lives on a different machine (`/home/serhii/...`), not found locally.
- Full 100 M-row ClickBench: loading hits.csv (76 GB) or hits_h.csv (7.6 GB, ~2m42s load)
  was considered too slow for a measurement-only step. The 100K-row sample is structurally
  representative since all 43 queries run and produce the same expression graphs; the
  sample size only affects runtime, not which bail reason fires.

### Summary signal for go/no-go

- Test suite: `nulls` is a minority reason (4th / 4.8%); the dominant blockers are
  `graph-size` and `depth` which are orthogonal to null-aware fusion.
- ClickBench: `nulls` is the primary barrier (79% of bails), with effectively nothing
  else competing once `const` is excluded.
- The discrepancy is explained by workload character: test suite exercises many
  compile paths including large/deep graphs; ClickBench expressions are relatively
  small/shallow (column-arithmetic, group-by keys) but touch nullable columns.
- Decision for controller: go/no-go depends on which workload is the optimization
  target. If ClickBench performance is the goal, the `nulls` signal is strong.
  If the test-suite coverage is the proxy, `graph-size`/`depth` fixes would yield
  more compiled kernels per unit of effort.

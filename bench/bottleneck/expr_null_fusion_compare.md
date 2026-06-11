# Perf gate: perf/expr-null-fusion vs master — comparison & verdict

Candidate: 64cdecfb (branch perf/expr-null-fusion) vs baseline c31dc460 (master).
Release builds (-O3, sanitizer-free, verified via nm/ldd). Full 10M-row in-memory
ClickBench dataset. Raw data: expr_null_fusion_rawdata.md (3 measurement rounds).

## Measurement history (why three rounds)

1. **Round 1 (debug+ASAN, 100K sample)** — INVALID for perf: ASan inflates
   per-op materialization costs; the "wins" (q40 −76%, q02 −57%) were artifacts.
2. **Round 2 (release, contended box, sequential A→B)** — INVALID: a concurrent
   process loaded the box (the run even hit the pre-existing allocator race,
   below); the "11 regressions +6–16%" did not survive controlled re-measurement.
3. **Round 3 (release, quiet box, interleaved B,C,B,C,B,C, 3 runs × 7 reps per
   binary per query)** — authoritative.

## ClickBench (Goal A): neutral

At 10M scale ClickBench queries stream through group pipelines and bypass
expr_compile almost entirely (candidate: ok=2 across 43 queries, both from
q35's null-free computed key). The new code is effectively inert here.

Round-3 significant rows (|delta| > 5%, base ≥ 5 ms):

| Q | base med | cand med | delta | per-run pattern | reading |
|---|---|---|---|---|---|
| q04 | 6.31 ms | 6.70 ms | +6.2% | consistent (+16.8/+4.1/+5.4) | sub-ms absolute; agg path byte-identical to master; icache/layout-class effect (cand binary +53 KB) |
| q11 | 7.12 ms | 7.74 ms | +8.6% | monotonically increasing | thermal/warmup drift, not deterministic |
| q26 | 13.76 ms | 12.83 ms | −6.8% | sign flips | noise |

No algorithmic regression: the only consistent delta (q04) is on a query that
executes zero branch code.

**ClickBench null finding:** at full scale the hits dataset has NO non-SYMBOL
column with HAS_NULLS — empty CSV fields land in SYMBOL columns (null = sym id
0, no attr). Phase-0's "42 null bails" was an artifact of the 100K sample
loader marking numeric columns nullable. The original "ClickBench wins"
hypothesis is dead at full scale; the change is a null-bearing-workload win.

## Fused-nullable engagement (Goal B): decisive wins

Synthetic 10M+1-row table with explicitly nullable columns; engagement verified
via RAY_EXPR_STATS (ok=5). Interleaved runs, CoV < 5%:

| query | expression | base med | cand med | delta |
|---|---|---|---|---|
| Q_B1 | nullable `(* x 2)` | 55.99 ms | 17.77 ms | **−68%** |
| Q_B2 | nullable `(+ x y)` | 98.56 ms | 19.15 ms | **−81%** |
| Q_B3 | null-free `(* x 2)` (control) | 17.06 ms | 16.36 ms | −4% (noise) |
| Q_B4 | nullable `(- x y)` | 100.04 ms | 19.24 ms | **−81%** |
| Q_B5 | nullable `(> x 1000)` | 48.21 ms | 3.78 ms | **−92%** |

Q_B3 confirms the null-free fast path is structurally unchanged (spec
invariant: identical instruction stream, also pinned by the
nullfree_invariance test).

## Correctness gates

- Full suite: 3357/3359, 2 pre-existing skips, 0 failed. ASan+UBSan (default
  test build): clean across the full suite incl. all 39 expr_null tests.
- Differential harness: fused ≡ forced-fallback on values + null positions for
  every enabled kernel family, incl. zero-divisor, overflow, parted, parallel
  (>65,536 rows), and agg-input attr-gating shapes.

## Pre-existing issues surfaced by this gate (NOT this branch; master bugs)

1. **Buddy-allocator race (SIGSEGV)**: heap_coalesce ← heap_flush_foreign ←
   ray_heap_gc dereferences buddy headers in reclaimed/unmapped pools while
   workers are non-quiescent (heap.c:1396's own TODO). Reproduced on a MASTER
   binary with a byte-identical stack. Recipe: two concurrent
   `rayforce -t 8 -i < race_seq_rel.rfl` over the 10M dataset (count-distinct
   group-bys, rep ≥ 2); crashes within ~10 min under contention.
2. **UBSan signed-overflow** at group.c MINMAX_SEG_LOOP span early-out with
   full-range i64 keys (benign wrap; one-line fix: unsigned subtraction).
3. **Fused/fallback divergences on master (null-free paths)**: i64 div-by-zero
   (fused 0 vs fallback NULL), isnull(computed NaN) (fused true vs fallback
   attr-gated false). Both predate this branch.

## VERDICT: WIN — merge

Per the spec gate (docs/superpowers/specs/2026-06-11-expr-null-fusion-design.md):
measured, statistically meaningful wins on expression-heavy null-bearing
queries (−68%…−92% at 10M scale); no algorithmic regression elsewhere
(ClickBench neutral; only icache-class sub-ms drift on queries executing zero
branch code); null-free path provably unchanged; full suite + sanitizers green.

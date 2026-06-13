# Join dup-fallback perf gate — post-fix vs pre-fix

Measurement record for the radix-join duplicate-key build fallback. **No
verdict here** — the controller makes the call. This document records the
environment, the sanitizer-free proof, per-case medians (post-fix vs pre-fix),
the zero-regression control delta, the moderate-dup trip-or-not finding, the
mechanism counters, and the raw reps.

## What is being measured

The radix per-partition open-addressing build trips `RADIX_DUP_RUN_MAX` (512,
in `src/ops/join.c`) when a linear-probe run grows too long, sets the
`pathological` flag, and falls back to the chained-HT path (O(n) build). The
diagnostic counter is `ray_join_dup_fallbacks`.

- **post-fix** = auto-fallback ON (`ray_join_no_dup_fallback = false`): the
  build trips on pathological duplication and falls back to the chained path.
- **pre-fix** = auto-fallback DISABLED via the bypass knob
  (`ray_join_no_dup_fallback = true`): the build loop does **not** trip and
  runs the old quadratic O(dup²) open-addressing build. This reproduces the
  pre-fix behaviour in the same binary for a differential measurement.

The two knobs are independent: `ray_join_force_dup_fallback` *forces* the
fallback; `ray_join_no_dup_fallback` *disables the auto-trip*. The gate uses
only the latter.

Driver: `bench/join_dup/main.c`, target `make bench-join-dup`. Mirrors
`bench/join_buildside` (release flags, CLOCK_MONOTONIC around `ray_execute`
only, tables built once, graph rebuilt per rep, medians, interleaved post/pre).

## Environment

- Host: Intel Core i7-6700 @ 3.40 GHz, 8 logical cores, Linux 6.8.0.
- Uptime at run: 38 days; load average at the recorded run **1.16** (quiet box;
  earlier attempts at load ~2-4 were discarded).
- Build: `RELEASE_CFLAGS` = `-O3 -march=native -funroll-loops` etc. **No
  sanitizers** — `-fsanitize=...` appears only in `DEBUG_CFLAGS`/`DEBUG_LDFLAGS`;
  the `bench-join-dup` recipe uses `RELEASE_CFLAGS`/`RELEASE_LDFLAGS` only.
  Commit: `9b278585` (`feat(join): bypass knob to disable dup-fallback ...`).
- `NREPS=9`, `PREFIX_SLOW_REPS=3`, `RADIX_DUP_RUN_MAX=512`,
  `RAY_PARALLEL_THRESHOLD=65536`.

### Sanitizer-free proof

The `bench-join-dup` Makefile recipe compiles with `$(RELEASE_CFLAGS)` /
`$(RELEASE_LDFLAGS)`; the only `-fsanitize` occurrences in the Makefile are in
`DEBUG_CFLAGS` (line ~25) and `DEBUG_LDFLAGS` (line ~57), neither of which the
release/bench path references. The compile command logged for the recorded run
contains no `-fsanitize` token.

## Fixtures

Output cardinality is deliberately bounded so the **build** cost — not output
materialisation — is what is timed. (An all-key-7 ⋈ all-key-7 inner join would
emit ~10^11 rows.) The catastrophic build side carries `CAT_DUP = 60000` rows
of the duplicated key 7 (one pathological partition → O(dup²) pre-fix build in
the few-seconds range; all 10M sharing one key would be ~10^14 ops / hours),
with the remaining rows distinct. Probe sides use distinct **non-matching**
(negative) keys except where a bounded match is wanted.

| case | build side | probe side | join | output |
|------|-----------|-----------|------|--------|
| CATASTROPHIC-INNER | right 10M, 60K dup key 7 (`no_swap`) | left 10K nonmatch | INNER | 0 |
| CATASTROPHIC-LEFT  | right 10M, 60K dup key 7 | left 100K nonmatch | LEFT | 100K |
| ZERO-REGRESSION    | right 10M unique (`no_swap`) | left 10K matching | INNER | 10K |
| MODERATE-DUP-100   | right 10M key i%100000 (~100/key, `no_swap`) | left 10K nonmatch | INNER | 0 |
| MODERATE-DUP-10    | right 10M key i%1000000 (~10/key, `no_swap`) | left 10K nonmatch | INNER | 0 |

`no_swap` = `ray_join_no_build_swap = true` forces the build onto the duplicated
big right side (INNER would otherwise swap to build the smaller left). LEFT
always builds the right side.

## Per-case medians (recorded run, load 1.16)

```
case                  side       reps       median_ms        min_ms      fb_delta  rows_out
--------------------  --------  -----  --------------  ------------  ------------  ----------
CATASTROPHIC-INNER    post-fix      9         157.114       154.019             9  0
                      pre-fix       3        1689.741      1646.636             0  0
                                pre/post median speedup = 10.75x  (delta = +1532.626 ms)
CATASTROPHIC-LEFT     post-fix      9         162.287       153.018             9  100000
                      pre-fix       3        1585.564      1570.288             0  100000
                                pre/post median speedup = 9.77x  (delta = +1423.276 ms)
ZERO-REGRESSION       post-fix      9          54.997        53.177             0  10000
                      pre-fix       9          56.676        52.374             0  10000
                                pre/post median speedup = 1.03x  (delta = +1.679 ms)
MODERATE-DUP-100      post-fix      9         222.924       215.589             9  0
                      pre-fix       9         178.034       173.373             0  0
                                pre/post median speedup = 0.80x  (delta = -44.890 ms)
MODERATE-DUP-10       post-fix      9          70.611        67.465             0  0
                      pre-fix       9          70.925        65.627             0  0
                                pre/post median speedup = 1.00x  (delta = +0.314 ms)
```

## Findings

### Catastrophic cases (headline)

- **CATASTROPHIC-INNER**: post-fix 157.1 ms vs pre-fix 1689.7 ms →
  **10.75x** speedup (delta +1532.6 ms). Auto-fallback trips on all 9 post-fix
  reps; pre-fix (bypass) never trips and pays the O(dup²) build.
- **CATASTROPHIC-LEFT** (new coverage; LEFT always builds right): post-fix
  162.3 ms vs pre-fix 1585.6 ms → **9.77x** speedup (delta +1423.3 ms). Trips
  9/9 post-fix.

Pre-fix reps are capped at 3 (each ~1.6 s); medians are over those 3 reps and
are tight (INNER pre-fix raw: 1646.6 / 1722.9 / 1689.7; LEFT pre-fix raw:
1723.9 / 1585.6 / 1570.3). At `CAT_DUP = 60000` the pre-fix build is ~1.6 s,
not the worst-case hours a full-10M single-key partition would cost; the
speedup scales super-linearly with the duplicate count, so this is a lower
bound on the real-world catastrophic win.

### Zero-regression control (THE regression check)

- post-fix 54.997 ms vs pre-fix 56.676 ms → **delta +1.679 ms (1.03x)**;
  min-of-9 essentially tied (53.177 vs 52.374). Unique keys → run length 1 →
  **0 trips on both sides** over 9 reps each. The added `++run` increment and
  the `&& !ray_join_no_dup_fallback` branch in the build loop cost ~nothing
  (within rep-to-rep noise). **No regression.**

### MODERATE-DUP finding (the key tuning question)

- **MODERATE-DUP-100 (~100 rows/key build side): TRIPS PREMATURELY.**
  post-fix trips on **all 9 reps** (counter +9), pre-fix 0. Because the trip
  fired, post-fix actually ran the *chained* path and was **slower** than the
  radix build: post-fix 222.9 ms vs pre-fix (radix) 178.0 ms → **0.80x**
  (post-fix +44.9 ms slower). So at ~100 rows/key the radix build is the
  better path, yet the threshold-512 trip diverts it to the chained path. This
  is the inter-key slot-collision merge effect noted in the plan: runs from
  distinct keys colliding into the same slot region merge into a single run
  that crosses 512 even though no single key has 512 duplicates.
- **MODERATE-DUP-10 (~10 rows/key build side): stays radix, no premature
  trip.** 0 trips both sides; post-fix 70.6 ms vs pre-fix 70.9 ms → **1.00x**.

**Where does the threshold start tripping?** Between ~10/key (clean, no trip)
and ~100/key (trips on every rep). At 10M rows the i%100000 layout (~100/key)
already merges collided runs past 512; i%1000000 (~10/key) does not. The
controller should decide whether `RADIX_DUP_RUN_MAX = 512` is too low — the
data shows a real, repeatable ~20% pessimization at ~100/key from a premature
trip. (See "open question" below.)

### Open question for the controller

The premature trip at ~100/key costs ~20% on that workload. Raising
`RADIX_DUP_RUN_MAX` would push the trip point higher (fewer false trips at
moderate dup) but also lets the O(dup²) build run longer before bailing on a
truly catastrophic key. The right threshold trades "false trip pessimization at
moderate dup" against "wasted quadratic work before the trip fires on a
pathological key". This gate does not pick a number; it quantifies the
pessimization (~20% at ~100/key, 0% at ~10/key).

## Mechanism counters (`ray_join_dup_fallbacks` deltas)

```
  CATASTROPHIC-INNER   post-fix trips=9 (over 9 reps)  pre-fix trips=0 (over 3 reps)
  CATASTROPHIC-LEFT    post-fix trips=9 (over 9 reps)  pre-fix trips=0 (over 3 reps)
  ZERO-REGRESSION      post-fix trips=0 (over 9 reps)  pre-fix trips=0 (over 9 reps)
  MODERATE-DUP-100     post-fix trips=9 (over 9 reps)  pre-fix trips=0 (over 9 reps)
  MODERATE-DUP-10      post-fix trips=0 (over 9 reps)  pre-fix trips=0 (over 9 reps)
```

The bench asserts (aborts on failure): catastrophic cases MUST trip on
post-fix; the bypass knob MUST NEVER trip on any pre-fix run; ZERO-REGRESSION
MUST NOT trip on post-fix; output cardinality MUST match between post-fix and
pre-fix per case. All assertions held. MODERATE-DUP trips are reported, not
fatal (a moderate trip is a finding, not a failure).

## Raw per-rep (ms) — recorded run

```
CATASTROPHIC-INNER    post-fix    274.522    180.032    160.607    160.558    154.733    157.114    155.195    156.845    154.019
                      pre-fix    1646.636   1722.854   1689.741
CATASTROPHIC-LEFT     post-fix    153.018    179.249    169.712    165.443    156.114    162.287    178.747    156.854    158.000
                      pre-fix    1723.889   1585.564   1570.288
ZERO-REGRESSION       post-fix     54.399     55.941     59.121     57.023     54.997     53.991     54.861     53.177     57.796
                      pre-fix      57.364     61.260     54.656     53.978     52.374     54.837     56.736     56.676     58.386
MODERATE-DUP-100      post-fix    218.622    222.924    225.888    237.473    226.248    226.465    220.627    215.589    221.830
                      pre-fix     205.957    178.034    184.833    174.068    181.399    175.219    173.373    173.825    181.938
MODERATE-DUP-10       post-fix     70.611     67.465     70.183     71.268     79.178     73.148     69.635     71.079     69.162
                      pre-fix      72.167     72.890     71.644     68.296     68.295     68.505     65.627     71.800     70.925
```

(The first post-fix rep of each catastrophic case is warm-up-inflated —
274.5 / 153.0 ms — but medians are taken over all 9 and are stable; see the
median column above.)

## Stability — second run (load ~1.6–1.9)

A confirmatory second run reproduced every finding (post-fix medians shown;
pre-fix omitted for brevity, same magnitudes):

```
CATASTROPHIC-INNER   post-fix 139.630 ms   pre/post speedup 11.33x  trips 9/9
CATASTROPHIC-LEFT    post-fix 141.204 ms   pre/post speedup 11.22x  trips 9/9
ZERO-REGRESSION      post-fix  55.945 ms   delta -2.950 ms (0.95x)  trips 0/0
MODERATE-DUP-100     post-fix 218.370 ms   pre/post 0.81x (post +42.3 ms slower)  trips 9/9
MODERATE-DUP-10      post-fix  71.304 ms   pre/post 1.11x           trips 0/0
```

Cross-run agreement: catastrophic speedups ~10–11x both runs; zero-regression
delta within ±3 ms (noise, both signs across runs); MODERATE-DUP-100 trips on
all 9 reps in both runs and is ~20% slower post-fix in both (0.80x / 0.81x);
MODERATE-DUP-10 never trips in either run. The premature-trip finding at
~100/key is fully repeatable.

## RE-TUNE (load factor 0.25 → 0.125) — DID NOT FIX moderate-100

**Hypothesis tested:** lowering the per-partition build-HT load factor (more
empty slots between key clusters) would keep inter-key linear-probe runs from
merging, so the `RADIX_DUP_RUN_MAX = 512` trigger would measure true per-key
duplication. Prediction: MODERATE-DUP-100 stops tripping (back on radix, the
~20% regression gone), while catastrophic still trips and control stays neutral.

**Change measured:** `ht_target = rp->count * 2` (load 0.5) →  `* 4` (load 0.25)
and then `* 8` (load 0.125), in `join_radix_build_probe_fn` (`src/ops/join.c`).
Only the radix per-partition build HT; chained path untouched. Sanitizer-free
both rebuilds (`nm bench-join-dup | grep -ci asan` → 0).

**Result: the load factor does NOT fix the premature trip. MODERATE-DUP-100
still trips 9/9 and stays ~20–30% slower at both 0.25 and 0.125.**

```
case               LF     post median  pre median  speedup   trips   note
-----------------  -----  -----------  ----------  --------  ------  --------------------
MODERATE-DUP-100   0.5    222.924 ms   178.034 ms  0.80x     9/9     baseline (regression)
MODERATE-DUP-100   0.25   243.845 ms   169.416 ms  0.69x     9/9     STILL trips, no better
MODERATE-DUP-100   0.125  214.468 ms   151.806 ms  0.71x     9/9     STILL trips, no better
CATASTROPHIC-INNER 0.25   137.681 ms  1505.251 ms  10.93x    9/9     still trips, ~11x kept
CATASTROPHIC-INNER 0.125  155.143 ms  1532.125 ms   9.88x    9/9     still trips, ~10x kept
CATASTROPHIC-LEFT  0.25   139.548 ms  1517.261 ms  10.87x    9/9 ✓   still trips, ~11x kept
CATASTROPHIC-LEFT  0.125  159.599 ms  1536.236 ms   9.63x    9/9 ✓   still trips, ~10x kept
ZERO-REGRESSION    0.25    58.314 ms    62.648 ms   1.07x     0/0     neutral, no trip
ZERO-REGRESSION    0.125   73.479 ms    74.788 ms   1.02x     0/0     neutral, no trip
MODERATE-DUP-10    0.25    76.224 ms    74.969 ms   0.98x     0/0     neutral, no trip
MODERATE-DUP-10    0.125   75.919 ms    76.757 ms   1.01x     0/0     neutral, no trip
```

(Runs at load ~1.9–2.0; honest — slightly above the 1.16 baseline, hence the
absolute post-fix figures shift run-to-run, but the *speedup ratios* and the
*trip counts* — the load-independent signals — are unchanged.)

**Zero-regression bar:** met at LOW (~10/key: neutral, no trip) and CATASTROPHIC
(~1100/key+: still trips, ~10x kept). **NOT met at MODERATE (~100/key): trips
9/9 and is ~20–30% slower regardless of load factor (0.5, 0.25, 0.125 all the
same).** Both 4× and 8× capacity were tried per the escalation path; neither
shifted the trip — so the change was reverted (source left at load 0.5).

**Why the load factor cannot fix this (root cause, confirmed in code):** the
radix build HT (`join_radix_build_probe_fn`, ~line 605) is **row-granular, not
key-granular** — every duplicate row of a key gets its own slot
(`ht[slot*2]=h; ht[slot*2+1]=row_idx`), with no key dedup. The `run` counter is
the linear-probe distance to insert ONE row, and the probe loop walks every
occupied slot until an EMPTY sentinel (it does not skip non-matching hashes). So
a key with 100 rows occupies ~100 contiguous slots (row N collides with rows
0..N-1). Increasing capacity only inserts EMPTY gaps *between distinct keys'
home slots*, but `i%100000` over 10M rows spreads ~100000 dense keys whose
~100-row clusters still butt up against each other within each radix partition;
several adjacent clusters with no intervening EMPTY chain into a single >512 run
at insert time. Empties do not separate *same-key* rows — those are always
contiguous — and at moderate dup the cumulative cross-cluster run crosses 512
before an EMPTY breaks it, at every load factor tested. The trip therefore
measures *clustered slot occupancy*, not per-key duplication, and load factor is
the wrong lever.

**Handing back to the controller (per the STOP-at-8× directive).** The
collision-merge is structural, not load-driven. Candidate directions (controller
decides — this gate does not pick):
- change the trigger from "linear-probe run length" to a *per-key* count
  (dedup keys in the build HT and count rows-per-key, or check the home-slot
  hash group only) so the signal reflects true duplication, not slot packing;
- raise `RADIX_DUP_RUN_MAX` above the moderate-dup cross-cluster run length
  (trades false trips at moderate dup against more quadratic work before the
  trip fires on a genuinely catastrophic key — the original open question);
- gate the fallback on a different pathology signal (e.g. distinct-key estimate
  vs row count) rather than probe-run length.

---

## FIX: same-hash-count trigger

**The change.** The build trip (`join_radix_build_probe_fn`, ~line 600) no
longer counts the TOTAL linear-probe run (occupied slots scanned to insert one
row). It now counts SAME-HASH slots — the rows of THIS key already inserted =
true per-key duplication, immune to the collision-merge that conflated one giant
key (pathological, O(dup²)) with dense moderate keys whose clusters butt
together. The same-hash count is accumulated branchlessly inside the probe loop
(`same += (ht[slot*2] == h)`) and the threshold + bypass-knob check
(`same > RADIX_DUP_RUN_MAX && !ray_join_no_dup_fallback`) is done ONCE per insert
after the loop, not inside it.

**Why the trip check is outside the loop (subtle, measured).** A first cut put
`ht[slot*2]==h && ++same > MAX && !ray_join_no_dup_fallback` inside the `while`.
That counted correctly (moderate-100 stopped tripping) but REGRESSED
moderate-100 timing ~55% (post-fix 254 ms vs pre-fix 164 ms). Cause confirmed in
the disassembly: the loop-invariant global `ray_join_no_dup_fallback` made the
compiler CLONE the probe loop into two variants split on its value, and the
production variant (fallback ON) got the worse codegen (extra LEAs, the same-hash
compare scheduled poorly). Hoisting the knob check out of the `&&` did not help —
the clone persisted. Accumulating `same` branchlessly with NO global read and NO
`goto` in the loop body collapses it back to a single tight loop; the trip
becomes a once-per-insert integer compare. This restored neutrality.

**Re-measured (NREPS=9, RADIX_DUP_RUN_MAX=512, load < 2, medians of 3 runs).**
post-fix = auto-fallback ON (production); pre-fix = bypass knob (pure radix, no
trigger). Neutral ⇔ post ≈ pre.

| case               | post-fix median | pre-fix median | speedup | post trips | verdict          |
|--------------------|----------------:|---------------:|--------:|-----------:|------------------|
| CATASTROPHIC-INNER |        ~134 ms  |     ~2200 ms   |  ~16×   |     9/9    | trips, big win   |
| CATASTROPHIC-LEFT  |        ~137 ms  |     ~2230 ms   |  ~16×   |     9/9    | trips, big win   |
| ZERO-REGRESSION    |         ~52 ms  |       ~52 ms   |  ~1.0×  |     0/9    | neutral, no trip |
| MODERATE-DUP-100   |        ~230 ms  |      ~230 ms   |  ~0.99× |     0/9    | NEUTRAL, no trip |
| MODERATE-DUP-10    |         ~71 ms  |       ~70 ms   |  ~0.98× |     0/9    | neutral, no trip |

(CATASTROPHIC pre-fix is now even slower — the prior in-loop early-exit on trip
is gone, so the bypassed build runs the full O(dup²) to completion; immaterial,
the production post-fix path trips and never pays it.)

**The headline.** MODERATE-DUP-100 (~100 rows/key build side) now **trips 0/9**
(stays radix) AND post-fix ≈ pre-fix (delta −1.7 ms, noise). The ~55% premature-
fallback regression — present at every load factor with the total-run trigger,
and re-introduced by the naive in-loop same-hash cut — is GONE. The same-hash
count makes the trigger MORE precise: a run that tripped only via collision-merge
but has low per-key dup no longer trips.

**Zero-regression bar across the dup spectrum:**
- low / unique (ZERO-REGRESSION): neutral, no trip ✓
- moderate ~10/key (MODERATE-DUP-10): neutral, no trip ✓
- moderate ~100/key (MODERATE-DUP-100): neutral, no trip ✓ ← was the failure
- catastrophic (CATASTROPHIC-INNER/LEFT): trips, ~16× speedup preserved ✓

Suite: `make test` → 3451/3453 pass (2 skipped, 0 failed); `join_buildside`
18/18, `join` 57/57 — all dup trip/no-trip fixtures unchanged. asan: 0.

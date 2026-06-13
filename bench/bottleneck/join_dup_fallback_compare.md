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

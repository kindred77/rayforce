# Aggregation Engine Redesign — Phase 3 Implementation Plan (delete the zoo + flip v2 on)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development — BUT the oracle re-baseline steps are judgment-heavy; the coordinator MUST verify each oracle change against the triage rule (do NOT let an agent blindly update expected values). Steps use checkbox (`- [ ]`).
>
> **Commit steps are checkpoints.** Follow the user's commit cadence. Do NOT commit docs under `docs/design/`.

**Goal:** Delete the six `OP_GROUP_*_ROWFORM` operators + their planner/executor gates, flip `ray_agg_engine_v2` on by default, and re-baseline the conformance oracles to v2's **corrected** semantics. This is where the order-dependence cliff and the planner/executor double-bookkeeping die. Decisions (made): **delete + flip on**, **corrected** profile.

**The corrected behaviors that go live** (the ONLY justified oracle changes — anything else is a regression):
1. **Group order:** sorted-by-key (old) → **first-occurrence** (v2) — affects any order-sensitive group-by oracle.
2. **Pearson:** rowform r² → **signed r** (v2).
3. **count over a HAS_NULLS column:** slot-count → **live-rows-only** (v2).

**TRIAGE RULE (load-bearing):** when an oracle/test fails after a step, classify it:
- **(a) explained by 1/2/3 above** → the values otherwise match (verify as a multiset where order changed; verify the numeric is r-not-r²; verify the count delta equals the null count) → re-baseline (update expected).
- **(b) anything else** → a REAL REGRESSION. STOP. Investigate. Do NOT update the oracle to pass.
Every re-baselined oracle must have a one-line justification (which of 1/2/3). The coordinator reviews this list.

**Tech Stack:** C11, in-repo harness. Builds on Phase 2d (branch `feat/agg-engine-phase0`, through `b9108b97`).

**Deletion surface (mapped):** opcodes ops.h:225-256 (+ ext comments :699-715); dispatch exec.c:1055, :1559-1573; executors group.c:10753 (topk), 11318 (pearson), 11829 (maxmin), 12405 (median_stddev), 12990 (sum_count); planner gates query.c:~6960-7212 (topk/prf/mm/ms/sc + `ray_group_*_rowform` calls); the `ray_group_*_rowform` builders (graph.c); rowform oracle/C tests (rowform_topk.rfl, rowform_maxmin.rfl, rowform_sum_count.rfl, per_group_holistic.rfl, atom_i64_med_topk.rfl, group_coverage_extension.rfl, exec_coverage.rfl, etc.).

---

## Task 1: Flip the flag ON — discover & re-baseline generic-path order changes

This isolates "what flipping v2 on changes" (for plain-OP_GROUP queries v2 now handles) from "what deleting rowforms changes" (Task 2). The rowform queries still use their operators here (gates intact), so this step only surfaces order/count changes on the GENERIC group-by queries v2 admits.

- [ ] **Step 1: Flip the default**

`src/ops/agg_engine.c`: `bool ray_agg_engine_v2 = true;`. Build.

- [ ] **Step 2: Run the full suite; capture every failure**

`make test 2>&1 | tee /tmp/p3t1.log | tail -40`. List every failed test/oracle.

- [ ] **Step 3: Triage each failure (THE careful step)**

For each failure, apply the TRIAGE RULE. Expected category-(a) causes here: first-occurrence order on group-by oracles that assert exact row order or full-table output; count-over-nulls. Re-baseline category-(a) oracles (update the expected output in the .rfl, or fix order-sensitive C tests to sort/compare-as-multiset). For any category-(b), STOP and report — it's a real v2 bug or an unexpected divergence (e.g. a type/value mismatch the differential didn't cover).
> Build a table: failing test → category (1/2/3) → justification → action. The coordinator reviews this table before any oracle is updated.

- [ ] **Step 4: Re-run to green; commit**

`make test 2>&1 | tail -8` — 0 failures. Commit the flag flip + the justified oracle updates together:
```bash
git add -A   # (NOT docs/) — src + the re-baselined test/rfl files
git commit -m "feat(agg): enable v2 aggregation engine by default; re-baseline order-changed oracles (first-occurrence)"
```
> If category-(b) regressions were found, they are fixed in src (with their own commits) BEFORE this commit — the suite must be green for the right reasons.

---

## Task 2: Delete the planner rowform gates — route rowform queries through v2

With the gates gone, the planner emits plain `OP_GROUP` for the formerly-rowform queries; v2 (now on) handles them. The rowform executors become unreachable (deleted in Task 3).

- [ ] **Step 1: Remove the rowform gate blocks in query.c**

Delete the gate logic (~query.c:6960-7212) that sets `mm_ok`/`ms_ok`/`sc_ok`/`prf_ok`/the topk admission and calls `ray_group_maxmin_rowform`/`ray_group_median_stddev_rowform`/`ray_group_sum_count_rowform`/`ray_group_pearson_rowform`/`ray_group_topk_rowform`. The fallthrough must be the plain `ray_group(g, key_ops, n_keys, agg_ops, agg_ins, n_aggs)` (which already exists as the else-branch). For pearson/topk that need the binary/K inputs, ensure the plain `ray_group` path carries `agg_ins2`/`agg_k` — use `ray_group2`/`ray_group3` (ops.h:680/686) as the builder so the OP_GROUP node carries the second input / K (v2's gate + drivers consume them). VERIFY: a `(pearson x y) by k` and a `(top v K) by k` query now build an OP_GROUP node with agg_ins2/agg_k set, which v2 admits.

- [ ] **Step 2: Run suite; triage (THE careful step again)**

`make test 2>&1 | tee /tmp/p3t2.log | tail -40`. Now the rowform queries route through v2. Expected category-(a) changes: rowform order → v2 first-occurrence (the rowform oracle files); pearson r²→r (rowform_pearson / per_group_holistic oracles); top/bot LIST output (should match v2 — already differential-validated). Triage table; re-baseline category-(a); STOP on category-(b).
> Pay special attention to pearson oracles: the rowform produced r², v2 produces r — these WILL change and must be re-baselined to r (justification: corrected behavior #2). Confirm the new value is r (e.g. anti-correlated → -1), not a bug.

- [ ] **Step 3: Re-run to green; commit**

```bash
git add -A   # src/ops/query.c + re-baselined test/rfl
git commit -m "feat(agg): planner emits OP_GROUP for all group-by (delete rowform gates); route through v2; re-baseline rowform oracles (corrected)"
```

---

## Task 3: Delete the dead rowform operators

After Task 2 the rowform opcodes are never emitted → their executors + opcodes + dispatch are dead. Remove them.

- [ ] **Step 1: Delete**

- `src/ops/exec.c`: remove the `case OP_GROUP_*_ROWFORM:` dispatch (:1559-1573) and the opc check at :1055 (drop the rowform opcodes from it).
- `src/ops/group.c`: delete the 5 executor functions (`exec_group_topk_rowform`, `exec_group_pearson_rowform`, `exec_group_maxmin_rowform`, `exec_group_median_stddev_rowform`, `exec_group_sum_count_rowform`) and any now-unused static helpers they alone used (the compiler -Werror=unused will flag them).
- `src/ops/graph.c`: delete the `ray_group_*_rowform` builder functions (now uncalled) + their declarations in ops.h.
- `src/ops/ops.h`: remove the 6 `OP_GROUP_*_ROWFORM` opcode defines + the ext-struct comment references (:699-715). (Leave the opcode NUMBERS retired — do not renumber other opcodes, to avoid churn; just delete the defines.)
- Delete/repurpose C tests that call the rowform executors/builders directly (grep test/ for `rowform`); rfl oracle files that test the QUERY (not the internal op) stay (already re-baselined in Task 2).

- [ ] **Step 2: Build + full suite**

`make test 2>&1 | tail -8` — 0 failures (deletions are dead code; suite already green from Task 2). -Werror clean (no unused-static warnings → confirms full removal). ASAN clean.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "feat(agg): delete the 6 OP_GROUP_*_ROWFORM operators + builders (subsumed by v2)"
```

---

## Task 4: Final conformance + perf sanity + cleanup

- [ ] **Step 1: Full conformance, clean rebuild**

`make clean && make test 2>&1 | tail -8` — 0 failures from a clean build. Confirm the re-baselined oracle count and that every change has a triage justification (the coordinator's review table from Tasks 1-2).

- [ ] **Step 2: Perf sanity (if the bench harness is runnable)**

Run the group-related benches (`make bench-group-pushdown` etc. if present) to confirm v2-default is not a catastrophic regression vs the deleted rowforms on the h2o-shaped queries. v2 is serial-grouping + per-worker-buffer (1c) — expect it to be competitive but possibly behind the hand-tuned rowforms on some shapes; record the numbers. (No hard perf gate until a separate perf phase; this is a sanity check + a record for the future radix optimization.)
> KNOWN follow-up (record, don't fix here): v2's per-worker buffer duplication (1c) and the absence of the radix/disjoint-partition strategy mean high-cardinality parallel group-by uses more memory than the old radix. The original design's Phase-2 radix strategy is the remedy; note it as the next perf phase.

- [ ] **Step 3: Commit any final cleanup**

```bash
git add -A
git commit -m "chore(agg): phase 3 cleanup; v2 is the default group-by engine"
```

---

## Phase 3 Done — exit criteria

- The six rowform operators + their gates + builders are deleted; the planner emits plain `OP_GROUP`; v2 is the default group-by engine.
- **The order-dependence cliff and the planner/executor gate double-bookkeeping are gone** (the gates that embodied them no longer exist).
- Conformance suite green from a clean build; every changed oracle justified by a corrected behavior (1/2/3), reviewed by the coordinator; no category-(b) regression.
- ASAN/UBSan clean.

**Behavior now live (corrected profile):** group order first-occurrence; pearson signed r; count = live rows. Recorded for users/changelog.

**Recorded follow-ups (separate future phases):**
- Perf: add the radix/disjoint-partition grouping strategy (removes 1c's per-worker memory duplication; closes any remaining perf gap vs the deleted rowforms). This is the original design's cost-based-selector/strategy work.
- Optional: a `strict-compat` conformance profile (REVIEW-ARCH rec 11) if bit-exact-with-old is ever needed for migration.
- The minor cleanups from earlier reviews (test comparator O(n²) sort; `ray_valid_at` default for future nullable types; unchecked realloc in buffered push).

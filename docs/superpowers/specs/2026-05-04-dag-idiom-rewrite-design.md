# DAG Idiom Rewrite Pass — Design

**Status:** Draft
**Date:** 2026-05-04
**Author:** Anton (with Claude)
**Scope:** Main DAG only (group/sort/window/select/join sub-DAGs out of scope for v1)

---

## Problem

Several Rayforce expressions decompose at the DAG level into shapes whose
optimal evaluation is already implemented as a single primitive opcode, but
the planner emits the unfused form. The canonical example:

```rfl
(count (distinct V))
```

Today this evaluates as:

1. `OP_DISTINCT` — runs `ray_distinct_fn` (`src/ops/collection.c:700`),
   which materialises a deduplicated vector via a hash set, then sorts the
   surviving indices, then gathers them into a fresh column.
2. `OP_COUNT` — reads `len` of that column.

A dedicated opcode `OP_COUNT_DISTINCT` already exists
(`src/ops/ops.h:170`), is wired through the DAG builder
(`ray_count_distinct` at `src/ops/graph.c:671`), executor
(`exec_count_distinct` at `src/ops/group.c:158`), and lazy chain
(`ray_lazy_append` at `src/ops/graph.c:1685`) — but no surface
construction path produces it. The fast path is unreachable from
Rayfall today.

The same applies to a small family of related shapes
(see §4 — Day-1 catalog).

## Non-goals

- **Grouped aggregation rewrites.** `OP_GROUP`'s `agg_ins[a]` sub-DAGs are
  out of scope; group is already optimised on its own path.
- **Surface-language sugar.** This pass operates on the post-construction
  DAG. Adding `count_distinct` / `countd` as a Rayfall-callable function
  is a separate, future concern.
- **A pattern DSL or e-graph.** A static C table is sufficient for the
  catalog size we anticipate (≤ ~30 idioms).

## Architecture

### Pipeline insertion

A new pass `pass_idiom_rewrite` is inserted as **Pass 3** in
`ray_optimize` (`src/ops/opt.c:2013`), between constant folding and SIP:

```
Pass 1: type inference
Pass 2: constant folding
Pass 3: idiom rewrite           ← NEW
Pass 4: SIP
Pass 5: factorize
Pass 6: predicate pushdown
Pass 7: filter reorder
Pass 8: projection pushdown
Pass 9: partition pruning
Pass 10: fusion
Pass 11: DCE
```

Rationale:

- **After const-fold** — folded literals can collapse some inputs (e.g.
  `(asc [3 1 2])`) to a constant vector, which the rewriter would
  otherwise see as a real `OP_ASC` and try (harmlessly) to match.
  Running after const-fold removes this concern.
- **Before SIP and everything downstream** — subsequent passes see the
  simplified shape. Projection-pushdown's reachability walk is shorter,
  fusion has fewer chains to inspect, DCE has fewer dead nodes.
- DCE at the end already sweeps anything the rewriter marks
  `OP_FLAG_DEAD`. No new cleanup pass needed.

### File layout

New files:

- `src/ops/idiom.h` — public interface (`ray_idiom_pass`, `ray_idiom_t`).
- `src/ops/idiom.c` — pass implementation, dispatch index, idiom table,
  per-row predicate and rewrite functions.

Both compile automatically — `Makefile` uses `wildcard src/*/*.c`
(verified per the layout memo) and there is no `CORE_OBJECTS` list to
edit.

## Data structures

```c
/* src/ops/idiom.h */
typedef bool      (*ray_idiom_pre_t)(ray_graph_t* g, ray_op_t* node);
typedef ray_op_t* (*ray_idiom_rw_t) (ray_graph_t* g, ray_op_t* node);

typedef struct {
    uint16_t          root_op;     /* matches if node->opcode == root_op       */
    uint16_t          child0_op;   /* matches if node->inputs[0]->opcode == .. */
    ray_idiom_pre_t   pre;         /* optional precondition; NULL = always     */
    ray_idiom_rw_t    rewrite;     /* returns replacement node, or NULL        */
    const char*       name;        /* "count(distinct) -> count_distinct"      */
} ray_idiom_t;

extern const ray_idiom_t ray_idioms[];
extern const int         ray_idioms_count;

void ray_idiom_pass(ray_graph_t* g, ray_op_t* root);
```

The table itself is `static const ray_idiom_t ray_idioms[] = { … };`
in `idiom.c`. Adding an idiom is a one-line addition to that array
plus (in the typical case) a small `static` rewrite function above it.
There is no registration API — patterns are baked in at build time.

### Dispatch index

A naive matcher would scan the full table for every node. Instead, on
first call the pass builds a per-opcode bucket index:

```c
/* Sized generously above the highest currently-defined opcode
 * (OP_KNN_RERANK = 103 in src/ops/ops.h as of writing). A
 * _Static_assert in idiom.c guards against silent overflow if
 * a new opcode pushes past the cap. */
#define RAY_IDIOM_OPCODE_CAP 128
#define RAY_IDIOM_MAX_ROWS    64

static int8_t first_idiom[RAY_IDIOM_OPCODE_CAP]; /* -1 = no idioms */
static int8_t next_idiom [RAY_IDIOM_MAX_ROWS];   /* chain link     */
static bool   index_built;

_Static_assert(/* highest used root_op */ < RAY_IDIOM_OPCODE_CAP,
               "idiom dispatch index too small");
```

Build cost: O(N) where N = `ray_idioms_count`. Per-node lookup: O(1)
bucket head + chain walk over only those rows whose `root_op` matches.
With 6 rows in v1 across 3 distinct root ops (`OP_COUNT`, `OP_FIRST`,
`OP_LAST`), average chain length is 2.

Index is process-static — built once, never freed. 192 bytes of BSS
total (`128 + 64` int8s).

If the catalog ever exceeds 127 rows or 127 opcodes, widen both
arrays to `int16_t`. The static-assert keeps this honest.

## Matcher mechanics

### Walk order

Single post-order traversal of the live graph rooted at `root`,
children-before-parents. Implementation reuses the visited/stack
machinery already present in `count_refs` (`src/ops/fuse.c:59`) —
**not** a fixpoint loop. None of the day-1 idioms produce shapes
that re-match.

If we later add cascading idioms, we wrap the body in
`do { changed = false; … } while (changed && iter++ < CAP);` with a
small iteration cap (8 is more than enough for any realistic chain).
The cap exists strictly to bound pathological cases — the DCE pass
already protects against unbounded graph growth via the dead-marking
convention.

### Per-node match

For a node `n`:

1. Look up the bucket head: `idx = first_idiom[n->opcode]`.
   If `< 0`, skip.
2. Walk the chain. For each candidate row:
   - Check `n->inputs[0]->opcode == row->child0_op` (and
     `n->inputs[0] != NULL`, defensive).
   - If `row->pre`, call it. If it returns false, continue chain.
   - Call `row->rewrite(g, n)`. If it returns `NULL`, continue chain.
   - On success: install the replacement (see below) and break out
     of the chain.

Day-1 patterns are all unary-over-unary, so only `inputs[0]` is
inspected. The schema permits future expansion to `child1_op` if a
binary-rooted idiom needs it; for now that field is implicit (any).

### Replacement protocol

`rewrite` returns the node that should take the original's place. The
matcher then:

1. `redirect_consumers(g, n->id, replacement->id)` — already in
   `src/ops/opt.c:1312`. Walks the graph, repoints any node whose
   `inputs[i] == &g->nodes[n->id]` to the replacement.
2. `n->flags |= OP_FLAG_DEAD` — same convention every other rewrite
   pass uses (`factorize_pass`, `pass_predicate_pushdown`, etc.).
3. The original's child (e.g. the `OP_DISTINCT` node) becomes
   reference-orphaned and will be marked dead by `pass_dce` at Pass 11.

The matcher does **not** mutate `n->opcode` in place. In-place mutation
would invalidate any other node's input pointer to `n` — a hazard
sidestepped entirely by always producing a fresh node and using
`redirect_consumers`.

### Fresh-node allocation

Rewrite callbacks allocate via `graph_alloc_node_opt` (`opt.c:1027`),
not the public `graph_alloc_node`, so they participate in the
optimizer's existing extension-tracking discipline. The new node's
`out_type` is set explicitly per opcode (e.g. `RAY_I64` for
`OP_COUNT_DISTINCT`).

### Ext-bearing nodes

Any node whose `opcode` is one of `OP_GROUP`, `OP_SORT`, `OP_JOIN`,
`OP_WINDOW`, `OP_WINDOW_JOIN`, `OP_SELECT` is **skipped as a root**
in v1 — those carry sub-DAGs in `ext` data. Per-scope, the idiom
catalog targets only top-level main-DAG shapes.

A future extension would teach the matcher to recurse into ext sub-DAGs
(the `count_refs` enumeration in `fuse.c:111-167` already shows the
exact set of ext children to traverse). That recursion is not
implemented in v1.

## Day-1 catalog

Six rows. No new opcodes needed — every replacement maps to an
already-defined `OP_*`.

| # | Pattern                  | Replacement              | Precondition                                  | Notes                                                                                            |
|---|--------------------------|--------------------------|-----------------------------------------------|--------------------------------------------------------------------------------------------------|
| 1 | `(count (distinct v))`   | `OP_COUNT_DISTINCT(v)`   | none                                          | Headline rewrite. Hash-set executor at `group.c:158` normalises null/NaN explicitly — sound.     |
| 2 | `(count (asc v))`        | `OP_COUNT(v)`            | none                                          | Sort doesn't change cardinality; `count` returns `len` either way (including null-bearing rows). |
| 3 | `(count (desc v))`       | `OP_COUNT(v)`            | none                                          | Same.                                                                                            |
| 4 | `(count (reverse v))`    | `OP_COUNT(v)`            | none                                          | Same.                                                                                            |
| 5 | `(first (asc v))`        | `OP_MIN(v)`              | `!(v->attrs & RAY_ATTR_HAS_NULLS)` statically | Without the gate, semantics diverge: `(first (asc v))` returns null on null-bearing input (asc places nulls first per `xasc`); `OP_MIN` skips nulls. |
| 6 | `(last (asc v))`         | `OP_MAX(v)`              | `!(v->attrs & RAY_ATTR_HAS_NULLS)` statically | Same nulls reasoning.                                                                            |

### Precondition for rows 5 & 6

The precondition reads the input vector's `attrs` bitfield. It returns
`true` only when the input's null-state is **statically known** to be
clean. Specifically:

- For an `OP_SCAN` whose source column carries `RAY_ATTR_HAS_NULLS == 0`
  on its column header — return `true`.
- For a constant vector with no `RAY_ATTR_HAS_NULLS` bit — return
  `true`.
- For any other input (lazy, computed, transformed by an op whose null
  propagation isn't tracked) — return `false`. The slow form runs.

This is the safe-default discipline: we only fire the rewrite when
we can prove the precondition holds; otherwise we leave the original
shape alone. False negatives (missed rewrites) are acceptable; false
positives (incorrect results) are not.

## Tests

### Behavioural-equivalence tests

A new file `test/rfl/ops/idiom.rfl` covers each row with the standard
`expr -- expected` DSL. For each idiom:

- Empty input.
- Single-element input.
- All-equal input.
- Mixed input.
- **Null-bearing input** for rows 5–6 (must verify the slow form
  still runs — i.e. the result equals `(first (asc v))` not
  `OP_MIN(v)`).
- Negative-numbers / boundary values where applicable.

Per the test-harness memo, `.rfl` files are auto-discovered by
`test/main.c`. Run via `make test` or
`./rayforce.test -f idiom`.

### No structural-shape tests in v1

Asserting "the rewriter actually fired" via dump-string comparison is
brittle (any future opcode rename breaks the test). If we want firing
assertions later, the precedent is per-pass `ray_profile_tick` plus a
counter — out of scope for v1.

## Profiling & observability

- `ray_profile_span_start("idiom")` / `ray_profile_span_end("idiom")`
  around the pass body.
- `ray_profile_tick("idiom rewrite")` after the pass.

Both follow the convention every other pass in `ray_optimize` uses
(`opt.c:2018-2059`). The `name` field on each idiom row is reserved
for future `--debug-opt` style output; v1 does not consume it but the
field is part of the schema so we don't have to expand it later.

## Edge cases & failure modes

| Case                                        | Behaviour                                                                                              |
|---------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `node->inputs[0] == NULL`                   | Skip the row (defensive; should never happen on a well-formed graph).                                  |
| Rewrite callback returns `NULL`             | Treat as "doesn't apply"; continue chain. Used for guard-via-callback rather than precondition-field.  |
| `graph_alloc_node_opt` returns `NULL` (OOM) | Rewrite callback returns `NULL`; original shape executes. Optimization is best-effort, never required. |
| Multiple rows match (shouldn't happen)      | First-match-wins (chain order). Document by ordering rows from most-specific to least-specific.        |
| `node` already `OP_FLAG_DEAD`               | Walk skips dead nodes — same convention as other passes.                                               |
| Cyclic graph (shouldn't happen)             | Visited-set guards the post-order walk against re-entry.                                               |

## Out of scope (explicitly deferred)

- **Grouped form.** Rewrites inside `OP_GROUP`'s `agg_ins[]`. Group has
  its own optimised path.
- **Multi-input / binary idioms.** The schema supports a `child1_op`
  field but v1 only matches on `child0_op` (left-deep unary chains).
- **Fixpoint iteration.** Day-1 idioms don't cascade; matcher is
  single-shot.
- **New executor opcodes.** Every day-1 row maps to an existing
  `OP_*`. The next obvious wave (`count(where …) → OP_COUNT_WHERE`,
  algebraic reductions like `sum(c*v) → c*sum(v)`) needs new executor
  surface and is its own design.
- **Surface-language exposure.** `count_distinct` / `countd` as a
  Rayfall function is a separate concern.
- **Pattern DSL.** Static C table is sufficient at our catalog size.

## Open questions

None at design time. All four design forks (matcher style, scope, catalog
size, null-handling) were closed during brainstorming.

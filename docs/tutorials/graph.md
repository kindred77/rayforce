# Graph Queries Tutorial

Model a social network as a graph and query it: find neighbors, traverse paths, detect communities, and rank influence — all from the Rayfall REPL.

Rayforce models graphs using its built-in Datalog engine. Nodes are entities (integer IDs), and edges are facts stored as **(entity, attribute, value)** triples. Recursive rules give you transitive closure, path finding, and pattern matching without writing loops. This tutorial walks through a complete social network example.

Prerequisites: you have [built Rayforce](../getting-started/quick-start.md) and can start the REPL with `./rayforce`.

## 1. Building a Graph

Create a small social network with 6 people and 10 directed "follows" edges. Each person is an entity with a `name` attribute, and each edge is a `follows` fact pointing to the target entity ID:

```lisp
; Create an empty graph database
(set db (datoms))

; Add 6 people (nodes)
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 4 'name 'Diana))
(set db (assert-fact db 5 'name 'Eve))
(set db (assert-fact db 6 'name 'Frank))

; Add 10 directed edges (follows relationships)
(set db (assert-fact db 1 'follows 2))  ; Alice  -> Bob
(set db (assert-fact db 1 'follows 3))  ; Alice  -> Charlie
(set db (assert-fact db 2 'follows 3))  ; Bob    -> Charlie
(set db (assert-fact db 2 'follows 4))  ; Bob    -> Diana
(set db (assert-fact db 3 'follows 5))  ; Charlie -> Eve
(set db (assert-fact db 4 'follows 5))  ; Diana  -> Eve
(set db (assert-fact db 4 'follows 6))  ; Diana  -> Frank
(set db (assert-fact db 5 'follows 6))  ; Eve    -> Frank
(set db (assert-fact db 6 'follows 1))  ; Frank  -> Alice
(set db (assert-fact db 3 'follows 1))  ; Charlie -> Alice
```

Verify the graph by listing all edges:

```lisp
(scan-eav db 'follows)
```

```text
┌─────┬───────────────────────────────┐
│  e  │               v               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 1   │ 2                             │
│ 1   │ 3                             │
│ 2   │ 3                             │
│ 2   │ 4                             │
│ 3   │ 5                             │
│ 4   │ 5                             │
│ 4   │ 6                             │
│ 5   │ 6                             │
│ 6   │ 1                             │
│ 3   │ 1                             │
├─────┴───────────────────────────────┤
│ 10 rows (10 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Each row is a directed edge: entity `e` follows entity `v`. The `scan-eav` function filters triples by attribute, returning only the `follows` edges.

## 2. One-Hop Neighbors

Query who Alice (entity 1) directly follows. The constant `1` in the entity position pins the query to Alice, and `?target` binds to each person she follows:

```lisp
(query db (find ?target ?name)
  (where (1 :follows ?target)
         (?target :name ?name)))
```

```text
┌─────────┬───────────────────────────┐
│ ?target │           ?name           │
│   i64   │            i64            │
├─────────┼───────────────────────────┤
│ 2       │ 165                       │
│ 3       │ 166                       │
├─────────┴───────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Alice follows Bob (2) and Charlie (3). The `?name` column shows symbol IDs — use `sym-name` to decode them (e.g. `(sym-name 165)` returns `Bob`).

You can also query in the reverse direction — who follows Eve (entity 5)?

```lisp
(query db (find ?follower)
  (where (?follower :follows 5)))
```

```text
┌─────────────────────────────────────┐
│              ?follower              │
│                 i64                 │
├─────────────────────────────────────┤
│ 3                                   │
│ 4                                   │
├─────────────────────────────────────┤
│ 2 rows (2 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

Charlie (3) and Diana (4) follow Eve.

## 3. Multi-Hop Traversal

Chain multiple patterns to traverse 2 hops. Find Alice's friends-of-friends — people followed by someone Alice follows:

```lisp
; Friends-of-friends: 2-hop traversal from Alice
(query db (find ?mid ?end)
  (where (1 :follows ?mid)
         (?mid :follows ?end)))
```

```text
┌──────┬──────────────────────────────┐
│ ?mid │             ?end             │
│ i64  │             i64              │
├──────┼──────────────────────────────┤
│ 2    │ 3                            │
│ 2    │ 4                            │
│ 3    │ 1                            │
│ 3    │ 5                            │
├──────┴──────────────────────────────┤
│ 4 rows (4 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Through Bob (2), Alice reaches Charlie (3) and Diana (4). Through Charlie (3), she reaches back to herself (1) and Eve (5).

For arbitrary-depth traversal, define a recursive rule. The `reaches` rule computes the transitive closure of `follows`:

```lisp
; Base case: direct follow
(rule (reaches ?a ?b) (?a :follows ?b))
; Recursive case: follow a chain
(rule (reaches ?a ?c) (?a :follows ?b) (reaches ?b ?c))

; Everyone reachable from Alice
(query db (find ?b) (where (reaches 1 ?b)))
```

```text
┌─────────────────────────────────────┐
│                 ?b                  │
│                 i64                 │
├─────────────────────────────────────┤
│ 1                                   │
│ 2                                   │
│ 3                                   │
│ 4                                   │
│ 5                                   │
│ 6                                   │
├─────────────────────────────────────┤
│ 6 rows (6 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

Alice can reach all 6 people (including herself via the cycle Frank→Alice). The Datalog engine evaluates recursive rules to a fixpoint — it keeps discovering new paths until no more are found.

## 4. Shortest Path

To find a specific path between two people, use a `friends-of-friends` rule that exposes intermediate nodes. Here we find 2-hop paths from Alice (1) to Eve (5):

```lisp
; Define a friends-of-friends rule
(rule (fof ?a ?mid ?c)
  (?a :follows ?mid)
  (?mid :follows ?c))

; Find 2-hop paths from Alice to Eve
(query db (find ?mid)
  (where (fof 1 ?mid 5)))
```

```text
┌─────────────────────────────────────┐
│                ?mid                 │
│                 i64                 │
├─────────────────────────────────────┤
│ 3                                   │
├─────────────────────────────────────┤
│ 1 rows (1 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

There is one 2-hop path: Alice → Charlie (3) → Eve. You can also check whether a direct (1-hop) path exists first:

```lisp
; Check for a direct edge from Alice to Eve
(query db (find ?x) (where (1 :follows 5) (1 :name ?x)))
```

```text
┌─────────────────────────────────────┐
│                 ?x                  │
│                 i64                 │
├─────────────────────────────────────┤
├─────────────────────────────────────┤
│ 0 rows (0 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

No direct edge — the shortest path from Alice to Eve is 2 hops (through Charlie). For weighted shortest paths and more advanced graph algorithms (A*, Yen's k-shortest), see the [Graph Algorithms](../graph/algorithms.md) C API reference.

## 5. Finding Influencers

Who has the most followers? Extract the edge list with `query`, then use `select` with `by:` to aggregate follower counts:

```lisp
; Get all follow edges as a table
(set edges (query db (find ?src ?dst)
  (where (?src :follows ?dst))))

; Count followers per person
(set followers (select {from:edges by: ?dst
  followers: (count ?src)}))

; Sort by follower count, highest first
(xdesc followers 'followers)
```

```text
┌──────┬──────────────────────────────┐
│ ?dst │          followers           │
│ i64  │             i64              │
├──────┼──────────────────────────────┤
│ 1    │ 2                            │
│ 3    │ 2                            │
│ 5    │ 2                            │
│ 6    │ 2                            │
│ 2    │ 1                            │
│ 4    │ 1                            │
├──────┴──────────────────────────────┤
│ 6 rows (6 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Four people tie with 2 followers each: Alice (1), Charlie (3), Eve (5), and Frank (6). Bob (2) and Diana (4) each have 1 follower. This is the in-degree of each node — a simple popularity metric.

You can also find people who both Alice and Bob follow (common connections):

```lisp
(query db (find ?common)
  (where (1 :follows ?common)
         (2 :follows ?common)))
```

```text
┌─────────────────────────────────────┐
│               ?common               │
│                 i64                 │
├─────────────────────────────────────┤
│ 3                                   │
├─────────────────────────────────────┤
│ 1 rows (1 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

Charlie (3) is the only person both Alice and Bob follow.

## 6. Community Detection

Find tightly-connected groups by detecting **triangles** — three people who all follow each other in a cycle. Define a rule that matches the pattern A→B→C→A:

```lisp
; A triangle is a directed 3-cycle
(rule (triangle ?a ?b ?c)
  (?a :follows ?b)
  (?b :follows ?c)
  (?c :follows ?a))

(query db (find ?a ?b ?c)
  (where (triangle ?a ?b ?c)))
```

```text
┌─────┬─────┬─────────────────────────┐
│ ?a  │ ?b  │           ?c            │
│ i64 │ i64 │           i64           │
├─────┼─────┼─────────────────────────┤
│ 1   │ 2   │ 3                       │
│ 2   │ 3   │ 1                       │
│ 3   │ 1   │ 2                       │
├─────┴─────┴─────────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

There is one triangle in the graph: Alice (1) → Bob (2) → Charlie (3) → Alice (1). The three rows are rotations of the same cycle. This group of three forms a tight-knit community.

You can also find **mutual follows** (bidirectional edges) — the strongest signal of a connection:

```lisp
(rule (mutual ?a ?b)
  (?a :follows ?b)
  (?b :follows ?a))

(query db (find ?a ?b)
  (where (mutual ?a ?b)))
```

```text
┌─────┬───────────────────────────────┐
│ ?a  │              ?b               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 1   │ 3                             │
│ 3   │ 1                             │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Alice (1) and Charlie (3) mutually follow each other — the only bidirectional pair in this network.

## 7. Putting It Together

A complete example that builds the graph, defines all rules, and runs a multi-step analysis. Copy and paste this into a `.rfl` file or the REPL:

```lisp
; === Build the social network ===
(set db (datoms))
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 4 'name 'Diana))
(set db (assert-fact db 5 'name 'Eve))
(set db (assert-fact db 6 'name 'Frank))
(set db (assert-fact db 1 'follows 2))
(set db (assert-fact db 1 'follows 3))
(set db (assert-fact db 2 'follows 3))
(set db (assert-fact db 2 'follows 4))
(set db (assert-fact db 3 'follows 5))
(set db (assert-fact db 4 'follows 5))
(set db (assert-fact db 4 'follows 6))
(set db (assert-fact db 5 'follows 6))
(set db (assert-fact db 6 'follows 1))
(set db (assert-fact db 3 'follows 1))

; === Define graph rules ===
(rule (reaches ?a ?b) (?a :follows ?b))
(rule (reaches ?a ?c) (?a :follows ?b) (reaches ?b ?c))
(rule (mutual ?a ?b) (?a :follows ?b) (?b :follows ?a))
(rule (triangle ?a ?b ?c)
  (?a :follows ?b) (?b :follows ?c) (?c :follows ?a))

; === Analysis ===

; 1. Follower counts (influence ranking)
(set edges (query db (find ?src ?dst) (where (?src :follows ?dst))))
(xdesc (select {from:edges by: ?dst followers: (count ?src)}) 'followers)

; 2. Triangles (tight communities)
(query db (find ?a ?b ?c) (where (triangle ?a ?b ?c)))

; 3. Mutual follows (strongest connections)
(query db (find ?a ?b) (where (mutual ?a ?b)))
```

This produces three result tables:

1. **Follower ranking:** Alice, Charlie, Eve, and Frank lead with 2 followers each
2. **Triangles:** One community cluster: Alice ↔ Bob ↔ Charlie
3. **Mutual follows:** Alice and Charlie have the strongest tie (bidirectional)

## Next Steps

- [**Building a Knowledge Base**](../guides/datalog.md) — More Datalog patterns: negation, retraction, the programmatic API
- [**Graph Algorithms (C API)**](../graph/algorithms.md) — BFS, shortest path, A*, PageRank, betweenness centrality, and MST via the C API
- [**CSR Storage**](../graph/storage.md) — How graphs are stored and persisted using compressed sparse row format
- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Tables, filtering, joins, pivots, and CSV I/O

# Building a Knowledge Base with Datalog

Rayforce includes a built-in Datalog engine that stores facts as entity-attribute-value (EAV) triples and evaluates rules to fixpoint. This guide walks through building a practical knowledge base from scratch.

## 1. Modeling Data as Triples

A Datalog database stores facts as **(entity, attribute, value)** triples. Create a database with `datoms` and add facts with `assert-fact`:

```lisp
; Create an empty Datalog database
(set db (datoms))

; Assert facts about entities
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 1 'role 'Engineer))
(set db (assert-fact db 1 'dept 'Platform))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 2 'role 'Manager))
(set db (assert-fact db 2 'dept 'Platform))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 3 'role 'Engineer))
(set db (assert-fact db 3 'dept 'Frontend))
```

Each `assert-fact` call adds one triple: `(entity-id, attribute, value)`. Entity IDs are integers you assign. Attributes and values are symbols.

### EAV vs Regular Tables

Use EAV (Datalog) when your data is **schema-flexible** — entities can have different attributes, and you need rule-based reasoning. Use regular tables when your data has a **fixed schema** and you need fast columnar analytics.

## 2. Querying Relationships

Use `query` with `find` and `where` clauses. Variables start with `?` and are bound by pattern matching across triples:

```lisp
; Find all names
(query db (find ?e ?n) (where (?e :name ?n)))
```

```text
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 1   │ 158                           │
│ 2   │ 163                           │
│ 3   │ 165                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

The value column shows internal symbol IDs (integers). Use `sym-name` to convert them to readable strings (see [Section 7](#sym-name)). You can join across multiple patterns to find related facts:

```lisp
; Find name and department for each person
(query db (find ?n ?d) (where (?e :name ?n) (?e :dept ?d)))
```

```text
┌─────┬───────────────────────────────┐
│ ?n  │              ?d               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 158 │ 162                           │
│ 163 │ 162                           │
│ 165 │ 166                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Filter by constant values in patterns:

```lisp
; Find only Engineers
(query db (find ?n) (where (?e :name ?n) (?e :role 'Engineer)))
```

```text
┌─────────────────────────────────────┐
│                 ?n                  │
│                 i64                 │
├─────────────────────────────────────┤
│ 158                                 │
│ 165                                 │
├─────────────────────────────────────┤
│ 2 rows (2 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

## 3. Writing Rules

Rules define derived relations. The pattern is: "if these patterns match, then this relation holds."

```lisp
; Define a "team-member" rule
(rule (team-member ?name ?dept)
  (?e :name ?name)
  (?e :dept ?dept))

; Use the rule in a query
(query db (find ?name ?dept) (where (team-member ?name ?dept)))
```

```text
┌───────┬─────────────────────────────┐
│ ?name │            ?dept            │
│  i64  │             i64             │
├───────┼─────────────────────────────┤
│ 158   │ 162                         │
│ 163   │ 162                         │
│ 165   │ 166                         │
├───────┴─────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Rules act like views — they are re-evaluated each time they appear in a query. Rules can reference other rules, enabling modular knowledge bases.

## 4. Recursive Queries

Rules can be recursive, enabling transitive closure. Here we model a management hierarchy where entity 1 manages 2, 2 manages 3, and 3 manages 4:

```lisp
(set db (datoms))
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 4 'name 'Diana))
(set db (assert-fact db 1 'manages 2))
(set db (assert-fact db 2 'manages 3))
(set db (assert-fact db 3 'manages 4))

; Base case: direct management
(rule (reports-to ?x ?y) (?x :manages ?y))
; Recursive case: transitive closure
(rule (reports-to ?x ?z) (?x :manages ?y) (reports-to ?y ?z))

(query db (find ?x ?y) (where (reports-to ?x ?y)))
```

```text
┌─────┬───────────────────────────────┐
│ ?x  │              ?y               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 1   │ 2                             │
│ 1   │ 3                             │
│ 2   │ 3                             │
│ 2   │ 4                             │
│ 3   │ 4                             │
│ 1   │ 4                             │
├─────┴───────────────────────────────┤
│ 6 rows (6 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Entity 1 (Alice) reaches all others transitively — including entity 4 (Diana) who is three levels down.

## 5. Negation

Use `not` in a `where` clause to find entities that *lack* a given attribute:

```lisp
(set db (datoms))
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 1 'certified 'true))
(set db (assert-fact db 3 'certified 'true))

; Find people WITHOUT certification
(query db (find ?n) (where (?e :name ?n) (not (?e :certified ?c))))
```

```text
┌─────────────────────────────────────┐
│                 ?n                  │
│                 i64                 │
├─────────────────────────────────────┤
│ 161                                 │
├─────────────────────────────────────┤
│ 1 rows (1 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

Only Bob (sym ID 161) lacks a `certified` attribute.

## 6. Retracting Facts

Remove facts with `retract-fact`:

```lisp
; Remove Bob's name fact
(set db (retract-fact db 2 'name 'Bob))

; Query again — Bob is gone
(query db (find ?e ?n) (where (?e :name ?n)))
```

The database is immutable in the functional sense — `retract-fact` returns a new database value, so previous versions are unaffected if you kept a reference.

## 7. Reading Sym IDs { #sym-name }

Query results show raw symbol IDs (integers) for string values. Use `sym-name` to convert an ID back to its symbol name, and `pull` to retrieve all attributes of an entity:

```lisp
; Pull all attributes for entity 1
(pull db 1)
```

```text
['name 158 'role 160 'dept 162]
```

```lisp
; Pull specific attributes
(pull db 1 [name role])
```

```text
['name 158 'role 160]
```

```lisp
; Convert a sym ID to its name
(sym-name 158)
```

```text
'Alice
```

The `pull` function returns a flat list of alternating attribute-symbol and value pairs. For string-valued attributes, the value is a symbol ID that you decode with `sym-name`.

## 8. Real-World Example: Org Chart

Let's build a complete org chart with a CEO, VPs, and team leads:

```lisp
(set db (datoms))

; CEO
(set db (assert-fact db 1 'name 'Elena))
(set db (assert-fact db 1 'title 'CEO))

; VPs
(set db (assert-fact db 2 'name 'Frank))
(set db (assert-fact db 2 'title 'VP_Engineering))
(set db (assert-fact db 3 'name 'Grace))
(set db (assert-fact db 3 'title 'VP_Sales))

; Team leads
(set db (assert-fact db 4 'name 'Henry))
(set db (assert-fact db 4 'title 'Tech_Lead))
(set db (assert-fact db 5 'name 'Iris))
(set db (assert-fact db 5 'title 'Sales_Lead))

; Reporting structure
(set db (assert-fact db 1 'manages 2))
(set db (assert-fact db 1 'manages 3))
(set db (assert-fact db 2 'manages 4))
(set db (assert-fact db 3 'manages 5))

; Rules
(rule (chain-of-command ?top ?bottom)
  (?top :manages ?bottom))
(rule (chain-of-command ?top ?bottom)
  (?top :manages ?mid)
  (chain-of-command ?mid ?bottom))
```

Direct reports:

```lisp
(query db (find ?mgr ?emp)
  (where (?mgr :manages ?emp)))
```

```text
┌──────┬──────────────────────────────┐
│ ?mgr │             ?emp             │
│ i64  │             i64              │
├──────┼──────────────────────────────┤
│ 1    │ 2                            │
│ 1    │ 3                            │
│ 2    │ 4                            │
│ 3    │ 5                            │
├──────┴──────────────────────────────┤
│ 4 rows (4 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Full chain of command (transitive closure):

```lisp
(query db (find ?top ?bottom)
  (where (chain-of-command ?top ?bottom)))
```

```text
┌──────┬──────────────────────────────┐
│ ?top │           ?bottom            │
│ i64  │             i64              │
├──────┼──────────────────────────────┤
│ 1    │ 2                            │
│ 1    │ 3                            │
│ 1    │ 4                            │
│ 1    │ 5                            │
│ 2    │ 4                            │
│ 3    │ 5                            │
├──────┴──────────────────────────────┤
│ 6 rows (6 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Who does the CEO (entity 1) manage, directly or indirectly?

```lisp
(query db (find ?emp) (where (chain-of-command 1 ?emp)))
```

```text
┌─────────────────────────────────────┐
│                ?emp                 │
│                 i64                 │
├─────────────────────────────────────┤
│ 2                                   │
│ 3                                   │
│ 4                                   │
│ 5                                   │
├─────────────────────────────────────┤
│ 4 rows (4 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

The CEO reaches all four employees through the recursive `chain-of-command` rule.

## Next Steps

- [**Datalog Reference**](datalog-reference.md) — Complete syntax for rules, queries, and built-in predicates
- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Tables, filtering, joins, and CSV I/O
- [**Analytics Cookbook**](analytics.md) — Time-series, pivots, ASOF joins, and running totals

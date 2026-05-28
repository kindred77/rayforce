# Datalog Tutorial

A hands-on walkthrough: assert facts, query patterns, define recursive rules, use negation, and pull entity attributes — all from the Rayfall REPL.

This tutorial assumes you have already [built Rayforce](../getting-started/quick-start.md) and can start the REPL with `./rayforce`. Every example below can be pasted directly into the REPL.

## 1. What is Datalog?

Datalog is a declarative query language based on pattern matching over facts. Instead of writing imperative loops, you declare *what* you want to find and the engine figures out *how*. Rayforce implements Datalog natively with semi-naive evaluation and stratified negation, operating directly over an entity-attribute-value (EAV) triple store.

## 2. Asserting Facts

All data lives in an EAV triple store created with `datoms`. Each fact is a triple: **entity** (integer ID), **attribute** (symbol), and **value** (integer or symbol). Use `assert-fact` to add triples:

```lisp
; Create an empty triple store
(set db (datoms))

; Add employees: entity ID, attribute, value
(set db (assert-fact db 100 'name 'Alice))
(set db (assert-fact db 100 'dept 'Engineering))
(set db (assert-fact db 101 'name 'Bob))
(set db (assert-fact db 101 'dept 'Sales))
(set db (assert-fact db 102 'name 'Carol))
(set db (assert-fact db 102 'dept 'Engineering))

; Reporting relationships (value is an entity ID)
(set db (assert-fact db 100 'boss 101))
(set db (assert-fact db 102 'boss 100))
```

Each `assert-fact` returns a new table (the store is immutable). View the raw triples by evaluating `db`:

```lisp
db
```

```text
┌─────┬──────┬────────────────────────┐
│  e  │  a   │           v            │
│ i64 │ sym  │          i64           │
├─────┼──────┼────────────────────────┤
│ 100 │ name │ ...                    │
│ 100 │ dept │ ...                    │
│ 101 │ name │ ...                    │
│ 101 │ dept │ ...                    │
│ 102 │ name │ ...                    │
│ 102 │ dept │ ...                    │
│ 100 │ boss │ 101                    │
│ 102 │ boss │ 100                    │
├─────┴──────┴────────────────────────┤
│ 8 rows (8 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

**Note:** Symbol values (like `'Alice`, `'Engineering`) are stored as intern IDs in the `v` column. Integer values (like the `boss` entity references) appear directly. This is an implementation detail — the query engine handles the mapping transparently.

## 3. Querying Facts

Use `query` with `find`/`where` clauses to pattern-match over the triple store. Variables start with `?`. The `eav` predicate matches entity-attribute-value triples:

### Find all employees

```lisp
(query db (find ?e ?name) (where (eav ?e 'name ?name)))
```

```text
┌─────┬───────────────────────────────┐
│ ?e  │             ?name             │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 100 │ ...                           │
│ 101 │ ...                           │
│ 102 │ ...                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

The query finds every entity `?e` that has a `name` attribute. The `?name` column contains the intern IDs for the symbol values.

### Find employees in Engineering

Add a second pattern to filter by department. Constants (like `'Engineering`) constrain the match:

```lisp
(query db
  (find ?e ?name)
  (where
    (eav ?e 'dept 'Engineering)
    (eav ?e 'name ?name)))
```

```text
┌─────┬───────────────────────────────┐
│ ?e  │             ?name             │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 100 │ ...                           │
│ 102 │ ...                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Only Alice (100) and Carol (102) are returned — Bob is in Sales.

### Find who reports to whom

Query the `boss` attribute to find reporting relationships. Since `boss` stores integer entity IDs, the values are directly readable:

```lisp
(query db
  (find ?e ?boss)
  (where (eav ?e 'boss ?boss)))
```

```text
┌─────┬───────────────────────────────┐
│ ?e  │             ?boss             │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 100 │ 101                           │
│ 102 │ 100                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Alice (100) reports to Bob (101), and Carol (102) reports to Alice (100).

## 4. Defining Rules

Rules create derived relations from existing facts. Use `rule` to define a named relation with a head and one or more body clauses:

```lisp
; "manages" means someone is your direct boss
(rule (manages ?mgr ?sub)
  (eav ?sub 'boss ?mgr))
```

This says: `?mgr` manages `?sub` if `?sub` has a `boss` attribute pointing to `?mgr`. Query it like any other predicate:

```lisp
(query db
  (find ?mgr ?sub)
  (where (manages ?mgr ?sub)))
```

```text
┌──────┬──────────────────────────────┐
│ ?mgr │             ?sub             │
│ i64  │             i64              │
├──────┼──────────────────────────────┤
│ 100  │ 102                          │
│ 101  │ 100                          │
├──────┴──────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Rules persist across queries in the same session. Define them once, use them in any subsequent `query` call.

## 5. Recursive Queries

Datalog supports recursion natively. Define a transitive closure of the `boss` relationship with two rules — a base case and a recursive step:

```lisp
; Base case: direct report
(rule (chain ?top ?sub)
  (eav ?sub 'boss ?top))

; Recursive step: indirect report
(rule (chain ?top ?sub)
  (eav ?sub 'boss ?mid)
  (chain ?top ?mid))
```

Now find everyone who reports to Bob (101), directly or indirectly:

```lisp
(query db
  (find ?sub)
  (where (chain 101 ?sub)))
```

```text
┌─────────────────────────────────────┐
│                ?sub                 │
│                 i64                 │
├─────────────────────────────────────┤
│ 100                                 │
│ 102                                 │
├─────────────────────────────────────┤
│ 2 rows (2 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

Alice (100) reports directly to Bob, and Carol (102) reports to Alice — so both are in Bob's reporting chain. The engine evaluates to a fixpoint using semi-naive evaluation, adding new tuples each iteration until no more can be derived.

## 6. Negation

Use `not` to exclude matching patterns. Find all employees who are NOT in Sales:

```lisp
(query db
  (find ?e ?name)
  (where
    (eav ?e 'name ?name)
    (not (eav ?e 'dept 'Sales))))
```

```text
┌─────┬───────────────────────────────┐
│ ?e  │             ?name             │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 100 │ ...                           │
│ 102 │ ...                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Bob (101) is excluded because he is in Sales. Negation is stratified — the engine ensures negated predicates are fully evaluated before being used, preventing circular reasoning.

## 7. The Pull API

Use `pull` to retrieve all attributes of an entity as a dictionary:

```lisp
(pull db 100)
```

```text
{name:... dept:... boss:101}
```

The result is a dict mapping attribute names to values. You can also pull a specific subset of attributes:

```lisp
(pull db 100 ['name 'dept])
```

```text
{name:... dept:...}
```

Symbol values appear as intern IDs in the dict. Integer values (like the `boss` reference) appear directly.

You can also use `scan-eav` for attribute-level lookups. Given an attribute, it returns an `[e v]` table:

```lisp
(scan-eav db 'name)
```

```text
┌─────┬───────────────────────────────┐
│  e  │               v               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 100 │ ...                           │
│ 101 │ ...                           │
│ 102 │ ...                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Given an entity and attribute, `scan-eav` returns the single value:

```lisp
(scan-eav db 100 'boss)
```

```text
101
```

## 8. Complete Example: Org Chart

Build a full org chart, define rules for reporting chains and department heads, then query the structure:

```lisp
; Build the org chart
(set db (datoms))

; CEO
(set db (assert-fact db 100 'name  'Alice))
(set db (assert-fact db 100 'title 'CEO))
(set db (assert-fact db 100 'dept  'Executive))

; Engineering VP
(set db (assert-fact db 101 'name  'Bob))
(set db (assert-fact db 101 'title 'VP))
(set db (assert-fact db 101 'dept  'Engineering))
(set db (assert-fact db 101 'boss  100))

; Engineering Lead
(set db (assert-fact db 102 'name  'Carol))
(set db (assert-fact db 102 'title 'Lead))
(set db (assert-fact db 102 'dept  'Engineering))
(set db (assert-fact db 102 'boss  101))

; Developer
(set db (assert-fact db 103 'name  'Dave))
(set db (assert-fact db 103 'title 'Dev))
(set db (assert-fact db 103 'dept  'Engineering))
(set db (assert-fact db 103 'boss  102))

; Sales VP
(set db (assert-fact db 104 'name  'Eve))
(set db (assert-fact db 104 'title 'VP))
(set db (assert-fact db 104 'dept  'Sales))
(set db (assert-fact db 104 'boss  100))

; Sales Rep
(set db (assert-fact db 105 'name  'Frank))
(set db (assert-fact db 105 'title 'Rep))
(set db (assert-fact db 105 'dept  'Sales))
(set db (assert-fact db 105 'boss  104))
```

Define the reporting chain (transitive closure):

```lisp
(rule (chain ?top ?sub)
  (eav ?sub 'boss ?top))

(rule (chain ?top ?sub)
  (eav ?sub 'boss ?mid)
  (chain ?top ?mid))
```

Query everyone who reports to the CEO (100):

```lisp
(query db
  (find ?sub)
  (where (chain 100 ?sub)))
```

```text
┌─────────────────────────────────────┐
│                ?sub                 │
│                 i64                 │
├─────────────────────────────────────┤
│ 101                                 │
│ 102                                 │
│ 103                                 │
│ 104                                 │
│ 105                                 │
├─────────────────────────────────────┤
│ 5 rows (5 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

All five employees report to Alice (the CEO), directly or through the chain.

Define a rule for department heads (employees with title VP):

```lisp
(rule (dept_head ?e ?d)
  (eav ?e 'dept ?d)
  (eav ?e 'title 'VP))

(query db
  (find ?e ?d)
  (where (dept_head ?e ?d)))
```

```text
┌─────┬───────────────────────────────┐
│ ?e  │              ?d               │
│ i64 │              i64              │
├─────┼───────────────────────────────┤
│ 101 │ ...                           │
│ 104 │ ...                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Bob (101, Engineering VP) and Eve (104, Sales VP) are identified as department heads.

Pull the full profile of a department head:

```lisp
(pull db 101)
```

```text
{name:... title:... dept:... boss:100}
```

## Next Steps

- [**Datalog Rules & Queries**](../guides/datalog-reference.md) — Full reference for the Datalog engine: predicates, stratification, and the programmatic API
- [**Building a Knowledge Base**](../guides/datalog.md) — Patterns for modeling domain knowledge with EAV triples
- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Tables, filtering, grouping, joins, and CSV I/O
- [**Functions Reference**](../language/functions.md) — Complete list of all built-in functions

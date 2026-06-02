# Joins

Equi-joins, left outer joins, as-of joins for time-series alignment, and window joins with aggregation. All join types compile to the DAG and execute via radix-partitioned hash join.

## Inner Join

The `inner-join` function performs an equi-join on one or more key columns. Only rows with matching keys in both tables appear in the result.

### Signature

`(inner-join [keys] left right)`

Join two tables on shared key columns. Returns a table with all columns from both sides (key columns appear once).

### Examples

```lisp
(set orders (table [id sym qty]
    (list [1 2 3 4] [AAPL GOOG MSFT AAPL] [100 200 50 150])))

(set prices (table [sym price]
    (list [AAPL GOOG TSLA] [150 280 245])))

; Join on sym — MSFT and TSLA have no match, so they are excluded
(inner-join [sym] orders prices)
; id sym  qty price
; -- ---  --- -----
;  1 AAPL 100   150
;  2 GOOG 200   280
;  4 AAPL 150   150
```

Join on multiple keys:

```lisp
(set x (table [a b c]
    (list (take (list "aa" "bb" "cc") 10)
          (take [I J K] 10)
          (til 10))))

(set y (table [a b d]
    (list (take (list "aa" "bb") 10)
          (take [I J K] 10)
          (til 10))))

; Join on both [a b]
(inner-join [a b] x y)
```

## Left Join

The `left-join` function keeps all rows from the left table. Unmatched rows have null in the right-side columns.

### Signature

`(left-join [keys] left right)`

Left outer join. All left rows are preserved; unmatched right columns are null.

### Examples

```lisp
(left-join [sym] orders prices)
; id sym  qty price
; -- ---  --- -----
;  1 AAPL 100   150
;  2 GOOG 200   280
;  3 MSFT  50     0  ← 0 (no MSFT in prices)
;  4 AAPL 150   150
```

Left join on multiple keys:

```lisp
(left-join [a b] x y)
```

## As-of Join

The `asof-join` function is designed for time-series data. For each row in the left table, it finds the most recent matching row in the right table where the time column is less than or equal to the left's time value. The last column in the key list is the temporal column; any preceding keys are exact-match equality keys.

### Signature

`(asof-join [keys time-col] left right)`

As-of join for time-series alignment. The last key is the temporal column. For each left row, returns the most recent right row where `right.time <= left.time`.

Equality keys are **optional**:

- `(asof-join [time-col] left right)` — a lone time key runs an un-partitioned as-of join across all rows.
- `(asof-join [keys time-col] left right)` — preceding keys partition the join; within each partition the temporal match applies.

### Examples

```lisp
; Trades and quotes for time-series alignment
(set trades (table [Sym Ts Price]
    (list [AAPL AAPL MSFT]
          [10:00:01 10:00:05 10:00:04]
          [190.05 190.1 410.25])))

(set quotes (table [Sym Ts Bid Ask]
    (list [AAPL AAPL AAPL MSFT]
          [09:59:55 10:00:03 10:00:07 10:00:02]
          [189.9 190 190.05 410.1]
          [190.1 190.2 190.25 410.3])))

; As-of join: for each trade, find prevailing quote
(asof-join [Sym Ts] trades quotes)
; Sym  Ts       Price  Bid   Ask
; ---  --------  ----- ----- -----
; AAPL 10:00:01 190.05 189.9 190.1   ← quote from 09:59:55
; AAPL 10:00:05  190.1 190   190.2   ← quote from 10:00:03
; MSFT 10:00:04 410.25 410.1 410.3   ← quote from 10:00:02
```

### Pre-sorted fast-path

The as-of executor normally sorts both inputs by `(equality-keys, time)` on every call. [Column attributes](attributes.md) let it skip that sort when the data already satisfies the required ordering:

- **Un-partitioned** — if both input time columns carry the `sorted` attribute, both per-join sorts are skipped (O(n+m) instead of O(n log n)).
- **Partitioned** — if a single numeric equality key carries the `parted` attribute and its time column is non-descending within each part, that side's sort is skipped.

These are pure opt-in acceleration: when the attribute is absent the executor falls back to the usual sort-merge, so results are identical either way.

```lisp
; Stamp the time column as sorted, then run an un-partitioned as-of join.
(set Ls (table [Time Price] (list (.attr.set 'sorted [10:00:01.000 10:00:03.000]) [100.0 101.0])))
(.attr.get (at Ls 'Time))                                        ; ⇒ ['sorted]

; Partitioned: a parted numeric equality key (parted is numeric-only — not a symbol).
(set Lgp (table [ID Time Price]
    (list (.attr.set 'parted [1 1 2 2])
          [10:00:01.000 10:00:03.000 10:00:01.000 10:00:02.000]
          [1.0 2.0 3.0 4.0])))
(asof-join [ID Time] Lgp Rgp)
```

See [Column Attributes](attributes.md) for how to stamp and verify these properties.

### Large-scale Usage

```lisp
; 10 million row as-of join
(set n 10000000)
(set tsym (take (concat (take 'AAPL 99) (take 'MSFT 1)) n))
(set ttime (+ 09:00:00 (as 'TIME (/ (* (til n) 3) 10))))
(set trades (table [Sym Ts Price] (list tsym ttime (+ 10 (til n)))))
; ... build quotes similarly ...
(asof-join [Sym Ts] trades quotes)
```

### Null-key semantics

As-of-join treats **NULL keys as never matching**:

- A left row with a null in any equality key or in the time column is kept in the output (left-outer behaviour) but the right-side columns are explicitly null.
- A right row with a null in any equality key or time column is skipped entirely during the merge walk — it cannot become the best match for any left row.
- Right-side columns for unmatched rows are filled with null (null bit set on the destination vector), distinguishable from genuine zero values via `(nil? x)`.

This means a null time in the left table is never silently treated as time zero, and two null sym keys on opposite sides do not merge into a spurious match.

## Window Join

The `window-join` function extends the as-of join with a time window. For each left row, it finds all right rows within a time interval and applies aggregation functions to the matched rows.

### Signature

`(window-join [keys time-col] intervals left right {col: (agg expr) ...})`

Window join with per-row time intervals. `intervals` is a list of two vectors `[lo hi]` defining the time window for each left row. Aggregation functions are applied to all right rows falling within each window.

### Examples

```lisp
(set trades (table [Sym Time Price]
    (list [x x x]
          [12:00:01 12:00:04 12:00:06]
          [89.17 70.5 80.54])))

(set quotes (table [Sym Time Size]
    (list [x x x x x x x x x x]
          [12:00:00 12:00:01 12:00:02 12:00:03 12:00:04
           12:00:05 12:00:06 12:00:07 12:00:08 12:00:09]
          [928 528 648 914 918 626 577 817 620 698])))

; Build per-row intervals: [lo, hi] for each trade
(set intervals (map-left + [-1000 1000] (at trades 'Time)))

; Window join: min bid and max ask within each window
(window-join [Sym Time] intervals trades quotes
    {size_min: (min Size) size_max: (max Size)})
```

### Large-scale Window Join

```lisp
(set n 100000)
(set trades (table [Sym Ts Price]
    (list tsym ttime (+ 10 (til n)))))
(set quotes (table [Sym Ts Bid Ask]
    (list bsym btime bid ask)))

; Build intervals from the trades timestamp
(set intervals (map-left + [-1000 1000] (at trades 'Ts)))

(window-join [Sym Ts] intervals trades quotes
    {bid: (min Bid) ask: (max Ask)})
```

## How Joins Compile to the DAG

All join operations compile to the Rayforce execution DAG. The optimizer and executor handle the details:

1. **DAG construction** — `inner-join` and `left-join` emit `OP_JOIN` nodes with join type flags. `asof-join` emits `OP_ASOF_JOIN`. `window-join` emits `OP_WINDOW_JOIN`.
2. **Optimizer** — Predicate pushdown moves filters before the join when possible. Type inference propagates column types through join boundaries. SIP (Sideways Information Passing) can prune the build side using selection bitmaps.
3. **Execution** — Equi-joins use a radix-partitioned hash join: the build side is partitioned by hash, then each morsel from the probe side looks up matches in the corresponding partition. As-of and window joins use sorted merge with binary search on the temporal column — the as-of executor skips the per-join sort when the inputs carry the `sorted` / `parted` [attributes](attributes.md) described above.

!!! note "Performance note"
    For large joins, ensure key columns use efficient types. Symbol columns (`RAY_SYM`) are dictionary-encoded integers and join fastest. String columns (`RAY_STR`) work but require hash comparison of variable-length data.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `inner-join` | `(inner-join [keys] left right)` | Equi-join; only matching rows |
| `left-join` | `(left-join [keys] left right)` | Left outer join; all left rows preserved |
| `asof-join` | `(asof-join [keys time-col] left right)` | Time-series alignment; most recent match |
| `window-join` | `(window-join [keys tc] intervals left right {aggs})` | Window aggregation over time intervals |

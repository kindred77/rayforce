# Data Loading & Storage Tutorial

Read CSV files, explore and filter data, serialize tables to binary, persist with splayed storage, and export results — all from the Rayfall REPL.

This tutorial assumes you have [built Rayforce](../getting-started/quick-start.md) and can start the REPL with `./rayforce`. We use `/tmp/rayforce-test/` as the working directory for all file operations.

## 1. Reading CSV Files

Create a CSV file on disk, then load it with `.csv.read`. The first row is treated as column headers, and types are inferred automatically:

```lisp
(set trades (.csv.read "/tmp/rayforce-test/trades.csv"))
trades
```

Assuming `trades.csv` contains:

```text
Symbol,Price,Qty
AAPL,150.5,100
GOOG,280.0,200
MSFT,420.0,50
AAPL,155.0,300
GOOG,275.5,150
```

Output:

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

String-like columns (e.g. ticker symbols, names) are loaded as `sym` (dictionary-encoded symbols). Numeric columns are inferred as `i64` or `f64` depending on whether any value has a decimal point.

You can also create tables in-memory and skip CSV entirely:

```lisp
(set trades (table [Symbol Price Qty]
  (list
    [AAPL GOOG MSFT AAPL GOOG]
    [150.5 280.0 420.0 155.0 275.5]
    [100 200 50 300 150])))
```

## 2. Exploring Data

Use `count`, `type`, and `show` to inspect a loaded table:

```lisp
(count trades)
```

```text
5
```

```lisp
(type trades)
```

```text
TABLE
```

`show` limits the display to the first *n* rows — useful for large datasets:

```lisp
(show trades 3)
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## 3. Filtering Rows

Use `select` with a `where:` clause to filter rows. Find all trades where the price exceeds 200:

```lisp
(select {from:trades where: (> Price 200.0)})
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## 4. Grouping and Aggregation

Group by a column and compute aggregates with `by:`:

```lisp
(select {from:trades by: Symbol
         total_qty: (sum Qty)
         avg_price: (avg Price)})
```

```text
┌────────┬───────────┬────────────────┐
│ Symbol │ total_qty │   avg_price    │
│  sym   │    i64    │      f64       │
├────────┼───────────┼────────────────┤
│ AAPL   │ 400       │ 152.75         │
│ GOOG   │ 350       │ 277.75         │
│ MSFT   │ 50        │ 420.0          │
├────────┴───────────┴────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Available aggregation functions: `sum`, `avg`, `min`, `max`, `count`, `first`, `last`, `med`.

## 5. Binary Serialization

Use `ser` to serialize any Rayforce object to a compact binary format (a U8 byte vector), and `de` to deserialize it back. This is useful for caching, IPC, or storing intermediate results:

```lisp
(set bytes (ser trades))
(count bytes)
```

```text
197
```

The entire table is encoded in 197 bytes. Deserialize with `de`:

```lisp
(set restored (de bytes))
restored
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

The deserialized table is identical to the original — all types, column names, and values are preserved.

## 6. Splayed Tables

Splayed tables store each column as a separate file on disk. This is Rayforce's native columnar format — faster than CSV and preserving exact types. Use `.db.splayed.set` to save and `.db.splayed.get` to load:

```lisp
(.db.splayed.set "/tmp/rayforce-test/trades_db" trades)
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

On disk, this creates one file per column plus a symbol table:

```text
/tmp/rayforce-test/trades_db/
  Symbol    Price    Qty    sym    sym.lk
```

Load it back in a new session (or the same one) with `.db.splayed.get`:

```lisp
(set loaded (.db.splayed.get "/tmp/rayforce-test/trades_db"))
loaded
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

All types are preserved exactly — `f64` stays `f64`, `sym` stays `sym`. No type inference needed on reload.

## 7. Writing CSV

Export any table to CSV with `.csv.write`. The return value `0` indicates success:

```lisp
(set big (select {from:trades where: (> Price 200.0)}))
(.csv.write big "/tmp/rayforce-test/big_trades.csv")
```

```text
0
```

Verify by reading it back:

```lisp
(.csv.read "/tmp/rayforce-test/big_trades.csv")
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Note: float values written without a fractional part (e.g. `280.0`) may be read back as `i64` since the CSV reader infers the narrowest fitting type. Use splayed tables when exact type preservation matters.

## 8. Complete Example

End-to-end workflow: create data, save as CSV, reload, filter, serialize, deserialize, and verify:

```lisp
; 1. Create a table
(set trades (table [Symbol Price Qty]
  (list
    [AAPL GOOG MSFT AAPL GOOG]
    [150.5 280.0 420.0 155.0 275.5]
    [100 200 50 300 150])))

; 2. Save to CSV
(.csv.write trades "/tmp/rayforce-test/trades.csv")

; 3. Reload from CSV
(set loaded (.csv.read "/tmp/rayforce-test/trades.csv"))

; 4. Filter: only trades above 200
(set big (select {from:loaded where: (> Price 200.0)}))

; 5. Serialize to binary
(set bytes (ser big))

; 6. Deserialize and verify
(set restored (de bytes))
(count restored)

; 7. Save as splayed table for fast reload
(.db.splayed.set "/tmp/rayforce-test/big_db" big)

; 8. Load splayed table back
(.db.splayed.get "/tmp/rayforce-test/big_db")
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  sym   │  f64  │        i64         │
├────────┼───────┼────────────────────┤
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## Storage Format Comparison

| Format | Function | Preserves Types | Best For |
|---|---|---|---|
| CSV | `.csv.read` / `.csv.write` | No (re-inferred on load) | Interoperability, human-readable data |
| Binary | `ser` / `de` | Yes | Caching, IPC, embedding in messages |
| Splayed | `.db.splayed.set` / `.db.splayed.get` | Yes | Persistent storage, fast reload, mmap |

## Next Steps

- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Tables, filtering, joins, pivots, and sorting
- [**Data Persistence**](../guides/storage.md) — Partitioned storage and advanced file I/O
- [**Analytics Cookbook**](../guides/analytics.md) — Time-series, top-N, ASOF joins
- [**Functions Reference**](../language/functions.md) — Complete list of all built-in functions

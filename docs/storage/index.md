# Storage

Columnar file I/O, splayed tables, date-partitioned storage, symbol table persistence, and CSV import/export — everything for getting data in and out of Rayforce.

## Columnar Files

Rayforce's native binary format stores a single vector per file. Each file contains a 32-byte header followed by the raw element data and an optional null bitmap. The format is designed for direct memory mapping — no deserialization needed.

### File Structure

```c
/*
 * Column file layout:
 *
 * Bytes  0-31:   ray_t header (type, attrs, len, etc.)
 * Bytes 32-N:    element data (len * elem_size bytes)
 * Bytes N-M:     null bitmap (if RAY_ATTR_HAS_NULLS + RAY_ATTR_NULLMAP_EXT)
 *                  — (len + 7) / 8 bytes
 */
```

### C API

| Function | Description |
|---|---|
| `ray_col_save(vec, path)` | Write a vector to a column file. Handles slices, external null bitmaps, and string pools transparently. |
| `ray_col_load(path)` | Read a column file into a heap-allocated vector. The file is read entirely into memory. |
| `ray_col_mmap(path)` | Memory-map a column file for zero-copy access. The returned vector points directly into the mapped file pages. Ideal for large datasets that exceed available RAM. |

```c
// Save a vector
ray_t* prices = ray_vec_from_raw(RAY_F64, data, 1000000);
ray_err_t err = ray_col_save(prices, "db/trades/price");

// Load into memory
ray_t* loaded = ray_col_load("db/trades/price");

// Memory-map for zero-copy access
ray_t* mapped = ray_col_mmap("db/trades/price");
// mapped->data points into file pages — no allocation
```

!!! note "Memory-mapped vectors"
    Memory-mapped vectors have `mmod = 1` in their header, distinguishing them from heap-allocated vectors. The buddy allocator skips them during free. Slices of mmap'd vectors retain a reference to the parent mapping.

## Splayed Tables

A splayed table stores each column as a separate file in a directory. This is the standard on-disk representation for a Rayforce table. The schema (column names) is stored in a `.d` file alongside the data files.

### Directory Layout

```text
db/trades/
  .d                 — I64 vector of column name symbol IDs
  sym                — SYM column (stock tickers)
  price              — F64 column (trade prices)
  qty                — I64 column (quantities)
  time               — TIMESTAMP column
```

### C API

| Function | Description |
|---|---|
| `ray_splay_save(tbl, dir, sym_path)` | Save a table as a splayed directory. Each column becomes a file named after its column symbol. Pass `sym_path` to also save the symbol table. |
| `ray_splay_load(dir, sym_path)` | Load a splayed table from a directory. Columns are memory-mapped by default. Pass `sym_path` to load the associated symbol table. |

```c
// Save table to disk
ray_err_t err = ray_splay_save(table, "db/trades", "db/sym");

// Load table (columns are mmap'd)
ray_t* trades = ray_splay_load("db/trades", "db/sym");
```

!!! note "Rayfall builtins"
    Use `.db.splayed.set` and `.db.splayed.get` from Rayfall to save and load splayed tables without writing C code. See the [Rayfall Storage Builtins](#rayfall-storage) section below.

## Date-Partitioned Tables

For large time-series datasets, Rayforce supports date-partitioned storage. Data is split into directories named by date, each containing a splayed table for that day's data.

### Directory Layout

```text
db/trades/
  sym                 — shared symbol table
  2024.01.15/
    sym
    price
    qty
    time
  2024.01.16/
    sym
    price
    qty
    time
  2024.01.17/
    ...
```

### Loading Partitioned Data

The `ray_part_load()` function scans all partition directories, memory-maps every column file, and assembles them into a single logical table with [parted columns](../data-types/collections.md#parted-types) and a virtual `MAPCOMMON` date column.

```c
// C API: load all partitions
ray_t* trades = ray_part_load("db", "trades");

// The result is a single table with:
// - A MAPCOMMON 'date' column derived from directory names
// - Parted columns (RAY_PARTED_BASE + base_type) for each data column
// - All segments are memory-mapped — no data copy
```

!!! note "Rayfall builtin"
    Use `.db.parted.get` from Rayfall to load partitioned tables: `(.db.parted.get "db" 'trades)`. See the [Rayfall Storage Builtins](#rayfall-storage) section below.

### Partition Pruning

The query optimizer recognizes predicates on the `MAPCOMMON` column and eliminates entire partitions from the scan plan. This means a query filtering on a single date in a year of data only touches 1/365th of the files on disk — with zero per-row cost for the pruned partitions.

## Symbol Table Persistence

The global symbol intern table maps strings to integer IDs. When saving data to disk, the symbol table must be persisted so that symbol vectors can be correctly interpreted when reloaded.

### Append-Only .sym Files

Symbol files use an append-only format. New symbols are appended to the end of the file without rewriting existing entries. This makes concurrent writes safe and enables incremental updates.

| Function | Description |
|---|---|
| `ray_sym_save(path)` | Persist the current global symbol table to a `.sym` file |
| `ray_sym_load(path)` | Load a symbol table from disk, merging with any existing entries |
| `ray_sym_intern(str, len)` | Intern a string, returning its integer ID |
| `ray_sym_find(str, len)` | Look up a string without interning (returns -1 if absent) |
| `ray_sym_str(id)` | Resolve an ID back to its string |

```c
// Save symbol table alongside data
ray_sym_save("db/sym");

// On startup, load symbols before loading data
ray_sym_load("db/sym");
```

### Concurrency and Integrity

- **File locking** — `flock()` on POSIX, `LockFileEx()` on Windows. Multiple processes can safely read the symbol file; writes acquire an exclusive lock.
- **Corruption detection** — symbol files include checksums. If a file is truncated or corrupted, `ray_sym_load()` returns `RAY_ERR_CORRUPT` and leaves the in-memory table unchanged.
- **Arena backing** — interned strings are allocated from a dedicated arena (`ray_arena_t`), making bulk allocation fast and ensuring all strings are freed together when the symbol table is destroyed.

## CSV Import and Export

Rayforce includes a high-performance CSV loader with parallel parsing, automatic type inference, and null handling. No external libraries are used — the parser operates directly on memory-mapped file contents.

### Reading CSV Files

```lisp
; Basic CSV load — auto-detect types, comma delimiter, header row
ray> (set data (.csv.read "trades.csv"))
sym  price   qty  date
----------------------------
AAPL 150.25  100  2024.01.15
GOOG 140.50  200  2024.01.15
MSFT 380.00   50  2024.01.15
```

### C API

| Function | Description |
|---|---|
| `ray_read_csv(path)` | Load a CSV file with default options: comma delimiter, first row as header, and automatic type inference. Empty fields are treated as null. |
| `ray_read_csv_opts(path, delim, header, col_types, n_types)` | Load with custom options: delimiter character, whether first row is a header, explicit column type array (`int8_t*`), and number of type entries. Pass `NULL, 0` for automatic type inference. |
| `ray_write_csv(table, path)` | Write a table to a CSV file with header row and comma delimiter. |

```c
// Default options
ray_t* data = ray_read_csv("trades.csv");

// Tab-delimited, no header, auto type inference
ray_t* tsv = ray_read_csv_opts("data.tsv", '\t', false, NULL, 0);

// Write results back
ray_write_csv(result, "output.csv");
```

### Type Inference

The CSV loader samples values in each column and infers types in priority order:

1. **BOOL** — `true`/`false`, `1`/`0`
2. **I64** — integer values within 64-bit range
3. **F64** — floating-point values
4. **DATE** — `YYYY-MM-DD` format (hyphen-separated)
5. **TIMESTAMP** — date + time with nanosecond precision
6. **SYM** — short repeated strings (auto-interned as symbols)
7. **STR** — fallback for everything else

### Parallel Parsing

The CSV file is memory-mapped and split into chunks. Multiple threads parse chunks in parallel, with a merge step that reconciles column types and combines partial results. For large files (100 MB+), this delivers near-linear speedup with core count.

### Null Handling

Empty fields and fields matching the null string (default: `""`) are recognized as null values. The loader sets the appropriate null bitmap bits on the resulting vectors and marks them with `RAY_ATTR_HAS_NULLS`.

### Symbol Merge

When loading CSV data with symbol columns, the loader interns all unique strings into the global symbol table. If a symbol table was previously loaded from disk, existing IDs are preserved and new symbols are appended.

## Cross-Platform File I/O

All file operations go through a portable abstraction layer in `src/store/fileio.{h,c}` that handles platform differences:

| Feature | POSIX | Windows |
|---|---|---|
| File locking | `flock()` | `LockFileEx()` |
| Sync to disk | `fsync()` | `FlushFileBuffers()` |
| Atomic rename | `rename()` | `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` |
| Memory mapping | `mmap()` | `CreateFileMapping()` + `MapViewOfFile()` |

!!! note "Atomic writes"
    When saving column files, Rayforce writes to a temporary file first, calls `fsync()`, then atomically renames it to the target path. This prevents data corruption if the process is interrupted during a write.

## Rayfall Storage Builtins {#rayfall-storage}

Rayfall provides three builtins for working with on-disk storage directly from the REPL or scripts.

### `.db.splayed.set` — Save a Splayed Table

Writes a table as column files to a directory. A symbol table file is saved automatically alongside the data.

```lisp
(set t (table [City Temp Rain]
  (list [London Paris Tokyo]
        [15 22 28]
        [120.5 60.3 200.1])))

; Save to /tmp/weather
(.db.splayed.set "/tmp/weather" t)
```

An optional third argument specifies a custom symbol table path: `(.db.splayed.set "/tmp/weather" t "/tmp/my_sym")`. Without it, a `sym` file is created in the table directory.

### `.db.splayed.get` — Load a Splayed Table

Loads a splayed table from a directory. Columns are memory-mapped for zero-copy access.

```lisp
(.db.splayed.get "/tmp/weather")
```

```text
┌─────────┬──────┬────────────────────┐
│  City   │ Temp │        Rain        │
│   sym   │ i64  │        f64         │
├─────────┼──────┼────────────────────┤
│ 'London │ 15   │ 120.5              │
│ 'Paris  │ 22   │ 60.3               │
│ 'Tokyo  │ 28   │ 200.1              │
└─────────┴──────┴────────────────────┘
```

An optional second argument specifies the symbol table path: `(.db.splayed.get "/tmp/weather" "/tmp/my_sym")`.

### `.db.parted.get` — Load a Partitioned Table

Loads a date-partitioned table. The first argument is the database root directory, the second is the table name as a quoted symbol.

```lisp
; Load the "trades" partitioned table from db/
(.db.parted.get "db" 'trades)
```

This scans all date-named subdirectories under `db/trades/`, memory-maps every column, and returns a single table with a virtual date column. Partition pruning applies to subsequent queries.

## Symbol Table Management

Symbol tables are persisted automatically when you use `.db.splayed.set`. A `sym` file is written into the table directory containing all interned symbol strings. When loading with `.db.splayed.get` or `.db.parted.get`, the symbol table is loaded first so that symbol columns decode correctly.

At the C API level, `ray_sym_save(path)` and `ray_sym_load(path)` handle persistence directly. The format is append-only — new symbols are appended without rewriting existing entries, making concurrent access safe.

## Working Example: Round-Trip

A complete workflow: create a table, save it to disk, load it back, and verify the data.

```lisp
; 1. Create a table
(set t (table [City Temp Rain]
  (list [London Paris Tokyo]
        [15 22 28]
        [120.5 60.3 200.1])))

; 2. Save as splayed table
(.db.splayed.set "/tmp/weather" t)

; 3. Load it back
(set loaded (.db.splayed.get "/tmp/weather"))

; 4. Query the loaded table
(select {from:loaded where: (> Temp 20)})
```

```text
┌────────┬──────┬────────────────────┐
│  City  │ Temp │        Rain        │
│  sym   │ i64  │        f64         │
├────────┼──────┼────────────────────┤
│ 'Paris │ 22   │ 60.3               │
│ 'Tokyo │ 28   │ 200.1              │
└────────┴──────┴────────────────────┘
```

The loaded table is fully functional — you can run `select`, `update`, joins, and aggregations on it just like an in-memory table. Symbol columns resolve correctly because the symbol table was saved and loaded alongside the data.

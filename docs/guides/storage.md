# Data Persistence Guide

Rayforce provides multiple storage layers, from simple CSV for interchange to high-performance columnar files for production workloads. CSV I/O is available from Rayfall; the columnar storage layers are currently accessible through the C API.

## 1. CSV I/O { #csv-io }

The simplest way to persist and exchange data. Available directly from Rayfall.

### Reading CSV

```lisp
(set data (.csv.read "path/to/data.csv"))
```

The reader:

- Treats the first row as column headers
- Infers column types automatically: `i64`, `f64`, `date`, `time`, `timestamp`, `sym`
- Uses parallel parsing for large files
- Handles null values (empty cells)

### Writing CSV

```lisp
(.csv.write table "path/to/output.csv")
```

### Example: Round-Trip

```lisp
(set data (table [Name Score Grade]
  (list [Alice Bob Charlie]
        [95 87 92]
        [A B A])))

(.csv.write data "/tmp/grades.csv")
(.csv.read "/tmp/grades.csv")
```

```text
┌─────────┬───────┬───────────────────┐
│  Name   │ Score │       Grade       │
│   sym   │  i64  │        sym        │
├─────────┼───────┼───────────────────┤
│ Alice   │ 95    │ A                 │
│ Bob     │ 87    │ B                 │
│ Charlie │ 92    │ A                 │
├─────────┴───────┴───────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

**Note:** Float values without a fractional part (e.g., 150.0) will be read back as integers. The CSV reader always infers the narrowest matching type.

## 2. Symbol Table Persistence { #symbol-table }

Rayforce maintains a global symbol intern table for `sym` columns. When saving and loading columnar data, the symbol table must be persisted alongside it so that symbol IDs remain meaningful.

The C API provides:

```c
// Save the global symbol table to a file
ray_err_t ray_sym_save(const char* path);

// Load symbols from a file (merges with the current table)
ray_err_t ray_sym_load(const char* path);
```

The symbol file uses an append-only format: new symbols are appended on save, and loading merges them into the running table. File locking (`flock` on POSIX, `LockFileEx` on Windows) ensures safe concurrent access.

In typical usage, you do not call these directly — the splayed and partitioned table functions handle symbol persistence automatically.

## 3. Columnar Files { #columnar-files }

!!! note "C API only"

    The functions below are in internal headers (`src/store/col.h`, `src/store/splay.h`, `src/store/part.h`). Compile with `-Isrc` and include the specific header. These are not yet exposed as Rayfall builtins.

A **column file** stores a single vector (column) in Rayforce's native binary format. This is the building block for all higher-level storage.

```c
// Save a vector to a column file
ray_err_t ray_col_save(ray_t* vec, const char* path);

// Load a vector from a column file
ray_t* ray_col_load(const char* path);

// Memory-map a column file (zero-copy, read-only)
ray_t* ray_col_mmap(const char* path);
```

The file format is compact: a header with type and length, followed by the raw element data. For string columns (`RAY_STR`), the pool data is written after the element array.

The difference between `ray_col_load` and `ray_col_mmap`:

| Function | Memory | Access | Use case |
|---|---|---|---|
| `ray_col_load` | Copies data into heap | Read/write | Data you will modify |
| `ray_col_mmap` | Maps file directly | Read-only | Large data, zero startup cost |

## 4. Splayed Tables { #splayed-tables }

A **splayed table** stores each column as a separate file inside a directory. This allows loading individual columns on demand instead of the entire table.

```c
// Save a table as a splayed directory
ray_err_t ray_splay_save(ray_t* tbl, const char* dir,
                         const char* sym_path);

// Load a splayed table from a directory
ray_t* ray_splay_load(const char* dir,
                      const char* sym_path);
```

The directory layout looks like:

```text
trades/
  .d            # schema (column name symbol IDs)
  Symbol        # sym column
  Price         # f64 column
  Qty           # i64 column
```

Pass a `sym_path` to automatically save/load the symbol table alongside the data. Pass `NULL` if you manage symbols separately.

## 5. Partitioned Tables { #partitioned-tables }

For very large datasets, Rayforce supports **date-partitioned** storage. Each partition is a splayed table inside a date-named subdirectory.

```c
// Load a partitioned table
ray_t* ray_part_load(const char* db_root,
                     const char* table_name);
```

Expected directory layout:

```text
db/
  sym                  # shared symbol table
  2024.01.01/
    trades/
      Symbol
      Price
      Qty
  2024.01.02/
    trades/
      Symbol
      Price
      Qty
  ...
```

The loader:

1. Loads the shared `sym` file from `db_root`
2. Discovers date directories (sorted numerically)
3. Loads each partition as a splayed table
4. Adds a virtual `Date` column from the directory name
5. Concatenates all partitions into a single table

This is the recommended layout for time-series data that grows continuously. New partitions can be added without rewriting existing data.

For datasets larger than available RAM, Rayforce can process partitioned tables one segment at a time using **block offloading** — streaming through partitions without loading them all at once. The optimizer's partition pruning pass can skip entire partitions that don't match filter predicates. See [Block Offloading](../architecture/offloading.md) for details.

## 6. Memory-Mapped I/O { #mmap }

Memory-mapped I/O (`mmap`) lets Rayforce access on-disk data without copying it into memory. The operating system pages data in on demand as it is accessed.

### Benefits

- **Zero startup cost** — opening a 10 GB file is instant; only accessed pages are loaded
- **Shared memory** — multiple processes can mmap the same file without duplicating data in RAM
- **OS-managed caching** — the kernel manages which pages stay in memory vs. on disk

### Trade-offs

- **Read-only** — mmap vectors cannot be modified (COW will copy if you try)
- **Random I/O** — if access patterns are truly random across a large file, performance can be worse than sequential reads

### Usage

Use `ray_col_mmap` for individual columns:

```c
ray_t* prices = ray_col_mmap("db/2024.01.01/trades/Price");
```

Splayed table loading can also use mmap internally. For most analytics workloads — scans, filters, aggregations — mmap provides the best combination of startup time and throughput.

## Choosing a Storage Layer { #choosing }

| Layer | Interface | Best for |
|---|---|---|
| CSV | Rayfall | Data interchange, small datasets, prototyping |
| Columnar | C API | Single-vector persistence, embedding in applications |
| Splayed tables | C API | Multi-column tables, column-selective loading |
| Partitioned tables | C API | Large time-series, append-only growth, date-range queries |

## Next Steps

- [**Storage Reference**](../storage/index.md) — Detailed file format specifications and API reference
- [**Core C API**](../c-api/core.md) — Working with `ray_t` objects, vectors, and tables in C
- [**Memory Model**](../architecture/memory.md) — Buddy allocator, arenas, COW, and per-VM heaps
- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Hands-on introduction to Rayfall

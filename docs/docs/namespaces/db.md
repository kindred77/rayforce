# `.db.*` — on-disk tables

Persist and reload tables. Rayforce stores tables in two on-disk shapes:

- **Splayed**: one directory per table, one file per column, plus a `.d` schema file and a `.sym` symbol-table file. The standard layout for a single-table dataset.
- **Partitioned (parted)**: a database root containing one subdirectory per partition (date or other numeric/dotted name); each partition contains splayed-style table directories. A single shared `.sym` file sits at the root. The query optimizer prunes partitions when predicates select on the virtual `MAPCOMMON` partition column.

The `get` builtins memory-map every column file — load is constant-time regardless of dataset size. `set` writes a table's columns to a splayed directory.

!!! note "Restricted under `-U`"
    `.db.splayed.set` is `RAY_FN_RESTRICTED` (writes to disk). The `get` builtins are read-only and unrestricted.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.db.splayed.set`](#db-splayed-set) | variadic | restricted | Save a table to a splayed directory. |
| [`.db.splayed.get`](#db-splayed-get) | variadic | — | Load a splayed table (columns mmap'd). |
| [`.db.parted.get`](#db-parted-get) | variadic | — | Load a partitioned table by name from a db root. |

## `.db.splayed.set` { #db-splayed-set }

Signatures:

- `(.db.splayed.set "dir" tbl)` — write columns to `dir/`; emit a `.sym` file in the same directory.
- `(.db.splayed.set "dir" tbl "sym_path")` — write the symbol table to a custom location (typically a shared db-root `.sym`).

Returns `tbl` (retained), so the call can be threaded inside a larger expression.

Errors: `type` (dir/sym not strings, tbl not a table), `domain` (paths empty / too long), and any `io`/`oom`/`corrupt` error code surfaced by the splayed writer.

```lisp
(set t (table [city temp rain]
  (list ['London 'Paris 'Tokyo] [15 22 28] [120.5 60.3 200.1])))

(.db.splayed.set "/tmp/weather" t)
```

## `.db.splayed.get` { #db-splayed-get }

Signatures:

- `(.db.splayed.get "dir")` — look for `dir/.sym` automatically.
- `(.db.splayed.get "dir" "sym_path")` — use a custom symbol-table path (e.g. a shared db-root sym).

Returns a `table` with every column memory-mapped — zero allocation per row.

```lisp
(set loaded (.db.splayed.get "/tmp/weather"))
(select {from: loaded where: (> temp 20)})
```

## `.db.parted.get` { #db-parted-get }

Signature: `(.db.parted.get "db_root" 'tbl_name)`. The table name **must** be a quoted symbol atom (e.g. `'trades`), not a string.

Returns a single logical table assembled from every partition directory under `db_root/tbl_name/`. The result carries a virtual `MAPCOMMON` partition column derived from directory names, and every data column is a `RAY_PARTED_*` view over the segment files. Partition pruning kicks in automatically for `select` predicates on the virtual column.

```lisp
(set trades (.db.parted.get "/data/db" 'trades))

;; Partition prune — only the matching day's files are touched.
(select {from: trades where: (= date 2024.01.15)})
```

Errors: `domain` (arity != 2 or `tbl_name` invalid), `type` (root not a string or name not a sym), `name` (sym ID unknown).

## See also

- [`.csv.splayed`](csv.md#csv-splayed) / [`.csv.parted`](csv.md#csv-parted) — stream CSV directly into splayed / parted layouts.
- [Columnar Storage](../storage/index.md) — file format, mmap semantics, symbol-table persistence.

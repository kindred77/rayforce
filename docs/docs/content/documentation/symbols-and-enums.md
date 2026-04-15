# :material-alphabetical-variant: Symbols, Enums, and Symfiles

This guide provides an in-depth explanation of how symbols work in RayforceDB, why enumerations (enums) are essential for efficient storage, and how symfiles enable consistent symbol management across partitioned tables.

## Understanding Symbols

### What is a Symbol?

A **symbol** is an interned string — a unique, immutable string that exists only once in memory. In RayforceDB (and kdb+), symbols are the primary way to represent categorical data like stock tickers, country codes, or any repeated string values.

```clj
↪ 'AAPL
AAPL
↪ (type 'AAPL)
symbol
↪ ['AAPL 'GOOG 'MSFT]
[AAPL GOOG MSFT]
```

### How Symbol Interning Works

When you create a symbol, RayforceDB performs **string interning**:

1. **Hash Lookup**: The string is hashed and looked up in a global symbol table (hash table)
2. **Deduplication**: If the string already exists, the existing pointer is returned
3. **Storage**: If new, the string is copied to a dedicated memory pool and added to the hash table
4. **Pointer Return**: The symbol value is actually a memory pointer (64-bit integer) to the interned string

```
Symbol Table (Hash Table)
┌─────────┬───────────────────┐
│  Hash   │     Pointer       │
├─────────┼───────────────────┤
│ 0x3A2F  │ → "AAPL\0"        │
│ 0x7B1C  │ → "GOOG\0"        │
│ 0x5D9E  │ → "MSFT\0"        │
└─────────┴───────────────────┘

String Pool (Memory Region)
┌──────────────────────────────────────────┐
│ [4]AAPL\0[4]GOOG\0[4]MSFT\0...           │
└──────────────────────────────────────────┘
  ↑         ↑         ↑
  ptr1      ptr2      ptr3
```

The `[4]` prefix stores the string length for efficient operations.

### Why Interning Matters

| Aspect | Without Interning | With Interning |
|--------|-------------------|----------------|
| **Storage** | Each string stored separately | Single copy per unique string |
| **Comparison** | Character-by-character O(n) | Pointer comparison O(1) |
| **Memory** | Duplicates waste space | Deduplicated, compact |
| **Hashing** | Compute hash each time | Pre-computed at creation |

### The Critical Problem: Process-Local Symbol Tables

**Here's the key insight that newcomers often miss:**

The symbol table is **process-local**. Each RayforceDB process maintains its own independent symbol table in memory. When a process starts:

1. A fresh, empty symbol table is created
2. Symbols are assigned pointers dynamically as they're encountered
3. The same string may get **different pointers** in different process instances

```
Process A                    Process B
┌─────────────────┐         ┌─────────────────┐
│ "AAPL" → 0x1000 │         │ "AAPL" → 0x2000 │
│ "GOOG" → 0x1008 │         │ "MSFT" → 0x2008 │
│ "MSFT" → 0x1010 │         │ "GOOG" → 0x2010 │
└─────────────────┘         └─────────────────┘
```

**This means you cannot simply save a symbol vector to disk and load it into another process** — the raw pointers would be meaningless garbage!

---

## The Solution: Enumerations (Enums)

### What is an Enum?

An **enum** (enumeration) is a special data type that represents symbols as indices into a reference symbol vector, rather than as raw pointers.

```clj
↪ (set sym ['AAPL 'GOOG 'MSFT])
[AAPL GOOG MSFT]

↪ (enum 'sym ['AAPL 'GOOG 'MSFT 'AAPL 'MSFT 'MSFT 'GOOG])
'sym#[AAPL GOOG MSFT AAPL MSFT MSFT GOOG]
```

### Enum Internal Structure

An enum consists of two parts:

1. **Key**: A symbol pointing to the reference symbol vector (e.g., `'sym`)
2. **Value**: A vector of integer indices into the reference vector

```
Reference Vector (sym):
Index:  0       1       2
Value: AAPL   GOOG    MSFT

Enum Data:
Original:  AAPL  GOOG  MSFT  AAPL  MSFT  MSFT  GOOG
Indices:   [0,    1,    2,    0,    2,    2,    1]
```

### Why Enums are Essential for Storage

Consider storing a table with 10 million rows where a Symbol column has only 100 unique values:

| Storage Method | Per-Value Size | Total Size |
|---------------|----------------|------------|
| **Raw Pointers** | 8 bytes (64-bit pointer) | 80 MB |
| **Enum (I64 indices)** | 8 bytes (64-bit index) | 80 MB |
| **Enum (I32 indices)** | 4 bytes (32-bit index) | 40 MB |
| **Enum (I16 indices)** | 2 bytes (16-bit index) | 20 MB |

But the real benefit is **portability**: indices are just numbers that can be saved to disk and loaded by any process, as long as the reference vector is available.

### Creating and Using Enums

```clj
;; Create a symbol vector and bind it to a name
↪ (set universe ['AAPL 'GOOG 'MSFT 'AMZN 'META])
[AAPL GOOG MSFT AMZN META]

;; Enumerate a symbol vector against the universe
↪ (set data (enum 'universe ['AAPL 'MSFT 'AAPL 'GOOG]))
'universe#[AAPL MSFT AAPL GOOG]

;; Access works transparently using 'at' function
↪ (at data 2)
AAPL

;; Type is ENUM
↪ (type data)
ENUM
```

---

## Symfiles: Persistent Symbol Storage

### What is a Symfile?

A **symfile** is a file that stores the reference symbol vector for enumerated columns in a splayed or parted table. It's the bridge that allows symbol data to be persisted and loaded correctly.

When you save a table with symbol columns:

1. All unique symbols across symbol columns are collected
2. These distinct symbols are saved to the symfile
3. Symbol columns are converted to enums with indices into the symfile
4. The enum indices are saved as the column data

!!! important "No Symbol Columns = No Symfile"
    If your table has **no symbol columns**, no symfile will be created on disk. The symfile is only generated when there are symbol columns that need to be enumerated. Tables containing only numeric types (I64, F64, Timestamp, etc.) don't require symbol management.

### Symfile Structure

```
Database Directory Structure:
/tmp/db/
├── sym                    ← The symfile (symbol vector)
├── 2024.01.01/
│   └── trades/
│       ├── .d            ← Column schema
│       ├── OrderId       ← GUID column
│       ├── Symbol        ← Enum column (indices into sym)
│       ├── Price         ← Float column
│       └── Timestamp     ← Timestamp column
├── 2024.01.02/
│   └── trades/
│       ├── .d
│       ├── OrderId
│       ├── Symbol        ← Same indices reference same sym file
│       ├── Price
│       └── Timestamp
```

### How Symfiles Enable Loading

When loading a table:

1. **Load Symfile**: The `sym` file is loaded and bound to variable `sym` in the environment
2. **Load Columns**: Each column file is loaded
3. **Enum Resolution**: Enum columns use `'sym` as their key, resolving indices through the loaded symfile

```clj
;; Loading a splayed table (symfile loaded automatically)
↪ (get-splayed "/tmp/db/2024.01.01/trades/")

;; Or explicitly specify symfile location
↪ (get-splayed "/tmp/db/2024.01.01/trades/" "/tmp/db/sym")
```

---

## Why Shared Symfiles for Parted Tables

### The Partitioned Table Challenge

A **parted table** is a table split across multiple directories (usually by date). Each partition could theoretically have its own symbol values:

```
Day 1: Trades with symbols [AAPL, GOOG]
Day 2: Trades with symbols [MSFT, GOOG, META]
Day 3: Trades with symbols [AAPL, AMZN]
```

If each partition had its own independent symfile:

```
Partition 1 sym: [AAPL, GOOG]        → AAPL=0, GOOG=1
Partition 2 sym: [MSFT, GOOG, META]  → MSFT=0, GOOG=1, META=2
Partition 3 sym: [AAPL, AMZN]        → AAPL=0, AMZN=1
```

**Problem**: The same symbol has different indices in different partitions! You cannot query across partitions consistently.

### The Shared Symfile Solution

With a **shared symfile**, all partitions use the same symbol-to-index mapping:

```
Shared sym: [AAPL, GOOG, MSFT, META, AMZN]
            AAPL=0, GOOG=1, MSFT=2, META=3, AMZN=4

Partition 1: Symbol indices [0, 1]      (AAPL, GOOG)
Partition 2: Symbol indices [2, 1, 3]   (MSFT, GOOG, META)
Partition 3: Symbol indices [0, 4]      (AAPL, AMZN)
```

Now `GOOG` is consistently index `1` everywhere!

### Creating Parted Tables with Shared Symfiles

```clj
;; Define database and symfile paths
(set dbpath "/tmp/db")
(set sympath (format "%/sym" dbpath))

;; Save partitions with shared symfile
(set-splayed (format "%/2024.01.01/trades/" dbpath) day1-trades sympath)
(set-splayed (format "%/2024.01.02/trades/" dbpath) day2-trades sympath)
(set-splayed (format "%/2024.01.03/trades/" dbpath) day3-trades sympath)
```

The `set-splayed` function with a symfile path:

1. Reads existing symbols from the symfile (if it exists)
2. Finds new symbols not already in the file
3. Appends only the new symbols
4. Saves enum columns using indices into the combined symbol vector

---

## Why You Can't Just Load Symbol Vectors

### The Fundamental Problem

Newcomers often ask: *"Why can't I just save `['AAPL 'GOOG 'MSFT]` to a file and load it back?"*

The answer lies in understanding what a symbol actually **is** at runtime:

```clj
;; What you see:
↪ ['AAPL 'GOOG]
[AAPL GOOG]
```

What's actually stored (simplified):

```
[0x7fff80001000, 0x7fff80001008]  ← Raw memory pointers!
```

If you save these pointers and load them in a new process:

1. The new process has a **different** symbol table
2. The memory addresses are meaningless (likely unmapped or pointing to random data)
3. Any operation on these "symbols" would crash or return garbage

### The Correct Approach

Instead of saving raw symbol vectors, RayforceDB:

1. **Serializes symbol strings**: When saving, symbols are converted to their string representations
2. **Re-interns on load**: When loading, strings are interned into the new process's symbol table
3. **Uses enums for columns**: Table columns store indices, not pointers

```
Save Process:
Symbol Vector → Extract Strings → Write ["AAPL", "GOOG", "MSFT"]

Load Process:
Read ["AAPL", "GOOG", "MSFT"] → Intern Each → New Symbol Vector
```

### Why Tables Can Be Loaded Correctly

When you load a splayed table:

```clj
↪ (get-splayed "/tmp/db/trades/")
```

Behind the scenes:

1. **Symfile loaded**: `/tmp/db/trades/sym` is read as raw strings and interned into the current process's symbol table, creating a new symbol vector bound to `sym`

2. **Enum columns loaded**: The `Symbol` column is loaded as an enum with key `'sym` and values as integer indices

3. **Resolution works**: When you access `table.Symbol`, the enum looks up indices in the freshly-loaded `sym` vector, returning properly interned symbols for the current process

### Memory Mapping Considerations

For efficiency, RayforceDB memory-maps column files directly. This works for numeric types but requires special handling for symbols:

| Data Type | Memory Mapping | Notes |
|-----------|---------------|-------|
| **I64, F64, etc.** | Direct mapping | Values are portable |
| **Symbol** | Cannot map directly | Pointers are process-local |
| **Enum** | Map indices + load sym | Indices are portable |

This is another reason why splayed tables use enums: the index data can be memory-mapped for zero-copy access, while only the (relatively small) symfile needs parsing.

---

## Summary

| Concept | Purpose |
|---------|---------|
| **Symbol** | Interned string for O(1) comparison and deduplication |
| **Symbol Table** | Process-local hash table mapping strings to pointers |
| **Enum** | Indices into a symbol vector, enabling portable storage |
| **Symfile** | Persisted symbol vector for enum resolution |
| **Shared Symfile** | Single symfile for all partitions, ensuring consistent indices |

### Key Takeaways for Newcomers

1. **Symbols are pointers** — they only make sense within a single process
2. **Enums are portable** — integer indices can be saved and loaded anywhere
3. **Symfiles are conditional** — they are only created when tables contain symbol columns
4. **Symfiles are essential for symbols** — they provide the symbol-to-index mapping for loading
5. **Shared symfiles enable queries** — consistent indices across partitions allow cross-partition operations
6. **Always use shared symfiles with parted tables** — otherwise each partition is an isolated island

```clj
;; Correct: Use shared symfile for parted tables
(set-splayed "/tmp/db/2024.01.01/trades/" t "/tmp/db/sym")
(set-splayed "/tmp/db/2024.01.02/trades/" t "/tmp/db/sym")

;; Load parted table (automatically uses shared sym)
(get-parted "/tmp/db/" 'trades)
```

### :material-arrow-right: Related Topics

- [:material-text: Symbol](./data-types/symbol.md) — Basic symbol type
- [:material-numeric: Enum](./data-types/enum.md) — Enumeration type
- [:material-table: Table](./data-types/table.md) — Table storage and loading functions

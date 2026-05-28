# Functions Reference

Complete reference for all Rayfall built-in functions. Each entry shows the function name, arity type (unary/binary/variadic), flags, description, and example usage.

!!! note "Legend"
    Functions marked **atomic** auto-map element-wise over vectors. Functions marked **aggr** reduce vectors to scalars. Functions marked **special** receive unevaluated arguments.

## Arithmetic

All arithmetic operators are **atomic** — they auto-map over vectors and broadcast scalars.

| Function | Type | Description | Example |
|---|---|---|---|
| `+` | binary | Addition | `(+ 3 4)` → `7` |
| `-` | binary | Subtraction | `(- 10 3)` → `7` |
| `*` | binary | Multiplication | `(* 3 4)` → `12` |
| `/` | binary | Integer division (floor) | `(/ 7 2)` → `3` |
| `%` | binary | Modulo | `(% 7 3)` → `1` |
| `div` | binary | Float division (always f64) | `(div 7 2)` → `3.5` |
| `neg` | unary | Negate | `(neg 5)` → `-5` |
| `round` | unary | Round to nearest integer | `(round 3.7)` → `4.0` |
| `floor` | unary | Floor (round down) | `(floor 3.7)` → `3.0` |
| `ceil` | unary | Ceiling (round up) | `(ceil 3.2)` → `4.0` |
| `xbar` | binary | Round down to nearest multiple (bucketing) | `(xbar [3 7 12] 5)` → `[0 5 10]` |

Vector examples:

```lisp
(+ [1 2 3] [10 20 30])   ; [11 22 33]
(* [1 2 3] 10)            ; [10 20 30]  scalar broadcast
(neg [1 -2 3])             ; [-1 2 -3]
(xbar [3 15 27] 10)      ; [0 10 20]
```

## Comparison

All comparison operators are **atomic** and return boolean results.

| Function | Type | Description | Example |
|---|---|---|---|
| `>` | binary | Greater than | `(> 5 3)` → `true` |
| `<` | binary | Less than | `(< 3 5)` → `true` |
| `>=` | binary | Greater than or equal | `(>= 5 5)` → `true` |
| `<=` | binary | Less than or equal | `(<= 3 5)` → `true` |
| `==` | binary | Equal | `(== 3 3)` → `true` |
| `!=` | binary | Not equal | `(!= 3 4)` → `true` |
| `within` | binary | Check which vector elements fall within a range | `(within [1 5 10] [3 7])` → `[false true false]` |

## Logic

| Function | Type | Description | Example |
|---|---|---|---|
| `and` | binary | Logical AND | `(and true false)` → `false` |
| `or` | binary | Logical OR | `(or true false)` → `true` |
| `not` | unary | Logical NOT | `(not true)` → `false` |

## Aggregation

Aggregation functions are marked **aggr** and reduce vectors to scalar values. Used in `select` with `by:` for group-by aggregation.

| Function | Type | Description | Example |
|---|---|---|---|
| `sum` | unary, aggr | Sum of all elements | `(sum [1 2 3])` → `6` |
| `count` | unary, aggr | Count of elements | `(count [1 2 3])` → `3` |
| `avg` | unary, aggr | Arithmetic mean | `(avg [1 2 3])` → `2.0` |
| `min` | unary, aggr | Minimum value | `(min [3 1 2])` → `1` |
| `max` | unary, aggr | Maximum value | `(max [3 1 2])` → `3` |
| `med` | unary, aggr | Median value | `(med [1 3 2])` → `2` |
| `dev` | unary, aggr | Standard deviation | `(dev [1 2 3])` → `0.816...` |
| `first` | unary | First element of vector | `(first [10 20 30])` → `10` |
| `last` | unary | Last element of vector | `(last [10 20 30])` → `30` |

```lisp
; Group-by aggregation example
(select {from: trades
         by:   {sym: sym}
         cols: {hi: (max price) lo: (min price) n: (count price)}})
```

## Higher-Order Functions

Functions that take other functions as arguments.

| Function | Type | Description | Example |
|---|---|---|---|
| `map` | variadic | Apply function to each element | `(map (fn [x] (* x 2)) [1 2 3])` → `[2 4 6]` |
| `pmap` | variadic | Parallel map (multi-threaded) | `(pmap (fn [x] (* x x)) [1 2 3])` → `[1 4 9]` |
| `filter` | binary | Keep elements where boolean mask is true | `(filter [1 2 3 4] (> [1 2 3 4] 2))` → `[3 4]` |
| `fold` | variadic | Reduce with function and initial value | `(fold + 0 [1 2 3])` → `6` |
| `fold-left` | variadic | Left-associative fold | `(fold-left - 10 [1 2 3])` → `4` |
| `fold-right` | variadic | Right-associative fold | `(fold-right - 10 [1 2 3])` → `-8` |
| `scan` | variadic | Running fold (returns all intermediate results) | `(scan + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-left` | variadic | Left-to-right running fold | `(scan-left + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-right` | variadic | Right-to-left running fold | `(scan-right + (enlist 1 2 3))` → `[6 5 3]` |
| `apply` | variadic | Zip-apply function pairwise over two lists | `(apply + (enlist 1 2) (enlist 3 4))` → `[4 6]` |
| `map-left` | variadic | Map with left argument fixed | `(map-left + 10 [1 2 3])` → `[11 12 13]` |
| `map-right` | variadic | Map with right argument fixed | `(map-right - [10 20 30] 5)` → `[5 15 25]` |

## Collection Operations

Operations on vectors as collections.

| Function | Type | Description | Example |
|---|---|---|---|
| `distinct` | unary | Remove duplicates | `(distinct [1 2 2 3])` → `[1 2 3]` |
| `in` | binary | Membership test (element in vector) | `(in 2 [1 2 3])` → `true` |
| `except` | binary | Set difference | `(except [1 2 3] [2])` → `[1 3]` |
| `union` | binary | Set union | `(union [1 2] [2 3])` → `[1 2 3]` |
| `sect` | binary | Set intersection | `(sect [1 2 3] [2 3 4])` → `[2 3]` |
| `take` | binary | Take first/last N elements | `(take [10 20 30] 2)` → `[10 20]` |
| `at` | binary | Index into vector | `(at [10 20 30] 1)` → `20` |
| `find` | binary | Find index of value | `(find [10 20 30] 20)` → `1` |
| `reverse` | unary | Reverse order | `(reverse [1 2 3])` → `[3 2 1]` |
| `til` | unary | Range [0..n) | `(til 5)` → `[0 1 2 3 4]` |
| `enlist` | variadic | Wrap value(s) in a vector | `(enlist 1 2 3)` → `[1 2 3]` |
| `concat` | binary | Concatenate two vectors | `(concat [1 2] [3 4])` → `[1 2 3 4]` |
| `raze` | unary | Flatten a list of vectors into one | `(raze (list [1 2] [3 4]))` → `[1 2 3 4]` |
| `where` | unary | Indices where boolean vector is true | `(where [true false true])` → `[0 2]` |
| `group` | unary | Group indices by value | `(group [A B A])` → dict of groups |
| `diverse` | unary | Check if all elements are unique | `(diverse [1 2 3])` → `true` |
| `rand` | binary | N random values from range or vector | `(rand 3 100)` → 3 random ints 0..99 |
| `bin` | binary | Binary search (left boundary) | `(bin [10 20 30] 25)` → `1` |
| `binr` | binary | Binary search (right boundary) | `(binr [10 20 30] 25)` → `2` |
| `unify` | binary | Merge two tables/dicts, second takes precedence | `(unify d1 d2)` |

## Sorting & Ordering

| Function | Type | Description | Example |
|---|---|---|---|
| `asc` | unary | Sort ascending | `(asc [3 1 2])` → `[1 2 3]` |
| `desc` | unary | Sort descending | `(desc [3 1 2])` → `[3 2 1]` |
| `iasc` | unary | Indices that would sort ascending | `(iasc [30 10 20])` → `[1 2 0]` |
| `idesc` | unary | Indices that would sort descending | `(idesc [30 10 20])` → `[0 2 1]` |
| `rank` | unary | Rank of each element | `(rank [30 10 20])` → `[2 0 1]` |
| `xasc` | binary | Sort table ascending by column(s) | `(xasc 'price trades)` |
| `xdesc` | binary | Sort table descending by column(s) | `(xdesc 'price trades)` |
| `xrank` | binary | Assign rank buckets (quantiles) | `(xrank 4 [10 20 30 40])` → `[0 1 2 3]` |

## Table Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `list` | variadic | Create a list from vectors | `(list [1 2] [A B])` |
| `table` | binary | Create table from column names + list of vectors | `(table [x y] (list [1 2] [A B]))` |
| `key` | unary | Get column names (table) or keys (dict) | `(key trades)` → `[sym price size]` |
| `value` | unary | Get column data (table) or values (dict) | `(value trades)` |
| `dict` | binary | Create dictionary from keys and values | `(dict [a b] [1 2])` |
| `get` | binary | Lookup key in dict/table | `(get d 'a)` → `1` |
| `remove` | binary | Remove key from dict | `(remove d 'a)` |
| `row` | binary | Extract single row from table as dict | `(row trades 0)` |
| `meta` | unary | Get metadata (column types, lengths) | `(meta trades)` |
| `alter` | variadic, special | In-place mutation of table column | `(alter trades 'price (* price 1.1))` |
| `del` | variadic, special | Delete columns or rows from table | `(del trades 'temp_col)` |
| `modify` | variadic | Functional table update (returns new table) | `(modify trades 'price (fn [p] (* p 1.1)))` |

## Query Operations

These are **special forms** that bridge to the Rayforce DAG executor.

| Function | Type | Description |
|---|---|---|
| `select` | variadic, special | Query table with optional filter, projection, grouping, and aggregation |
| `update` | variadic, special | Add or modify columns in a table |
| `insert` | variadic, special | Append a row to a table, append to a vector/list, or insert at position(s) |
| `upsert` | variadic, special | Insert or update rows (by key) |

```lisp
; Select with filter and projection
(select {from: trades
         where: (> price 100)
         cols: {sym: sym notional: (* price size)}})

; Group-by with multiple aggregates
(select {from: trades
         by:   {sym: sym}
         cols: {vwap: (/ (sum (* price size)) (sum size))
                count: (count price)}})

; Update: add a column
(update {from: trades
          cols: {notional: (* price size)}})
```

`insert` overloads on arity. With two arguments it appends; with three arguments it inserts at a position (or positions) given by the second argument. The first argument is a quoted symbol for in-place mutation, e.g. `'v`, or any expression that evaluates to a table/vector/list.

```lisp
; Append a row to a table
(insert 'trades (list 'AAPL 150.0 100))

; Vector / list operations
(def v (til 5))               ; [0 1 2 3 4]
(insert 'v 99)                  ; append:    [0 1 2 3 4 99]
(insert 'v 0 -1)                ; head:      [-1 0 1 2 3 4 99]
(insert 'v 3 [100 200])         ; splice:    [-1 0 1 100 200 2 3 4 99]
(insert 'v [0 3] 77)             ; broadcast 77 at pre-positions 0 and 3
(insert 'v [1 3] [10 30])         ; parallel: 10 at pos 1, 30 at pos 3
```

Indices are *pre-insertion* positions in `[0, count]`; `idx == count` is equivalent to append. Vector positional inserts of a same-typed vector splice that vector in. List positional inserts always add the value as a single slot — use `concat` to splice. Multi-insert is stable on duplicate indices, preserving input order. Typed-null atoms (`0Nl`, `0Nf`, …) carry their null flag through — the inserted slot is marked null, not zero.

## Joins

Rayforce supports four join types, all with time-series-aware semantics.

| Function | Type | Description |
|---|---|---|
| `left-join` | variadic | Left join on matching columns. Unmatched rows filled with nulls. |
| `inner-join` | variadic | Inner join — only matching rows. |
| `window-join` | variadic, special | Join with time window constraint. Match rows within a time range. |
| `asof-join` | variadic | As-of join — match the most recent preceding value. |

```lisp
; Left join two tables on the sym column
(left-join trades quotes 'sym)

; Inner join
(inner-join orders products 'product_id)

; Window join: match trades to quotes within 1-second window
(window-join {left: trades  right: quotes
              on: [sym]  window: [-1000 0]
              cols: {avg_bid: (avg bid)}})

; As-of join: find most recent quote for each trade
(asof-join trades quotes 'sym)
```

## Pivot & Window

| Function | Type | Description | Example |
|---|---|---|---|
| `pivot` | variadic | Pivot table — reshape long to wide format | `(pivot trades 'sym 'date 'price)` |
| `xbar` | binary, atomic | Bucket values (time bucketing for OHLC bars) | `(xbar [3 7 12] 5)` → `[0 5 10]` |
| `xrank` | binary | Assign N rank buckets (quantile ranking) | `(xrank 4 [10 20 30 40])` |

```lisp
; OHLC bars: bucket trades by 5-minute intervals
(select {from: trades
         by:   {sym: sym  bucket: (xbar time 300000)}
         cols: {open: (first price)
                high: (max price)
                low:  (min price)
                close: (last price)
                vol:  (sum size)}})
```

## String Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `split` | binary | Split string by delimiter | `(split "a,b,c" ",")` → `["a" "b" "c"]` |
| `like` | binary | Glob pattern match: `*` any, `?` one, `[abc]`/`[a-z]`/`[!abc]` char class | `(like "hello" "hel*")` → `true` |
| `concat` | binary | Concatenate two strings or vectors | `(concat "hello" " world")` → `"hello world"` |
| `format` | variadic | Format values as string (% is placeholder) | `(format "x=%" 42)` → `"x=42"` |

!!! note "Note"
    The executor supports additional string opcodes (STRLEN, UPPER, LOWER, TRIM, SUBSTR, REPLACE, CONCAT) at the DAG level. All string transformations propagate nulls: null input rows produce null output rows.

## Date & Time

| Function | Type | Description | Example |
|---|---|---|---|
| `date` | unary | Current date or extract date from timestamp | `(date 0)` → today's date |
| `time` | unary | Current time or extract time from timestamp | `(time 0)` → current time |
| `timestamp` | unary | Current timestamp (nanosecond precision) | `(timestamp 0)` |

### Calendar/Clock Field Extraction

Unary functions that pull a single calendar or clock field out of a DATE / TIME / TIMESTAMP atom or vector. Null input rows propagate to null output rows (the null sentinel bit pattern is *not* decoded as a bogus year / second). The same set of fields is reachable via dotted access on a temporal value (e.g. `ts.yyyy` — see [Dotted Namespaces](../language/syntax.md)).

| Function | Range | Example |
|---|---|---|
| `yyyy` | year | `(yyyy 2024.03.15)` → `2024` |
| `mm` | 1..12 | `(mm 2024.03.15)` → `3` |
| `dd` | 1..31 | `(dd 2024.03.15)` → `15` |
| `hh` | 0..23 | `(hh 12:34:56)` → `12` |
| `minute` | 0..59 | `(minute 12:34:56)` → `34` |
| `ss` | 0..59 | `(ss 12:34:56)` → `56` |
| `dow` | 1..7 (Mon=1) | `(dow 2024.03.15)` → `5` |
| `doy` | 1..366 | `(doy 2024.03.15)` → `75` |

`mm` is unambiguously MONTH; the minute spelling stays long-form because a two-letter token can't serve both meanings in a uniform dotted walk. Two dotted-only truncations exist: `.date` drops the time-of-day component (keeps the day), and `.time` drops the date component (keeps the microseconds within the day).

Cross-temporal comparisons are supported: dates, times, and timestamps are all converted to nanoseconds internally for comparison operations.

## Type Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `type` | unary | Get type name of a value | `(type 42)` → `i64` |
| `as` | binary | Cast value to another type | `(as 'i64 "42")` → `42` |
| `nil?` | unary | Test if value is null | `(nil? x)` |
| `rc` | unary | Reference count of an object | `(rc x)` → `1` |
| `guid` | unary | Generate N GUIDs (0 for one) | `(guid 0)` |

## I/O & File Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `println` | variadic | Print values with newline | `(println "hello" 42)` |
| `print` | unary | Print value without newline | `(print "hello")` |
| `show` | variadic | Pretty-print a value (tables formatted) | `(show trades)` |
| `format` | variadic | Format value to string (% is placeholder) | `(format "val=%" 42)` → `"val=42"` |
| `.csv.read` | variadic | Load CSV file into table | `(.csv.read "data.csv")` |
| `.csv.write` | variadic | Write table to CSV file | `(.csv.write trades "out.csv")` |
| `read` | unary | Read file contents as string | `(read "file.txt")` |
| `write` | binary | Write string to file | `(write "file.txt" "content")` |
| `load` | unary | Load and evaluate a Rayfall script | `(load "lib.rfl")` |

## Control Flow

| Function | Type | Description | Example |
|---|---|---|---|
| `set` | binary, special | Bind value to global variable | `(set x 42)` |
| `let` | binary, special | Bind value to local variable | `(let y (+ x 1))` |
| `if` | variadic, special | Conditional (if/then/else) | `(if (> x 0) "pos" "neg")` |
| `do` | variadic, special | Sequential execution, returns last | `(do (set x 1) (set y 2) (+ x y))` |
| `fn` | variadic, special | Create lambda function | `(fn [x] (* x x))` |
| `try` | binary, special | Error handling (expr handler) | `(try (/ 1 0) (fn [e] 0))` |
| `raise` | unary | Throw an error | `(raise "bad input")` |
| `return` | variadic | Early return from compiled lambda (0 args → null) | `(return 42)` |
| `quote` | variadic, special | Return argument unevaluated | `(quote (+ 1 2))` → `(+ 1 2)` |
| `resolve` | variadic, special | Resolve a symbol in current scope | `(resolve 'x)` |

## System & Utility

| Function | Type | Description | Example |
|---|---|---|---|
| `eval` | unary | Evaluate a parsed expression | `(eval (parse "(+ 1 2)"))` → `3` |
| `parse` | unary | Parse string into Rayfall expression | `(parse "(+ 1 2)")` |
| `.sys.gc` | variadic | Trigger garbage collection, returns `0` | `(.sys.gc)` |
| `.sys.exec` | unary | Execute shell command, return exit code | `(.sys.exec "ls -la")` |
| `.os.getenv` | unary | Get environment variable | `(.os.getenv "HOME")` |
| `.os.setenv` | binary | Set environment variable | `(.os.setenv "KEY" "value")` |
| `exit` | unary | Exit with status code | `(exit 0)` |
| `timeit` | variadic, special | Benchmark an expression | `(timeit (sum (til 1000000)))` |
| `.time.now` | variadic | Current monotonic time in milliseconds | `(.time.now)` |
| `.time.timer.set` | variadic, restricted | Schedule a callback every `ms` milliseconds, `num` times (0 = forever). Returns timer id. | `(.time.timer.set 1000 0 (fn [t] (println t)))` |
| `.time.timer.del` | unary, restricted | Cancel a scheduled timer by id. Returns null. | `(.time.timer.del 0)` |
| `args` | unary | Command-line arguments | `(args 0)` |
| `env` | unary | List all global environment bindings | `(env 0)` |
| `.sys.build` | variadic | Build metadata: `version` + `build-date` | `(.sys.build)` |
| `.sys.mem` | variadic | Memory allocator statistics (alloc/peak/slab) | `(.sys.mem)` |
| `.sys.info` | variadic | System information (cores, page size, memory) | `(.sys.info)` |

## Serialization & Storage

| Function | Type | Description | Example |
|---|---|---|---|
| `ser` | unary | Serialize value to binary format | `(ser [1 2 3])` |
| `de` | unary | Deserialize from binary format | `(de bytes)` |
| `.db.splayed.set` | variadic | Save table as splayed columns to directory | `(.db.splayed.set "db/trades" trades)` |
| `.db.splayed.get` | variadic | Load splayed table from directory | `(.db.splayed.get "db/trades")` |
| `.db.parted.get` | variadic | Load partitioned table by name from root directory | `(.db.parted.get "db" 'trades)` |

## EAV (Entity-Attribute-Value)

Built-in support for triple stores. The EAV table has three columns: `e` (entity, i64), `a` (attribute, sym), `v` (value, i64).

| Function | Type | Description | Example |
|---|---|---|---|
| `datoms` | nullary | Create empty EAV table | `(datoms)` |
| `assert-fact` | variadic | Append triple to datoms | `(assert-fact db 1 'name 100)` |
| `retract-fact` | variadic | Remove triple from datoms | `(retract-fact db 1 'name 100)` |
| `scan-eav` | binary | Query EAV by attribute | `(scan-eav db 'name)` |
| `pull` | binary | Entity-centric retrieval (returns dict) | `(pull db 1)` → `{name:100 age:30}` |
| `sym-name` | unary | Convert sym intern ID to symbol string | `(sym-name 0)` |

```lisp
; Build an EAV store
(set db (datoms))
(set db (assert-fact db 1 'name 100))
(set db (assert-fact db 1 'age 30))
(pull db 1)              ; {name:100 age:30}
(scan-eav db 'name)     ; table of (e, v) pairs where a='name
```

## Table Set Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `union-all` | binary | Concatenate two tables (all rows) | `(union-all t1 t2)` |
| `table-distinct` | unary | Remove duplicate rows from table | `(table-distinct t)` |
| `antijoin` | variadic | Anti-semi-join: rows in left not in right | `(antijoin t1 t2 [x])` |

```lisp
; Table concatenation and deduplication
(set t1 (table [x] (list [1 2])))
(set t2 (table [x] (list [2 3])))
(union-all t1 t2)          ; 4 rows: 1 2 2 3
(table-distinct (union-all t1 t2))  ; 3 rows: 1 2 3
(antijoin t1 t2 [x])     ; 1 row: 1
```

## Datalog

Datalog rules and queries integrate with the EAV store. Rules use the `(?entity :attribute ?value)` pattern to match triples.

| Function | Type | Description | Example |
|---|---|---|---|
| `rule` | variadic, special | Define Datalog rule | `(rule (path ?x ?y) (?x :edge ?y))` |
| `query` | variadic, special | Compile and execute Datalog query | `(query db (find ?x ?y) (where (path ?x ?y)))` |
| `not` | special (in Datalog WHERE) | Stratified negation in WHERE clause | `(not (?y :edge 3))` |

```lisp
; Define rules and query
(rule (path ?x ?y) (?x :edge ?y))
(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))

(set db (datoms))
(set db (assert-fact db 1 'edge 2))
(set db (assert-fact db 2 'edge 3))
(query db (find ?x ?y) (where (path ?x ?y)))
; returns table: (1,2) (1,3) (2,3)
```

## Datalog Program API

Low-level API for building and evaluating Datalog programs directly, bypassing the EAV store.

| Function | Type | Description | Example |
|---|---|---|---|
| `dl-program` | nullary | Create Datalog program | `(dl-program)` |
| `dl-add-edb` | variadic | Register base relation (table + arity) | `(dl-add-edb prog 'edge tbl 2)` |
| `dl-stratify` | unary | Compute strata for the program | `(dl-stratify prog)` |
| `dl-eval` | unary | Evaluate program to fixpoint | `(dl-eval prog)` |
| `dl-query` | binary | Query a derived or base relation | `(dl-query prog 'edge)` |
| `dl-provenance` | binary | Get derivation tracking (if available) | `(dl-provenance prog 'rel)` |

```lisp
; Low-level Datalog program
(set prog (dl-program))
(set edges (table [x y] (list [1 2 3] [2 3 4])))
(dl-add-edb prog 'edge edges 2)
(dl-stratify prog)
(dl-eval prog)
(dl-query prog 'edge)  ; returns the edge table
```

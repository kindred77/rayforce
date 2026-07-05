# `.sys.*` — system info and control

Process-level introspection (build, memory, host info) and command-style operations (shell-out, profiler toggle, IPC listener bind, env dump, GC trigger). The dotted `.sys.<name>` builtins are the typed entry points; `.sys.cmd` parses a colon-style command string and dispatches to the same handlers — used by the REPL's `:listen 5000` style commands.

!!! note "Restricted under `-U`"
    `.sys.exec`, `.sys.cmd`, and `.sys.listen` are `RAY_FN_RESTRICTED` — they can either run arbitrary shell commands or change the process's network surface. The introspection entries (`.sys.args`, `.sys.build`, `.sys.info`, `.sys.mem`, `.sys.prof`, `.sys.gc`, `.sys.env`, `.sys.timeit`) are unrestricted.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.sys.args`](#sys-args) | variadic | — | Command-line arguments as a typed dict. |
| [`.sys.build`](#sys-build) | variadic | — | Version + build date as a dict. |
| [`.sys.info`](#sys-info) | variadic | — | Host facts: cores, page size, total memory. |
| [`.sys.mem`](#sys-mem) | variadic | — | Allocator statistics. |
| [`.sys.prof`](#sys-prof) | variadic | — | Last profiled query's per-step statistics as a table. |
| [`.sys.gc`](#sys-gc) | variadic | — | Garbage-collect hint (no-op; returns 0). |
| [`.sys.env`](#sys-env) | variadic | — | Count or list of globally bound names. |
| [`.sys.exec`](#sys-exec) | unary | restricted | Run a shell command; return its exit code. |
| [`.sys.cmd`](#sys-cmd) | unary | restricted | Dispatch a colon-command string. |
| [`.sys.listen`](#sys-listen) | unary | restricted | Bind an IPC listener on a TCP port. |
| [`.sys.timeit`](#sys-timeit) | variadic | — | Toggle / set the per-expression profiler. |

## `.sys.args` { #sys-args }

Signature: `(.sys.args)`. Returns the process's command-line arguments as a dictionary. Because the launcher flags have a known format, each top-level value carries its natural type; everything after a `--` separator is collected into a `user` subdict (`string → string`), so an application can read its own options without colliding with Rayforce's own flags.

| Key | Type | Source flag | Notes |
|---|---|---|---|
| `file` | str | `-f` / positional | Script path; empty if none. |
| `port` | i64 | `-p` | IPC listen port; `0` if unset. |
| `cores` | i64 | `-c` | Worker-pool size; `0` = auto. |
| `timeit` | bool | `-t` | Profiler enabled at startup. |
| `interactive` | bool | `-i` | Force the REPL after a script. |
| `log` | str | `-l` / `-L` | Journal base path; empty if none. |
| `user` | dict | after `--` | The application's own arguments. |

The top-level schema is **stable** — every launcher key is always present with its effective value (the default when the flag wasn't passed), so `(get (.sys.args) 'port)` never misses. Auth passwords (`-u` / `-U`) are deliberately **not** exposed.

**`user` parsing.** Tokens after `--` are paired `-key value` / `--key value`: a token starting with `-` is a key (leading dashes stripped to a symbol), and the next token is its value — unless that token also starts with `-`, in which case the value is the empty string (a bare flag). Duplicate keys keep the last value.

```lisp
;; launched as:  ./rayforce app.rfl -- -opt 123 --verbose
(.sys.args)
;; => {file:"app.rfl" port:0 cores:0 timeit:false interactive:false log:0Nc
;;     user:{opt:"123" verbose:0Nc}}

(at (.sys.args) 'user)     ;; just the application's options
;; => {opt:"123" verbose:0Nc}

(get (.sys.args) 'port)    ;; a typed launcher value
;; => 0
```

Empty strings render as `0Nc` in the REPL (`log` and the bare `verbose` flag above). See [passing arguments to a script](../getting-started/quick-start.md#passing-arguments-to-a-script) for the launcher side.

## `.sys.build` { #sys-build }

Signature: `(.sys.build)`. Returns a dict with `version` (string) and `build-date` (string). Both fall back to `"unknown"` when the build didn't define `RAYFORCE_VERSION` / `RAYFORCE_BUILD_DATE`.

```lisp
(.sys.build)
;; => {version: "0.3.1", build-date: "2026-05-27"}
```

## `.sys.info` { #sys-info }

Signature: `(.sys.info)`. Returns `{cores: i64, page-size: i64, total-mem: i64}` on POSIX. On Windows the response is `{cores: 1}` (a fallback — the sysconf-backed values aren't wired).

```lisp
(.sys.info)
;; => {cores: 10, page-size: 16384, total-mem: 68719476736}
```

## `.sys.mem` { #sys-mem }

Signature: `(.sys.mem)`. Returns the buddy allocator's running counters:

| Key | Meaning |
|---|---|
| `alloc-count` | Cumulative `ray_alloc` calls. |
| `bytes-allocated` | Live bytes held by `ray_t` objects. |
| `peak-bytes` | High-watermark since process start. |
| `slab-hits` | Cumulative fast-path slab allocations. |
| `sys-current` | Currently-allocated bytes from the system allocator. |

```lisp
(.sys.mem)
;; => {alloc-count: 12345, bytes-allocated: 524288, peak-bytes: 2097152, ...}
```

## `.sys.prof` { #sys-prof }

Signature: `(.sys.prof)`. Returns the profile of the **most recently
completed** query as a table — one row per pipeline step (parse, the
optimizer passes, each heavy operator, and materialize). Because it is an
ordinary table it can be queried like any other, and an IPC client gets it
with no extra protocol.

Profiling is opt-in: toggle it with the REPL `:t` command, `.sys.timeit`,
or the `-t` startup flag. With profiling off the table is empty. Capturing
the payload adds no measurable cost to a non-profiled query.

| Column | Meaning |
|---|---|
| `op` | Step / operator label (symbol). |
| `depth` | Nesting depth (0 = top level). |
| `dur_us` | Wall-clock microseconds. |
| `rows` | Result element / row count (operators). |
| `kb_out` | Result serialized footprint, KiB (bandwidth produced). |
| `alloc_kb` | Net process bytes allocated across the step, KiB. |
| `workers` | Worker threads that ran a task for this step. |
| `busy_ms` | Summed worker busy time, ms. |
| `par_eff` | Effective parallelism = `busy_ms` / `dur_us` (worker time ÷ wall time). |

```lisp
;; run a query with the profiler on (:t 1), then:
(.sys.prof)
;; a table: op | depth | dur_us | rows | kb_out | alloc_kb | workers | busy_ms | par_eff
;; e.g. the GROUP row → rows=5000, kb_out=78.2, workers=28, par_eff=7.2

;; it is a normal table, so query it:
(select {from: (.sys.prof) where: (> dur_us 1000.0)})
```

The `:t` command also prints this as an indented tree after each query,
with the same payload appended to every operator line.

## `.sys.gc` { #sys-gc }

Signature: `(.sys.gc)`. No-op stub — Rayforce uses reference counting, so there's no global collector to kick. Kept as a registered builtin so existing scripts that call it don't error. Returns `0`.

## `.sys.env` { #sys-env }

Signature: `(.sys.env)`. From a script / IPC context returns the **count** of globally bound names (i64). In a REPL context the same dispatcher prints one line per binding (name + type label) and returns null — the variadic registration accommodates both forms.

```lisp
(.sys.env)
;; => 42  (number of global bindings)
```

## `.sys.exec` { #sys-exec }

Signature: `(.sys.exec "command")`. Runs `command` through `system(3)` and returns its exit status as an `i64`. Used pervasively in tests to set up / tear down `/tmp` fixtures.

Errors: `type` (arg not a string), `domain` (null pointer).

```lisp
(.sys.exec "rm -rf /tmp/scratch")   ;; => 0
(.sys.exec "false")                  ;; => 256  (waitpid-encoded)
```

## `.sys.cmd` { #sys-cmd }

Signature: `(.sys.cmd "name args")`. Parses a colon-style command name (without the leading `:`) and dispatches. Names supported: `timeit`/`t`, `listen`, `env`, `clear`, `help`/`?`, `q`/`quit`. Unknown names fall back to `system()` (so `(.sys.cmd "ls /tmp")` works too).

`clear`, `help`, and `q` are REPL-only — invoking them from a script returns a `domain` error rather than silently no-op'ing.

```lisp
(.sys.cmd "listen 5000")   ;; => listener id
(.sys.cmd "env")           ;; => binding count
(.sys.cmd "timeit 1")      ;; => 1   (profiler enabled)
```

## `.sys.listen` { #sys-listen }

Signature: `(.sys.listen port)`. Binds an IPC listener on `port` using the runtime's main poll. Returns the listener id (i64).

Errors: `type` (port not an int / not parseable from string), `domain` (port outside `[1, 65535]`), `nyi` (no main loop — embedded library without a poll), `io` (`bind` failure, e.g. port already in use).

```lisp
(.sys.listen 5000)
;; => 12  (listener id)
```

## `.sys.timeit` { #sys-timeit }

Signature: `(.sys.timeit [flag])`. Toggles the per-expression profiler. Calling with no argument flips the current state; passing `0` disables, anything non-zero enables. Returns the new state as `i64` (0/1).

```lisp
(.sys.timeit 1)   ;; enable profiling
(select {from: trades by: sym total: (sum qty)})
;; (timings printed for each subexpression)
(.sys.timeit 0)   ;; disable
```

## See also

- [`.os.*`](os.md) — filesystem and env-variable primitives that don't shell out.
- [`.ipc.*`](ipc.md) — the client side; `.sys.listen` binds the server.
- [`.time.*`](time.md) — `.time.now` / `.time.timer.set` for measurement and scheduling without the profiler.

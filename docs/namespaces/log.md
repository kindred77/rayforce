# `.log.*` — write-ahead log

A binary append-only journal of serialised Rayfall expressions. The runtime keeps one journal open at a time. While open, every IPC mutation (and any value explicitly handed to `.log.write`) is appended; `replay` deserialises and re-evaluates each entry to reconstruct state. Files use the same 16-byte IPC header + serialized-payload frames as `.ipc.send`, with optional delta+RLE compression on large entries — the journal is interoperable with the wire format.

The journal is intentionally minimal: there's no per-entry timestamp or transaction grouping at the journal layer. Snapshot + replay is the only recovery model.

!!! note "Restricted under `-U`"
    `.log.open`, `.log.replay`, `.log.roll`, `.log.snapshot`, `.log.close` are `RAY_FN_RESTRICTED`. The introspection / I/O-only entries (`.log.write`, `.log.sync`, `.log.validate`) are unrestricted — they cannot escalate privilege beyond what the IPC peer could already do.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.log.open`](#log-open) | variadic | restricted | Open a journal in async or sync mode. |
| [`.log.write`](#log-write) | unary | — | Append a serialised expression to the open journal. |
| [`.log.sync`](#log-sync) | variadic | — | `fsync` the journal. |
| [`.log.snapshot`](#log-snapshot) | variadic | restricted | Write a snapshot of current state and roll to a fresh segment. |
| [`.log.roll`](#log-roll) | variadic | restricted | Close the active segment and start a new one. |
| [`.log.replay`](#log-replay) | unary | restricted | Replay a journal file; return entry count. |
| [`.log.validate`](#log-validate) | unary | — | Scan a journal file; return `(chunks valid_bytes)`. |
| [`.log.close`](#log-close) | variadic | restricted | Flush and close the active journal. |

## `.log.open` { #log-open }

Signature: `(.log.open mode "base")`. `mode` is `'async` or `'sync`; `base` is the journal base path (the actual file ends with `.log` and rolled segments use numeric suffixes).

- `'async` — buffered writes; faster, may lose entries on a crash within the write window.
- `'sync` — each write is `fsync`'d before returning. Durable, ~10–100× slower per write.

Returns the null object on success.

Errors: `rank` (arity != 2), `type` (mode not a sym / base not a string), `domain` (unknown mode), `io` (open failed).

```lisp
(.log.open 'async "/var/lib/rayforce/journal")
```

The server CLI flags `-l` (async) and `-L` (sync) call this automatically at startup.

## `.log.write` { #log-write }

Signature: `(.log.write expr)`. Serialises `expr` and appends a frame to the open journal.

Errors: `noopen` (no journal active — surfaced explicitly so callers don't believe a no-op succeeded), `type` (no arg), `domain` (`expr` has zero serialised size), `oom`, `io`. Lazy values are forced before serialisation.

```lisp
(.log.open 'async "/tmp/j")
(.log.write 42)
(.log.write [1 2 3 4 5])
(.log.close)
```

## `.log.sync` { #log-sync }

Signature: `(.log.sync)`. Forces an `fsync` on the active segment. Returns null. No-op when no journal is open (the underlying call returns `RAY_OK`).

## `.log.snapshot` { #log-snapshot }

Signature: `(.log.snapshot)`. Writes a snapshot of current state (env globals serialised into a special frame) and rolls to a new segment. Snapshots let `replay` start from a recent state and only re-apply entries written after.

Errors: `domain` (no journal open), `io`.

## `.log.roll` { #log-roll }

Signature: `(.log.roll)`. Closes the active segment and opens a fresh one with the next numeric suffix. Useful for time-bounded rotation (cron-driven) without involving a snapshot.

Errors: `domain` (no journal open), `io`.

## `.log.replay` { #log-replay }

Signature: `(.log.replay "path")`. Reads each frame in the file, deserialises the payload, and `eval`s the result. Returns the count of successfully-replayed chunks.

Errors are precise enough to distinguish recovery strategies:

| Error code | Meaning | Action |
|---|---|---|
| `badtail` | Framing broken after some valid prefix. | Truncate to `valid_bytes` from `.log.validate`. |
| `deser` | Frame intact but payload deserialisation failed — content/version skew. | Do **not** truncate; inspect and fix. |
| `decompr` | Compressed-frame decompression failed. | Same as `deser`. |
| `oom` / `io` | Resource exhaustion / read failure. | Retry. |

```lisp
(.log.replay "/var/lib/rayforce/journal.log")
;; => 1247   (chunks replayed)
```

## `.log.validate` { #log-validate }

Signature: `(.log.validate "path")`. Scans the file framing-only — does not deserialise payloads. Returns a 2-list `(chunks valid_bytes)` where `valid_bytes` is the offset at which framing breaks (= file size if intact).

```lisp
(.log.validate "/var/lib/rayforce/journal.log")
;; => (1247 524288)
```

## `.log.close` { #log-close }

Signature: `(.log.close)`. Flushes and closes the active journal. Returns null. No-op if no journal is open.

## See also

- [IPC & Serialization](../storage/ipc.md) — wire format used by journal frames.

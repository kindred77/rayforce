# Namespaces

Rayfall's builtins are organised under dotted namespaces. Names beginning with `.` are reserved — user code cannot shadow them with `set` or `let` (the only exception being the five `.ipc.on.*` hook slots). Each page below documents every builtin in that namespace: arity, restricted-mode flag, signature, semantics, and at least one worked example.

## Reference

| Namespace | Purpose |
|---|---|
| [`.col.*`](col.md) | Foreign-key style column-to-table linking and dotted dereference. |
| [`.csv.*`](csv.md) | CSV import/export — in-memory, splayed-on-disk, and partitioned variants. |
| [`.db.*`](db.md) | On-disk table I/O: splayed and partitioned `get` / `set` / `mount`. |
| [`.graph.*`](graph.md) | Graph builders and algorithms (PageRank, Louvain, Dijkstra, MST, BFS/DFS, expand, …). |
| [`.idx.*`](idx.md) | Accelerator indexes: bloom, hash, sort, zone. |
| [`.ipc.*`](ipc.md) | TCP client IPC and the server connection-hook accessor. |
| [`.log.*`](log.md) | Write-ahead log: open, write, sync, snapshot, roll, replay, validate. |
| [`.os.*`](os.md) | Filesystem and environment: `getenv`, `setenv`, `list`, `size`. |
| [`.repl.*`](repl.md) | Interactive REPL control — attach the local REPL to a remote server. |
| [`.sys.*`](sys.md) | System info and shell-style commands: build, info, mem, gc, exec, listen, timeit, env, cmd. |
| [`.time.*`](time.md) | Monotonic clock and timer scheduler. |

## Restricted-mode summary

When the server is started with `-U <password>`, the following dotted builtins are blocked (return an `access` error) for IPC peers — see [IPC restricted mode](../storage/ipc.md). Page-level admonitions on each namespace flag the exact builtins that carry the `RAY_FN_RESTRICTED` attribute.

- `.csv.read`, `.csv.write`, `.csv.splayed`, `.csv.parted`
- `.db.splayed.set`
- `.ipc.open`, `.ipc.close`, `.ipc.send`
- `.log.open`, `.log.replay`, `.log.roll`, `.log.snapshot`, `.log.close`
- `.os.getenv`, `.os.setenv`
- `.repl.connect`, `.repl.disconnect`
- `.sys.exec`, `.sys.cmd`, `.sys.listen`
- `.time.timer.set`, `.time.timer.del`

`.col.*`, `.graph.*`, `.idx.*`, and pure read/inspect builtins (`.db.splayed.get`, `.db.parted.get`, `.os.size`, `.os.list`, `.log.write`, `.log.sync`, `.log.validate`, `.sys.build`, `.sys.info`, `.sys.mem`, `.sys.gc`, `.sys.env`, `.sys.timeit`, `.ipc.handle`, `.time.now`) are always available.

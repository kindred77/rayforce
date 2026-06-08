# `.ipc.*` — client IPC

Connect to a Rayforce server (`./rayforce -p <port>`) and exchange messages over TCP. The wire format is the same compact binary serialisation used for `ser`/`de`, with automatic delta+RLE compression for payloads larger than 2,000 bytes. All five client builtins live here; the server-side connection hooks (`.ipc.on.open` / `.on.close` / `.on.sync` / `.on.async` / `.on.auth`) live on the [IPC Connection Hooks](../storage/ipc-hooks.md) page — they are user-settable lambda slots, not registered builtins, so they don't appear in the reference table below.

!!! note "Restricted under `-U`"
    `.ipc.open`, `.ipc.close`, `.ipc.send`, and `.ipc.post` are `RAY_FN_RESTRICTED` — IPC chaining is blocked on a `-U` server (a peer cannot dial outward to a third server). `.ipc.handle` is always available so hooks can read the current connection ID.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.ipc.open`](#ipc-open) | unary | restricted | Open a TCP connection; return an i64 handle. |
| [`.ipc.send`](#ipc-send) | binary | restricted | Send a message synchronously; return the server's result. |
| [`.ipc.post`](#ipc-post) | binary | restricted | Send a message asynchronously (fire-and-forget); return the null object. |
| [`.ipc.close`](#ipc-close) | unary | restricted | Close a connection handle. |
| [`.ipc.handle`](#ipc-handle) | variadic | — | The current connection's handle inside a server-side hook; `-1` otherwise. |

## `.ipc.open` { #ipc-open }

Signature: `(.ipc.open "host:port")` or `(.ipc.open "host:port:user:password")`.

Returns: an `i64` handle. Negative handles never escape — errors are surfaced as Rayfall error objects:

- `type` — argument is not a string.
- `domain` — malformed address (missing port, port out of `(0, 65535]`, oversized host/user/password).
- `access` — server requires auth and you didn't supply credentials, **or** the password is wrong.
- `io` — connection refused / network error.

The handshake exchanges a 2-byte `{wire_version, auth_flag}` greeting. A wire-version mismatch closes the connection before any payload is exchanged.

```lisp
;; Unauthenticated
(set h (.ipc.open "127.0.0.1:5000"))

;; With credentials (server started with -u or -U)
(set h (.ipc.open "127.0.0.1:5000:admin:secret123"))
```

## `.ipc.send` { #ipc-send }

Signature: `(.ipc.send h msg)`. Sends `msg` synchronously — blocks until the server replies.

The server's behaviour depends on `msg`'s type:

| Payload | Server behaviour |
|---|---|
| `STR` | Parsed as Rayfall, evaluated, result returned. |
| anything else | Passed straight into `ray_eval` — identity for plain data, execution for an expression list. |

Errors: `type` (`h` not an `i64`/`i32`, or `msg` not serialisable). Server-side errors are returned as error objects too — `.ipc.send` does not raise them, the caller decides what to do.

```lisp
;; String-form query
(.ipc.send h "(select {from: trades where: (> price 100) take: 10})")

;; Expression form — dict stays unevaluated until the server processes it
(.ipc.send h (list select {from: trades by: sym total: (sum qty)}))

;; Plain data round-trips
(.ipc.send h 42)         ;; => 42
(.ipc.send h (til 1000)) ;; => the same vector
```

For async fire-and-forget, see `.ipc.post` below. See [IPC & Serialization](../storage/ipc.md) for the wire format and message-type byte.

## `.ipc.post` { #ipc-post }

Signature: `(.ipc.post h msg)`. Sends `msg` asynchronously — fire-and-forget. Unlike `.ipc.send`, it does **not** wait for a reply: the server runs the message through its `.ipc.on.async` hook (or default evaluation) and sends nothing back.

Because no response is returned, the only failures the caller can observe are **local** ones. A server-side evaluation error is logged on the server and dropped — it never reaches the sender.

Returns the null object on a successful local send. Errors: `type` (`h` not an `i64`/`i32`, or `msg` not serialisable), `io` (handle invalid / connection closed / socket write failed).

```lisp
;; Push a state update to the server and move on — no round-trip.
;; The string payload is parsed and evaluated server-side, just like `.ipc.send`.
(.ipc.post h "(set last-update 42)")
```

## `.ipc.close` { #ipc-close }

Signature: `(.ipc.close h)`. Closes the socket for the handle. Returns the null object.

```lisp
(.ipc.close h)
```

## `.ipc.handle` { #ipc-handle }

Signature: `(.ipc.handle)` — variadic but invoked with no args by convention.

Returns the handle of the connection that triggered the currently-executing server-side hook (`.ipc.on.open`, `.on.close`, `.on.sync`, `.on.async`, `.on.auth`). Outside any hook it returns `-1`.

This is the only way to read the handle inside `on.sync` / `on.async` / `on.auth`, whose lambda signatures don't include `h`. For `on.open` and `on.close` the value always equals the `h` argument the hook receives.

```lisp
;; Server-side
(set .ipc.on.open
     (fn [h] (println "opened handle=" (.ipc.handle) " arg=" h)))

;; Outside any hook
(.ipc.handle)
;; => -1
```

## Server-side connection hooks

The five `.ipc.on.*` slots are user-settable lambdas that intercept the server connection lifecycle. They are not part of the builtin table — install them with plain `set`:

```lisp
(set .ipc.on.open  (fn [h] (println "+ " h)))
(set .ipc.on.close (fn [h] (println "- " h)))
(set .ipc.on.sync  (fn [m] (eval (parse m))))
(set .ipc.on.async (fn [m] (eval (parse m))))
(set .ipc.on.auth  (fn [u p] (!= u "ban")))   ;; runs AFTER password check
```

See [IPC Connection Hooks](../storage/ipc-hooks.md) for full semantics: install/clear rules, per-hook error handling, restricted-mode interaction, the reserved-namespace carve-out, and the rejection contract for `.ipc.on.auth`.

## See also

- [IPC & Serialization](../storage/ipc.md) — server setup, authentication modes, wire format, compression, supported types.
- [IPC Connection Hooks](../storage/ipc-hooks.md) — server-side `.ipc.on.*` reference.
- [REPL — `.repl.connect`](repl.md) — a thin wrapper over `.ipc.open` that points the local REPL prompt at a remote server.

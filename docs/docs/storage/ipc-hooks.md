# IPC Connection Hooks

Five user-installable lambdas under `.ipc.on.*` that intercept the server-side connection lifecycle, plus the `.ipc.handle` accessor for the current connection's handle.

The handle a hook receives is a first-class connection handle — the same namespace `.ipc.open` allocates from. Pass it to `.ipc.post` (async push to the connected client), `.ipc.send` (sync round-trip), or `.ipc.close` (kick the client), from inside the hook or any time later. Lifecycle hooks fire for **inbound** connections only; connections the process itself opened via `.ipc.open` never trigger `on.open`/`on.close`.

!!! note "See also"
    The [IPC & Serialization](ipc.md) page documents the client-side `.ipc.open` / `.ipc.send` / `.ipc.close` and the wire format. Hooks live on the server and fire in response to inbound connections.

## Hook Overview

The server side exposes the inbound connection lifecycle to Rayfall code through five hook slots under `.ipc.on.*`. Each is a user-installable lambda; when unbound the server falls back to its built-in default, so installing a hook is purely opt-in.

| Name | Signature | Fires | Return value |
|---|---|---|---|
| `.ipc.on.open` | `(fn [h] ...)` | Inbound connection fully handshaked (and authed when `-u`/`-U` is on), just before the first header read. | Ignored. |
| `.ipc.on.close` | `(fn [h] ...)` | Inbound connection about to close, before its socket is closed and per-conn state is freed. Pairs 1-to-1 with `.ipc.on.open` — never fires for connections that died mid-handshake. | Ignored. |
| `.ipc.on.sync` | `(fn [m] ...)` | Inbound sync request, after the payload is deserialised into `m`. Replaces the default in-process `eval`. | Serialised and shipped to the client as the response. |
| `.ipc.on.async` | `(fn [m] ...)` | Inbound async message. Same dispatch point as `on.sync`, but the wire produces no response. | Ignored (errors are logged to stderr). |
| `.ipc.on.auth` | `(fn [u p] ...)` | After the constant-time secret compare in `-u`/`-U` auth passes. Servers started without auth never reach this hook. | Truthy = accept connection, falsy / error = reject and close. |
| `.ipc.handle` | `(.ipc.handle)` | Builtin readable inside any of the five hooks above — returns the current connection's handle. | `-1` outside any hook. |

## Installing Hooks

Install with plain `set` or the colon binder:

```lisp
(set .ipc.on.open  (fn [h] (println "+ " h)))
(set .ipc.on.close (fn [h] (println "- " h)))

;; Server push: greet every client as soon as it connects, and remember
;; the handle for later pushes from anywhere on the server.
(set .ipc.on.open
     (fn [h] (set last-client h) (.ipc.post h "(println \"welcome\")")))

;; Sync hook receives the raw deserialised payload.  Strings need an
;; explicit parse before eval; the default in-server dispatch does this
;; for you, but a hook gets the message as-is.
(set .ipc.on.sync  (fn [m] (eval (parse m))))

;; Narrow auth: hook runs AFTER the password check, so it can only
;; deny extras — never widen access.  Here, deny the username "ban".
(set .ipc.on.auth  (fn [u p] (!= u "ban")))
```

!!! note "Reserved-namespace carve-out"
    The five `.ipc.on.*` names are the only dotted-reserved names a user can `set` — the rest of `.ipc.*` (`open`, `close`, `send`, `handle`) and every other system namespace (`.sys.*`, `.os.*`, `.csv.*`, …) stays unsettable and returns a `reserve` error on any binding attempt.

## Clearing Hooks

Clear a hook by assigning a non-lambda value:

```lisp
;; Cleared — server falls back to the default behaviour.
(set .ipc.on.open 0)
```

Anything that isn't a callable lambda is treated as "no hook installed", so a stale binding never wedges the server.

## Error Handling

| Hook | Error in hook body |
|---|---|
| `.ipc.on.open` / `.ipc.on.close` | Logged to stderr, swallowed. Connection teardown proceeds. |
| `.ipc.on.sync` | Serialised and shipped to the client as the response — same as a raw `eval` error. |
| `.ipc.on.async` | Logged to stderr, dropped. No wire response on async. |
| `.ipc.on.auth` | Treated as reject. Same `0x01` handshake byte as a wrong-password rejection. |

## Restricted Mode

Hooks run under the same restricted-mode flag the inbound message would otherwise see — a `.ipc.on.sync` installed on a `-U` server cannot escalate privilege; the blocked-builtins list in [Authentication](ipc.md#authentication) applies inside the hook body too.

## Reading the Current Handle

Inside any hook, `(.ipc.handle)` returns the handle of the connection that triggered the hook. Outside any hook it returns `-1`.

```lisp
(set .ipc.on.open
     (fn [h] (println "opened handle=" (.ipc.handle) "  arg=" h)))

;; Outside any hook:
(.ipc.handle)
;; => -1
```

The argument `h` passed to `on.open` / `on.close` always equals `(.ipc.handle)` at the moment the hook fires — the builtin is the only way to reach the handle from `on.sync` / `on.async` / `on.auth`, whose signatures don't include it directly.

A handle read here stays valid for the connection's lifetime: store it and `.ipc.post` to it later (publish/subscribe), or `.ipc.close` it to drop the client. After the connection closes, the integer may be reused by a future connection — don't cache handles across `on.close`.

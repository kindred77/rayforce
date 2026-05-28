# `.os.*` — filesystem and environment

Minimal filesystem and process-environment primitives. The namespace is deliberately small: four builtins cover the common needs (read/write env, file size, directory listing) and the predicate cases (exists / is-file / is-dir) are reached by `try`-wrapping `.os.size` or `.os.list`.

!!! note "Restricted under `-U`"
    `.os.getenv` and `.os.setenv` are `RAY_FN_RESTRICTED` (environment is process-global state). `.os.size` and `.os.list` are read-only and unrestricted.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.os.getenv`](#os-getenv) | unary | restricted | Read an environment variable as a string. |
| [`.os.setenv`](#os-setenv) | binary | restricted | Set an environment variable. |
| [`.os.size`](#os-size) | unary | — | File size in bytes. |
| [`.os.list`](#os-list) | unary | — | Sorted sym vector of directory entries. |

## `.os.getenv` { #os-getenv }

Signature: `(.os.getenv "NAME")`. Returns the value as a string, or the empty string if the variable is unset.

Errors: `type` (arg not a string), `domain` (empty arg).

```lisp
(.os.getenv "HOME")
;; => "/Users/anton"

(.os.getenv "NONEXISTENT")
;; => ""
```

## `.os.setenv` { #os-setenv }

Signature: `(.os.setenv "NAME" "value")`. Both args must be strings. Returns `value` (retained) so the call composes inside larger expressions.

Errors: `type` (either arg not a string), `domain` (null pointer).

```lisp
(.os.setenv "RAY_DEBUG" "1")
```

## `.os.size` { #os-size }

Signature: `(.os.size "path")`. Returns the file size in bytes as an `i64`.

Errors: `type` (arg not a string), `io` (path missing or names a directory rather than a file). Wrap in `try` to distinguish "doesn't exist" from "is a directory" without parsing error messages — both surface as `io`.

```lisp
(.os.size "/etc/hosts")
;; => 213

(try (.os.size "/missing"))
;; => <error: io>
```

## `.os.list` { #os-list }

Signature: `(.os.list "path")`. Returns a sym vector of entries in the directory, sorted lexicographically. The `.` and `..` entries are filtered out.

Errors: `type` (arg not a string), `io` (path is not a directory, or doesn't exist).

```lisp
(.os.list "/etc")
;; => ['hosts 'passwd 'resolv.conf ...]
```

Use this as a building block — `find`-style recursive walks can be expressed by mapping `.os.list` over the result and filtering with `.os.size` / `try`.

## See also

- [`.sys.exec`](sys.md#sys-exec) — shell out for anything not covered here (test predicates, deletes, recursive operations).

# `.sys.args` — Application Arguments

**Date:** 2026-06-01
**Status:** Approved design

## Problem

Rayforce cannot pass command-line arguments through to Rayfall code:

1. `./rayforce -- -opt 123` fails with `cannot open '123'`. The launcher
   (`src/app/main.c`) has no `--` terminator: any non-flag token falls through
   to `file = argv[i]`, so `123` is treated as a script path.
2. The `args` builtin is a stub. `ray_args_fn` (`src/ops/system.c:608`) returns
   an empty list with the comment *"CLI args not wired into eval context"*, and
   `ray_runtime_create(argc, argv)` (`src/core/runtime.c:320`) ignores both
   `argc` and `argv`.
3. `args` is misnamed. The name belongs to lambda-parameter introspection, not
   application arguments. Application arguments belong in a proper system
   namespace, alongside the existing `.sys.*`, `.os.*`, `.time.*` builtins.

## Solution

Expose application arguments through a new builtin `.sys.args` that returns a
dictionary, and remove the misnamed `args` builtin.

### Shape

`(.sys.args)` returns a fixed-schema dict. Values are strings. Recognized
launcher flags sit at the top level; everything after `--` is parsed into a
`user` subdict.

```
./rayforce -p 5000 -c 4 -- -opt 123 --verbose --name ray

(.sys.args) → {
  file:        ""           / launcher flags — always present (stable schema)
  port:        "5000"
  cores:       "4"
  timeit:      "0"
  interactive: "0"
  log:         ""
  user:      { opt:"123", verbose:"", name:"ray" }
}
```

The top-level schema is **stable**: every non-sensitive launcher flag is always
present with its effective value (default when not passed), so Rayfall code can
read `(.sys.args)[`port]` without existence checks.

Top-level keys and defaults:

| key           | source flag        | default | notes                          |
|---------------|--------------------|---------|--------------------------------|
| `file`        | `-f` / positional  | `""`    | script path                    |
| `port`        | `-p`               | `"0"`   | IPC listen port                |
| `cores`       | `-c`               | `""`    | worker-pool size; `""` = auto  |
| `timeit`      | `-t`               | `"0"`   | `"0"` / `"1"`                  |
| `interactive` | `-i`               | `"0"`   | `"0"` / `"1"`                  |
| `log`         | `-l` / `-L`        | `""`    | journal base path              |

**Auth passwords are deliberately excluded.** `-u` / `-U` set a secret; there is
no `auth` key, so the password is never inspectable from Rayfall. Exposing it
would leak the secret to any code calling `(.sys.args)`.

### Parsing rules for the `user` subdict

Applied to the tokens after `--`:

- A token starting with `-` or `--` is a **key**; leading dashes are stripped to
  form the symbol (`-opt` and `--opt` both → symbol `opt`).
- Its **value** is the next token *iff* that token does not start with `-`;
  otherwise (or at end of argv) the value is `""`.
  - `-opt 123` → `opt:"123"`
  - `--verbose` → `verbose:""`
  - `-a -b` → `a:""`, `b:""`
- Duplicate keys: last wins (dict upsert semantics).
- A bare value token with no preceding key is ignored (it cannot form a pair).
- An empty key (a lone `-` or another `--`) is skipped.

## Architecture

Mirror the existing `poll` storage pattern on the runtime — `poll` is held as an
opaque `void*`, set once by the host, read back by builtins
(`runtime.h:96`, `runtime.c:340–346`).

1. **Runtime storage.** Add `void *sys_args` to `ray_runtime_t`
   (`src/core/runtime.h`). Add accessors:
   ```c
   void  ray_runtime_set_sys_args(void* dict);   /* takes ownership */
   void* ray_runtime_get_sys_args(void);
   ```
   `ray_runtime_destroy` releases `sys_args` if set.

2. **Dict builder.** New function in `src/ops/system.c` (part of librayforce, so
   it is unit-testable without a live process):
   ```c
   ray_t* ray_build_sys_args(int argc, char** argv);
   ```
   It scans `argv`, recognizes the launcher flags into the stable top-level
   schema (overlaying defaults), and on encountering `--` switches to the
   `user`-subdict pairing rules above. Dict construction follows the existing
   `ray_sym_vec_new` + `ray_list_new` + `ray_dict_new` idiom (see
   `ray_internals_fn`, `ray_sysinfo_fn`).

3. **Launcher wiring (`src/app/main.c`).**
   - Add a `--` terminator to the existing parse loop: `else if
     (strcmp(argv[i], "--") == 0) break;` placed before the final
     `else file = argv[i];`. This fixes the `cannot open '123'` bug by stopping
     post-`--` tokens from being read as a script path.
   - After parsing (and after the `--help` early-exit path, which never reaches
     this), call `ray_runtime_set_sys_args(ray_build_sys_args(argc, argv))`.
   - Update the `--help` text to document `--` argument passthrough.

4. **Builtin.** New `.sys.args`:
   ```c
   ray_t* ray_sys_args_fn(ray_t** args, int64_t n);   /* n must be 0 */
   ```
   Registered with `register_vary(".sys.args", RAY_FN_NONE, ray_sys_args_fn)` in
   `src/lang/eval.c`. Returns the stored dict (retained). In embedded/test
   contexts where no dict was set, returns an empty dict.

### Removing `args`

Delete every trace of the misnamed builtin:

- registration — `src/lang/eval.c:2852`
- prototype — `src/lang/internal.h:458`
- implementation `ray_args_fn` — `src/ops/system.c:608`
- test `test_syscov_args` — `test/test_runtime.c:475` and its table row (`:702`)
- doc rows — `docs/docs/language/functions.md:340`,
  `docs/docs/reference/all-functions.md:463`

A grep confirms these are the only references; no examples or other code call
`(args ...)`.

## Data flow

```
argv ──main.c parse──┐
                     ├─► ray_build_sys_args(argc, argv) ─► dict
                     │        (fixed schema + user subdict)
                     ▼
        ray_runtime_set_sys_args(dict)   [runtime owns it]
                     │
   Rayfall: (.sys.args) ─► ray_sys_args_fn ─► ray_runtime_get_sys_args ─► retained dict
                     │
        ray_runtime_destroy ─► release(sys_args)
```

## Error handling

- `(.sys.args extra)` (n != 0) → `domain` error, matching the other nullary
  `.sys.*` builtins.
- OOM during dict construction propagates a `ray_error("oom", ...)`, following
  the existing builder functions.
- No dict set (embedded use) → empty dict, never NULL.

## Testing

- **`ray_build_sys_args` unit tests** (in `test/test_runtime.c`), driving sample
  `argv` arrays directly:
  - no args → stable schema with all defaults, empty `user`.
  - recognized flags before `--` → correct top-level values.
  - `-- -opt 123 --verbose --name ray` → `user` = `{opt:"123", verbose:"",
    name:"ray"}`.
  - bare flags, adjacent flags (`-a -b`), trailing key at end of argv → `""`
    values.
  - duplicate user key → last wins.
  - auth flags `-u secret` → no `auth` key present; secret absent from dict.
- **Builtin test:** set a dict via `ray_runtime_set_sys_args`, assert
  `(.sys.args)` returns it; assert `(.sys.args 0)` arity error.
- **Removal:** delete `test_syscov_args`; confirm the suite builds with no
  remaining `args` references.

## Out of scope

- Repurposing a builtin for lambda-parameter introspection (the "rightful"
  meaning of `args`). That is a separate feature with its own design.
- GNU `--key=value` syntax and raw-argv access. Only `-k v` / `--k v` pairing is
  supported, per the agreed parsing rules.

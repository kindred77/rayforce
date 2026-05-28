# Early-returning `(return ...)` in compiled Rayfall lambdas

**Date:** 2026-05-28
**Status:** Design approved, ready for implementation plan

## Problem

`(return x)` is registered as a unary builtin (`ray_return_fn` in `src/ops/system.c:589`), but its implementation is a runtime identity — `ray_retain(x); return x;`. Inside a compiled lambda body, `(return x)` therefore has no effect on control flow:

```
((fn [] (return 7) 99))   ; today: 99   want: 7
```

The intent (per the existing source comment, *"early return from function (identity in Rayfall)"*) is that `return` should exit the enclosing lambda early. This design restores that behavior.

## Goal

Make `(return)` and `(return x)` exit the enclosing compiled lambda immediately, returning `null` or `x` respectively.

## Non-goals

- New `OP_RETURN` opcode distinct from `OP_RET`. We reuse the existing `OP_RET` (opcode 0).
- Multi-value return.
- Labeled / non-local returns (returning from an outer lambda from inside a nested one).
- Peephole optimization to elide the trailing epilogue `OP_RET` when the last user instruction is already `OP_RET`.

## Design

### Surface change

`return` becomes variadic with arity 0 or 1:

| Form | Result |
|---|---|
| `(return)` | exits enclosing lambda with `null` (`RAY_NULL_OBJ`) |
| `(return x)` | exits enclosing lambda with value of `x` |
| `(return a b ...)` | `domain` error |

When `return` is used outside a compiled lambda (e.g. typed at the REPL, or passed as a value such as `(map return xs)`), it falls back to the runtime builtin which behaves as identity for one arg, returns `null` for zero args, and errors otherwise.

### VM

No change. `OP_RET` already:

1. Pops the top-of-stack return value if present, otherwise uses `RAY_NULL_OBJ`.
2. Releases all locals and leftover stack slots above the current frame pointer.
3. Pops the return frame, restoring caller's `fn`/`code`/`cpool`/`n_locals`/`ip`/`fp`.
4. Pushes the result onto the caller's stack.

The compiler is the only thing changing.

### Compiler (`src/lang/compile.c`)

**Cached symbol id.** Alongside the other special-form sym caches:

```c
static _Thread_local int64_t sf_set = -1, sf_let = -1, sf_if = -1,
                              sf_do  = -1, sf_fn  = -1, sf_self = -1,
                              sf_try = -1, sf_return = -1;
...
sf_return = ray_sym_intern("return", 6);
```

**Trap-depth tracking.** Add a counter to `compiler_t`:

```c
int32_t trap_depth;   /* open OP_TRAP frames in current lambda */
```

Update the existing `(try body handler)` compilation so the counter brackets the body:

```c
if (sym_id == sf_try && n == 3) {
    int32_t err_slot = add_local(c, -1);
    if (err_slot < 0) { c->error = true; return; }
    int32_t trap_pos = emit_jump(c, OP_TRAP);
    c->trap_depth++;                  /* NEW */
    compile_expr(c, elems[1]);        /* body */
    c->trap_depth--;                  /* NEW */
    emit(c, OP_TRAP_END);
    int32_t jmp_pos = emit_jump(c, OP_JMP);
    patch_jump(c, trap_pos);
    /* handler path: trap already popped by vm_error_cleanup, so depth
       is already back to pre-try value, no extra adjustment needed. */
    emit(c, OP_STOREENV);
    emit(c, (uint8_t)err_slot);
    compile_expr(c, elems[2]);
    emit(c, OP_LOADENV);
    emit(c, (uint8_t)err_slot);
    emit(c, OP_CALLF);
    emit(c, 1);
    patch_jump(c, jmp_pos);
    return;
}
```

**`(return ...)` recognition.** Add to the special-form chain inside `compile_list`, before the general call path:

```c
/* (return) | (return x) — exit enclosing lambda */
if (sym_id == sf_return && (n == 1 || n == 2)) {
    if (n == 2) {
        compile_expr(c, elems[1]);
    } else {
        /* (return) — explicitly push null. OP_RET takes top-of-stack
           as the return value; if (return) is nested in an outer
           expression (e.g. (+ 1 (return))), the stack already holds
           intermediate values, so we must push our own null marker
           rather than relying on OP_RET's stack-empty fallback. */
        int32_t idx = add_constant(c, RAY_NULL_OBJ);
        emit_const(c, idx);
    }
    for (int32_t i = 0; i < c->trap_depth; i++)
        emit(c, OP_TRAP_END);
    emit(c, OP_RET);
    return;
}
```

`add_constant` deduplicates by pointer equality, so `RAY_NULL_OBJ` (a singleton) is added at most once per lambda. `ray_retain`/`ray_release` on it are no-ops because it is ARENA-flagged.

`(return a b)` and higher arities don't match this branch; they fall through to the general-call path, the builtin is invoked at runtime, and the variadic builtin raises a `domain` error.

### Runtime builtin (`src/ops/system.c`, `src/lang/internal.h`)

Change signature from unary to variadic.

`src/lang/internal.h`:

```c
ray_t* ray_return_fn(ray_t** args, int64_t n);
```

`src/ops/system.c`:

```c
/* (return) | (return x) — early exit from enclosing compiled lambda.
   Outside a compiled lambda this collapses to identity for one arg,
   null for zero. */
ray_t* ray_return_fn(ray_t** args, int64_t n) {
    if (n == 0) return RAY_NULL_OBJ;
    if (n == 1) { ray_retain(args[0]); return args[0]; }
    return ray_error("domain", "return expects 0 or 1 argument");
}
```

### Registration (`src/lang/eval.c:2849`)

```c
register_vary("return", RAY_FN_NONE, ray_return_fn);
```

(Replaces the current `register_unary("return", RAY_FN_NONE, ray_return_fn);`.)

## Edge cases

| Case | Behavior | Notes |
|---|---|---|
| `(return x)` at lambda tail | Two `OP_RET`s — user's and the epilogue's. The second is unreachable but harmless. | Optional peephole elision is out of scope. |
| `(return x)` inside `(if ...)` branch | Branch emits `OP_RET`; other branch and post-`if` code continue normally. | Standard early-exit; no special handling. |
| `(return x)` inside `(try body handler)` | One `OP_TRAP_END` emitted before `OP_RET`, popping the open trap frame. | Trap stack stays balanced. |
| `(return x)` inside `(try (do ... (try ...)) ...)` | `trap_depth == 2`; two `OP_TRAP_END`s emitted before `OP_RET`. | Nested try is handled by the counter. |
| `(return x)` inside a nested `(fn ...)` body | The inner lambda is compiled as a separate body; its `trap_depth` starts at 0. The return exits only the inner lambda. | Same semantics as every language with first-class closures. No "non-local return" supported. |
| `return` referenced as a value, e.g. `(map return xs)` | Symbol resolves to the variadic builtin; never enters the special-form branch. | Identity / null preserved. |
| `(return 1 2)` literal in compiled lambda | Falls through to general call path → vary builtin → `domain` error at runtime. | Arity error consistent with other vary builtins. |
| `(return)` in compiled lambda | Recognised by the `n == 1` branch; pushes `RAY_NULL_OBJ` from the constant pool, then `OP_RET`. | |
| `(+ 1 (return))` inside a compiled lambda | The explicit null push makes `OP_RET` use null, not the dangling `1`. The `1` is released by `OP_RET`'s frame-cleanup loop. | Verified by the design: see push-null reasoning in the compiler section. |
| `(return x)` at top level (not inside `fn`) | Not in a compiled lambda; tree-walking interpreter dispatches the builtin, which returns `x`. | No frame to unwind. |

## Tests

Append to `test/test_runtime.c::test_syscov_return` (or add an adjacent `test_syscov_return_early` test). All examples use only confirmed Rayfall syntax (integers, lists, `null`, `fn`, `if`, `try`, `map`, `>`).

| Expression | Expected |
|---|---|
| `((fn [] (return 7) 99))` | `7` |
| `((fn [] (return)))` | `null` |
| `((fn [x] (if (> x 0) (return 1)) 0) 5)` | `1` |
| `((fn [x] (if (> x 0) (return 1)) 0) -1)` | `0` |
| `((fn [] (try (return 42) (fn [e] e))))` | `42` |
| `((fn [] (+ 1 (return))))` | `null` |
| `(map return [1 2 3])` | `[1 2 3]` |
| `(return 1 2)` | `domain` error |

Existing assertions remain valid:

| Expression | Expected |
|---|---|
| `(return 7)` | `7` |
| `(return "hello")` | `"hello"` |

## Files touched

| File | Change | Approx LoC |
|---|---|---|
| `src/ops/system.c` | Rewrite `ray_return_fn` body | 5 |
| `src/lang/internal.h` | Update declaration | 1 |
| `src/lang/eval.c` | `register_unary` → `register_vary` | 1 |
| `src/lang/compile.c` | `sf_return` cache, `trap_depth` field, increment/decrement in `try`, `return` special-form branch | ~15 |
| `test/test_runtime.c` | Expand `test_syscov_return` | ~10 |

## Risk

Low. The change is mechanical, additive at the compiler level (one new special-form branch), and reuses an already-exercised opcode. The only correctness concern is trap-stack balance inside `try`, which is handled by `trap_depth`. The signature change for `ray_return_fn` is private to the language layer — the only existing reference outside the registration site is `test/test_runtime.c:403`, which evaluates `return` only through the public `ray_eval_str` API and is not affected by the C signature.

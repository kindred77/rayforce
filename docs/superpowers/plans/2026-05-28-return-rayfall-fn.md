# Early-returning `(return ...)` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `(return)` and `(return x)` exit the enclosing compiled Rayfall lambda early, returning `null` or `x` respectively, by recognising `return` as a compile-time form that emits `OP_RET` (preceded by balancing `OP_TRAP_END`s for any open `try` frames).

**Architecture:** Three coupled changes. (1) The runtime builtin `ray_return_fn` becomes variadic (0 or 1 arg), so the fallback / value-passed case (`(map return xs)`) keeps working. (2) The bytecode compiler in `src/lang/compile.c` gets a `trap_depth` counter on `compiler_t`, brackets the `try` body with `++/--`, and a new special-form branch for `(return ...)` that emits a value-push, the right number of `OP_TRAP_END`s, then `OP_RET`. (3) Registration is switched from `register_unary` to `register_vary`. No VM-side change — `OP_RET` (opcode 0) already does the work.

**Tech Stack:** C17, GNU Make, AddressSanitizer + UBSan via `make test`. No new dependencies.

**Source of truth:** `docs/superpowers/specs/2026-05-28-return-rayfall-fn-design.md`.

---

## File Structure

| File | Role | Change kind |
|---|---|---|
| `src/lang/internal.h` | C declaration of `ray_return_fn` | Modify (signature) |
| `src/ops/system.c` | Runtime builtin body | Modify (rewrite ~5 lines) |
| `src/lang/eval.c` | Builtin registration | Modify (one line) |
| `src/lang/compile.c` | Bytecode compiler — `try` body bracketing + new `return` special-form branch | Modify (~20 lines added) |
| `test/test_runtime.c` | Test cases for runtime builtin and compiled-lambda semantics | Modify (expand `test_syscov_return`) |

No new files. All changes localised to the `lang` + `ops` subtrees plus the test harness.

---

## Pre-flight (run once before starting)

- [ ] **Establish a green baseline.**

Run: `make clean && make test`
Expected: binary builds, `./rayforce.test` runs to completion with no failures. Note the test count; it should not drop later.

If this fails, fix the failure first — do not proceed with this plan on top of pre-existing breakage.

---

## Task 1: Convert `ray_return_fn` to variadic (runtime builtin)

**Files:**
- Modify: `src/lang/internal.h:457`
- Modify: `src/ops/system.c:589-593`
- Modify: `src/lang/eval.c:2849`
- Test: `test/test_runtime.c` — existing `test_syscov_return` covers the runtime path

- [ ] **Step 1: Update declaration in `src/lang/internal.h`.**

Find the existing line:

```c
ray_t* ray_return_fn(ray_t* x);
```

Replace with:

```c
ray_t* ray_return_fn(ray_t** args, int64_t n);
```

- [ ] **Step 2: Rewrite the body in `src/ops/system.c`.**

Replace lines 589-593 (the current 5-line identity stub) with:

```c
/* (return) | (return x) — early exit from enclosing compiled lambda.
 * Outside a compiled lambda (e.g. value position: (map return xs), or
 * REPL top-level) this collapses to: identity for one arg, null for
 * zero, domain error otherwise. The early-exit semantics are emitted
 * by the bytecode compiler — see compile.c. */
ray_t* ray_return_fn(ray_t** args, int64_t n) {
    if (n == 0) return RAY_NULL_OBJ;
    if (n == 1) { ray_retain(args[0]); return args[0]; }
    return ray_error("domain", "return expects 0 or 1 argument");
}
```

- [ ] **Step 3: Switch registration in `src/lang/eval.c:2849`.**

Find:

```c
    register_unary("return",     RAY_FN_NONE, ray_return_fn);
```

Replace with:

```c
    register_vary("return",      RAY_FN_NONE, ray_return_fn);
```

(Keep column alignment with neighbouring `register_vary` calls.)

- [ ] **Step 4: Build.**

Run: `make`
Expected: clean build, no warnings (Makefile uses `-Werror`).

If a warning fires about an unused/changed signature, the most likely cause is a stale declaration somewhere else — grep for `ray_return_fn` and check.

- [ ] **Step 5: Run existing return tests.**

Run: `make test 2>&1 | grep -E "syscov_return|FAIL"`
Expected: `runtime/syscov_return ... PASS`. No FAIL lines.

The two existing assertions — `(return 7)` → `7` and `(return "hello")` → `"hello"` — still hit the runtime builtin (not yet compiled). They go through the new variadic dispatch and must keep working.

- [ ] **Step 6: Commit.**

```bash
git add src/lang/internal.h src/ops/system.c src/lang/eval.c
git commit -m "$(cat <<'EOF'
refactor(lang): return becomes variadic (0 or 1 arg)

Switch ray_return_fn from unary to RAY_VARY:
  (return)   -> null
  (return x) -> x
  (return a b ...) -> domain error

Runtime behaviour is unchanged for the existing one-arg case; this
prepares the builtin for the compiler-side early-return change.
EOF
)"
```

---

## Task 2: Add `trap_depth` bookkeeping in the compiler

**Files:**
- Modify: `src/lang/compile.c:33-52` (compiler_t struct)
- Modify: `src/lang/compile.c:306-328` (try special form)

- [ ] **Step 1: Add the field to `compiler_t`.**

In `src/lang/compile.c`, find the `compiler_t` struct (starts around line 33). Locate the existing field block ending with `int32_t   dbg_len;` (around line 51). Add a new field directly below it, inside the struct:

```c
    int32_t  trap_depth;  /* open OP_TRAP frames in current lambda */
```

The struct is `memset`-zeroed in `compiler_init`, so the field starts at 0 — no extra initialiser code needed.

- [ ] **Step 2: Bracket the `try` body with `++/--`.**

Find the existing `try` special-form branch in `compile_list`:

```c
        /* (try body handler) — compile to OP_TRAP/OP_TRAP_END */
        if (sym_id == sf_try && n == 3) {
            /* Reserve a hidden local for err_val */
            int32_t err_slot = add_local(c, -1);
            if (err_slot < 0) { c->error = true; return; }

            int32_t trap_pos = emit_jump(c, OP_TRAP);
            compile_expr(c, elems[1]);       /* body */
            emit(c, OP_TRAP_END);
            ...
```

Insert `c->trap_depth++` immediately after `emit_jump(c, OP_TRAP)` and `c->trap_depth--` immediately before `emit(c, OP_TRAP_END)`. Result:

```c
        /* (try body handler) — compile to OP_TRAP/OP_TRAP_END */
        if (sym_id == sf_try && n == 3) {
            /* Reserve a hidden local for err_val */
            int32_t err_slot = add_local(c, -1);
            if (err_slot < 0) { c->error = true; return; }

            int32_t trap_pos = emit_jump(c, OP_TRAP);
            c->trap_depth++;
            compile_expr(c, elems[1]);       /* body */
            c->trap_depth--;
            emit(c, OP_TRAP_END);
            int32_t jmp_pos = emit_jump(c, OP_JMP);
            patch_jump(c, trap_pos);         /* handler starts here */
            /* err_val is on stack (pushed by vm_error_cleanup).
             * Stash it, compile handler fn, reload err_val, call. */
            emit(c, OP_STOREENV);
            emit(c, (uint8_t)err_slot);
            compile_expr(c, elems[2]);       /* handler fn */
            emit(c, OP_LOADENV);
            emit(c, (uint8_t)err_slot);
            emit(c, OP_CALLF);
            emit(c, 1);                     /* call handler(err_val) */
            patch_jump(c, jmp_pos);          /* end */
            return;
        }
```

Note the handler body is compiled with `trap_depth` at its outer (pre-`try`) value — correct, because if we're in the handler at runtime the trap frame has already been popped by `vm_error_cleanup`.

- [ ] **Step 3: Build, verify no regressions yet.**

Run: `make && make test 2>&1 | tail -20`
Expected: clean build, all tests still pass. Trap depth is tracked but not yet read by anyone, so behaviour is unchanged.

- [ ] **Step 4: Commit.**

```bash
git add src/lang/compile.c
git commit -m "$(cat <<'EOF'
compile: track open trap depth in compiler_t

Adds trap_depth counter on compiler_t and brackets the (try ...) body
with ++/--. No behaviour change yet — used by the upcoming (return ...)
compile-time form to emit the right number of OP_TRAP_ENDs before
OP_RET.
EOF
)"
```

---

## Task 3: Compile-time `(return ...)` special form

**Files:**
- Modify: `src/lang/compile.c:194-206` (`sf_*` thread-locals + `init_sf_syms`)
- Modify: `src/lang/compile.c:218-329` (special-form chain in `compile_list`)

- [ ] **Step 1: Add `sf_return` to the cached-symbol declarations.**

Find:

```c
static _Thread_local int64_t sf_set = -1, sf_let = -1, sf_if = -1, sf_do = -1, sf_fn = -1, sf_self = -1, sf_try = -1;
```

Replace with:

```c
static _Thread_local int64_t sf_set = -1, sf_let = -1, sf_if = -1, sf_do = -1, sf_fn = -1, sf_self = -1, sf_try = -1, sf_return = -1;
```

- [ ] **Step 2: Intern the symbol in `init_sf_syms`.**

Find the existing body:

```c
static void init_sf_syms(void) {
    if (sf_set >= 0) return;
    sf_set  = ray_sym_intern("set", 3);
    sf_let  = ray_sym_intern("let", 3);
    sf_if   = ray_sym_intern("if",  2);
    sf_do   = ray_sym_intern("do",  2);
    sf_fn   = ray_sym_intern("fn",  2);
    sf_self = ray_sym_intern("self", 4);
    sf_try  = ray_sym_intern("try",  3);
}
```

Add the `return` row at the end (still inside the function):

```c
    sf_return = ray_sym_intern("return", 6);
```

Also fix the reset helper at the bottom of the file. Find:

```c
void ray_compile_reset(void) {
    sf_set = sf_let = sf_if = sf_do = sf_fn = sf_self = sf_try = -1;
}
```

Replace with:

```c
void ray_compile_reset(void) {
    sf_set = sf_let = sf_if = sf_do = sf_fn = sf_self = sf_try = sf_return = -1;
}
```

- [ ] **Step 3: Add the `(return ...)` branch in `compile_list`.**

Find the `try` branch (the last special-form check in the chain, ends with `return;` just before the closing `}` of the `if (head->type == -RAY_SYM ...)` block). Insert the new branch immediately after the `try` branch, still inside the `if (head->type == -RAY_SYM && (head->attrs & RAY_ATTR_NAME))` block:

```c
        /* (return) | (return x) — early exit from enclosing compiled
         * lambda. (return) pushes RAY_NULL_OBJ explicitly because
         * OP_RET takes top-of-stack as the result; if (return) sits
         * inside an outer expression that has already pushed values
         * (e.g. (+ 1 (return))), relying on OP_RET's stack-empty
         * fallback would return a stale value. */
        if (sym_id == sf_return && (n == 1 || n == 2)) {
            if (n == 2) {
                compile_expr(c, elems[1]);
            } else {
                int32_t idx = add_constant(c, RAY_NULL_OBJ);
                emit_const(c, idx);
            }
            for (int32_t i = 0; i < c->trap_depth; i++)
                emit(c, OP_TRAP_END);
            emit(c, OP_RET);
            return;
        }
```

`(return a b ...)` (arity ≥ 2 user args ⇒ `n ≥ 3`) does not match this branch; it falls through to the general-call path and dispatches the variadic builtin at runtime, which raises the `domain` error from Task 1.

- [ ] **Step 4: Build.**

Run: `make`
Expected: clean build.

- [ ] **Step 5: Run the full suite to confirm no regressions.**

Run: `make test 2>&1 | tail -20`
Expected: all green. Existing tests don't exercise `(return ...)` inside `(fn ...)`, so this should be pure addition.

- [ ] **Step 6: Commit.**

```bash
git add src/lang/compile.c
git commit -m "$(cat <<'EOF'
compile: (return ...) special form emits OP_RET

Recognise (return) and (return x) as a compile-time form inside
compile_list. Emits the value (or RAY_NULL_OBJ for zero-arg), then
one OP_TRAP_END per open trap frame, then OP_RET — letting the
existing OP_RET handler unwind the lambda's frame and return to the
caller.

(return ...) as a value (e.g. (map return xs)) still falls through to
the runtime builtin. (return a b ...) falls through to a runtime
domain error from the variadic builtin.
EOF
)"
```

---

## Task 4: Tests — runtime builtin variadic + compile-time early return

**Files:**
- Modify: `test/test_runtime.c:403-408` (replace `test_syscov_return`)

- [ ] **Step 1: Replace the existing test function.**

Find:

```c
/* return builtin (ray_return_fn) */
static test_result_t test_syscov_return(void) {
    TEST_ASSERT_TRUE(eval_eq("(return 7)", "7"));
    TEST_ASSERT_TRUE(eval_eq("(return \"hello\")", "\"hello\""));
    PASS();
}
```

Replace with:

```c
/* return builtin (ray_return_fn) — runtime variadic + compiled early-exit */
static test_result_t test_syscov_return(void) {
    /* ── Runtime builtin path (no enclosing compiled lambda) ── */
    TEST_ASSERT_TRUE(eval_eq("(return 7)", "7"));
    TEST_ASSERT_TRUE(eval_eq("(return \"hello\")", "\"hello\""));
    TEST_ASSERT_TRUE(eval_eq("(return)", "null"));
    TEST_ASSERT_TRUE(eval_err("(return 1 2)", "domain"));

    /* ── Compile-time early-return inside (fn ...) ── */
    /* Plain early return overrides trailing expressions. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (return 7) 99))", "7"));
    /* Zero-arg form returns null. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (return)))", "null"));
    /* Early return from a conditional branch. */
    TEST_ASSERT_TRUE(eval_eq("((fn [x] (if (> x 0) (return 1)) 0) 5)",  "1"));
    TEST_ASSERT_TRUE(eval_eq("((fn [x] (if (> x 0) (return 1)) 0) -1)", "0"));
    /* return inside (try ...) — must emit OP_TRAP_END before OP_RET. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (try (return 42) (fn [e] e))))", "42"));
    /* (return) nested in a partially-evaluated expression — must push
     * null, not bubble the lingering 1. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (+ 1 (return))))", "null"));
    /* return passed as a value still works as the variadic builtin. */
    TEST_ASSERT_TRUE(eval_eq("(map return [1 2 3])", "[1 2 3]"));

    PASS();
}
```

- [ ] **Step 2: Build.**

Run: `make`
Expected: clean build.

- [ ] **Step 3: Run the new test alone for fast feedback.**

Run: `./rayforce.test 2>&1 | grep -E "syscov_return|FAIL"`
Expected: `runtime/syscov_return ... PASS`, no FAIL lines.

If a single assertion fails, run the binary with verbose mode (the test harness's verbose flag may differ; in the absence of one, isolate the failing line by commenting out preceding `TEST_ASSERT_TRUE` calls and rebuilding).

Likely first-failure suspects and fixes:
- `((fn [] (return)))` → wrong value: likely `(return)` not recognised (Task 3 Step 1/3) or constant pool not deduping null properly.
- `((fn [] (try (return 42) (fn [e] e))))` returns the trap frame value or crashes under ASan: `trap_depth` not bracketed correctly (Task 2 Step 2).
- `((fn [] (+ 1 (return))))` returns `1`: forgot the explicit `emit_const(RAY_NULL_OBJ)` in the `n == 1` branch (Task 3 Step 3).
- `(map return [1 2 3])` errors with arity / type: variadic registration not in effect (Task 1 Step 3).

- [ ] **Step 4: Run the full suite to confirm nothing else regressed.**

Run: `make test`
Expected: full test binary passes, including the IPC-server-spawning tests that depend on `./rayforce`.

- [ ] **Step 5: Commit.**

```bash
git add test/test_runtime.c
git commit -m "$(cat <<'EOF'
test(runtime): cover variadic return + compiled early-exit

Adds assertions for:
- (return), (return x), (return a b ...) at the runtime builtin path
- (return ...) inside a compiled (fn ...) — plain, conditional, inside
  (try ...), and nested in an outer expression
- (return) as a value via (map return [...])
EOF
)"
```

---

## Task 5: Verification pass — sanitisers, full suite, sample REPL session

**Files:** none (verification only)

- [ ] **Step 1: Sanitiser-clean build + run.**

Run: `make clean && make test`
Expected: AddressSanitizer + UBSan are on in `DEBUG_CFLAGS`; the test binary must complete with no `==ERROR:` lines and no UBSan diagnostics. If anything fires, fix before claiming completion — do not silence the sanitisers.

- [ ] **Step 2: Optional manual REPL check.**

Run: `echo '((fn [] (return 42) 99))' | ./rayforce`
Expected: `42`

Run: `echo '((fn [] (try (return 42) (fn [e] e))))' | ./rayforce`
Expected: `42`

Run: `echo '(map return [1 2 3])' | ./rayforce`
Expected: a 3-element list of integers `1`, `2`, `3` in whatever shape the pretty-printer uses.

- [ ] **Step 3: Tidy.**

If any commit message needs a fix or earlier commits are out of order, leave them alone — the plan favours new commits over amend/rebase. The five commits from Tasks 1-4 form the change set.

---

## Self-review pass

**Spec coverage:**
- Variadic builtin (`(return)`, `(return x)`, error on ≥2): Task 1.
- Registration switch: Task 1, Step 3.
- `trap_depth` counter + try bracketing: Task 2.
- `sf_return` interning + `compile_list` branch + explicit null push: Task 3.
- All seven new test cases + existing two: Task 4.
- Sanitiser-clean verification: Task 5.

No spec section is uncovered.

**Placeholder scan:** Every step has either the exact code to write, the exact command to run, or an explicit "no-op verification" purpose. No TBDs, no "add error handling", no "similar to Task N".

**Type consistency:** `ray_return_fn` signature appears in three places — internal.h declaration (Task 1 Step 1), system.c definition (Task 1 Step 2), eval.c registration (Task 1 Step 3). All three use `(ray_t**, int64_t)` exactly. `trap_depth` field name is consistent between the struct (Task 2 Step 1) and reader (Task 3 Step 3). `sf_return` is consistent across declaration (Task 3 Step 1), intern (Task 3 Step 2), reset (Task 3 Step 2), and reader (Task 3 Step 3).

Plan is complete and consistent with the spec.

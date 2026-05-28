# `.time.*` namespace and v1-style scheduling timer

**Date:** 2026-05-28
**Status:** Design approved, ready for implementation plan

## Problem

Master has a `timer` builtin (`ray_timer_fn` in `src/ops/system.c:650`) that is just a wrapper around `clock()` returning nanoseconds — useful for one-shot benchmarking, but not what a user calling `(timer 1000 0 (fn [x] ...))` expects.

v1 has a real scheduling timer (`ray_timer` in `core/chrono.c:361`): a heap of pending callbacks, integrated with the IOCP/poll loop, that fires lambdas after a delay (and optionally repeats). The polymorphic `(timer ...)` overload distinguishes "schedule" from "cancel" by arity.

This design ports v1's scheduler into master, splits the polymorphic form into clearly-named functions, and parks the existing nanosecond clock under the same namespace.

## Goal

Add three Rayfall functions:

| Form | Returns |
|---|---|
| `(.time.now)` | current monotonic time in **milliseconds** (i64) |
| `(.time.timer.set ms num fn)` | i64 timer id |
| `(.time.timer.del id)` | null |

Integrate the timer heap into `ray_poll_run` so callbacks fire whenever the poll loop is active (REPL idle, IPC server, server-only mode).

## Non-goals

- Pause / resume / list / introspect timers.
- Sub-millisecond precision.
- Firing timers during a long-running synchronous evaluation (timers only fire when control returns to the poll loop).
- Wall-clock scheduling (this is monotonic; suspend during a long sleep won't shift the deadline).
- Backwards compatibility with the existing flat `timer` name — the nanosecond helper is removed.

## Semantics

### `(.time.now)`

- Arity 0 (variadic, n must be 0; any other arity → `domain`).
- Returns an i64 — milliseconds since some monotonic epoch. Same source as the scheduler's deadlines (`clock_gettime(CLOCK_MONOTONIC, ...)` on Linux/macOS, `QueryPerformanceCounter` on Windows).
- Not restricted; safe from untrusted IPC peers.

### `(.time.timer.set ms num fn)`

- Arity 3 (variadic; any other arity → `domain`).
- `ms` (arg 1): i64. The interval between fires AND the delay until the first fire. Must be ≥ 0.
- `num` (arg 2): i64. **`0` = fire forever.** `N ≥ 1` = fire exactly N times. Negative → `domain`.
  - This differs from v1, where `0` quirkily mapped to `NULL_I64` and meant "fire once".
- `fn` (arg 3): a `RAY_LAMBDA` whose declared parameter list has exactly one entry.
- Returns the new timer's i64 id. The id is monotonically increasing per runtime, never reused after deletion.
- Restricted: blocked under `-U` with an `access` error.
- All argument validation happens **before** the timer is added to the heap; on any error, the heap is unchanged.

### `(.time.timer.del id)`

- Arity 1 (unary). Non-i64 id → `type` error.
- Removes the timer with that id from the heap, releasing its retained lambda.
- Returns null. If the id is not present, returns null without error (idempotent delete).
- Restricted under `-U`.

### Callback invocation

When a timer fires:
1. The heap entry is popped.
2. The lambda is called via `call_fn1(fn, make_i64(now_ms))`.
3. Whatever the callback returns is released and discarded — there is no value channel back to the scheduler. The lambda is treated as a procedure for its side effects.
4. If the lambda raises an error, the error is formatted via `ray_fmt` and printed to `stderr` (one line, prefixed `timer <id>:` so it can be filtered). The error is then released. The timer continues as if the call had succeeded — one fire is still counted toward `num`.
5. Re-schedule logic:
   - `num == 0` (forever): re-push with `exp_ms += tic_ms`. `num` stays at 0.
   - `num == 1`: last fire just happened; release the lambda and free the entry.
   - `num ≥ 2`: decrement `num`, re-push with `exp_ms += tic_ms`.

### Timer heap ordering

- Min-heap keyed on `exp_ms`.
- Ties broken by `id` (lower id fires first when deadlines coincide).
- Heap is per `ray_poll_t`; one timer set per runtime. (No timers without a poll loop — which means no timers in the worktree-less REPL fallback at `src/app/repl.c:run_interactive`'s `else` branch. Acceptable: that fallback is only reached on OOM.)

## Architecture

### Data structures (new)

`src/core/timer.h`:

```c
typedef struct ray_timer {
    int64_t id;       /* monotonically increasing per runtime */
    int64_t tic_ms;   /* interval between fires */
    int64_t exp_ms;   /* next fire deadline (monotonic) */
    int64_t num;      /* 0 = forever, ≥1 = remaining fires */
    ray_t*  fn;       /* retained lambda */
} ray_timer_t;

typedef struct ray_timers {
    ray_timer_t** heap;     /* min-heap by exp_ms */
    int64_t       n;        /* current size */
    int64_t       cap;      /* allocated capacity */
    int64_t       next_id;  /* monotonic counter */
} ray_timers_t;
```

`src/core/timer.c` exports:

```c
ray_timers_t* ray_timers_create(int64_t initial_cap);
void          ray_timers_destroy(ray_timers_t* t);  /* releases all callbacks */
int64_t       ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn);
bool          ray_timers_del(ray_timers_t* t, int64_t id);
int64_t       ray_timers_next_deadline_ms(ray_timers_t* t);  /* INT64_MAX if empty */
void          ray_timers_fire_expired(ray_timers_t* t);      /* uses ray_time_now_ms() */
int64_t       ray_time_now_ms(void);                          /* CLOCK_MONOTONIC ms */
```

### Poll integration

`src/core/poll.h` — `ray_poll_t` gains:

```c
struct ray_poll {
    ...
    ray_timers_t* timers;   /* nullable; lazily created on first .time.timer.set */
};
```

`src/core/poll.c` / per-platform backend (`epoll.c`, `kqueue.c`, `iocp.c`):

`ray_poll_run`'s loop becomes:

```c
while (poll->code < 0) {
    int64_t deadline = poll->timers
        ? ray_timers_next_deadline_ms(poll->timers)
        : INT64_MAX;
    int64_t now      = ray_time_now_ms();
    int wait_ms      = (deadline == INT64_MAX) ? -1 /* infinite */
                     : (int)MAX(0, MIN(deadline - now, INT_MAX));

    int n_events = backend_wait(poll, wait_ms);
    /* process I/O events as today */
    for (i = 0; i < n_events; i++) { ... }

    if (poll->timers) ray_timers_fire_expired(poll->timers);
}
```

`ray_poll_create` does NOT eagerly allocate the heap — `timers` stays `NULL` until the first `.time.timer.set` call lazily creates it. This keeps poll cost zero for callers that never use timers.

`ray_poll_destroy` calls `ray_timers_destroy(poll->timers)` which walks the heap releasing each `fn`.

### Builtins (`src/ops/system.c`)

```c
ray_t* ray_time_now_fn       (ray_t** args, int64_t n);
ray_t* ray_time_timer_set_fn (ray_t** args, int64_t n);
ray_t* ray_time_timer_del_fn (ray_t*  id);
```

All three reach the active poll via `ray_runtime_get_poll()` (defined in `src/core/runtime.h:113`) — `runtime.h` keeps `poll` opaque (`void*`) to avoid a header cycle with `poll.h`, so callers cast at the use site. Same access pattern as existing IPC builtins.

### Registration (`src/lang/eval.c`)

Replace the existing line `register_unary("timer", RAY_FN_NONE, ray_timer_fn);` with three new registrations:

```c
/* .time.* — time access and scheduling */
register_vary (".time.now",       RAY_FN_NONE,       ray_time_now_fn);
register_vary (".time.timer.set", RAY_FN_RESTRICTED, ray_time_timer_set_fn);
register_unary(".time.timer.del", RAY_FN_RESTRICTED, ray_time_timer_del_fn);
```

The old `ray_timer_fn` is removed from `src/ops/system.c` and `src/lang/internal.h`.

### Reserved-name guards

Symbols beginning with `.` are already reserved in master (see `ray_sym_is_reserved` in `src/core/sym.c`). The new names `.time.now`, `.time.timer.set`, `.time.timer.del` inherit that protection — user code cannot shadow them via `let` or `set`.

## Reference counting

| Event | Action |
|---|---|
| `.time.timer.set` accepts `fn` | `ray_retain(fn)`, store on heap |
| Heap entry popped on fire | `call_fn1(fn, time)`; do NOT release after call |
| Re-push (num > 0 or 0) | keep same retained ref |
| Final fire (num == 1 → 0 path) | `ray_release(fn)`, free entry |
| `.time.timer.del` finds entry | `ray_release(fn)`, free entry |
| `ray_poll_destroy` walks heap | for each entry, `ray_release(fn)`, free |

Callback's return value is `ray_release`d after each fire (it's owned by the caller per the standard call protocol).

## Errors

| Form / condition | Error |
|---|---|
| `(.time.now x ...)` | `domain` |
| `(.time.timer.set ...)` arity ≠ 3 | `domain` |
| `(.time.timer.set non-i64 ...)` | `type` |
| `(.time.timer.set neg-ms ...)` | `domain` |
| `(.time.timer.set ms neg-num ...)` | `domain` |
| `(.time.timer.set ms num non-lambda)` | `type` |
| `(.time.timer.set ms num lambda-with-arity≠1)` | `domain` |
| `.time.timer.set` under `-U` | `access` (existing RAY_FN_RESTRICTED guard fires before the body runs) |
| `(.time.timer.del non-i64)` | `type` |
| `(.time.timer.del unknown-id)` | returns `null` (idempotent) |
| `.time.timer.del` under `-U` | `access` |
| Callback lambda raises during fire | error printed to stderr; timer continues |

## Tests

New test functions in `test/test_runtime.c`:

**`test_syscov_time_now`** — covers the simple case:

```c
ray_t* r = ray_eval_str("(.time.now)");
TEST_ASSERT_NOT_NULL(r);
TEST_ASSERT_FALSE(RAY_IS_ERR(r));
TEST_ASSERT_EQ_I(r->type, -RAY_I64);
TEST_ASSERT_TRUE(r->i64 > 0);
ray_release(r);
/* arity error on non-zero args */
TEST_ASSERT_TRUE(eval_err("(.time.now 1)", "domain"));
```

**`test_syscov_time_timer_set_del`** — argument validation only (the fire-loop test below is the integration test):

```c
TEST_ASSERT_TRUE(eval_err("(.time.timer.set)",             "domain"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000)",        "domain"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0)",      "domain"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set \"x\" 0 (fn [t] t))", "type"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 \"x\" (fn [t] t))", "type"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set -1 0 (fn [t] t))",   "domain"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 -1 (fn [t] t))","domain"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0 42)",         "type"));
TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0 (fn [a b] a))","domain"));

/* del returns null on unknown id and on a freshly cancelled id */
{
    ray_t* id = ray_eval_str("(.time.timer.set 100000 1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(id);
    TEST_ASSERT_EQ_I(id->type, -RAY_I64);
    /* cancel */
    char src[128];
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)id->i64);
    ray_t* r = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_NULL(r));
    ray_release(id);
    ray_release(r);
}
/* del of bogus id is also null */
{
    ray_t* r = ray_eval_str("(.time.timer.del 999999)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_NULL(r));
    ray_release(r);
}
TEST_ASSERT_TRUE(eval_err("(.time.timer.del \"x\")", "type"));
```

**`test_syscov_time_timer_fires`** — integration test that drives the poll loop briefly:

This test needs the runtime's poll instance to be present and `ray_poll_run` to be called for a bounded number of iterations. The plan will add a small test helper `ray_poll_run_for(poll, ms)` (or set a stop-after-N-ticks flag) — exact shape TBD in the plan. The body of the test:

```c
/* schedule: increment a counter, then exit the poll loop after 3 fires */
ray_eval_str("(set counter 0)");
ray_eval_str("(set tid (.time.timer.set 5 3 (fn [t] (set counter (+ counter 1)))))");
ray_poll_run_for(rt->poll, 100 /* ms budget */);
ray_t* c = ray_eval_str("counter");
TEST_ASSERT_EQ_I(c->i64, 3);
ray_release(c);
```

Three test cases registered in the test table (around `test/test_runtime.c:568-580`):

```c
{ "runtime/syscov_time_now",            test_syscov_time_now,            sys_setup, sys_teardown },
{ "runtime/syscov_time_timer_set_del",  test_syscov_time_timer_set_del,  sys_setup, sys_teardown },
{ "runtime/syscov_time_timer_fires",    test_syscov_time_timer_fires,    sys_setup, sys_teardown },
```

The existing `test_syscov_timer` (which tested the removed nanosecond helper) is dropped from both the function set and the registration table.

## Files touched

| File | Change |
|---|---|
| `src/core/timer.h` | NEW — types + API |
| `src/core/timer.c` | NEW — heap operations + fire loop + `ray_time_now_ms` |
| `src/core/poll.h` | add `ray_timers_t* timers` field to `ray_poll_t` |
| `src/core/poll.c` | optional shared helper if timer-fire logic is consolidated outside the backend |
| `src/core/epoll.c` | wire timer-aware timeout in `ray_poll_run` wait, call `ray_timers_fire_expired` after `epoll_wait` returns |
| `src/core/kqueue.c` | same change for macOS/BSD backend |
| `src/core/iocp.c` | same change for Windows backend |
| `src/ops/system.c` | remove `ray_timer_fn`; add `ray_time_now_fn`, `ray_time_timer_set_fn`, `ray_time_timer_del_fn` |
| `src/lang/internal.h` | remove `ray_timer_fn` decl; add three new decls |
| `src/lang/eval.c` | swap registration: drop `register_unary("timer", ...)`, add three `.time.*` registrations |
| `test/test_runtime.c` | drop `test_syscov_timer`; add three new tests + table rows |
| `Makefile` | should pick up `src/core/timer.c` automatically via the `LIB_SRC` glob — no edit expected |
| `website/docs/rayfall-functions.html` | drop the old `timer` row; add three `.time.*` rows |
| `website/docs/control-flow.html` OR a new page | document scheduling-timer semantics, example, restricted-mode note |

## Risk

Moderate. The core heap and dispatch are straightforward (mirror of v1). The risks live at the integration boundary:

1. **Poll-loop timeout arithmetic.** Each backend (`epoll_wait`, `kqueue`, `iocp`) takes a timeout in slightly different units / signs. The plan will require a per-backend wrapper that converts an i64 ms into the right type and handles "infinite wait" correctly.

2. **Test harness for fire loop.** The fire-loop test needs the poll loop to actually run. Master's `ray_poll_run` is open-ended (runs until `ray_poll_exit` is called). The plan will add a bounded variant (`ray_poll_run_for(poll, budget_ms)`) used only by the test — production code stays on the open-ended form.

3. **Callback errors that themselves call `.time.timer.del`** on the currently-firing id. Need to confirm `ray_timers_fire_expired` reads the popped entry from a local variable, not from the heap (so a mid-fire deletion doesn't corrupt the iteration). The design pops first, then fires, then re-pushes — which naturally avoids this hazard.

4. **Timer firing during REPL eval.** Timers won't fire during a long synchronous eval (`(map slow-fn xs)`). This is intentional — matches v1, no surprise for users. Documented in the website page.

## Out of scope (future)

- `.time.timer.list` for introspection.
- A timer-friendly progress-bar / heartbeat hook.
- Pausing the runtime's timer set (e.g. for journal replay).
- Replacing the in-process timer heap with a kernel timer (timerfd, kqueue EVFILT_TIMER) — the user-space heap is simpler and avoids platform divergence.

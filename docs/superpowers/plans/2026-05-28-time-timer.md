# `.time.*` namespace + scheduling timer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `(.time.now)` (monotonic ms), `(.time.timer.set ms num fn)` (returns id), and `(.time.timer.del id)` to Rayfall. Timers are stored in a min-heap on `ray_poll_t` and fired during `ray_poll_run` whenever expired. Remove master's existing `(timer 0)` benchmarking helper.

**Architecture:** Standalone timer module (`src/core/timer.{h,c}`) owning a min-heap and a monotonic clock. `ray_poll_t` carries an optional pointer to that heap (lazily allocated on first `.time.timer.set`). Each backend's `ray_poll_run` (`epoll.c`, `kqueue.c`) becomes: compute deadline, wait with bounded timeout, process I/O, then fire expired timers. Builtins in `src/ops/system.c` reach the active poll via `ray_runtime_get_poll()`.

**Tech Stack:** C17. POSIX `clock_gettime(CLOCK_MONOTONIC)` for time. `epoll_wait` / `kevent` for waits. No new dependencies.

**Source of truth:** `docs/superpowers/specs/2026-05-28-time-timer-design.md`.

---

## File Structure

| File | Role | Change kind |
|---|---|---|
| `src/core/timer.h` | Types (`ray_timer_t`, `ray_timers_t`) + API | Create |
| `src/core/timer.c` | Heap operations, monotonic clock, fire loop | Create |
| `src/core/poll.h` | `ray_poll_t` gains `ray_timers_t* timers` field | Modify (1 line in struct) |
| `src/core/epoll.c` | Wire `ray_poll_run` to use bounded timeout + fire timers | Modify (~15 lines in run loop, ~2 lines in destroy) |
| `src/core/kqueue.c` | Same as epoll.c for BSD/macOS backend | Modify (~15 lines, ~2 lines) |
| `src/ops/system.c` | Drop `ray_timer_fn`; add three `.time.*` builtins | Modify (~80 lines net) |
| `src/lang/internal.h` | Drop `ray_timer_fn` decl; add 3 new decls | Modify (4 lines) |
| `src/lang/eval.c` | Swap registrations: drop `timer`, add 3 `.time.*` | Modify (3 lines) |
| `test/test_runtime.c` | Drop `test_syscov_timer`; add 3 new tests | Modify (~80 lines + 3 table rows) |
| `website/docs/rayfall-functions.html` | Drop old `timer` row, add 3 `.time.*` rows | Modify (4 lines) |

The fire-loop test needs a bounded-runtime helper to drive the poll loop. **It will be added as a test-only entry point in `src/core/timer.c`** (not the poll backends), to keep production backends untouched. The helper is `ray_timers_pump_for(timers, budget_ms)` and runs the timer fire-loop in a tight sleep poll — see Task 8.

---

## Pre-flight

- [ ] **Verify a green baseline.**

Run: `make clean && make test 2>&1 | tail -5`
Expected: build is clean and `=== <N> of <N+small> passed (... skipped, 0 failed) ===`. Note the test count.

If anything fails, fix it before starting. The pre-existing flaky `streaming_large_dag` stack overflow may surface intermittently — if you hit it, re-run; it is not introduced by this branch.

---

## Task 1: Create timer module (skeleton + clock + heap creation/destruction)

**Files:**
- Create: `src/core/timer.h`
- Create: `src/core/timer.c`

This task introduces the header and the heap-allocation skeleton, plus the monotonic clock helper. No fire loop or push/pop yet — those land in later tasks.

- [ ] **Step 1: Create `src/core/timer.h`.**

Content:

```c
/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_TIMER_H
#define RAY_TIMER_H

#include <rayforce.h>
#include <stdbool.h>

/* Single scheduled timer.  Lives in ray_timers_t.heap. */
typedef struct {
    int64_t id;       /* unique within a ray_timers_t; never reused */
    int64_t tic_ms;   /* interval between fires (and delay until first fire) */
    int64_t exp_ms;   /* next fire deadline, monotonic ms */
    int64_t num;      /* 0 = forever; ≥1 = remaining fires (decremented each fire) */
    ray_t*  fn;       /* retained 1-arity lambda */
} ray_timer_t;

/* Min-heap keyed on (exp_ms, id).  Owns its entries and their lambdas. */
typedef struct {
    ray_timer_t** heap;
    int64_t       n;
    int64_t       cap;
    int64_t       next_id;
} ray_timers_t;

/* Create an empty heap with `initial_cap` slots (rounded up to 1).
 * Returns NULL on allocation failure. */
ray_timers_t* ray_timers_create(int64_t initial_cap);

/* Free the heap and release every retained callback lambda. */
void ray_timers_destroy(ray_timers_t* t);

/* Push a new timer.  Retains `fn`; caller's reference is unchanged.
 * Returns the new id (≥ 0) on success, -1 on allocation failure. */
int64_t ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn);

/* Remove the timer with `id`.  Releases its lambda.
 * Returns true if found+removed, false otherwise. */
bool ray_timers_del(ray_timers_t* t, int64_t id);

/* Deadline (monotonic ms) of the soonest-firing timer, or INT64_MAX if empty. */
int64_t ray_timers_next_deadline_ms(ray_timers_t* t);

/* Fire every timer whose exp_ms <= now.  Pops, calls callback via call_fn1,
 * then either re-pushes (forever / num>1) or frees (num==1 / num==0 one-shot).
 * Callback errors are printed to stderr and counted as a successful fire. */
void ray_timers_fire_expired(ray_timers_t* t);

/* Current monotonic time in milliseconds. */
int64_t ray_time_now_ms(void);

#endif /* RAY_TIMER_H */
```

- [ ] **Step 2: Create `src/core/timer.c` (skeleton with clock + create/destroy only).**

Content:

```c
/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "core/timer.h"
#include "mem/sys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int64_t ray_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

ray_timers_t* ray_timers_create(int64_t initial_cap) {
    if (initial_cap < 1) initial_cap = 1;
    ray_timers_t* t = (ray_timers_t*)ray_sys_alloc(sizeof(ray_timers_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->heap = (ray_timer_t**)ray_sys_alloc((size_t)initial_cap * sizeof(ray_timer_t*));
    if (!t->heap) { ray_sys_free(t); return NULL; }
    t->cap = initial_cap;
    return t;
}

void ray_timers_destroy(ray_timers_t* t) {
    if (!t) return;
    for (int64_t i = 0; i < t->n; i++) {
        ray_release(t->heap[i]->fn);
        ray_sys_free(t->heap[i]);
    }
    ray_sys_free(t->heap);
    ray_sys_free(t);
}

/* Stubs — implemented in later tasks. */
int64_t ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn) {
    (void)t; (void)tic_ms; (void)num; (void)fn;
    return -1;
}

bool ray_timers_del(ray_timers_t* t, int64_t id) {
    (void)t; (void)id;
    return false;
}

int64_t ray_timers_next_deadline_ms(ray_timers_t* t) {
    if (!t || t->n == 0) return INT64_MAX;
    return t->heap[0]->exp_ms;
}

void ray_timers_fire_expired(ray_timers_t* t) {
    (void)t;
}
```

- [ ] **Step 3: Build.**

Run: `make`
Expected: `src/core/timer.c` is picked up by the `wildcard src/*/*.c` glob in the Makefile. Build succeeds with no warnings (`-Werror` active).

- [ ] **Step 4: Commit.**

```bash
git add src/core/timer.h src/core/timer.c
git commit -m "$(cat <<'EOF'
feat(timer): module skeleton — types, monotonic clock, create/destroy

ray_time_now_ms returns CLOCK_MONOTONIC milliseconds.
ray_timers_create / destroy manage heap allocation and lambda release.
Heap operations and fire loop land in subsequent commits.
EOF
)"
```

---

## Task 2: Heap push/pop primitives (internal)

**Files:**
- Modify: `src/core/timer.c`

Add static helpers (`timer_heap_up`, `timer_heap_down`, `timer_heap_push`, `timer_heap_pop`) used by `ray_timers_add` / `ray_timers_del` / `ray_timers_fire_expired`. Keep them file-static so they don't pollute the public API.

- [ ] **Step 1: Replace the stub `ray_timers_add` and add internal heap helpers.**

In `src/core/timer.c`, find the block of stubs and replace them with the following (the `_fire_expired` and `_del` stubs stay; only `_add` becomes real, and helpers are added above it):

```c
/* Min-heap ordering: lower exp_ms wins; ties broken by lower id. */
static int heap_less(const ray_timer_t* a, const ray_timer_t* b) {
    if (a->exp_ms != b->exp_ms) return a->exp_ms < b->exp_ms;
    return a->id < b->id;
}

static void heap_swap(ray_timer_t** heap, int64_t i, int64_t j) {
    ray_timer_t* tmp = heap[i];
    heap[i] = heap[j];
    heap[j] = tmp;
}

static void heap_up(ray_timer_t** heap, int64_t i) {
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap_less(heap[i], heap[parent])) {
            heap_swap(heap, i, parent);
            i = parent;
        } else break;
    }
}

static void heap_down(ray_timer_t** heap, int64_t n, int64_t i) {
    for (;;) {
        int64_t l = 2 * i + 1;
        int64_t r = 2 * i + 2;
        int64_t best = i;
        if (l < n && heap_less(heap[l], heap[best])) best = l;
        if (r < n && heap_less(heap[r], heap[best])) best = r;
        if (best == i) break;
        heap_swap(heap, i, best);
        i = best;
    }
}

static bool heap_grow(ray_timers_t* t) {
    int64_t new_cap = t->cap * 2;
    ray_timer_t** new_heap = (ray_timer_t**)ray_sys_alloc(
        (size_t)new_cap * sizeof(ray_timer_t*));
    if (!new_heap) return false;
    memcpy(new_heap, t->heap, (size_t)t->n * sizeof(ray_timer_t*));
    ray_sys_free(t->heap);
    t->heap = new_heap;
    t->cap = new_cap;
    return true;
}

int64_t ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn) {
    if (!t) return -1;
    if (t->n >= t->cap && !heap_grow(t)) return -1;

    ray_timer_t* timer = (ray_timer_t*)ray_sys_alloc(sizeof(ray_timer_t));
    if (!timer) return -1;
    timer->id     = t->next_id++;
    timer->tic_ms = tic_ms;
    timer->exp_ms = ray_time_now_ms() + tic_ms;
    timer->num    = num;
    timer->fn     = fn;
    ray_retain(fn);

    t->heap[t->n] = timer;
    heap_up(t->heap, t->n);
    t->n++;
    return timer->id;
}
```

- [ ] **Step 2: Build.**

Run: `make`
Expected: clean.

- [ ] **Step 3: Commit.**

```bash
git add src/core/timer.c
git commit -m "$(cat <<'EOF'
feat(timer): min-heap push + internal helpers

Adds static heap_less/swap/up/down/grow primitives and the real
ray_timers_add. Each push retains the callback lambda; capacity
doubles on overflow. ray_timers_del and ray_timers_fire_expired
remain stubs.
EOF
)"
```

---

## Task 3: Heap delete by id

**Files:**
- Modify: `src/core/timer.c`

Replace the `ray_timers_del` stub with a linear-scan delete (the heap is small enough that O(n) is fine; a heap-with-index-map is YAGNI).

- [ ] **Step 1: Implement `ray_timers_del`.**

Replace the existing stub with:

```c
bool ray_timers_del(ray_timers_t* t, int64_t id) {
    if (!t) return false;
    for (int64_t i = 0; i < t->n; i++) {
        if (t->heap[i]->id != id) continue;
        ray_release(t->heap[i]->fn);
        ray_sys_free(t->heap[i]);
        t->n--;
        if (i < t->n) {
            t->heap[i] = t->heap[t->n];
            /* Rebalance: may need to sift up or down. */
            heap_down(t->heap, t->n, i);
            heap_up(t->heap, i);
        }
        return true;
    }
    return false;
}
```

- [ ] **Step 2: Build.**

Run: `make`
Expected: clean.

- [ ] **Step 3: Commit.**

```bash
git add src/core/timer.c
git commit -m "$(cat <<'EOF'
feat(timer): delete-by-id via linear scan + heap rebalance

ray_timers_del swaps the removed slot with the heap tail and sifts
the new occupant into place. Linear scan is fine for small heaps;
a position-indexed heap is YAGNI.
EOF
)"
```

---

## Task 4: Fire-expired loop

**Files:**
- Modify: `src/core/timer.c`
- Modify (include): `src/core/timer.c` already includes what it needs; this task pulls in headers for `call_fn1` and `ray_fmt`.

Replace the `ray_timers_fire_expired` stub with the actual loop.

- [ ] **Step 1: Add additional includes at the top of `src/core/timer.c`.**

Below the existing `#include "core/timer.h"` line, add:

```c
#include "lang/eval.h"      /* call_fn1, ray_eval helpers, RAY_IS_ERR */
#include "lang/format.h"    /* ray_fmt */
```

- [ ] **Step 2: Implement `ray_timers_fire_expired`.**

Replace the existing stub with:

```c
void ray_timers_fire_expired(ray_timers_t* t) {
    if (!t || t->n == 0) return;

    int64_t now = ray_time_now_ms();

    while (t->n > 0 && t->heap[0]->exp_ms <= now) {
        /* Pop the head: swap with tail, sift down. */
        ray_timer_t* timer = t->heap[0];
        t->n--;
        if (t->n > 0) {
            t->heap[0] = t->heap[t->n];
            heap_down(t->heap, t->n, 0);
        }

        /* Fire the callback.  call_fn1 takes ownership of the arg
         * (a freshly-allocated i64) and returns a new ref the caller
         * must release. */
        ray_t* arg    = make_i64(now);
        ray_t* result = call_fn1(timer->fn, arg);
        ray_release(arg);
        if (result) {
            if (RAY_IS_ERR(result)) {
                ray_t* msg = ray_fmt(result, 0);
                if (msg && ray_str_ptr(msg)) {
                    fprintf(stderr, "timer %lld: %.*s\n",
                            (long long)timer->id,
                            (int)ray_str_len(msg),
                            ray_str_ptr(msg));
                }
                if (msg) ray_release(msg);
                ray_error_free(result);
            } else {
                ray_release(result);
            }
        }

        /* Re-schedule or free. */
        if (timer->num == 0) {
            /* Forever: re-push. */
            timer->exp_ms += timer->tic_ms;
            t->heap[t->n] = timer;
            heap_up(t->heap, t->n);
            t->n++;
        } else if (timer->num > 1) {
            timer->num--;
            timer->exp_ms += timer->tic_ms;
            t->heap[t->n] = timer;
            heap_up(t->heap, t->n);
            t->n++;
        } else {
            /* num == 1: last fire just happened. */
            ray_release(timer->fn);
            ray_sys_free(timer);
        }

        /* Refresh `now` in case callbacks took meaningful time. */
        now = ray_time_now_ms();
    }
}
```

- [ ] **Step 3: Build.**

Run: `make`
Expected: clean. If `call_fn1`, `make_i64`, `ray_fmt`, `ray_str_ptr`, `ray_str_len`, `ray_error_free` are not visible from the includes added, locate the right header (likely `lang/internal.h` for `make_i64` and the str helpers) and add the include.

- [ ] **Step 4: Commit.**

```bash
git add src/core/timer.c
git commit -m "$(cat <<'EOF'
feat(timer): fire-expired loop with re-schedule semantics

ray_timers_fire_expired pops expired entries, invokes each callback
via call_fn1, prints any error to stderr (prefixed with timer id),
then re-pushes (forever / num>1) or frees (num==1). Refreshes
the wall-clock between fires so a slow callback doesn't compound.
EOF
)"
```

---

## Task 5: Wire timers into `ray_poll_t`

**Files:**
- Modify: `src/core/poll.h`

Add the field. No backend changes yet — those land in Task 6 / Task 7.

- [ ] **Step 1: Add the field to `ray_poll`.**

In `src/core/poll.h`, find:

```c
struct ray_poll {
    int64_t          fd;       /* epoll/kqueue/iocp handle */
    int64_t          code;     /* exit code (-1 = running) */
    ray_selector_t** sels;     /* selector array */
    uint32_t         n_sels;
    uint32_t         sel_cap;
    char             auth_secret[256]; /* password from -u/-U, empty = no auth */
    bool             restricted;       /* true if -U (read-only IPC mode) */
};
```

Add one line before the closing `};`:

```c
    void*            timers;    /* opaque ray_timers_t*; lazily allocated */
```

(`void*` avoids pulling `core/timer.h` into `poll.h`. Backends and `system.c` cast at the use site, the same pattern `runtime.h` uses for `poll`.)

- [ ] **Step 2: Build.**

Run: `make`
Expected: clean (the new field is zero-initialised by the existing `memset(poll, 0, sizeof(*poll))` in `ray_poll_create`).

- [ ] **Step 3: Commit.**

```bash
git add src/core/poll.h
git commit -m "$(cat <<'EOF'
feat(poll): add opaque timers field to ray_poll_t

ray_poll_t carries an opaque pointer to ray_timers_t (declared in
core/timer.h). Lazily allocated by .time.timer.set; backends use
it to bound their wait timeout and fire expired callbacks.
EOF
)"
```

---

## Task 6: Wire timer fire-loop into `epoll.c`

**Files:**
- Modify: `src/core/epoll.c`

Two changes: in `ray_poll_destroy`, free the timer heap; in `ray_poll_run`, compute a bounded `epoll_wait` timeout from the next-deadline and call `ray_timers_fire_expired` after each wait returns.

- [ ] **Step 1: Add the include at the top of `src/core/epoll.c`.**

Find the existing `#include` block near the top of the file and add:

```c
#include "core/timer.h"
```

(Keep the include alphabetised with existing project-relative includes.)

- [ ] **Step 2: Add timer cleanup to `ray_poll_destroy`.**

Find `ray_poll_destroy` (around line 59). Before the final `ray_sys_free(poll);`, add:

```c
    if (poll->timers) {
        ray_timers_destroy((ray_timers_t*)poll->timers);
        poll->timers = NULL;
    }
```

- [ ] **Step 3: Modify the `ray_poll_run` loop to use a bounded timeout + fire expired timers.**

Find the existing `while (poll->code < 0)` loop (around line 159). Replace the existing `int n = epoll_wait((int)poll->fd, events, RAY_POLL_MAX_EVENTS, -1);` line with:

```c
        int wait_ms = -1;
        if (poll->timers) {
            int64_t deadline = ray_timers_next_deadline_ms(
                (ray_timers_t*)poll->timers);
            if (deadline != INT64_MAX) {
                int64_t now = ray_time_now_ms();
                int64_t delta = deadline - now;
                if (delta < 0) delta = 0;
                if (delta > INT_MAX) delta = INT_MAX;
                wait_ms = (int)delta;
            }
        }

        int n = epoll_wait((int)poll->fd, events, RAY_POLL_MAX_EVENTS, wait_ms);
```

Then find the end of the `for (int i = 0; i < n; i++)` loop. After the loop's closing brace but inside the `while (poll->code < 0)` block, add:

```c
        if (poll->timers)
            ray_timers_fire_expired((ray_timers_t*)poll->timers);
```

Make sure `<limits.h>` is included for `INT_MAX`. If it isn't already, add `#include <limits.h>` to the file's includes.

- [ ] **Step 4: Build.**

Run: `make`
Expected: clean. The change is no-op when `poll->timers == NULL` (the common case for IPC-only servers).

- [ ] **Step 5: Run existing tests to confirm no regression.**

Run: `make test 2>&1 | tail -3`
Expected: same pass count as the baseline.

- [ ] **Step 6: Commit.**

```bash
git add src/core/epoll.c
git commit -m "$(cat <<'EOF'
feat(epoll): timer-aware wait + fire expired callbacks

ray_poll_run now bounds the epoll_wait timeout to the next timer
deadline (or -1 if no timers are registered), and calls
ray_timers_fire_expired after each batch of I/O events.
ray_poll_destroy releases the timer heap.

No behaviour change when no timers exist (the common server path).
EOF
)"
```

---

## Task 7: Wire timer fire-loop into `kqueue.c`

**Files:**
- Modify: `src/core/kqueue.c`

Mirror of Task 6 for the macOS/BSD backend. kqueue's `kevent` takes a `struct timespec*` for the timeout; pass `NULL` for infinite, otherwise a stack-allocated `struct timespec`.

- [ ] **Step 1: Add the include.**

Add to the top:

```c
#include "core/timer.h"
```

- [ ] **Step 2: Add cleanup in `ray_poll_destroy`.**

Same as Task 6 Step 2: before the final `ray_sys_free(poll);` in `ray_poll_destroy`, add:

```c
    if (poll->timers) {
        ray_timers_destroy((ray_timers_t*)poll->timers);
        poll->timers = NULL;
    }
```

- [ ] **Step 3: Modify `ray_poll_run` for the timer-bounded wait.**

Find the existing `while (poll->code < 0)` loop (around line 163). Replace the existing `int n = kevent((int)poll->fd, NULL, 0, events, RAY_POLL_MAX_EVENTS, NULL);` with:

```c
        struct timespec  ts;
        struct timespec* timeout = NULL;
        if (poll->timers) {
            int64_t deadline = ray_timers_next_deadline_ms(
                (ray_timers_t*)poll->timers);
            if (deadline != INT64_MAX) {
                int64_t now = ray_time_now_ms();
                int64_t delta = deadline - now;
                if (delta < 0) delta = 0;
                ts.tv_sec  = (time_t)(delta / 1000);
                ts.tv_nsec = (long)((delta % 1000) * 1000000L);
                timeout = &ts;
            }
        }

        int n = kevent((int)poll->fd, NULL, 0, events,
                       RAY_POLL_MAX_EVENTS, timeout);
```

Then after the `for (int i = 0; i < n; i++)` loop's closing brace, inside the `while (poll->code < 0)` block, add:

```c
        if (poll->timers)
            ray_timers_fire_expired((ray_timers_t*)poll->timers);
```

- [ ] **Step 4: Build.**

Run: `make`
Expected: clean.

- [ ] **Step 5: Run tests.**

Run: `make test 2>&1 | tail -3`
Expected: same pass count as baseline.

- [ ] **Step 6: Commit.**

```bash
git add src/core/kqueue.c
git commit -m "$(cat <<'EOF'
feat(kqueue): timer-aware wait + fire expired callbacks

Mirrors the epoll backend: bounds kevent's timeout via a
stack-local struct timespec, calls ray_timers_fire_expired after
each batch. No-op when poll->timers is NULL.
EOF
)"
```

---

## Task 8: Test-only helper — `ray_timers_pump_for`

**Files:**
- Modify: `src/core/timer.h` (add prototype)
- Modify: `src/core/timer.c` (add implementation)

Production code drives timers via `ray_poll_run`. Tests need a bounded loop that fires timers without involving epoll/kqueue. This is the smallest possible affordance: a sleep + fire pump.

- [ ] **Step 1: Add the prototype to `src/core/timer.h`.**

Add after `int64_t ray_time_now_ms(void);`:

```c
/* TEST-ONLY: pump the timer heap for up to `budget_ms` milliseconds.
 * Sleeps in small slices and calls ray_timers_fire_expired between
 * slices.  Not intended for production code — ray_poll_run is the
 * real driver. */
void ray_timers_pump_for(ray_timers_t* t, int64_t budget_ms);
```

- [ ] **Step 2: Implement in `src/core/timer.c`.**

Add this function below `ray_timers_fire_expired`:

```c
void ray_timers_pump_for(ray_timers_t* t, int64_t budget_ms) {
    if (!t || budget_ms <= 0) return;
    int64_t end = ray_time_now_ms() + budget_ms;
    while (ray_time_now_ms() < end) {
        ray_timers_fire_expired(t);
        struct timespec slice = { 0, 1000000L };  /* 1 ms */
        nanosleep(&slice, NULL);
    }
    ray_timers_fire_expired(t);
}
```

- [ ] **Step 3: Build.**

Run: `make`
Expected: clean.

- [ ] **Step 4: Commit.**

```bash
git add src/core/timer.h src/core/timer.c
git commit -m "$(cat <<'EOF'
test(timer): add ray_timers_pump_for for bounded fire-loop in tests

Production code drives the timer heap via ray_poll_run; tests need a
deterministic-budget pump that doesn't require an epoll/kqueue fd.
1 ms sleep slices between fire calls, exits after budget_ms.
EOF
)"
```

---

## Task 9: Replace the old `timer` builtin with three `.time.*` builtins

**Files:**
- Modify: `src/ops/system.c`
- Modify: `src/lang/internal.h`
- Modify: `src/lang/eval.c`

This is the user-visible API change: drop `ray_timer_fn`, add `ray_time_now_fn`, `ray_time_timer_set_fn`, `ray_time_timer_del_fn`. Register them under the new names.

- [ ] **Step 1: Update `src/lang/internal.h`.**

Find the existing declaration:

```c
ray_t* ray_timer_fn(ray_t* x);
```

Replace with:

```c
ray_t* ray_time_now_fn       (ray_t** args, int64_t n);
ray_t* ray_time_timer_set_fn (ray_t** args, int64_t n);
ray_t* ray_time_timer_del_fn (ray_t*  id);
```

- [ ] **Step 2: Replace the body in `src/ops/system.c`.**

Find the existing `ray_timer_fn` definition (around line 650):

```c
/* (timer) -- return high-res timestamp in nanoseconds for benchmarking */
ray_t* ray_timer_fn(ray_t* x) {
    (void)x;
    clock_t t = clock();
    int64_t nanos = (int64_t)((double)t / (double)CLOCKS_PER_SEC * 1e9);
    return make_i64(nanos);
}
```

Replace it with the three new functions:

```c
/* (.time.now) -- current monotonic time in milliseconds */
ray_t* ray_time_now_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("domain", ".time.now takes no arguments");
    return make_i64(ray_time_now_ms());
}

/* (.time.timer.set ms num fn) -- schedule fn to fire every ms; returns id */
ray_t* ray_time_timer_set_fn(ray_t** args, int64_t n) {
    if (n != 3)
        return ray_error("domain", ".time.timer.set expects 3 arguments");
    if (args[0]->type != -RAY_I64) return ray_error("type", "ms must be i64");
    if (args[1]->type != -RAY_I64) return ray_error("type", "num must be i64");
    if (args[2]->type != RAY_LAMBDA)
        return ray_error("type", "fn must be a lambda");

    int64_t ms  = args[0]->i64;
    int64_t num = args[1]->i64;
    if (ms  < 0) return ray_error("domain", "ms must be non-negative");
    if (num < 0) return ray_error("domain", "num must be non-negative");

    /* Check lambda arity: declared params list must have exactly 1 element. */
    ray_t* params = LAMBDA_PARAMS(args[2]);
    if (!params || ray_len(params) != 1)
        return ray_error("domain", "fn must accept exactly 1 argument");

    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll) return ray_error("domain", "no poll loop active");

    if (!poll->timers) {
        poll->timers = ray_timers_create(16);
        if (!poll->timers) return ray_error("oom", NULL);
    }

    int64_t id = ray_timers_add((ray_timers_t*)poll->timers, ms, num, args[2]);
    if (id < 0) return ray_error("oom", NULL);
    return make_i64(id);
}

/* (.time.timer.del id) -- cancel a timer; returns null */
ray_t* ray_time_timer_del_fn(ray_t* id) {
    if (id->type != -RAY_I64) return ray_error("type", "id must be i64");
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll || !poll->timers) return RAY_NULL_OBJ;
    ray_timers_del((ray_timers_t*)poll->timers, id->i64);
    return RAY_NULL_OBJ;
}
```

You will need new includes at the top of `src/ops/system.c`. Add (after the existing ones):

```c
#include "core/poll.h"
#include "core/timer.h"
#include "core/runtime.h"
```

`LAMBDA_PARAMS` and `ray_len` are already used elsewhere in `src/ops/`; if they're not visible in `system.c`, add `#include "lang/eval.h"`.

- [ ] **Step 3: Swap the registrations in `src/lang/eval.c`.**

Find the existing line (around line 2870):

```c
    register_unary("timer",      RAY_FN_NONE, ray_timer_fn);
```

Replace with three lines, preserving the column alignment of neighbouring `register_*` calls:

```c
    register_vary (".time.now",       RAY_FN_NONE,       ray_time_now_fn);
    register_vary (".time.timer.set", RAY_FN_RESTRICTED, ray_time_timer_set_fn);
    register_unary(".time.timer.del", RAY_FN_RESTRICTED, ray_time_timer_del_fn);
```

- [ ] **Step 4: Build.**

Run: `make`
Expected: clean. If the linker reports `undefined symbol: ray_timer_fn`, search for any remaining references — there should be none after the internal.h + system.c + eval.c edits. The test in `test/test_runtime.c` still references the OLD name via `(timer 0)` in the source string; that doesn't cause a link error (it's a string), but the test will fail when invoked. That's fixed in Task 10.

- [ ] **Step 5: Commit.**

```bash
git add src/lang/internal.h src/ops/system.c src/lang/eval.c
git commit -m "$(cat <<'EOF'
feat(time): .time.now, .time.timer.set, .time.timer.del builtins

Replaces the previous nanosecond-clock `timer` builtin with a
three-function .time.* namespace:
  (.time.now)               -> monotonic ms
  (.time.timer.set ms num f) -> schedule, returns id (RESTRICTED)
  (.time.timer.del id)       -> cancel, returns null (RESTRICTED)

Lazily allocates the timer heap on the active poll on first .set.
Argument validation is fully synchronous (no heap mutation on errors).
num=0 means fire forever; num=N>=1 means exactly N fires.
EOF
)"
```

---

## Task 10: Tests — runtime coverage for `.time.*`

**Files:**
- Modify: `test/test_runtime.c`

Drop the old `test_syscov_timer`. Add three new test functions plus their registrations.

- [ ] **Step 1: Drop the old test.**

Find the existing test (around line 478-490 in master):

```c
/* timer builtin (ray_timer_fn) */
static test_result_t test_syscov_timer(void) {
    ray_t* r = ray_eval_str("(timer 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    ray_release(r);
    PASS();
}
```

Delete the function (and the divider comment line if it's exclusively for this test).

Find the registration row in the test table (search for `runtime/syscov_timer`), e.g.:

```c
    { "runtime/syscov_timer",                test_syscov_timer,                sys_setup, sys_teardown },
```

Delete that line.

- [ ] **Step 2: Add the new tests above the `args` test (or wherever fits the existing section ordering).**

Add an include near the top if `core/timer.h` is not already pulled in:

```c
#include "core/timer.h"
```

Then add the three test functions:

```c
/* .time.now */
static test_result_t test_syscov_time_now(void) {
    ray_t* r = ray_eval_str("(.time.now)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    TEST_ASSERT_TRUE(r->i64 > 0);
    ray_release(r);
    TEST_ASSERT_TRUE(eval_err("(.time.now 1)", "domain"));
    PASS();
}

/* .time.timer.set / .time.timer.del — argument validation + idempotent delete */
static test_result_t test_syscov_time_timer_set_del(void) {
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set)",              "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000)",         "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0)",       "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set \"x\" 0 (fn [t] t))", "type"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 \"x\" (fn [t] t))", "type"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set -1 0 (fn [t] t))",      "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 -1 (fn [t] t))",   "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0 42)",            "type"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0 (fn [a b] a))",  "domain"));

    /* Schedule then cancel — del returns null. */
    {
        ray_t* id = ray_eval_str("(.time.timer.set 100000 1 (fn [t] t))");
        TEST_ASSERT_NOT_NULL(id);
        TEST_ASSERT_FALSE(RAY_IS_ERR(id));
        TEST_ASSERT_EQ_I(id->type, -RAY_I64);
        char src[128];
        snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)id->i64);
        ray_t* r = ray_eval_str(src);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(id);
        ray_release(r);
    }

    /* Del of bogus id returns null (idempotent). */
    {
        ray_t* r = ray_eval_str("(.time.timer.del 999999)");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(r);
    }

    TEST_ASSERT_TRUE(eval_err("(.time.timer.del \"x\")", "type"));
    PASS();
}

/* Integration: schedule, pump the heap for 100 ms, verify N fires happened. */
static test_result_t test_syscov_time_timer_fires(void) {
    /* Reach the runtime's active poll directly. */
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    /* Set up a counter and schedule a 3-shot timer that increments it. */
    ray_t* setup_counter = ray_eval_str("(set .test.counter 0)");
    if (setup_counter) ray_release(setup_counter);
    ray_t* setup_timer = ray_eval_str(
        "(.time.timer.set 5 3 (fn [t] (set .test.counter (+ .test.counter 1))))");
    TEST_ASSERT_NOT_NULL(setup_timer);
    TEST_ASSERT_FALSE(RAY_IS_ERR(setup_timer));
    ray_release(setup_timer);

    /* Pump for 100 ms — enough for 3 fires at 5 ms each, with slack. */
    ray_timers_pump_for((ray_timers_t*)poll->timers, 100);

    /* Verify the counter advanced. */
    ray_t* c = ray_eval_str(".test.counter");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, -RAY_I64);
    TEST_ASSERT_EQ_I(c->i64, 3);
    ray_release(c);
    PASS();
}
```

Note: the integration test uses `set .test.counter` (an `.test.*` symbol). If reserved-name guards block `.test.*` in `set`, switch to a non-reserved name like `tcounter`. Check by running the test; if you see a `reserve` error, replace `.test.counter` with `tcounter` in both the setup and verify expressions.

- [ ] **Step 3: Register the new tests in the test table.**

Find the section where `runtime/syscov_*` tests are registered (near line 568 in master). Add three new rows:

```c
    { "runtime/syscov_time_now",             test_syscov_time_now,             sys_setup, sys_teardown },
    { "runtime/syscov_time_timer_set_del",   test_syscov_time_timer_set_del,   sys_setup, sys_teardown },
    { "runtime/syscov_time_timer_fires",     test_syscov_time_timer_fires,     sys_setup, sys_teardown },
```

Note: the `fires` test relies on a poll loop existing. `sys_setup` does `ray_runtime_create(0, NULL)` — verify that this also creates a poll. If it does not, the test must construct one explicitly. Inspect `sys_setup` in `test_runtime.c` (around line 375) and `ray_runtime_create` in `src/core/runtime.c`. If the poll is only created in `main.c`, add this snippet to a new `sys_setup_with_poll` setup function used by the `fires` test only:

```c
static ray_poll_t* g_test_poll = NULL;
static void sys_setup_with_poll(void) {
    ray_runtime_create(0, NULL);
    g_test_poll = ray_poll_create();
    ray_runtime_set_poll(g_test_poll);
}
static void sys_teardown_with_poll(void) {
    ray_runtime_set_poll(NULL);
    ray_poll_destroy(g_test_poll);
    g_test_poll = NULL;
    ray_runtime_destroy(__RUNTIME);
}
```

Then use `sys_setup_with_poll` / `sys_teardown_with_poll` for the `fires` row.

- [ ] **Step 4: Build.**

Run: `make`
Expected: clean.

- [ ] **Step 5: Run the new tests.**

Run: `make test 2>&1 | tee /tmp/timer_tests.log | tail -3 ; grep "syscov_time" /tmp/timer_tests.log`
Expected:
- `runtime/syscov_time_now ... PASS`
- `runtime/syscov_time_timer_set_del ... PASS`
- `runtime/syscov_time_timer_fires ... PASS`
- Summary line shows green.

If `syscov_time_timer_fires` fails because the counter reads 0:
- The poll either doesn't exist (Task 9 returned `"no poll loop active"` error) → fix the setup function.
- The lambda call_fn1 path is failing → check that `(set .test.counter ...)` actually mutates a visible binding by running it stand-alone first. If `.test.*` is reserved, switch to a non-reserved name as noted.

- [ ] **Step 6: Run the full suite.**

Run: `make test 2>&1 | tail -3`
Expected: same pass count as baseline + 2 (added 3, removed 1).

- [ ] **Step 7: Commit.**

```bash
git add test/test_runtime.c
git commit -m "$(cat <<'EOF'
test(runtime): cover .time.* namespace

- runtime/syscov_time_now: arity + return type
- runtime/syscov_time_timer_set_del: argument validation + idempotent delete
- runtime/syscov_time_timer_fires: integration — schedule 3 fires,
  pump for 100 ms, verify counter advanced

Drops the obsolete runtime/syscov_timer test (was checking the
removed nanosecond clock helper).
EOF
)"
```

---

## Task 11: Website documentation

**Files:**
- Modify: `website/docs/rayfall-functions.html`

The function reference page has one row for the old `timer` builtin. Replace it with three rows.

- [ ] **Step 1: Update the table row.**

Find the line (around 567):

```html
          <tr><td><code>timer</code></td><td>unary</td><td>High-resolution monotonic nanosecond clock</td><td><code>(timer 0)</code></td></tr>
```

Replace with three rows:

```html
          <tr><td><code>.time.now</code></td><td>variadic</td><td>Current monotonic time in milliseconds</td><td><code>(.time.now)</code></td></tr>
          <tr><td><code>.time.timer.set</code></td><td>variadic, restricted</td><td>Schedule a callback every <code>ms</code> milliseconds, <code>num</code> times (0 = forever). Returns timer id.</td><td><code>(.time.timer.set 1000 0 (fn [t] (println t)))</code></td></tr>
          <tr><td><code>.time.timer.del</code></td><td>unary, restricted</td><td>Cancel a scheduled timer by id. Returns null.</td><td><code>(.time.timer.del 0)</code></td></tr>
```

- [ ] **Step 2: Commit.**

```bash
git add website/docs/rayfall-functions.html
git commit -m "$(cat <<'EOF'
docs(website): replace timer table row with .time.* trio

EOF
)"
```

---

## Task 12: Verification pass

**Files:** none.

- [ ] **Step 1: Sanitiser-clean build + run.**

Run: `make clean && make test 2>&1 | tail -10`
Expected: `=== <N> of <N+small> passed (... skipped, 0 failed) ===`, no ASan/UBSan errors. The pre-existing flaky `streaming_large_dag` may surface; re-run once if it does.

- [ ] **Step 2: Manual REPL spot-check — `.time.now`.**

Run: `echo '(.time.now)' | ./rayforce`
Expected: a single large i64 (milliseconds since boot or similar — implementation-defined epoch).

- [ ] **Step 3: Manual REPL spot-check — schedule + observe.**

Run (note: REPL must be interactive because timers fire from the poll loop, and `echo | ./rayforce` exits as soon as the input is consumed; we use the journal-mode workaround):

```bash
./rayforce <<'RFL'
(.time.timer.set 100 3 (fn [t] (println t)))
RFL
```

Expected: at minimum, the timer id is printed. If the REPL exits before any fires happen, the spot-check is inconclusive — the integration test (Task 10) is the authoritative verification.

- [ ] **Step 4: Confirm restricted mode rejects `.time.timer.set` under `-U`.**

Run (in a separate terminal first, then check from a client):

```bash
./rayforce -U password -p 7777 &
SERVER=$!
sleep 1
# Sanity: connect and try .time.timer.set (will be blocked by RAY_FN_RESTRICTED)
echo '(.time.timer.set 1000 0 (fn [t] t))' | nc -q 1 localhost 7777 2>&1 | head -5
kill $SERVER
```

Expected: an `access` error from the server. (If `nc` doesn't translate the IPC framing, this step is best-effort — the unit test covers RAY_FN_RESTRICTED at the language level via `g_eval_restricted`.)

---

## Self-review pass

**Spec coverage:**

| Spec section | Task |
|---|---|
| Surface table (.time.now / .set / .del) | Task 9 (registration), Task 10 (tests), Task 11 (docs) |
| Semantics — num=0=forever | Task 4 |
| Semantics — callback arg = current ms | Task 4 |
| Semantics — error printed to stderr | Task 4 |
| Semantics — id monotonic, never reused | Task 2 |
| Heap min by (exp_ms, id) | Task 2 |
| Lazy heap creation in poll | Task 5 (field) + Task 9 (lazy alloc on first .set) |
| Bounded poll wait | Tasks 6 & 7 |
| Fire after I/O events | Tasks 6 & 7 |
| Cleanup in ray_poll_destroy | Tasks 6 & 7 |
| Ref-counting (retain on add / release on remove) | Tasks 2 & 3 & 4 |
| All error rows in spec | Task 9 (synchronous validation) + Task 10 (assertions) |

No spec section uncovered.

**Placeholder scan:** Each step has either the exact code to write, the exact command, or a clearly-bounded conditional ("if reserved names block `.test.*`, switch to `tcounter`"). No "TODO / TBD / similar to Task N" patterns.

**Type consistency:** `ray_timers_t`, `ray_timer_t`, `ray_time_now_ms`, `ray_timers_create/destroy/add/del/next_deadline_ms/fire_expired/pump_for` are consistently named across all tasks. Function signatures match between header (Task 1) and call sites (Tasks 6, 7, 9). `void*` casts at use sites in `epoll.c`, `kqueue.c`, `system.c` are spelled the same way.

Plan is complete.

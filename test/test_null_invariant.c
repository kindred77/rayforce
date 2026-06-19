/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* test/test_null_invariant.c — focused unit tests for the §2.1 null-model
 * invariant validator (ray_check_null_invariant, invariant 16.4).
 *
 *   1. sentinel present + HAS_NULLS unset  -> validator flags it (aborts)
 *   2. sentinel present + HAS_NULLS set     -> passes (no-op)
 *   3. no sentinel                          -> passes
 *
 * The validator aborts on violation (so it surfaces as a usable test
 * failure in the suite).  To test the abort path without killing this
 * process we fork() a child that triggers the violation and assert the
 * child died on SIGABRT.  The pass cases run inline.
 *
 * The validator is DEBUG-only (compiled out in release — ray_check_null_invariant
 * expands to ((void)0)).  The test binary is always built DEBUG, but we guard
 * the body on DEBUG anyway so the suite stays correct under any flavour.
 */

#include "test.h"
#include <rayforce.h>
#include "vec/vec.h"
#include "vec/str.h"             /* ray_str_vec_append */
#include "mem/heap.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

static void nullinv_setup(void)    { ray_heap_init(); }
static void nullinv_teardown(void) { ray_heap_destroy(); }

#ifdef DEBUG

/* Run `fn(arg)` in a forked child; return true iff the child aborted
 * (SIGABRT), i.e. the validator fired.  The child silences stderr so the
 * intentional diagnostic doesn't clutter test output. */
static bool child_aborts(void (*fn)(ray_t*), ray_t* arg) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        /* child: drop the diagnostic, then trip the validator */
        freopen("/dev/null", "w", stderr);
        fn(arg);
        _exit(0);   /* no abort -> distinguishable from SIGABRT */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void check_invariant_thunk(ray_t* v) {
    (void)ray_check_null_invariant(v);
}

/* 1. I64 column with NULL_I64 in the payload but HAS_NULLS NOT set. */
static test_result_t test_nullinv_sentinel_no_attr_flags(void) {
    int64_t data[] = { 1, 2, NULL_I64, 4 };
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 4);
    TEST_ASSERT_NOT_NULL(v);
    /* ray_vec_from_raw must not have set HAS_NULLS for this to be a real
     * violation; clear it defensively so the test asserts the validator,
     * not the constructor. */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_TRUE(child_aborts(check_invariant_thunk, v));
    ray_release(v);
    PASS();
}

/* 2. Same sentinel, but HAS_NULLS set -> invariant holds. */
static test_result_t test_nullinv_sentinel_with_attr_passes(void) {
    int64_t data[] = { 1, 2, NULL_I64, 4 };
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 4);
    TEST_ASSERT_NOT_NULL(v);
    v->attrs |= RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_TRUE(ray_check_null_invariant(v));   /* inline: no abort */
    ray_release(v);
    PASS();
}

/* 3. No sentinel anywhere -> passes regardless of HAS_NULLS. */
static test_result_t test_nullinv_no_sentinel_passes(void) {
    int64_t data[] = { 1, 2, 3, 4 };
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 4);
    TEST_ASSERT_NOT_NULL(v);
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_TRUE(ray_check_null_invariant(v));   /* inline: no abort */
    ray_release(v);
    PASS();
}

/* 4. F64 NaN sentinel without HAS_NULLS -> flagged (float arm, x != x). */
static test_result_t test_nullinv_f64_nan_flags(void) {
    double data[] = { 1.0, 2.0, NULL_F64, 4.0 };
    ray_t* v = ray_vec_from_raw(RAY_F64, data, 4);
    TEST_ASSERT_NOT_NULL(v);
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_TRUE(child_aborts(check_invariant_thunk, v));
    ray_release(v);
    PASS();
}

/* 5. STR empty string without HAS_NULLS is a LEGITIMATE value, not a
 *    reserved sentinel -> validator must NOT flag it. */
static test_result_t test_nullinv_str_empty_not_flagged(void) {
    ray_t* v = ray_vec_new(RAY_STR, 2);
    TEST_ASSERT_NOT_NULL(v);
    v = ray_str_vec_append(v, "x", 1);
    v = ray_str_vec_append(v, "", 0);   /* empty string — ordinary value for STR */
    TEST_ASSERT_NOT_NULL(v);
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_TRUE(ray_check_null_invariant(v));   /* no abort */
    ray_release(v);
    PASS();
}

const test_entry_t null_invariant_entries[] = {
    { "null_invariant/sentinel_no_attr_flags",   test_nullinv_sentinel_no_attr_flags,   nullinv_setup, nullinv_teardown },
    { "null_invariant/sentinel_with_attr_passes", test_nullinv_sentinel_with_attr_passes, nullinv_setup, nullinv_teardown },
    { "null_invariant/no_sentinel_passes",        test_nullinv_no_sentinel_passes,        nullinv_setup, nullinv_teardown },
    { "null_invariant/f64_nan_flags",             test_nullinv_f64_nan_flags,             nullinv_setup, nullinv_teardown },
    { "null_invariant/str_empty_not_flagged",     test_nullinv_str_empty_not_flagged,     nullinv_setup, nullinv_teardown },
    { NULL, NULL, NULL, NULL },
};

#else  /* !DEBUG: validator compiled out — register a single skip marker */

static test_result_t test_nullinv_release_noop(void) {
    PASS();
}

const test_entry_t null_invariant_entries[] = {
    { "null_invariant/release_noop", test_nullinv_release_noop, nullinv_setup, nullinv_teardown },
    { NULL, NULL, NULL, NULL },
};

#endif /* DEBUG */

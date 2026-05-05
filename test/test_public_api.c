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

#include "test.h"
#include <rayforce.h>

static test_result_t test_public_ipc_client_symbols(void) {
    int64_t   (*connect_fn)(const char*, uint16_t, const char*, const char*) = ray_ipc_connect;
    void      (*close_fn)(int64_t) = ray_ipc_close;
    ray_t*    (*send_fn)(int64_t, ray_t*) = ray_ipc_send;
    ray_err_t (*async_fn)(int64_t, ray_t*) = ray_ipc_send_async;
    ray_t*    (*verbose_fn)(int64_t, ray_t*) = ray_ipc_send_verbose;

    TEST_ASSERT_NOT_NULL((void*)connect_fn);
    TEST_ASSERT_NOT_NULL((void*)close_fn);
    TEST_ASSERT_NOT_NULL((void*)send_fn);
    TEST_ASSERT_NOT_NULL((void*)async_fn);
    TEST_ASSERT_NOT_NULL((void*)verbose_fn);
    PASS();
}

const test_entry_t public_api_entries[] = {
    { "public/ipc_client_symbols", test_public_ipc_client_symbols, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

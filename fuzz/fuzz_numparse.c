/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: the numeric-literal scanners.
 *
 *   ray_parse_i64 / _f64 / _u64_hex are pure (ptr, len) -> value routines
 *   with SWAR fast paths; they take no NUL terminator and touch no runtime
 *   state, so this is the cheapest target (millions of execs/sec) and the
 *   right place to shake out off-by-one / over-read bugs in the byte
 *   scanners.  No runtime init needed.
 */

#include <stddef.h>
#include <stdint.h>

#include "core/numparse.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* src = (const char*)data;

    int64_t  i;
    double   f;
    uint64_t h;

    /* Each scanner reads at most `size` bytes and reports how many it
     * consumed; the return value is deliberately ignored — we only care
     * that the scan stays in bounds under the sanitizers. */
    (void)ray_parse_i64(src, size, &i);
    (void)ray_parse_f64(src, size, &f);
    (void)ray_parse_u64_hex(src, size, &h);

    return 0;
}

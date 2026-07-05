/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: the binary deserializer / wire-frame decoder.
 *
 *   ray_de_raw() is the raw buffer decoder; ray_de() wraps it with the
 *   16-byte IPC/journal header validation.  Because the journal wire
 *   format is an IPC frame verbatim, the same corpus that hardens the
 *   live IPC path also hardens journal replay's decode step — this is the
 *   highest-value binary surface for a networked server.
 *
 *   The first input byte selects the entry point so one corpus exercises
 *   both the header-validating and raw paths.
 */

#include "common.h"

#include "store/serde.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();
    if (size == 0) return 0;

    uint8_t        mode = data[0];
    const uint8_t* body = data + 1;
    size_t         blen = size - 1;

    if (mode & 1) {
        /* Header-validating path: wrap the bytes in a U8 vector and let
         * ray_de() check magic/version/length before decoding. */
        ray_t* buf = ray_vec_new(RAY_U8, (int64_t)blen);
        if (buf && !RAY_IS_ERR(buf)) {
            buf->len = (int64_t)blen;
            if (blen) memcpy(ray_data(buf), body, blen);
            ray_t* r = ray_de(buf);
            if (r) { if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r); }
            ray_release(buf);
        } else if (buf && RAY_IS_ERR(buf)) {
            ray_error_free(buf);
        }
    } else {
        /* Raw path: ray_de_raw may write into the buffer, so hand it a
         * private writable copy rather than the engine's const input. */
        uint8_t* copy = (uint8_t*)malloc(blen ? blen : 1);
        if (!copy) return 0;
        if (blen) memcpy(copy, body, blen);
        int64_t consumed = (int64_t)blen;
        ray_t*  r = ray_de_raw(copy, &consumed);
        if (r) { if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r); }
        free(copy);
    }
    return 0;
}

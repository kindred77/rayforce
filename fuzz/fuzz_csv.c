/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: the CSV reader.
 *
 *   ray_read_csv* take a path, so the input bytes are exposed through an
 *   anonymous in-memory file (memfd) via /proc/self/fd — no disk I/O.  The
 *   first byte selects delimiter/header options so one corpus exercises
 *   both the default reader and the option-driven path (type inference,
 *   quoting, embedded newlines, ragged rows).
 */

#include "common.h"

#include "io/csv.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();
    if (size == 0) return 0;

    /* Steer options from the first byte; the rest is the file body. */
    uint8_t        opt   = data[0];
    const uint8_t* body  = data + 1;
    size_t         blen  = size - 1;

    char path[64];
    int fd = ray_fuzz_memfd(body, blen, path, sizeof(path));
    if (fd < 0) return 0;

    ray_t* r;
    if (opt & 1) {
        static const char delims[4] = { ',', '\t', ';', '|' };
        char delimiter = delims[(opt >> 1) & 3];
        bool header    = (opt & 8) != 0;
        r = ray_read_csv_opts(path, delimiter, header, NULL, 0);
    } else {
        r = ray_read_csv(path);
    }
    if (r) { if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r); }

    close(fd);
    return 0;
}

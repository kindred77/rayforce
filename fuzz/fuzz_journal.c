/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: journal validation and replay.
 *
 *   A journal is a sequence of IPC frames verbatim, so this shares the
 *   decoder surface with fuzz_de but adds the framing walk and the replay
 *   path (which decodes AND evaluates each frame — sandboxed under
 *   -DRAY_FUZZING like fuzz_eval).  Input is exposed as an in-memory file
 *   via /proc/self/fd.  Validate (parse only) runs first, then replay.
 */

#include "common.h"

#include "store/journal.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();

    char path[64];
    int fd = ray_fuzz_memfd(data, size, path, sizeof(path));
    if (fd < 0) return 0;

    /* Validate: walk the framing without evaluating.  Each call fopen()s
     * the path, getting its own file offset at 0, so no rewind is needed. */
    int64_t chunks = 0, valid_bytes = 0;
    ray_err_t verr = ray_journal_validate(path, &chunks, &valid_bytes);
    (void)verr;

    /* Replay: decode + evaluate each frame (sandboxed under RAY_FUZZING). */
    int64_t rchunks = 0, eval_errors = 0;
    ray_jreplay_status_t status = 0;
    ray_err_t rerr = ray_journal_replay(path, &rchunks, &eval_errors, &status);
    (void)rerr;

    close(fd);
    return 0;
}

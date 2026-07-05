/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: parse + evaluate a Rayfall program.
 *
 *   This drives the whole language pipeline (parse -> compile -> optimize
 *   -> execute) on untrusted input — the deepest surface, and the one that
 *   turns up logic bugs the parse-only target cannot.  It runs in-process
 *   with a shared runtime, so the escape hatches that would compromise the
 *   fuzzer (shell, exit, network listeners/dials, file writes) are compiled
 *   out under -DRAY_FUZZING (see the guards in syscmd.c / system.c /
 *   builtins.c / journal.c).
 *
 *   Two limitations are accepted by design:
 *     - Shared global state (e.g. `set`) leaks across iterations, so a rare
 *       crash may not reproduce standalone; libFuzzer restarts workers on
 *       crash and most cases still reproduce.
 *     - Loop / recursion constructs can legitimately run long; a `timeout-*`
 *       artifact is only a finding when the input has no such construct.
 */

#include "common.h"

extern ray_t* ray_eval_str(const char* source);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();

    char* src = ray_fuzz_cstr(data, size);
    if (!src) return 0;

    ray_t* r = ray_eval_str(src);
    free(src);

    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else               ray_release(r);
    }
    return 0;
}

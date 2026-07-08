/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: the Rayfall source parser.
 *
 *   Feeds engine-mutated bytes straight into ray_parse().  The parser is
 *   the widest untrusted-input surface after the wire protocol: every REPL
 *   line, every -e argument, and every replayed source string flows
 *   through it.  We only exercise parse (no eval) here — eval has its own
 *   sandboxed target — so this runs fast and isolates lexer/parser bugs.
 */

#include "common.h"

/* parse.h declares ray_parse; include via the public path used elsewhere. */
extern ray_t* ray_parse(const char* source);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();

    char* src = ray_fuzz_cstr(data, size);
    if (!src) return 0; /* allocation failure is not a parser finding */

    ray_t* r = ray_parse(src);
    free(src);

    /* ray_parse returns either an AST value or a RAY_ERROR sentinel.
     * ray_release is a documented no-op on error objects, so reclaim
     * errors explicitly to keep the fuzzer's own footprint flat. */
    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else               ray_release(r);
    }
    return 0;
}

/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fatal-signal crash handler.
 *
 *   Installs an async-signal-safe handler for the fault signals
 *   (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT) that writes a symbolized
 *   backtrace to stderr before re-raising the signal with the default
 *   disposition — so a cloud deployment gets a diagnosable trace instead
 *   of a silent exit, while still producing a core dump and the correct
 *   terminating-signal exit status for the orchestrator.
 *
 *   This is separate from src/app/term.c, which handles the graceful
 *   shutdown signals (SIGINT/SIGTERM/SIGQUIT) and restores terminal state.
 *   The two signal sets do not overlap.
 */

#ifndef RAY_CRASH_H
#define RAY_CRASH_H

/* Install the fatal-signal handler.  Idempotent; safe to call once from
 * main() at startup.  Registers an alternate signal stack so a
 * stack-overflow SIGSEGV can still be reported, and warms up the unwinder
 * so the handler itself never has to allocate. */
void ray_crash_install(void);

#endif /* RAY_CRASH_H */

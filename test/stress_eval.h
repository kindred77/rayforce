#ifndef RAY_TEST_STRESS_EVAL_H
#define RAY_TEST_STRESS_EVAL_H
#include "stress_store.h"

/* Runtime lifecycle (single runtime per process; pair these in suite
 * setup/teardown; the restart op recreates in place). */
bool stress_eval_runtime_up(void);
void stress_eval_runtime_down(void);

/* Evaluate generated source; on error: log source + error via the engine's
 * failure dump, return false.  Logs every source line to the op log first
 * (truncated to the log line width) so failures are REPL-replayable. */
bool stress_eval_exec(stress_ctx_t* c, const char* src);

/* Build a Rayfall table literal from rows into buf:
 * (table [ticker price qty] (list ['a 'b] [1.5 2.5] [10 20]))
 * Null rows use the verified null literal forms.  Returns false if buf is
 * too small. */
bool stress_eval_table_src(const stress_rows_t* rows, int64_t from, int64_t n,
                           char* buf, size_t bufsz);

/* Eval-mode op executors — mirror the engine's vocabulary, same shadow
 * mutations, real side through generated Rayfall. */
bool stress_eval_seed_initial(stress_ctx_t* c, int64_t live_rows, int nparts,
                              int64_t rows_per_part);
bool stress_eval_op_insert(stress_ctx_t* c, int64_t n, stress_sym_pattern_t pat);
bool stress_eval_op_upsert(stress_ctx_t* c, stress_sym_pattern_t pat);
bool stress_eval_op_trim(stress_ctx_t* c, int part_idx, bool tail, int64_t n);
bool stress_eval_op_part_append(stress_ctx_t* c, int part_idx, int64_t n,
                                stress_sym_pattern_t pat);
bool stress_eval_op_part_new(stress_ctx_t* c, int64_t n, stress_sym_pattern_t pat);
bool stress_eval_op_restart(stress_ctx_t* c);  /* runtime destroy + create */

/* Query oracle (Task 3) */
bool stress_eval_verify_queries(stress_ctx_t* c);

#endif

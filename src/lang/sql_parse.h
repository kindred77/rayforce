/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *   [license header abbreviated]
 */

#ifndef RAY_SQL_PARSE_H
#define RAY_SQL_PARSE_H

#include <rayforce.h>
#include <stdbool.h>

/* Check if input looks like a SQL statement (starts with SELECT, INSERT,
 * UPDATE, DELETE, CREATE, DROP, WITH, EXPLAIN, DESCRIBE).
 * Returns true if the first non-whitespace token is a SQL keyword. */
bool ray_is_sql(const char* input);

/* Parse a SQL statement and return the equivalent Lisp-style AST (ray_t*).
 * Returns an error ray_t on parse failure.
 * If nfo is non-NULL, source-location spans are recorded. */
ray_t* ray_sql_parse(const char* sql, ray_t* nfo);

/* Convenience: ray_sql_parse + ray_eval in one call. */
ray_t* ray_sql_eval(const char* sql);

#endif /* RAY_SQL_PARSE_H */
/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

// ==================== TABLE CREATION TESTS ====================
test_result_t test_table_creation() {
    // Basic table creation
    TEST_ASSERT_EQ(
        "(table [a b c] (list [1 2 3] [4 5 6] [7 8 9]))",
        "(table [a b c] (list [1 2 3] [4 5 6] [7 8 9]))");

    // Single column table
    TEST_ASSERT_EQ(
        "(table [x] (list [10 20 30]))",
        "(table [x] (list [10 20 30]))");

    // Single row table
    TEST_ASSERT_EQ(
        "(table [a b] (list [1] [2]))",
        "(table [a b] (list [1] [2]))");

    // Table with mixed types
    TEST_ASSERT_EQ(
        "(table [id name price] (list [1 2 3] [alice bob charlie] [10.5 20.0 30.5]))",
        "(table [id name price] (list [1 2 3] [alice bob charlie] [10.5 20.0 30.5]))");

    // Table with string column
    TEST_ASSERT_EQ(
        "(table [id label] (list [1 2] (list \"hello\" \"world\")))",
        "(table [id label] (list [1 2] (list \"hello\" \"world\")))");

    // Table with boolean column
    TEST_ASSERT_EQ(
        "(table [id flag] (list [1 2 3] [true false true]))",
        "(table [id flag] (list [1 2 3] [true false true]))");

    // Table with date column
    TEST_ASSERT_EQ(
        "(table [dt val] (list [2024.01.01 2024.01.02] [10 20]))",
        "(table [dt val] (list [2024.01.01 2024.01.02] [10 20]))");

    // Table with time column
    TEST_ASSERT_EQ(
        "(table [tm val] (list [10:00:00.000 11:00:00.000] [100 200]))",
        "(table [tm val] (list [10:00:00.000 11:00:00.000] [100 200]))");

    // Table with timestamp column
    TEST_ASSERT_EQ(
        "(table [ts val] (list [2024.01.01D00:00:00.0 2024.01.02D00:00:00.0] [1 2]))",
        "(table [ts val] (list [2024.01.01D00:00:00.000000000 2024.01.02D00:00:00.000000000] [1 2]))");

    // Table with f64 column
    TEST_ASSERT_EQ(
        "(table [x y] (list [1.0 2.0 3.0] [4.0 5.0 6.0]))",
        "(table [x y] (list [1.0 2.0 3.0] [4.0 5.0 6.0]))");

    // Table with i32 column
    TEST_ASSERT_EQ(
        "(table [a b] (list [1i 2i 3i] [4i 5i 6i]))",
        "(table [a b] (list [1i 2i 3i] [4i 5i 6i]))");

    // Table with i16 column
    TEST_ASSERT_EQ(
        "(table [a b] (list [1h 2h 3h] [4h 5h 6h]))",
        "(table [a b] (list [1h 2h 3h] [4h 5h 6h]))");

    PASS();
}

// ==================== TABLE METADATA TESTS ====================
test_result_t test_table_metadata() {
    // Count rows
    TEST_ASSERT_EQ(
        "(count (table [a b] (list [1 2 3] [4 5 6])))",
        "3");

    // Count single row
    TEST_ASSERT_EQ(
        "(count (table [a] (list [42])))",
        "1");

    // Column names via key
    TEST_ASSERT_EQ(
        "(key (table [a b c] (list [1 2] [3 4] [5 6])))",
        "[a b c]");

    // Column values via value
    TEST_ASSERT_EQ(
        "(value (table [a b] (list [1 2] [3 4])))",
        "(list [1 2] [3 4])");

    // Type of table
    TEST_ASSERT_EQ(
        "(type (table [a] (list [1 2])))",
        "'TABLE");

    // Column access by name
    TEST_ASSERT_EQ(
        "(set t (table [x y z] (list [10 20 30] [40 50 60] [70 80 90])))"
        "(at t 'x)",
        "[10 20 30]");

    TEST_ASSERT_EQ("(at t 'y)", "[40 50 60]");
    TEST_ASSERT_EQ("(at t 'z)", "[70 80 90]");

    // Row access by index
    TEST_ASSERT_EQ(
        "(at (table [a b] (list [1 2 3] [4 5 6])) 0)",
        "{a:1 b:4}");

    TEST_ASSERT_EQ(
        "(at (table [a b] (list [1 2 3] [4 5 6])) 2)",
        "{a:3 b:6}");

    // First and last row
    TEST_ASSERT_EQ(
        "(first (table [a b] (list [1 2 3] [4 5 6])))",
        "{a:1 b:4}");

    TEST_ASSERT_EQ(
        "(last (table [a b] (list [1 2 3] [4 5 6])))",
        "{a:3 b:6}");

    // Count columns
    TEST_ASSERT_EQ(
        "(count (key (table [a b c d] (list [1] [2] [3] [4]))))",
        "4");

    PASS();
}

// ==================== SELECT BASIC TESTS ====================
test_result_t test_select_basic() {
    // Setup
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1 2 3] [10 20 30] [100 200 300])))"
        "null",
        "null");

    // Select all (from only)
    TEST_ASSERT_EQ(
        "(select {from: t})",
        "(table [a b c] (list [1 2 3] [10 20 30] [100 200 300]))");

    // Select specific columns via named output
    TEST_ASSERT_EQ(
        "(select {a: a b: b from: t})",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    // Select single column
    TEST_ASSERT_EQ(
        "(select {a: a from: t})",
        "(table [a] (list [1 2 3]))");

    // Select with computed column
    TEST_ASSERT_EQ(
        "(select {total: (+ b c) from: t})",
        "(table [total] (list [110 220 330]))");

    // Select with renamed column
    TEST_ASSERT_EQ(
        "(select {x: a y: b from: t})",
        "(table [x y] (list [1 2 3] [10 20 30]))");

    // Select with expression over columns
    TEST_ASSERT_EQ(
        "(select {product: (* a b) from: t})",
        "(table [product] (list [10 40 90]))");

    // Select with constant
    TEST_ASSERT_EQ(
        "(select {a: a flag: 1 from: t})",
        "(table [a flag] (list [1 2 3] [1 1 1]))");

    // Nested expression: (a*b) + c
    TEST_ASSERT_EQ(
        "(select {r: (+ (* a b) c) from: t})",
        "(table [r] (list [110 240 390]))");

    // Subtraction expression
    TEST_ASSERT_EQ(
        "(select {d: (- c b) from: t})",
        "(table [d] (list [90 180 270]))");

    // Multiple computed columns
    TEST_ASSERT_EQ(
        "(select {s: (+ a b) p: (* a c) from: t})",
        "(table [s p] (list [11 22 33] [100 400 900]))");

    // Mix of raw and computed columns
    TEST_ASSERT_EQ(
        "(select {a: a total: (+ b c) from: t})",
        "(table [a total] (list [1 2 3] [110 220 330]))");

    PASS();
}

// ==================== SELECT WHERE TESTS ====================
test_result_t test_select_where() {
    // Setup
    TEST_ASSERT_EQ(
        "(set t (table [id name val] (list [1 2 3 4 5] [alice bob charlie dave eve] [10 20 30 40 50])))"
        "null",
        "null");

    // Simple equality filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (== id 3)})",
        "(table [id name val] (list [3] [charlie] [30]))");

    // Greater-than filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (> val 30)})",
        "(table [id name val] (list [4 5] [dave eve] [40 50]))");

    // Less-than filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (< val 20)})",
        "(table [id name val] (list [1] [alice] [10]))");

    // Greater-or-equal filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (>= val 30)})",
        "(table [id name val] (list [3 4 5] [charlie dave eve] [30 40 50]))");

    // Less-or-equal filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (<= val 20)})",
        "(table [id name val] (list [1 2] [alice bob] [10 20]))");

    // Not-equal filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (!= id 3)})",
        "(table [id name val] (list [1 2 4 5] [alice bob dave eve] [10 20 40 50]))");

    // AND filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (and (> val 10) (< val 40))})",
        "(table [id name val] (list [2 3] [bob charlie] [20 30]))");

    // OR filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (or (== id 1) (== id 5))})",
        "(table [id name val] (list [1 5] [alice eve] [10 50]))");

    // Filter returns no rows
    TEST_ASSERT_EQ(
        "(select {from: t where: (> val 100)})",
        "(table [id name val] (list [] (as 'SYMBOL []) []))");

    // Filter returns all rows
    TEST_ASSERT_EQ(
        "(count (select {from: t where: (> val 0)}))",
        "5");

    // NOT filter
    TEST_ASSERT_EQ(
        "(select {from: t where: (not (== id 3))})",
        "(table [id name val] (list [1 2 4 5] [alice bob dave eve] [10 20 40 50]))");

    // Select with both column projection and where
    TEST_ASSERT_EQ(
        "(select {name: name val: val from: t where: (>= val 30)})",
        "(table [name val] (list [charlie dave eve] [30 40 50]))");

    // Nested conditions
    TEST_ASSERT_EQ(
        "(select {from: t where: (and (or (== id 1) (== id 3)) (> val 5))})",
        "(table [id name val] (list [1 3] [alice charlie] [10 30]))");

    // IN filter on symbol column
    TEST_ASSERT_EQ(
        "(select {from: t where: (in name [alice eve])})",
        "(table [id name val] (list [1 5] [alice eve] [10 50]))");

    // Where on single row match
    TEST_ASSERT_EQ(
        "(count (select {from: t where: (== val 50)}))",
        "1");

    // Double NOT
    TEST_ASSERT_EQ(
        "(count (select {from: t where: (not (not (> val 30)))}))",
        "2");

    PASS();
}

// ==================== SELECT WHERE ON DIFFERENT TYPES ====================
test_result_t test_select_where_types() {
    // Where on f64 column
    TEST_ASSERT_EQ(
        "(set t (table [id price] (list [1 2 3] [10.5 20.0 30.5])))"
        "(select {from: t where: (> price 15.0)})",
        "(table [id price] (list [2 3] [20.0 30.5]))");

    // Where on date column
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.02 2024.01.03] [10 20 30])))"
        "(select {from: t where: (> dt 2024.01.01)})",
        "(table [dt val] (list [2024.01.02 2024.01.03] [20 30]))");

    // Where on i32 column
    TEST_ASSERT_EQ(
        "(set t (table [id x] (list [1 2 3] [10i 20i 30i])))"
        "(select {from: t where: (== x 20i)})",
        "(table [id x] (list [2] [20i]))");

    // Where on boolean column
    TEST_ASSERT_EQ(
        "(set t (table [flag val] (list [true false true] [10 20 30])))"
        "(select {from: t where: flag})",
        "(table [flag val] (list [true true] [10 30]))");

    // Where on time column
    TEST_ASSERT_EQ(
        "(set t (table [tm val] (list [09:00:00.000 10:00:00.000 11:00:00.000] [1 2 3])))"
        "(select {from: t where: (> tm 09:30:00.000)})",
        "(table [tm val] (list [10:00:00.000 11:00:00.000] [2 3]))");

    // Like filter on string column
    TEST_ASSERT_EQ(
        "(set t (table [name val] (list (list \"apple\" \"banana\" \"avocado\") [1 2 3])))"
        "(select {from: t where: (like name \"a*\")})",
        "(table [name val] (list (list \"apple\" \"avocado\") [1 3]))");

    PASS();
}

// ==================== SELECT AGGREGATION TESTS ====================
test_result_t test_select_aggregation() {
    // Setup
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b b] [10 20 30 40 50])))"
        "null",
        "null");

    // Sum by group
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: grp})",
        "(table [grp s] (list [a b] [30 120]))");

    // Count by group
    TEST_ASSERT_EQ(
        "(select {n: (count val) from: t by: grp})",
        "(table [grp n] (list [a b] [2 3]))");

    // Avg by group
    TEST_ASSERT_EQ(
        "(select {m: (avg val) from: t by: grp})",
        "(table [grp m] (list [a b] [15.0 40.0]))");

    // Min by group
    TEST_ASSERT_EQ(
        "(select {lo: (min val) from: t by: grp})",
        "(table [grp lo] (list [a b] [10 30]))");

    // Max by group
    TEST_ASSERT_EQ(
        "(select {hi: (max val) from: t by: grp})",
        "(table [grp hi] (list [a b] [20 50]))");

    // First by group
    TEST_ASSERT_EQ(
        "(select {f: (first val) from: t by: grp})",
        "(table [grp f] (list [a b] [10 30]))");

    // Last by group
    TEST_ASSERT_EQ(
        "(select {l: (last val) from: t by: grp})",
        "(table [grp l] (list [a b] [20 50]))");

    // Multiple aggregations
    TEST_ASSERT_EQ(
        "(select {s: (sum val) n: (count val) from: t by: grp})",
        "(table [grp s n] (list [a b] [30 120] [2 3]))");

    // Aggregation with where
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: grp where: (> val 15)})",
        "(table [grp s] (list [a b] [20 120]))");

    // Groupby dev — all same values in group → dev 0
    TEST_ASSERT_EQ(
        "(set t2 (table [g v] (list [a a a b b b] [1 1 1 2 2 2])))"
        "(select {d: (dev v) from: t2 by: g})",
        "(table [g d] (list [a b] [0f 0f]))");

    // Groupby med
    TEST_ASSERT_EQ(
        "(set t2 (table [g v] (list [a a a b b b] [10 30 20 5 15 10])))"
        "(select {m: (med v) from: t2 by: g})",
        "(table [g m] (list [a b] [20f 10f]))");

    // Expression-over-aggregate: max - min
    TEST_ASSERT_EQ(
        "(select {r: (- (max val) (min val)) from: t by: grp})",
        "(table [grp r] (list [a b] [10 20]))");

    // Aggregate of computed expression: sum(a * b)
    TEST_ASSERT_EQ(
        "(set t3 (table [a b] (list [1 2 3] [10 20 30])))"
        "(select {total: (sum (* a b)) from: t3})",
        "(table [total] (list [140]))");

    // Many aggregations in one select
    TEST_ASSERT_EQ(
        "(select {s: (sum val) n: (count val) lo: (min val) hi: (max val) m: (avg val) from: t by: grp})",
        "(table [grp s n lo hi m] (list [a b] [30 120] [2 3] [10 30] [20 50] [15.0 40.0]))");

    PASS();
}

// ==================== SELECT GROUPBY SINGLE KEY ====================
test_result_t test_select_groupby_single() {
    // Groupby symbol key
    TEST_ASSERT_EQ(
        "(set t (table [sym val] (list [x x y y] [1 2 3 4])))"
        "(select {s: (sum val) from: t by: sym})",
        "(table [sym s] (list [x y] [3 7]))");

    // Groupby i64 key
    TEST_ASSERT_EQ(
        "(set t (table [k val] (list [1 1 2 2] [10 20 30 40])))"
        "(select {s: (sum val) from: t by: k})",
        "(table [k s] (list [1 2] [30 70]))");

    // Groupby boolean key
    TEST_ASSERT_EQ(
        "(set t (table [flag val] (list [true false true false true] [10 20 30 40 50])))"
        "(select {s: (sum val) from: t by: flag})",
        "(table [flag s] (list [true false] [90 60]))");

    // Groupby date key
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.01 2024.01.02 2024.01.02] [1 2 3 4])))"
        "(select {s: (sum val) from: t by: dt})",
        "(table [dt s] (list [2024.01.01 2024.01.02] [3 7]))");

    // Single group (all same key)
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [x x x] [1 2 3])))"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [x] [6]))");

    // Many unique keys (each row is its own group)
    TEST_ASSERT_EQ(
        "(set t (table [k v] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(select {s: (sum v) from: t by: k})",
        "(table [k s] (list [1 2 3 4 5] [10 20 30 40 50]))");

    PASS();
}

// ==================== SELECT GROUPBY MULTI KEY ====================
test_result_t test_select_groupby_multi() {
    // Setup with 2 grouping keys
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [a a b b a] [x y x y x] [1 2 3 4 5])))"
        "null",
        "null");

    // Group by two keys
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: {k1: k1 k2: k2}})",
        "(table [k1 k2 s] (list [a a b b] [x y x y] [6 2 3 4]))");

    // Count by two keys
    TEST_ASSERT_EQ(
        "(select {n: (count val) from: t by: {k1: k1 k2: k2}})",
        "(table [k1 k2 n] (list [a a b b] [x y x y] [2 1 1 1]))");

    // 3-key groupby
    TEST_ASSERT_EQ(
        "(set t2 (table [a b c val] (list [x x x y] [1 1 2 1] [p p p q] [10 20 30 40])))"
        "(select {s: (sum val) from: t2 by: {a: a b: b c: c}})",
        "(table [a b c s] (list [x x y] [1 2 1] [p p q] [30 30 40]))");

    // Multi-key with aggregation + where (group order depends on filtered row order)
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: {k1: k1 k2: k2} where: (> val 2)})",
        "(table [k1 k2 s] (list [b b a] [x y x] [3 4 5]))");

    // Mixed key types: symbol + i64
    TEST_ASSERT_EQ(
        "(set t3 (table [sym id val] (list [a a b b] [1 2 1 2] [10 20 30 40])))"
        "(select {s: (sum val) from: t3 by: {sym: sym id: id}})",
        "(table [sym id s] (list [a a b b] [1 2 1 2] [10 20 30 40]))");

    PASS();
}

// ==================== SELECT GROUPBY WITH XBAR ====================
test_result_t test_select_groupby_xbar() {
    // xbar on time
    TEST_ASSERT_EQ(
        "(set t (table [tm val] (list [09:00:01.000 09:00:05.000 09:00:11.000 09:00:15.000] [10 20 30 40])))"
        "(select {s: (sum val) from: t by: (xbar tm 10000)})",
        "(table [tm s] (list [09:00:01.000 09:00:11.000] [30 70]))");

    // xbar on integers
    TEST_ASSERT_EQ(
        "(set t (table [x val] (list [1 2 3 4 5 6 7 8 9 10] (til 10))))"
        "(select {s: (sum val) from: t by: (xbar x 3)})",
        "(table [x s] (list [1 3 6 9] [1 9 18 17]))");

    PASS();
}

// ==================== SELECT WHERE + BY COMBINED ====================
test_result_t test_select_where_by() {
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b b] [10 20 30 40 50])))"
        "null",
        "null");

    // Filter before aggregate
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: grp where: (> val 15)})",
        "(table [grp s] (list [a b] [20 120]))");

    // Filter removes entire group
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: grp where: (> val 25)})",
        "(table [grp s] (list [b] [120]))");

    // Filter + by + multiple aggregations
    TEST_ASSERT_EQ(
        "(select {s: (sum val) n: (count val) from: t by: grp where: (> val 10)})",
        "(table [grp s n] (list [a b] [20 120] [1 3]))");

    // Where with != and by
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t by: grp where: (!= val 30)})",
        "(table [grp s] (list [a b] [30 90]))");

    PASS();
}

// ==================== SELECT COMPUTED COLUMNS ====================
test_result_t test_select_computed_columns() {
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "null",
        "null");

    // Arithmetic: addition
    TEST_ASSERT_EQ(
        "(select {r: (+ a b) from: t})",
        "(table [r] (list [11 22 33]))");

    // Arithmetic: multiplication
    TEST_ASSERT_EQ(
        "(select {r: (* a b) from: t})",
        "(table [r] (list [10 40 90]))");

    // Nested function: (a*b) - a
    TEST_ASSERT_EQ(
        "(select {r: (- (* a b) a) from: t})",
        "(table [r] (list [9 38 87]))");

    // Comparison producing bool column
    TEST_ASSERT_EQ(
        "(select {big: (> b 15) from: t})",
        "(table [big] (list [false true true]))");

    // Two comparisons
    TEST_ASSERT_EQ(
        "(select {g10: (> a 1) g20: (> b 20) from: t})",
        "(table [g10 g20] (list [false true true] [false false true]))");

    PASS();
}

// ==================== SELECT EMPTY TABLES ====================
test_result_t test_select_empty_tables() {
    // Select from empty table
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [] [])))"
        "(count (select {from: t}))",
        "0");

    // Where filtering to empty
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(count (select {from: t where: (> a 100)}))",
        "0");

    // Single row table
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(select {s: (sum b) from: t})",
        "(table [s] (list [10]))");

    // Aggregation on single group
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [x x x] [1 2 3])))"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [x] [6]))");

    PASS();
}

// ==================== SELECT LARGE TABLES ====================
test_result_t test_select_large_tables() {
    // Large-ish generated table
    TEST_ASSERT_EQ(
        "(set t (table [idx val] (list (til 100) (* (til 100) 2))))"
        "(count t)",
        "100");

    // Select on generated table
    TEST_ASSERT_EQ(
        "(select {s: (sum val) from: t})",
        "(table [s] (list [9900]))");

    // Filter on generated table
    TEST_ASSERT_EQ(
        "(count (select {from: t where: (> val 100)}))",
        "49");

    // 50K+ table for parallel paths
    TEST_ASSERT_EQ(
        "(set big (table [a b] (list (til 50001) (* (til 50001) 3))))"
        "(count big)",
        "50001");

    // Parallel filter
    TEST_ASSERT_EQ(
        "(count (select {from: big where: (> a 25000)}))",
        "25000");

    // Parallel aggregation
    TEST_ASSERT_EQ(
        "(select {s: (sum b) from: big})",
        "(table [s] (list [3750075000]))");

    // Groupby on large data (2 groups)
    TEST_ASSERT_EQ(
        "(set big2 (table [g v] (list (take [a b] 1000) (til 1000))))"
        "(select {s: (sum v) from: big2 by: g})",
        "(table [g s] (list [a b] [249500 250000]))");

    PASS();
}

// ==================== SELECT STRING GROUPBY ====================
test_result_t test_select_string_groupby() {
    // Group by string list column
    TEST_ASSERT_EQ(
        "(set t (table [Name Value] (list (list \"alice\" \"bob\" \"alice\" \"bob\") [10 20 30 40])))"
        "(select {s: (sum Value) from: t by: Name})",
        "(table [Name s] (list (list \"alice\" \"bob\") [40 60]))");

    // Count by string column
    TEST_ASSERT_EQ(
        "(set t (table [city pop] (list (list \"NYC\" \"LA\" \"NYC\" \"LA\" \"NYC\") [100 200 150 250 120])))"
        "(select {total: (sum pop) n: (count pop) from: t by: city})",
        "(table [city total n] (list (list \"NYC\" \"LA\") [370 450] [3 2]))");

    // Update with group by string
    TEST_ASSERT_EQ(
        "(set t (table [tp val] (list (list \"A\" \"B\" \"A\" \"B\") [10 20 30 40])))"
        "(update {from: 't tsum: (sum val) by: tp})"
        "t",
        "(table [tp val tsum] (list (list \"A\" \"B\" \"A\" \"B\") [10 20 30 40] [40 60 40 60]))");

    PASS();
}

// ==================== SELECT PARALLEL FILTER ====================
test_result_t test_select_parallel_filter() {
    // Regression: and with select on large data (parallel processing)
    TEST_ASSERT_EQ(
        "(set t (table ['a 'b 'c] (list (take [false true] 25001) (take [true false] 25001) (take 1 25001))))"
        "(count (select {c: c from: t where: (and a b)}))",
        "0");

    // OR on large data
    TEST_ASSERT_EQ(
        "(set t (table ['flag1 'flag2 'val] (list (take [true false] 1000) (take [false true] 1000) (til 1000))))"
        "(count (select {from: t where: (or flag1 flag2)}))",
        "1000");

    // AND on boolean columns
    TEST_ASSERT_EQ(
        "(set t (table [a b val] (list [true true false false] [true false true false] [1 2 3 4])))"
        "(select {s: (sum val) from: t where: (and a b)})",
        "(table [s] (list [1]))");

    PASS();
}

// ==================== UPDATE BASIC ====================
test_result_t test_update_basic() {
    // Add new column
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(update {from: 't c: 99})"
        "t",
        "(table [a b c] (list [1 2 3] [10 20 30] [99 99 99]))");

    // Update existing column with constant
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(update {from: 't b: 0})"
        "t",
        "(table [a b] (list [1 2 3] [0 0 0]))");

    // Update with expression
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(update {from: 't c: (* a b)})"
        "t",
        "(table [a b c] (list [1 2 3] [10 20 30] [10 40 90]))");

    // Immediate update (returns new table, original unchanged)
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2] [10 20])))"
        "(update {from: t c: 5})",
        "(table [a b c] (list [1 2] [10 20] [5 5]))");

    // Verify original unchanged after immediate
    TEST_ASSERT_EQ("t", "(table [a b] (list [1 2] [10 20]))");

    // Multiple columns at once
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(update {from: 't c: (+ a b) d: (* a b)})"
        "t",
        "(table [a b c d] (list [1 2 3] [10 20 30] [11 22 33] [10 40 90]))");

    PASS();
}

// ==================== UPDATE WHERE ====================
test_result_t test_update_where() {
    // Update matching rows
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(update {from: 't val: 99 where: (== id 2)})"
        "t",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    // Verify non-matching unchanged
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(update {from: 't val: 0 where: (> id 3)})"
        "(at t 'val)",
        "[10 20 30 0 0]");

    // Update with complex conditions
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(update {from: 't val: 99 where: (and (>= id 2) (<= id 4))})"
        "(at t 'val)",
        "[10 99 99 99 50]");

    // Update with != condition
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(update {from: 't val: 0 where: (!= id 2)})"
        "(at t 'val)",
        "[0 20 0]");

    // Where matching no rows (no-op)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(update {from: 't val: 99 where: (> id 100)})"
        "t",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    PASS();
}

// ==================== UPDATE BY ====================
test_result_t test_update_by() {
    // Grouped update: sum per group broadcast
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(update {from: 't total: (sum val) by: grp})"
        "t",
        "(table [grp val total] (list [a a b b] [10 20 30 40] [30 30 70 70]))");

    // Count per group
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b b] [1 2 3 4 5])))"
        "(update {from: 't n: (count val) by: grp})"
        "t",
        "(table [grp val n] (list [a a b b b] [1 2 3 4 5] [2 2 3 3 3]))");

    // Avg per group
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(update {from: 't m: (avg val) by: grp})"
        "t",
        "(table [grp val m] (list [a a b b] [10 20 30 40] [15.0 15.0 35.0 35.0]))");

    // Multiple grouped columns
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(update {from: 't s: (sum val) lo: (min val) by: grp})"
        "t",
        "(table [grp val s lo] (list [a a b b] [10 20 30 40] [30 30 70 70] [10 10 30 30]))");

    PASS();
}

// ==================== UPDATE IN-PLACE SEMANTICS ====================
test_result_t test_update_inplace() {
    // Verify 't modifies original
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(update {from: 't b: 0})"
        "t",
        "(table [a b] (list [1 2 3] [0 0 0]))");

    // Verify unquoted t returns copy without modifying original
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(set t2 (update {from: t b: 0}))"
        "t",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    TEST_ASSERT_EQ(
        "t2",
        "(table [a b] (list [1 2 3] [0 0 0]))");

    // Chain of in-place updates
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(update {from: 't c: (* a b)})"
        "(update {from: 't d: (+ a b)})"
        "t",
        "(table [a b c d] (list [1 2 3] [10 20 30] [10 40 90] [11 22 33]))");

    // Update with float multiplication (keep type consistent via copy)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [100 200 300])))"
        "(set t (update {val: (* val 1.5) from: t where: (> val 100)}))"
        "t",
        "(table [id val] (list [1 2 3] [100 300 450]))");

    PASS();
}

// ==================== UPDATE TYPES ====================
test_result_t test_update_types() {
    // Update f64 column
    TEST_ASSERT_EQ(
        "(set t (table [id price] (list [1 2 3] [10.0 20.0 30.0])))"
        "(update {from: 't price: 99.9 where: (== id 2)})"
        "(at t 'price)",
        "[10.0 99.9 30.0]");

    // Update date column
    TEST_ASSERT_EQ(
        "(set t (table [id dt] (list [1 2] [2024.01.01 2024.01.02])))"
        "(update {from: 't dt: 2024.06.15 where: (== id 1)})"
        "(at t 'dt)",
        "[2024.06.15 2024.01.02]");

    // Update i32 column with i32 constant
    TEST_ASSERT_EQ(
        "(set t (table [id x] (list [1 2 3] [10i 20i 30i])))"
        "(update {from: 't x: 0i where: (> x 15i)})"
        "(at t 'x)",
        "[10i 0i 0i]");

    // Update symbol column with where (use "fruit" not "sym" — "sym" is a keyword)
    TEST_ASSERT_EQ(
        "(set t (table [id fruit] (list [1 2 3] [apple banana cherry])))"
        "(update {from: 't fruit: 'kiwi where: (== id 2)})"
        "(at t 'fruit)",
        "[apple kiwi cherry]");

    PASS();
}

// ==================== INSERT BASIC ====================
test_result_t test_insert_basic() {
    // Insert single row
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2] [10 20])))"
        "(insert t (list 3 30))",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    // Verify original unchanged (return-copy semantics)
    TEST_ASSERT_EQ("t", "(table [a b] (list [1 2] [10 20]))");

    // Insert multiple rows
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert t (list [2 3] [20 30]))",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    // Insert via dict
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2] [10 20])))"
        "(insert t (dict [a b] (list 3 30)))",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    // Insert via table
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert t (table [a b] (list [2 3] [20 30])))",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    PASS();
}

// ==================== INSERT IN-PLACE ====================
test_result_t test_insert_inplace() {
    // Insert in-place
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert 't (list 2 20))"
        "t",
        "(table [a b] (list [1 2] [10 20]))");

    // Sequential in-place inserts
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert 't (list 2 20))"
        "(insert 't (list 3 30))"
        "(insert 't (list 4 40))"
        "t",
        "(table [a b] (list [1 2 3 4] [10 20 30 40]))");

    // Verify growing table
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert 't (list 2 20))"
        "(count t)",
        "2");

    // In-place insert via table
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert 't (table [a b] (list [2 3] [20 30])))"
        "t",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    PASS();
}

// ==================== INSERT TYPES ====================
test_result_t test_insert_types() {
    // Insert into table with f64 columns
    TEST_ASSERT_EQ(
        "(set t (table [id price] (list [1] [10.5])))"
        "(insert t (list 2 20.0))",
        "(table [id price] (list [1 2] [10.5 20.0]))");

    // Insert into table with symbol column
    TEST_ASSERT_EQ(
        "(set t (table [id sym] (list [1] [apple])))"
        "(insert t (list 2 'banana))",
        "(table [id sym] (list [1 2] [apple banana]))");

    // Insert into table with date column
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01] [10])))"
        "(insert t (list 2024.01.02 20))",
        "(table [dt val] (list [2024.01.01 2024.01.02] [10 20]))");

    // Insert into table with time column
    TEST_ASSERT_EQ(
        "(set t (table [tm val] (list [09:00:00.000] [100])))"
        "(insert t (list 10:00:00.000 200))",
        "(table [tm val] (list [09:00:00.000 10:00:00.000] [100 200]))");

    // Insert into table with string column
    TEST_ASSERT_EQ(
        "(set t (table [id label] (list [1] (list \"hello\"))))"
        "(insert t (list 2 \"world\"))",
        "(table [id label] (list [1 2] (list \"hello\"\"world\")))");

    PASS();
}

// ==================== UPSERT BASIC ====================
test_result_t test_upsert_basic() {
    // Upsert new record
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (list 3 30))",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    // Verify original unchanged (return-copy)
    TEST_ASSERT_EQ("t", "(table [id val] (list [1 2] [10 20]))");

    // Upsert existing record
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(upsert t 1 (list 2 99))",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    // Upsert via dict (new)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (dict [id val] (list 3 30)))",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    // Upsert via dict (update)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(upsert t 1 (dict [id val] (list 2 99)))",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    PASS();
}

// ==================== UPSERT MULTI ====================
test_result_t test_upsert_multi() {
    // Upsert multiple records (all new)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (list [3 4] [30 40]))",
        "(table [id val] (list [1 2 3 4] [10 20 30 40]))");

    // Mixed insert + update in single upsert (multiple records)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (list [2 3] [99 30]))",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    // Upsert via table
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (table [id val] (list [3] [30])))",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    // BUG: key_count=2, update existing row — multi-key upsert match
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [1 2 3] [a b c] [10 20 30])))"
        "(upsert t 2 (list 2 'b 99))",
        "(table [k1 k2 val] (list [1 2 3] [a b c] [10 99 30]))");

    // key_count=2, insert new row
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [1 2] [a b] [10 20])))"
        "(upsert t 2 (list 3 'c 30))",
        "(table [k1 k2 val] (list [1 2 3] [a b c] [10 20 30]))");

    PASS();
}

// ==================== UPSERT IN-PLACE ====================
test_result_t test_upsert_inplace() {
    // In-place upsert (new)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert 't 1 (list 3 30))"
        "t",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    // In-place upsert (update existing)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(upsert 't 1 (list 2 99))"
        "t",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    // Sequential in-place upserts
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1] [10])))"
        "(upsert 't 1 (list 2 20))"
        "(upsert 't 1 (list 3 30))"
        "(upsert 't 1 (list 1 99))"
        "t",
        "(table [id val] (list [1 2 3] [99 20 30]))");

    PASS();
}

// ==================== ALTER VECTORS ====================
test_result_t test_alter_vectors() {
    // Alter set on vector
    TEST_ASSERT_EQ(
        "(set v [1 2 3 4 5])"
        "(alter 'v set 0 100)"
        "v",
        "[100 2 3 4 5]");

    // Alter set on last element
    TEST_ASSERT_EQ(
        "(set v [10 20 30])"
        "(alter 'v set 2 99)"
        "v",
        "[10 20 99]");

    // Alter concat (append)
    TEST_ASSERT_EQ(
        "(set v [1 2 3])"
        "(alter 'v concat 4)"
        "v",
        "[1 2 3 4]");

    // Alter + (arithmetic on element)
    TEST_ASSERT_EQ(
        "(set v [10 20 30])"
        "(alter 'v + 1 5)"
        "v",
        "[10 25 30]");

    // Alter * (multiply element)
    TEST_ASSERT_EQ(
        "(set v [10 20 30])"
        "(alter 'v * 0 3)"
        "v",
        "[30 20 30]");

    PASS();
}

// ==================== ALTER TABLES ====================
test_result_t test_alter_tables() {
    // Alter table column with + (add constant to column value)
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(alter 't + 'b 100)"
        "(at t 'b)",
        "[110 120 130]");

    // Alter set on table column
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(alter 't set 'b [99 99 99])"
        "(at t 'b)",
        "[99 99 99]");

    PASS();
}

// ==================== TABLE CONCATENATION ====================
test_result_t test_table_concat() {
    // Concat two tables with same columns
    TEST_ASSERT_EQ(
        "(concat (table [a b] (list [1 2] [10 20])) (table [a b] (list [3 4] [30 40])))",
        "(table [a b] (list [1 2 3 4] [10 20 30 40]))");

    // Concat with reordered columns
    TEST_ASSERT_EQ(
        "(concat (table [a b] (list [1] [10])) (table [b a] (list [20] [2])))",
        "(table [a b] (list [1 2] [10 20]))");

    // Concat error: mismatched types
    TEST_ASSERT_ER(
        "(concat (table [a] (list [1 2])) (table [a] (list [1f])))",
        "type");

    // Concat error: column not found
    TEST_ASSERT_ER(
        "(concat (table [d c] (list [1 2] (list \"a\" \"b\"))) "
        "(table [c b a] (list (list \"A\") [100] [0])))",
        "value");

    // Concat multiple tables via chain
    TEST_ASSERT_EQ(
        "(concat (concat (table [a] (list [1])) (table [a] (list [2]))) (table [a] (list [3])))",
        "(table [a] (list [1 2 3]))");

    // Concat preserving types
    TEST_ASSERT_EQ(
        "(set r (concat (table [x] (list [1.0 2.0])) (table [x] (list [3.0 4.0]))))"
        "(type (at r 'x))",
        "'F64");

    // Concat with temporal columns
    TEST_ASSERT_EQ(
        "(concat (table [dt v] (list [2024.01.01] [1])) (table [dt v] (list [2024.01.02] [2])))",
        "(table [dt v] (list [2024.01.01 2024.01.02] [1 2]))");

    PASS();
}

// ==================== TABLE WITH NULLS ====================
test_result_t test_table_nulls() {
    // Table with null values
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 0Nl 3] [4 5 6])))"
        "(at (at t 'a) 1)",
        "0Nl");

    // Aggregation with nulls
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [a a b] [10 0Nl 30])))"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [a b] [0Nl 30]))");

    PASS();
}

// ==================== COMPLEX QUERY TESTS ====================
test_result_t test_query_complex() {
    // Query chain: create, select, count
    TEST_ASSERT_EQ(
        "(set t (table [sym price qty] (list "
        "[aapl aapl goog goog msft] [150 155 100 105 300] [100 200 50 75 500])))"
        "(count (select {from: t where: (> price 110)}))",
        "3");

    // Nested expression in select
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [4 5 6])))"
        "(select {r: (- (* a b) a) from: t})",
        "(table [r] (list [3 8 15]))");

    // Select by with where combined
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b b] [10 20 30 40 50])))"
        "(select {s: (sum val) from: t by: grp where: (> val 15)})",
        "(table [grp s] (list [a b] [20 120]))");

    // Aggregation of computed expression
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(select {total: (sum (* a b)) from: t})",
        "(table [total] (list [140]))");

    // Use table as dict (first row)
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1 2 3] [4 5 6] [7 8 9])))"
        "(at (first t) 'b)",
        "4");

    // Select with xbar for bucketing
    TEST_ASSERT_EQ(
        "(set t (table [tm val] (list [09:00:01.000 09:00:05.000 09:00:11.000 09:00:15.000] [10 20 30 40])))"
        "(select {s: (sum val) from: t by: (xbar tm 10000)})",
        "(table [tm s] (list [09:00:01.000 09:00:11.000] [30 70]))");

    PASS();
}

// ==================== TABLE TYPE DIVERSITY ====================
test_result_t test_table_all_types() {
    // Table with all supported column types
    TEST_ASSERT_EQ(
        "(set t (table [b u h i l f d tm ts sym] (list "
        "[true false] [0x0a 0x0b] [1h 2h] [10i 20i] [100 200] [1.5 2.5] "
        "[2024.01.01 2024.01.02] [10:00:00.000 11:00:00.000] "
        "[2024.01.01D00:00:00.0 2024.01.02D00:00:00.0] [aapl goog])))"
        "(count t)",
        "2");

    // Verify types of columns
    TEST_ASSERT_EQ(
        "(type (at t 'b))", "'B8");
    TEST_ASSERT_EQ(
        "(type (at t 'u))", "'U8");
    TEST_ASSERT_EQ(
        "(type (at t 'h))", "'I16");
    TEST_ASSERT_EQ(
        "(type (at t 'i))", "'I32");
    TEST_ASSERT_EQ(
        "(type (at t 'l))", "'I64");
    TEST_ASSERT_EQ(
        "(type (at t 'f))", "'F64");
    TEST_ASSERT_EQ(
        "(type (at t 'd))", "'DATE");
    TEST_ASSERT_EQ(
        "(type (at t 'tm))", "'TIME");
    TEST_ASSERT_EQ(
        "(type (at t 'ts))", "'TIMESTAMP");
    TEST_ASSERT_EQ(
        "(type (at t 'sym))", "'SYMBOL");

    PASS();
}

// ==================== TABLE DICT CONVERSION ====================
test_result_t test_table_dict_conversion() {
    // Table to dict conversion
    TEST_ASSERT_EQ(
        "(type (as 'DICT (table [a b] (list [1 2] [3 4]))))",
        "'DICT");

    // Dict to table conversion
    TEST_ASSERT_EQ(
        "(type (as 'TABLE {a: [1 2 3] b: [4 5 6]}))",
        "'TABLE");

    // Dict literal to table and back
    TEST_ASSERT_EQ(
        "(as 'TABLE {a: [1 2] b: [3 4]})",
        "(table [a b] (list [1 2] [3 4]))");

    // First row of table is dict
    TEST_ASSERT_EQ(
        "(type (first (table [a b] (list [1 2] [3 4]))))",
        "'DICT");

    // Dict key/value matches table key/value
    TEST_ASSERT_EQ(
        "(key (first (table [x y] (list [10 20] [30 40]))))",
        "[x y]");

    PASS();
}

// ==================== TABLE EDGE CASES ====================
test_result_t test_table_edge_cases() {
    // Single element table aggregation
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(select {s: (sum b) from: t})",
        "(table [s] (list [10]))");

    // Table with duplicate column values in groupby -> single group
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [x x x] [1 2 3])))"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [x] [6]))");

    // Insert then query
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(set t (insert t (list 2 20)))"
        "(select {s: (sum b) from: t})",
        "(table [s] (list [30]))");

    // Large-ish generated table
    TEST_ASSERT_EQ(
        "(set t (table [idx val] (list (til 100) (* (til 100) 2))))"
        "(count t)",
        "100");

    // Select on generated table
    TEST_ASSERT_EQ(
        "(set t (table [idx val] (list (til 100) (* (til 100) 2))))"
        "(select {s: (sum val) from: t})",
        "(table [s] (list [9900]))");

    // Filter on generated table
    TEST_ASSERT_EQ(
        "(set t (table [idx val] (list (til 100) (* (til 100) 2))))"
        "(count (select {from: t where: (> val 100)}))",
        "49");

    PASS();
}

// ==================== QUERY CHAINS ====================
test_result_t test_query_chains() {
    // Create -> insert -> update -> select
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1] [10])))"
        "(insert 't (list 2 20))"
        "(insert 't (list 3 30))"
        "(update {from: 't val: (* val 2)})"
        "(select {s: (sum val) from: t})",
        "(table [s] (list [120]))");

    // Query result as input to next query
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(set agg (select {s: (sum val) from: t by: grp}))"
        "(select {from: agg where: (> s 50)})",
        "(table [grp s] (list [b] [70]))");

    // Aggregation of updated table
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [x x y y] [1 2 3 4])))"
        "(update {from: 't v: (* v 10)})"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [x y] [30 70]))");

    // Insert then groupby
    TEST_ASSERT_EQ(
        "(set t (table [sym val] (list [a b] [10 20])))"
        "(insert 't (list 'a 30))"
        "(select {s: (sum val) from: t by: sym})",
        "(table [sym s] (list [a b] [40 20]))");

    // Complex: filter, aggregate, check count
    TEST_ASSERT_EQ(
        "(set t (table [cat val] (list [x x y y y z] [1 2 3 4 5 6])))"
        "(count (select {s: (sum val) from: t by: cat where: (> val 2)}))",
        "2");

    PASS();
}

// ==================== SELECT TAKE TESTS ====================
test_result_t test_select_take() {
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "null",
        "null");

    // Take first N rows
    TEST_ASSERT_EQ(
        "(select {from: t take: 3})",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    // Take last N rows (negative)
    TEST_ASSERT_EQ(
        "(select {from: t take: -2})",
        "(table [a b] (list [4 5] [40 50]))");

    // Take 1 row
    TEST_ASSERT_EQ(
        "(select {from: t take: 1})",
        "(table [a b] (list [1] [10]))");

    // Take more than table size tiles/repeats (like q)
    TEST_ASSERT_EQ(
        "(count (select {from: t take: 100}))",
        "100");

    // Take with column projection
    TEST_ASSERT_EQ(
        "(select {a: a from: t take: 2})",
        "(table [a] (list [1 2]))");

    // Take with where
    TEST_ASSERT_EQ(
        "(select {from: t where: (> a 2) take: 2})",
        "(table [a b] (list [3 4] [30 40]))");

    // Take with by (take from grouped result)
    TEST_ASSERT_EQ(
        "(set t2 (table [g v] (list [a a a b b b] [1 2 3 4 5 6])))"
        "(select {s: (sum v) from: t2 by: g take: 1})",
        "(table [g s] (list [a] [6]))");

    // Take with computed column
    TEST_ASSERT_EQ(
        "(select {total: (+ a b) from: t take: 3})",
        "(table [total] (list [11 22 33]))");

    // Negative take with where
    TEST_ASSERT_EQ(
        "(select {from: t where: (> b 20) take: -1})",
        "(table [a b] (list [5] [50]))");

    PASS();
}

// ==================== SELECT SORTING IN CONTEXT ====================
test_result_t test_select_sorting() {
    // Sort column ascending
    TEST_ASSERT_EQ(
        "(asc [3 1 2 5 4])",
        "[1 2 3 4 5]");

    // Sort column descending
    TEST_ASSERT_EQ(
        "(desc [3 1 2 5 4])",
        "[5 4 3 2 1]");

    // Sort indices ascending
    TEST_ASSERT_EQ(
        "(iasc [30 10 20])",
        "[1 2 0]");

    // Sort indices descending
    TEST_ASSERT_EQ(
        "(idesc [30 10 20])",
        "[0 2 1]");

    // Sort table by column using xasc (table first, symbol second)
    TEST_ASSERT_EQ(
        "(set t (table [name val] (list [c a b] [30 10 20])))"
        "(xasc t 'val)",
        "(table [name val] (list [a b c] [10 20 30]))");

    // Sort table descending by column
    TEST_ASSERT_EQ(
        "(set t (table [name val] (list [c a b] [30 10 20])))"
        "(xdesc t 'val)",
        "(table [name val] (list [c b a] [30 20 10]))");

    // Sort by symbol column
    TEST_ASSERT_EQ(
        "(set t (table [sym val] (list [c a b] [3 1 2])))"
        "(at (xasc t 'sym) 'val)",
        "[1 2 3]");

    // Sort by date column
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.03 2024.01.01 2024.01.02] [30 10 20])))"
        "(at (xasc t 'dt) 'val)",
        "[10 20 30]");

    // Sort then select
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [3 1 2] [30 10 20])))"
        "(set t (xasc t 'id))"
        "(select {from: t take: 2})",
        "(table [id val] (list [1 2] [10 20]))");

    // Rank of values
    TEST_ASSERT_EQ(
        "(rank [30 10 20])",
        "[2 0 1]");

    // Reverse a vector
    TEST_ASSERT_EQ(
        "(reverse [1 2 3 4 5])",
        "[5 4 3 2 1]");

    // Sort f64 column
    TEST_ASSERT_EQ(
        "(asc [3.0 1.0 2.0])",
        "[1.0 2.0 3.0]");

    PASS();
}

// ==================== NON-GROUPED DEV/MED IN SELECT ====================
test_result_t test_select_dev_med() {
    // Non-grouped dev: all same → 0
    TEST_ASSERT_EQ(
        "(set t (table [v] (list [5 5 5 5])))"
        "(select {d: (dev v) from: t})",
        "(table [d] (list [0.0]))");

    // Non-grouped med: odd count
    TEST_ASSERT_EQ(
        "(set t (table [v] (list [5 1 3 2 4])))"
        "(select {m: (med v) from: t})",
        "(table [m] (list [3.0]))");

    // Non-grouped med: even count
    TEST_ASSERT_EQ(
        "(set t (table [v] (list [1 2 3 4])))"
        "(select {m: (med v) from: t})",
        "(table [m] (list [2.5]))");

    // Dev on f64 column
    TEST_ASSERT_EQ(
        "(set t (table [v] (list [1.0 1.0 1.0])))"
        "(select {d: (dev v) from: t})",
        "(table [d] (list [0.0]))");

    // Med with where filter
    TEST_ASSERT_EQ(
        "(set t (table [id v] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(select {m: (med v) from: t where: (> v 15)})",
        "(table [m] (list [35.0]))");

    // Dev + med together
    TEST_ASSERT_EQ(
        "(set t (table [v] (list [1 1 1 1])))"
        "(select {d: (dev v) m: (med v) from: t})",
        "(table [d m] (list [0.0] [1.0]))");

    // Dev groupby with variation
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [a a b b] [10 20 100 200])))"
        "(< (at (select {d: (dev v) from: t by: g}) 'd) [10f 100f])",
        "[true true]");

    // Med groupby with different group sizes
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [a a a b b] [1 2 3 10 20])))"
        "(select {m: (med v) from: t by: g})",
        "(table [g m] (list [a b] [2.0 15.0]))");

    PASS();
}

// ==================== XBAR ON DATES ====================
test_result_t test_select_xbar_dates() {
    // xbar on dates (bucket by 7 days = weekly)
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.03 2024.01.08 2024.01.10] [10 20 30 40])))"
        "(select {s: (sum val) from: t by: (xbar dt 7)})",
        "(table [dt s] (list [2024.01.01 2024.01.08] [30 70]))");

    // xbar on dates: daily (bucket=1 is identity)
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.01 2024.01.02] [10 20 30])))"
        "(select {s: (sum val) from: t by: (xbar dt 1)})",
        "(table [dt s] (list [2024.01.01 2024.01.02] [30 30]))");

    // xbar on timestamps (bucket by nanosecond interval)
    TEST_ASSERT_EQ(
        "(set t (table [ts val] (list "
        "[2024.01.01D10:00:00.000000000 2024.01.01D10:00:00.500000000 2024.01.01D10:00:01.000000000] "
        "[1 2 3])))"
        "(count (select {s: (sum val) from: t by: (xbar ts 1000000000)}))",
        "2");

    PASS();
}

// ==================== UPDATE WHERE+BY COMBINED ====================
test_result_t test_update_where_by() {
    // Update with where + by: grouped aggregate on filtered rows
    // val>15 filters out row 0 (10). Filtered: a=[20], b=[30,40,50]. sum: a=20, b=120
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b b] [10 20 30 40 50])))"
        "(update {from: 't s: (sum val) by: grp where: (> val 15)})"
        "(at t 's)",
        "[0Nl 20 120 120 120]");

    // Count per group with filter
    // val>3 filters out row 0 (1). Filtered: a=[5,10], b=[20,30]. count: a=2, b=2
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a b b] [1 5 10 20 30])))"
        "(update {from: 't n: (count val) by: grp where: (> val 3)})"
        "(at t 'n)",
        "[0Nl 2 2 2 2]");

    PASS();
}

// ==================== SET OPERATIONS ON TABLE COLUMNS ====================
test_result_t test_set_operations() {
    // Distinct on column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b a] [10 20 30 40 50])))"
        "(distinct (at t 'grp))",
        "[a b]");

    // Union of two columns
    TEST_ASSERT_EQ(
        "(union [1 2 3] [3 4 5])",
        "[1 2 3 4 5]");

    // Intersection of columns
    TEST_ASSERT_EQ(
        "(sect [1 2 3 4] [2 4 6])",
        "[2 4]");

    // Except (difference)
    TEST_ASSERT_EQ(
        "(except [1 2 3 4 5] [2 4])",
        "[1 3 5]");

    // Within — range membership
    TEST_ASSERT_EQ(
        "(within [5 15 25] [10 20])",
        "[false true false]");

    // Find indices
    TEST_ASSERT_EQ(
        "(find [10 20 30 40 50] 30)",
        "2");

    // Distinct on numeric column
    TEST_ASSERT_EQ(
        "(distinct [1 2 2 3 3 3])",
        "[1 2 3]");

    // Distinct on date
    TEST_ASSERT_EQ(
        "(distinct [2024.01.01 2024.01.01 2024.01.02])",
        "[2024.01.01 2024.01.02]");

    // Bin
    TEST_ASSERT_EQ(
        "(bin [10 20 30 40 50] 25)",
        "1");

    // Bin vector lookup
    TEST_ASSERT_EQ(
        "(bin [10 20 30 40 50] [5 15 25 35 45 55])",
        "[-1 0 1 2 3 4]");

    // Binr (right-inclusive)
    TEST_ASSERT_EQ(
        "(binr [10 20 30 40 50] [10 20 30])",
        "[0 1 2]");

    PASS();
}

// ==================== SELECT WITH EXPRESSIONS IN WHERE ====================
test_result_t test_select_where_expr() {
    // Where using computed expression: a+b=[6 6 6 6 6], all > 5
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3 4 5] [5 4 3 2 1])))"
        "(count (select {from: t where: (> (+ a b) 5)}))",
        "5");

    // Where using computed expression with variation
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(select {from: t where: (> (+ a b) 30)})",
        "(table [a b] (list [3 4 5] [30 40 50]))");

    // Where using multiplication
    TEST_ASSERT_EQ(
        "(set t (table [price qty] (list [10 20 30] [5 3 1])))"
        "(select {from: t where: (>= (* price qty) 50)})",
        "(table [price qty] (list [10 20] [5 3]))");

    // Where on symbol with in and computed select
    TEST_ASSERT_EQ(
        "(set t (table [fruit qty price] (list [apple banana cherry] [10 5 20] [1.0 2.0 0.5])))"
        "(select {total: (* qty price) from: t where: (in fruit [apple cherry])})",
        "(table [total] (list [10.0 10.0]))");

    // Where with nested comparison
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3 4 5] [50 40 30 20 10])))"
        "(select {from: t where: (and (> id 1) (< val 40))})",
        "(table [id val] (list [3 4 5] [30 20 10]))");

    PASS();
}

// ==================== INSERT EDGE CASES ====================
test_result_t test_insert_edge_cases() {
    // Insert into empty table
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [] [])))"
        "(insert t (list 1 10))",
        "(table [a b] (list [1] [10]))");

    // Multiple sequential inserts (copy semantics)
    TEST_ASSERT_EQ(
        "(set t (table [a] (list [1])))"
        "(set t (insert t (list 2)))"
        "(set t (insert t (list 3)))"
        "t",
        "(table [a] (list [1 2 3]))");

    // Insert many rows at once
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(set t (insert t (list [2 3 4 5] [20 30 40 50])))"
        "(count t)",
        "5");

    // Insert via dict preserves column order
    TEST_ASSERT_EQ(
        "(set t (table [x y z] (list [1] [2] [3])))"
        "(insert t (dict [x y z] (list 4 5 6)))",
        "(table [x y z] (list [1 4] [2 5] [3 6]))");

    PASS();
}

// ==================== UPSERT EDGE CASES ====================
test_result_t test_upsert_edge_cases() {
    // Upsert into empty table
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [] [])))"
        "(upsert t 1 (list 1 100))",
        "(table [id val] (list [1] [100]))");

    // Upsert all updates (no inserts)
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(upsert t 1 (list [1 2 3] [99 99 99]))",
        "(table [id val] (list [1 2 3] [99 99 99]))");

    // Sequential single-record upserts with key_count=2
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [1] [a] [10])))"
        "(upsert 't 2 (list 2 'b 20))"
        "(upsert 't 2 (list 1 'a 99))"
        "t",
        "(table [k1 k2 val] (list [1 2] [a b] [99 20]))");

    PASS();
}

// ==================== ALTER EDGE CASES ====================
test_result_t test_alter_edge_cases() {
    // Alter with remove on vector
    TEST_ASSERT_EQ(
        "(set v [1 2 3 4 5])"
        "(alter 'v remove 2)"
        "v",
        "[1 2 4 5]");

    // Alter remove first element
    TEST_ASSERT_EQ(
        "(set v [10 20 30])"
        "(alter 'v remove 0)"
        "v",
        "[20 30]");

    // Alter remove last element
    TEST_ASSERT_EQ(
        "(set v [10 20 30])"
        "(alter 'v remove 2)"
        "v",
        "[10 20]");

    // Alter concat: append single element
    TEST_ASSERT_EQ(
        "(set v [1 2 3])"
        "(alter 'v concat 4)"
        "(alter 'v concat 5)"
        "v",
        "[1 2 3 4 5]");

    // Alter table column with *
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [10 20 30])))"
        "(alter 't * 'b 2)"
        "(at t 'b)",
        "[20 40 60]");

    PASS();
}

// ==================== SELECT WITH DISTINCT ====================
test_result_t test_select_distinct() {
    // Select distinct values from column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b a] [10 20 30 40 50])))"
        "(count (distinct (at t 'grp)))",
        "2");

    // Use distinct in query workflow: unique groups
    TEST_ASSERT_EQ(
        "(set t (table [cat item] (list [food food drink drink food] [apple banana water juice cherry])))"
        "(distinct (at t 'cat))",
        "[food drink]");

    // Distinct count on large data
    TEST_ASSERT_EQ(
        "(set v (take [a b c] 1000))"
        "(count (distinct v))",
        "3");

    PASS();
}

// ==================== TABLE OPERATIONS MIXED ====================
test_result_t test_table_operations_mixed() {
    // Select then insert into new table
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(set filtered (select {from: t where: (> val 20)}))"
        "(count filtered)",
        "3");

    // Update then aggregate
    TEST_ASSERT_EQ(
        "(set t (table [g v] (list [a a b b] [1 2 3 4])))"
        "(set t (update {from: t v: (* v 10)}))"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [a b] [30 70]))");

    // Sort then take (xdesc table 'column)
    TEST_ASSERT_EQ(
        "(set t (table [name score] (list [c a d b] [70 90 60 80])))"
        "(set t (xdesc t 'score))"
        "(select {from: t take: 2})",
        "(table [name score] (list [a b] [90 80]))");

    // Concat then aggregate
    TEST_ASSERT_EQ(
        "(set t1 (table [g v] (list [a a] [10 20])))"
        "(set t2 (table [g v] (list [b b] [30 40])))"
        "(set t (concat t1 t2))"
        "(select {s: (sum v) from: t by: g})",
        "(table [g s] (list [a b] [30 70]))");

    // Upsert then select
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(set t (upsert t 1 (list 2 99)))"
        "(select {from: t where: (> val 50)})",
        "(table [id val] (list [2] [99]))");

    // Multi-step pipeline: insert -> update -> sort -> take
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1] [100])))"
        "(insert 't (list 2 50))"
        "(insert 't (list 3 75))"
        "(update {from: 't val: (* val 2)})"
        "(set t (xasc t 'val))"
        "(select {from: t take: 2})",
        "(table [id val] (list [2 3] [100 150]))");

    PASS();
}

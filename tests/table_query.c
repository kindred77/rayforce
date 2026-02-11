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

    PASS();
}

// ==================== SELECT WITH MULTIPLE GROUP KEYS ====================
test_result_t test_select_multikey() {
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

    // Group by boolean key
    TEST_ASSERT_EQ(
        "(set t2 (table [flag val] (list [true false true false true] [10 20 30 40 50])))"
        "(select {s: (sum val) from: t2 by: flag})",
        "(table [flag s] (list [true false] [90 60]))");

    PASS();
}

// ==================== UPDATE TESTS ====================
test_result_t test_update_operations() {
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

    // Update with where clause
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(update {from: 't val: 99 where: (== id 2)})"
        "t",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    // Update with by (group)
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(update {from: 't total: (sum val) by: grp})"
        "t",
        "(table [grp val total] (list [a a b b] [10 20 30 40] [30 30 70 70]))");

    // Immediate update (returns new table)
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2] [10 20])))"
        "(update {from: t c: 5})",
        "(table [a b c] (list [1 2] [10 20] [5 5]))");

    // Verify original unchanged after immediate
    TEST_ASSERT_EQ("t", "(table [a b] (list [1 2] [10 20]))");

    // Update with float multiplication
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [100 200 300])))"
        "(set t (update {val: (* val 1.5) from: t where: (> val 100)}))"
        "t",
        "(table [id val] (list [1 2 3] [100 300 450]))");

    PASS();
}

// ==================== INSERT / UPSERT TESTS ====================
test_result_t test_insert_upsert() {
    // Insert single row
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2] [10 20])))"
        "(insert t (list 3 30))",
        "(table [a b] (list [1 2 3] [10 20 30]))");

    // Insert in-place
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1] [10])))"
        "(insert 't (list 2 20))"
        "t",
        "(table [a b] (list [1 2] [10 20]))");

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

    // Upsert new record
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (list 3 30))",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    // Upsert existing record
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [10 20 30])))"
        "(upsert t 1 (list 2 99))",
        "(table [id val] (list [1 2 3] [10 99 30]))");

    // Upsert in-place
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert 't 1 (list 3 30))"
        "t",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    // Upsert via dict
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2] [10 20])))"
        "(upsert t 1 (dict [id val] (list 3 30)))",
        "(table [id val] (list [1 2 3] [10 20 30]))");

    PASS();
}

// ==================== TABLE CONCATENATION TESTS ====================
test_result_t test_table_concat() {
    // Concat two tables with same columns
    TEST_ASSERT_EQ(
        "(concat (table [a b] (list [1 2] [10 20])) (table [a b] (list [3 4] [30 40])))",
        "(table [a b] (list [1 2 3 4] [10 20 30 40]))");

    // Concat with column subset (left has fewer)
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

// ==================== SELECT BY WITH STRING KEYS ====================
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

// ==================== SELECT WITH AND/OR PARALLEL BUG ====================
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

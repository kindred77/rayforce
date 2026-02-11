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

// ==================== DICT ADVANCED TESTS ====================
test_result_t test_dict_advanced() {
    // Dict creation with various value types
    TEST_ASSERT_EQ("(dict [a b c] [1 2 3])", "{a:1 b:2 c:3}");
    TEST_ASSERT_EQ("(dict [a b] [1.5 2.5])", "{a:1.5 b:2.5}");
    TEST_ASSERT_EQ("(dict [a b] ['x 'y])", "{a:'x b:'y}");

    // Empty dict
    TEST_ASSERT_EQ("(dict [] [])", "{}");
    TEST_ASSERT_EQ("(key (dict [] []))", "[]");
    TEST_ASSERT_EQ("(value (dict [] []))", "[]");

    // Single entry dict
    TEST_ASSERT_EQ("(dict [a] [42])", "{a:42}");
    TEST_ASSERT_EQ("(at (dict [a] [42]) 'a)", "42");

    // Missing key returns null
    TEST_ASSERT_EQ("(at (dict [a b c] [1 2 3]) 'z)", "0Nl");
    TEST_ASSERT_EQ("(at (dict [a b c] [1 2 3]) 'd)", "0Nl");

    // Nested dict
    TEST_ASSERT_EQ(
        "(set d (dict [a b] (list 1 (dict [x y] [10 20]))))"
        "(at (at d 'b) 'x)",
        "10");
    TEST_ASSERT_EQ("(at (at d 'b) 'y)", "20");

    // Dict with vector values
    TEST_ASSERT_EQ(
        "(set d (dict [a b] (list [1 2 3] [4 5 6])))"
        "(at d 'a)",
        "[1 2 3]");

    // Dict key and value functions
    TEST_ASSERT_EQ("(key (dict [x y z] [1 2 3]))", "[x y z]");
    TEST_ASSERT_EQ("(value (dict [x y z] [1 2 3]))", "[1 2 3]");

    // Dict literal syntax
    TEST_ASSERT_EQ("{a: 1 b: 2}", "{a: 1 b: 2}");
    TEST_ASSERT_EQ("{x: \"hello\" y: 42}", "{x: \"hello\" y: 42}");
    TEST_ASSERT_EQ("{a: [1 2 3] b: [4 5 6]}", "{a: [1 2 3] b: [4 5 6]}");

    // Nested dict literal
    TEST_ASSERT_EQ("{a: 1 b: {c: 2 d: 3}}", "{a: 1 b: {c: 2 d: 3}}");

    // Dict from table row
    TEST_ASSERT_EQ(
        "(first (table [a b c] (list [10 20] [30 40] [50 60])))",
        "{a:10 b:30 c:50}");

    // Dict count
    TEST_ASSERT_EQ("(count (dict [a b c] [1 2 3]))", "3");
    TEST_ASSERT_EQ("(count (dict [] []))", "0");

    PASS();
}

// ==================== LAMBDA ADVANCED TESTS ====================
test_result_t test_lambda_advanced() {
    // Basic lambda call
    TEST_ASSERT_EQ("((fn [x] (+ x 1)) 10)", "11");
    TEST_ASSERT_EQ("((fn [x y] (* x y)) 3 7)", "21");
    TEST_ASSERT_EQ("((fn [] 99))", "99");

    // Multi-param lambda
    TEST_ASSERT_EQ("((fn [a b c d] (+ a (+ b (+ c d)))) 1 2 3 4)", "10");

    // Stored lambda
    TEST_ASSERT_EQ("(set sq (fn [x] (* x x))) (sq 7)", "49");
    TEST_ASSERT_EQ("(set add3 (fn [a b c] (+ a (+ b c)))) (add3 10 20 30)", "60");

    // Lambda with map
    TEST_ASSERT_EQ("(map (fn [x] (* x x)) [1 2 3 4 5])", "[1 4 9 16 25]");
    TEST_ASSERT_EQ("(map (fn [x] (+ x 100)) [0 1 2])", "[100 101 102]");
    TEST_ASSERT_EQ("(map (fn [x] (neg x)) [1 -2 3])", "[-1 2 -3]");

    // Lambda with conditional
    TEST_ASSERT_EQ("((fn [x] (if (> x 0) 'pos 'neg)) 5)", "'pos");
    TEST_ASSERT_EQ("((fn [x] (if (> x 0) 'pos 'neg)) -5)", "'neg");
    TEST_ASSERT_EQ("((fn [x] (if (== x 0) 'zero 'nonzero)) 0)", "'zero");

    // Lambda with vector operations
    TEST_ASSERT_EQ("((fn [v] (sum v)) [1 2 3 4 5])", "15");
    TEST_ASSERT_EQ("((fn [v] (count v)) [10 20 30])", "3");
    TEST_ASSERT_EQ("((fn [v] (avg v)) [10 20 30])", "20.0");

    // Recursive lambda (factorial)
    TEST_ASSERT_EQ(
        "(set fact (fn [n] (if (<= n 1) 1 (* n (fact (- n 1))))))"
        "(fact 6)",
        "720");

    // Recursive lambda (fibonacci)
    TEST_ASSERT_EQ(
        "(set fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2))))))"
        "(fib 10)",
        "55");

    // Lambda as filter predicate
    TEST_ASSERT_EQ(
        "(set evens (fn [v] (filter v (== (% v 2) 0))))"
        "(evens [1 2 3 4 5 6])",
        "[2 4 6]");

    PASS();
}

// ==================== ITERATION TESTS ====================
test_result_t test_iteration_advanced() {
    // map-left: scalar op with vector
    TEST_ASSERT_EQ("(map-left - 10 [1 2 3])", "[9 8 7]");
    TEST_ASSERT_EQ("(map-left * 5 [1 2 3])", "[5 10 15]");
    TEST_ASSERT_EQ("(map-left / 100 [2 4 5 10])", "[50 25 20 10]");

    // map-right: vector op with scalar
    TEST_ASSERT_EQ("(map-right - [10 20 30] 5)", "[5 15 25]");
    TEST_ASSERT_EQ("(map-right * [1 2 3] 10)", "[10 20 30]");
    TEST_ASSERT_EQ("(map-right / [10 20 30] 2)", "[5 10 15]");

    // Map with complex lambda
    TEST_ASSERT_EQ(
        "(map (fn [x] (if (> x 3) (* x 2) x)) [1 2 3 4 5])",
        "[1 2 3 8 10]");

    // Pmap (parallel map) - same results as map
    TEST_ASSERT_EQ("(pmap (fn [x] (* x x)) [1 2 3 4 5])", "[1 4 9 16 25]");
    TEST_ASSERT_EQ("(pmap (fn [x] (sum (til 100))) (til 3))", "[4950 4950 4950]");

    // Map over list of strings
    TEST_ASSERT_EQ(
        "(map count (list \"hello\" \"hi\" \"hey\"))",
        "[5 2 3]");

    // Map with two vectors via map-left/right
    TEST_ASSERT_EQ("(map-left + 0 [10 20 30])", "[10 20 30]");
    TEST_ASSERT_EQ("(map-right + [10 20 30] 0)", "[10 20 30]");

    PASS();
}

// ==================== DO/LET TESTS ====================
test_result_t test_do_let_advanced() {
    // Do returns last expression
    TEST_ASSERT_EQ("(do 1 2 3)", "3");
    TEST_ASSERT_EQ("(do (set x 10) (set y 20) (+ x y))", "30");

    // Do with side effects
    TEST_ASSERT_EQ("(do (set a 5) (set b (* a 2)) (+ a b))", "15");

    // Do with conditional
    TEST_ASSERT_EQ("(do (set x 10) (if (> x 5) 'big 'small))", "'big");
    TEST_ASSERT_EQ("(do (set x 3) (if (> x 5) 'big 'small))", "'small");

    // Nested do
    TEST_ASSERT_EQ("(do (do (do 42)))", "42");

    // Do with table operations
    TEST_ASSERT_EQ(
        "(do (set t (table [a b] (list [1 2 3] [4 5 6]))) (sum (at t 'b)))",
        "15");

    // Do with multiple set
    TEST_ASSERT_EQ(
        "(do (set x 1) (set x (+ x 1)) (set x (+ x 1)) x)",
        "3");

    PASS();
}

// ==================== CONDITIONAL ADVANCED TESTS ====================
test_result_t test_conditionals_advanced() {
    // Basic if/else
    TEST_ASSERT_EQ("(if true 1 2)", "1");
    TEST_ASSERT_EQ("(if false 1 2)", "2");

    // If with non-trivial condition
    TEST_ASSERT_EQ("(if (> 5 3) 'yes 'no)", "'yes");
    TEST_ASSERT_EQ("(if (< 5 3) 'yes 'no)", "'no");
    TEST_ASSERT_EQ("(if (== 5 5) 'eq 'neq)", "'eq");
    TEST_ASSERT_EQ("(if (!= 5 5) 'eq 'neq)", "'neq");

    // Nested if chains
    TEST_ASSERT_EQ(
        "(set x 5) (if (< x 0) 'neg (if (== x 0) 'zero 'pos))",
        "'pos");
    TEST_ASSERT_EQ(
        "(set x 0) (if (< x 0) 'neg (if (== x 0) 'zero 'pos))",
        "'zero");
    TEST_ASSERT_EQ(
        "(set x -3) (if (< x 0) 'neg (if (== x 0) 'zero 'pos))",
        "'neg");

    // If with side effects
    TEST_ASSERT_EQ("(set r 0) (if true (set r 10) (set r 20)) r", "10");
    TEST_ASSERT_EQ("(set r 0) (if false (set r 10) (set r 20)) r", "20");

    // If with complex expressions
    TEST_ASSERT_EQ("(if (> (+ 2 3) 4) (* 2 3) (/ 10 2))", "6");

    // If with string result
    TEST_ASSERT_EQ("(if true \"yes\" \"no\")", "\"yes\"");
    TEST_ASSERT_EQ("(if false \"yes\" \"no\")", "\"no\"");

    // If with vector result
    TEST_ASSERT_EQ("(if true [1 2 3] [4 5 6])", "[1 2 3]");
    TEST_ASSERT_EQ("(if false [1 2 3] [4 5 6])", "[4 5 6]");

    // Deeply nested if
    TEST_ASSERT_EQ(
        "(if true (if true (if true (if true 'deep 'x) 'x) 'x) 'x)",
        "'deep");

    // Boolean operations in conditions
    TEST_ASSERT_EQ("(if (and true true) 1 0)", "1");
    TEST_ASSERT_EQ("(if (and true false) 1 0)", "0");
    TEST_ASSERT_EQ("(if (or false true) 1 0)", "1");
    TEST_ASSERT_EQ("(if (or false false) 1 0)", "0");

    PASS();
}

// ==================== ERROR HANDLING TESTS ====================
test_result_t test_error_handling() {
    // Try/catch success
    TEST_ASSERT_EQ("(try (+ 1 2) (fn [e] 0))", "3");

    // Try/catch with error
    TEST_ASSERT_EQ("(try (raise \"boom\") (fn [e] 99))", "99");

    // Nested try/catch
    TEST_ASSERT_EQ(
        "(try (try (raise \"inner\") (fn [e] (raise \"outer\"))) (fn [e] 42))",
        "42");

    // Try without error propagates value
    TEST_ASSERT_EQ("(try (* 5 5) (fn [e] -1))", "25");

    // Raise with different error message
    TEST_ASSERT_EQ("(try (raise \"custom error\") (fn [e] 'caught))", "'caught");

    // Try around vector operation
    TEST_ASSERT_EQ("(try (sum [1 2 3]) (fn [e] 0))", "6");

    PASS();
}

// ==================== TYPE ERROR TESTS ====================
test_result_t test_type_errors() {
    // Arithmetic on null
    TEST_ASSERT_ER("(+ null 1)", "type");
    TEST_ASSERT_ER("(sum null)", "type");

    // Arithmetic on wrong types
    TEST_ASSERT_ER("(+ 0Nf 2024.03.20)", "type");

    // Filter length mismatch
    TEST_ASSERT_ER("(filter [1 2 3] [true true])", "length");

    // Filter wrong mask type
    TEST_ASSERT_ER("(filter [true false] [1 2])", "type");

    // Concat type mismatch on tables
    TEST_ASSERT_ER("(concat (table [a] (list [1 2])) (table [a] (list [1f])))", "type");

    // Domain errors
    TEST_ASSERT_ER("(til -1)", "domain");
    TEST_ASSERT_ER("(til -100)", "domain");
    TEST_ASSERT_ER("(rand -1 10)", "domain");
    TEST_ASSERT_ER("(rand 5 0)", "domain");
    TEST_ASSERT_ER("(rand 5 -1)", "domain");

    // Out of range literals
    TEST_ASSERT_ER("33000h", "domain");
    TEST_ASSERT_ER("-10001230000i", "domain");

    // Arity errors
    TEST_ASSERT_ER("(do (set v [1 2]) (modify 'v * 2))", "arity");

    // Index out of bounds
    TEST_ASSERT_ER("(do (set v [1 2 3]) (alter 'v set -10 0))", "index");

    PASS();
}

// ==================== DOMAIN EDGE CASES ====================
test_result_t test_domain_edge_cases() {
    // Division by zero returns null
    TEST_ASSERT_EQ("(/ 1 0)", "0Nl");

    // Empty vector operations
    TEST_ASSERT_EQ("(til 0)", "[]");
    TEST_ASSERT_EQ("(rand 0 10)", "[]");

    // Access on empty vector
    TEST_ASSERT_EQ("(at [] 0)", "0Nl");
    TEST_ASSERT_EQ("(first [])", "0Nl");
    TEST_ASSERT_EQ("(last [])", "0Nl");

    // Group empty
    TEST_ASSERT_EQ("(group [])", "{}");

    // Distinct empty
    TEST_ASSERT_EQ("(distinct [])", "[]");

    // Count empty
    TEST_ASSERT_EQ("(count [])", "0");

    // Sum of empty
    TEST_ASSERT_EQ("(sum [])", "0");

    // Min/max of single element
    TEST_ASSERT_EQ("(min [42])", "42");
    TEST_ASSERT_EQ("(max [42])", "42");

    // Operations with null propagation
    TEST_ASSERT_EQ("(+ 1 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(* 5 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ [1 2 3] [0Nl 2 3])", "[0Nl 4 6]");

    // Null comparisons
    TEST_ASSERT_EQ("(== 0Nl 0Nl)", "true");
    TEST_ASSERT_EQ("(== null null)", "true");

    // Nil? checks
    TEST_ASSERT_EQ("(nil? null)", "true");
    TEST_ASSERT_EQ("(nil? 0Nl)", "true");
    TEST_ASSERT_EQ("(nil? 0)", "false");
    TEST_ASSERT_EQ("(nil? 1)", "false");
    TEST_ASSERT_EQ("(nil? \"\")", "false");

    PASS();
}

// ==================== LAMBDA WITH TABLE OPERATIONS ====================
test_result_t test_lambda_with_tables() {
    // Lambda that processes a table column
    TEST_ASSERT_EQ(
        "(set process (fn [t] (sum (at t 'val))))"
        "(process (table [id val] (list [1 2 3] [10 20 30])))",
        "60");

    // Lambda creating a table
    TEST_ASSERT_EQ(
        "(set make_table (fn [n] (table [idx val] (list (til n) (* (til n) 10)))))"
        "(count (make_table 5))",
        "5");

    // Lambda with map over table operations
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3] [4 5 6])))"
        "(map (fn [x] (* x 2)) (at t 'a))",
        "[2 4 6]");

    // Lambda for conditional table query
    TEST_ASSERT_EQ(
        "(set high_val (fn [t threshold] (count (select {from: t where: (> val threshold)}))))"
        "(high_val (table [id val] (list [1 2 3 4 5] [10 20 30 40 50])) 25)",
        "3");

    PASS();
}

// ==================== SCAN / OVER ITERATION TESTS ====================
test_result_t test_scan_over() {
    PASS();
}

// ==================== MAP-LEFT / MAP-RIGHT EDGE CASES ====================
test_result_t test_map_edge_cases() {
    // map-left with zero
    TEST_ASSERT_EQ("(map-left + 0 [1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(map-left * 1 [5 10 15])", "[5 10 15]");

    // map-right with zero
    TEST_ASSERT_EQ("(map-right + [1 2 3] 0)", "[1 2 3]");
    TEST_ASSERT_EQ("(map-right * [5 10 15] 1)", "[5 10 15]");

    // map with empty vector
    TEST_ASSERT_EQ("(map (fn [x] x) [])", "[]");

    // map with single element
    TEST_ASSERT_EQ("(map (fn [x] (* x 2)) [5])", "[10]");

    // Nested map
    TEST_ASSERT_EQ(
        "(map (fn [v] (sum v)) (list [1 2] [3 4] [5 6]))",
        "[3 7 11]");

    PASS();
}

// ==================== EACH / MAP ON LISTS ====================
test_result_t test_each_operations() {
    // Map count over list of lists
    TEST_ASSERT_EQ(
        "(map count (list [1 2 3] [4 5] [6]))",
        "[3 2 1]");

    // Map sum over list of vectors
    TEST_ASSERT_EQ(
        "(map sum (list [1 2 3] [10 20] [100]))",
        "[6 30 100]");

    // Map first over list of vectors
    TEST_ASSERT_EQ(
        "(map first (list [10 20] [30 40] [50 60]))",
        "[10 30 50]");

    // Map last over list of vectors
    TEST_ASSERT_EQ(
        "(map last (list [10 20] [30 40] [50 60]))",
        "[20 40 60]");

    // Map type over list of diverse values
    TEST_ASSERT_EQ(
        "(map count (list \"hello\" \"hi\" \"hey\" \"h\"))",
        "[5 2 3 1]");

    PASS();
}

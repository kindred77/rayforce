/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

// ==================== I16 ARITHMETIC EDGE CASES ====================
test_result_t test_math_i16_arithmetic() {
    // i16 + i16 basic
    TEST_ASSERT_EQ("(+ 100h 200h)", "300h");
    TEST_ASSERT_EQ("(+ -100h 50h)", "-50h");
    TEST_ASSERT_EQ("(+ 0h 0h)", "0h");

    // i16 null propagation all ops
    TEST_ASSERT_EQ("(+ 0Nh 0Nh)", "0Nh");
    TEST_ASSERT_EQ("(- 0Nh 5h)", "0Nh");
    TEST_ASSERT_EQ("(* 0Nh 3h)", "0Nh");
    TEST_ASSERT_EQ("(+ 0Nh 10)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Nh 10.0)", "0Nf");
    TEST_ASSERT_EQ("(- 10h 0Nh)", "0Nh");
    TEST_ASSERT_EQ("(* 10h 0Nh)", "0Nh");

    // i16 vector ops
    TEST_ASSERT_EQ("(+ [10h 20h 30h] [1h 2h 3h])", "[11h 22h 33h]");
    TEST_ASSERT_EQ("(- [30h 20h 10h] [1h 2h 3h])", "[29h 18h 7h]");
    TEST_ASSERT_EQ("(* [2h 3h 4h] [5h 6h 7h])", "[10h 18h 28h]");

    // i16 scalar-vector
    TEST_ASSERT_EQ("(+ 10h [1h 2h 3h])", "[11h 12h 13h]");
    TEST_ASSERT_EQ("(- 10h [1h 2h 3h])", "[9h 8h 7h]");
    TEST_ASSERT_EQ("(* 10h [1h 2h 3h])", "[10h 20h 30h]");

    // i16 vector-scalar
    TEST_ASSERT_EQ("(+ [1h 2h 3h] 10h)", "[11h 12h 13h]");
    TEST_ASSERT_EQ("(- [10h 20h 30h] 5h)", "[5h 15h 25h]");
    TEST_ASSERT_EQ("(* [1h 2h 3h] 10h)", "[10h 20h 30h]");

    // i16 with null in vector
    TEST_ASSERT_EQ("(+ [1h 0Nh 3h] [4h 5h 6h])", "[5h 0Nh 9h]");
    TEST_ASSERT_EQ("(- [1h 0Nh 3h] [4h 5h 6h])", "[-3h 0Nh -3h]");
    TEST_ASSERT_EQ("(* [1h 0Nh 3h] [4h 5h 6h])", "[4h 0Nh 18h]");

    // i16 promotion to i32
    TEST_ASSERT_EQ("(+ 5h 5i)", "10i");
    TEST_ASSERT_EQ("(- 10h 3i)", "7i");
    TEST_ASSERT_EQ("(* 5h 3i)", "15i");

    // i16 promotion to i64
    TEST_ASSERT_EQ("(+ 5h 5)", "10");
    TEST_ASSERT_EQ("(- 10h 3)", "7");
    TEST_ASSERT_EQ("(* 5h 3)", "15");

    // i16 promotion to f64
    TEST_ASSERT_EQ("(+ 5h 2.5)", "7.5");
    TEST_ASSERT_EQ("(- 10h 3.5)", "6.5");
    TEST_ASSERT_EQ("(* 5h 2.5)", "12.5");

    PASS();
}

// ==================== I32 ARITHMETIC EDGE CASES ====================
test_result_t test_math_i32_arithmetic() {
    // i32 basic
    TEST_ASSERT_EQ("(+ 100i 200i)", "300i");
    TEST_ASSERT_EQ("(+ -100i 50i)", "-50i");
    TEST_ASSERT_EQ("(+ 0i 0i)", "0i");
    TEST_ASSERT_EQ("(- 0i 0i)", "0i");

    // i32 null propagation all ops
    TEST_ASSERT_EQ("(+ 0Ni 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(- 0Ni 5i)", "0Ni");
    TEST_ASSERT_EQ("(* 0Ni 3i)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 3i)", "0Ni");
    TEST_ASSERT_EQ("(% 0Ni 3i)", "0Ni");

    // i32 vector ops
    TEST_ASSERT_EQ("(+ [100i 200i 300i] [1i 2i 3i])", "[101i 202i 303i]");
    TEST_ASSERT_EQ("(- [300i 200i 100i] [1i 2i 3i])", "[299i 198i 97i]");
    TEST_ASSERT_EQ("(* [2i 3i 4i] [5i 6i 7i])", "[10i 18i 28i]");

    // i32 scalar-vector mixed type
    TEST_ASSERT_EQ("(+ 10i [1 2 3])", "[11 12 13]");
    TEST_ASSERT_EQ("(+ 10i [1.0 2.0 3.0])", "[11.0 12.0 13.0]");

    // i32 with null in vector
    TEST_ASSERT_EQ("(+ [1i 0Ni 3i] [4i 5i 6i])", "[5i 0Ni 9i]");
    TEST_ASSERT_EQ("(* [1i 0Ni 3i] 2i)", "[2i 0Ni 6i]");

    // i32 promotion to i64
    TEST_ASSERT_EQ("(+ 100i 100)", "200");
    TEST_ASSERT_EQ("(* 100i 100)", "10000");

    // i32 promotion to f64
    TEST_ASSERT_EQ("(+ 100i 0.5)", "100.5");
    TEST_ASSERT_EQ("(* 100i 0.5)", "50.0");

    // i32 division
    TEST_ASSERT_EQ("(/ 10i 3i)", "3i");
    TEST_ASSERT_EQ("(/ -10i 3i)", "-4i");
    TEST_ASSERT_EQ("(/ 10i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 10i 0Ni)", "0Ni");

    // i32 modulo
    TEST_ASSERT_EQ("(% 10i 3i)", "1i");
    TEST_ASSERT_EQ("(% -10i 3i)", "2i");
    TEST_ASSERT_EQ("(% 10i 0i)", "0Ni");

    PASS();
}

// ==================== MIXED TYPE PROMOTION ====================
test_result_t test_math_type_promotion() {
    // i16 + i32 -> i32
    TEST_ASSERT_EQ("(+ 5h 10i)", "15i");
    TEST_ASSERT_EQ("(+ [5h] [10i])", "[15i]");
    TEST_ASSERT_EQ("(+ 5h [10i 20i])", "[15i 25i]");
    TEST_ASSERT_EQ("(+ [5h 10h] 10i)", "[15i 20i]");

    // i16 + i64 -> i64
    TEST_ASSERT_EQ("(+ 5h 10)", "15");
    TEST_ASSERT_EQ("(+ [5h] [10])", "[15]");

    // i32 + i64 -> i64
    TEST_ASSERT_EQ("(+ 5i 10)", "15");
    TEST_ASSERT_EQ("(+ [5i] [10])", "[15]");

    // i16 + f64 -> f64
    TEST_ASSERT_EQ("(+ 5h 1.5)", "6.5");
    TEST_ASSERT_EQ("(+ [5h] [1.5])", "[6.5]");

    // i32 + f64 -> f64
    TEST_ASSERT_EQ("(+ 5i 1.5)", "6.5");
    TEST_ASSERT_EQ("(+ [5i] [1.5])", "[6.5]");

    // i64 + f64 -> f64
    TEST_ASSERT_EQ("(+ 5 1.5)", "6.5");
    TEST_ASSERT_EQ("(+ [5] [1.5])", "[6.5]");

    // u8 promotions
    TEST_ASSERT_EQ("(+ 0x05 5h)", "10h");
    TEST_ASSERT_EQ("(+ 0x05 5i)", "10i");
    TEST_ASSERT_EQ("(+ 0x05 5)", "10");
    TEST_ASSERT_EQ("(+ 0x05 5.0)", "10.0");

    // Subtraction promotions
    TEST_ASSERT_EQ("(- 10h 5i)", "5i");
    TEST_ASSERT_EQ("(- 10i 5)", "5");
    TEST_ASSERT_EQ("(- 10 5.0)", "5.0");

    // Multiplication promotions
    TEST_ASSERT_EQ("(* 3h 4i)", "12i");
    TEST_ASSERT_EQ("(* 3i 4)", "12");
    TEST_ASSERT_EQ("(* 3 4.0)", "12.0");

    // Division promotions
    TEST_ASSERT_EQ("(/ 10h 3i)", "3i");
    TEST_ASSERT_EQ("(/ 10i 3)", "3");

    PASS();
}

// ==================== NULL PROPAGATION COMPREHENSIVE ====================
test_result_t test_math_null_propagation() {
    // i16 null propagation
    TEST_ASSERT_EQ("(+ 0Nh 0Nh)", "0Nh");
    TEST_ASSERT_EQ("(+ 0Nh 1h)", "0Nh");
    TEST_ASSERT_EQ("(+ 1h 0Nh)", "0Nh");
    TEST_ASSERT_EQ("(- 0Nh 1h)", "0Nh");
    TEST_ASSERT_EQ("(* 0Nh 1h)", "0Nh");

    // i16 null promotes to wider null
    TEST_ASSERT_EQ("(+ 0Nh 1i)", "0Ni");
    TEST_ASSERT_EQ("(+ 0Nh 1)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Nh 1.0)", "0Nf");
    TEST_ASSERT_EQ("(+ 1i 0Nh)", "0Ni");

    // i32 null propagation
    TEST_ASSERT_EQ("(+ 0Ni 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(+ 0Ni 1i)", "0Ni");
    TEST_ASSERT_EQ("(+ 1i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(- 0Ni 1i)", "0Ni");
    TEST_ASSERT_EQ("(* 0Ni 1i)", "0Ni");

    // i32 null promotes to wider null
    TEST_ASSERT_EQ("(+ 0Ni 1)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Ni 1.0)", "0Nf");
    TEST_ASSERT_EQ("(+ 1 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(+ 1.0 0Ni)", "0Nf");

    // i64 null propagation
    TEST_ASSERT_EQ("(+ 0Nl 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Nl 1)", "0Nl");
    TEST_ASSERT_EQ("(+ 1 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(- 0Nl 1)", "0Nl");
    TEST_ASSERT_EQ("(* 0Nl 1)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 1)", "0Nl");

    // i64 null + f64 -> f64 null
    TEST_ASSERT_EQ("(+ 0Nl 1.0)", "0Nf");
    TEST_ASSERT_EQ("(+ 1.0 0Nl)", "0Nf");

    // f64 null propagation
    TEST_ASSERT_EQ("(+ 0Nf 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Nf 1.0)", "0Nf");
    TEST_ASSERT_EQ("(+ 1.0 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(- 0Nf 1.0)", "0Nf");
    TEST_ASSERT_EQ("(* 0Nf 1.0)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 1.0)", "0Nf");

    // f64 null with integers
    TEST_ASSERT_EQ("(+ 0Nf 5)", "0Nf");
    TEST_ASSERT_EQ("(+ 5 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Nf 5i)", "0Nf");
    TEST_ASSERT_EQ("(+ 5i 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Nf 5h)", "0Nf");

    // Null in vector operations
    TEST_ASSERT_EQ("(+ [1 0Nl 3] [4 5 6])", "[5 0Nl 9]");
    TEST_ASSERT_EQ("(+ [1i 0Ni 3i] [4i 5i 6i])", "[5i 0Ni 9i]");
    TEST_ASSERT_EQ("(+ [1.0 0Nf 3.0] [4.0 5.0 6.0])", "[5.0 0Nf 9.0]");
    TEST_ASSERT_EQ("(* [1 0Nl 3] 2)", "[2 0Nl 6]");
    TEST_ASSERT_EQ("(/ [10 0Nl 30] 5)", "[2 0Nl 6]");

    // Division by zero produces null
    TEST_ASSERT_EQ("(/ 10 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 10i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 10.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(/ 10.0 -0.0)", "0Nf");
    TEST_ASSERT_EQ("(/ [10] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [10i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [10.0] [0.0])", "[0Nf]");

    // Modulo by zero produces null
    TEST_ASSERT_EQ("(% 10 0)", "0Nl");
    TEST_ASSERT_EQ("(% 10i 0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10.0 0.0)", "0Nf");

    PASS();
}

// ==================== DIVISION EDGE CASES ====================
test_result_t test_math_division_edges() {
    // Integer division truncates toward negative infinity
    TEST_ASSERT_EQ("(/ 7 2)", "3");
    TEST_ASSERT_EQ("(/ -7 2)", "-4");
    TEST_ASSERT_EQ("(/ 7 -2)", "-4");
    TEST_ASSERT_EQ("(/ -7 -2)", "3");

    // i32 integer division
    TEST_ASSERT_EQ("(/ 7i 2i)", "3i");
    TEST_ASSERT_EQ("(/ -7i 2i)", "-4i");
    TEST_ASSERT_EQ("(/ 7i -2i)", "-4i");
    TEST_ASSERT_EQ("(/ -7i -2i)", "3i");

    // Division by zero returns null for all types
    TEST_ASSERT_EQ("(/ 1 0)", "0Nl");
    TEST_ASSERT_EQ("(/ -1 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 1i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ -1i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 1.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.0 -0.0)", "0Nf");

    // Division with float rounding
    TEST_ASSERT_EQ("(/ 10 3.0)", "3");
    TEST_ASSERT_EQ("(/ 10i 3.0)", "3");

    // Vector division by zero
    TEST_ASSERT_EQ("(/ [10 20 30] [0 5 0])", "[0Nl 4 0Nl]");
    TEST_ASSERT_EQ("(/ [10i 20i 30i] [0i 5i 0i])", "[0Ni 4i 0Ni]");
    TEST_ASSERT_EQ("(/ [10.0 20.0 30.0] [0.0 5.0 0.0])", "[0Nf 4.0 0Nf]");

    // Division identity
    TEST_ASSERT_EQ("(/ 42 1)", "42");
    TEST_ASSERT_EQ("(/ 42i 1i)", "42i");
    TEST_ASSERT_EQ("(/ 42.0 1.0)", "42.0");

    // Zero divided by anything non-zero
    TEST_ASSERT_EQ("(/ 0 100)", "0");
    TEST_ASSERT_EQ("(/ 0i 100i)", "0i");
    TEST_ASSERT_EQ("(/ 0.0 100.0)", "0.0");

    // u8 division
    TEST_ASSERT_EQ("(/ 0x0a 0x02)", "0x05");
    TEST_ASSERT_EQ("(/ 0x0a 0x03)", "0x03");
    TEST_ASSERT_EQ("(/ 0x00 0x05)", "0x00");

    // i16 division
    TEST_ASSERT_EQ("(/ 10h 3h)", "3h");
    TEST_ASSERT_EQ("(/ -10h 3h)", "-4h");
    TEST_ASSERT_EQ("(/ [10h 20h] [3h 4h])", "[3h 5h]");

    PASS();
}

// ==================== MODULO EDGE CASES ====================
test_result_t test_math_modulo_edges() {
    // Basic modulo
    TEST_ASSERT_EQ("(% 10 3)", "1");
    TEST_ASSERT_EQ("(% 10i 3i)", "1i");
    TEST_ASSERT_EQ("(% 10.0 3.0)", "1.0");

    // Modulo with negative dividend (Euclidean-style: result has sign of divisor)
    TEST_ASSERT_EQ("(% -10 3)", "2");
    TEST_ASSERT_EQ("(% -10i 3i)", "2i");

    // Modulo with negative divisor
    TEST_ASSERT_EQ("(% 10 -3)", "-2");
    TEST_ASSERT_EQ("(% 10i -3i)", "-2i");

    // Modulo both negative
    TEST_ASSERT_EQ("(% -10 -3)", "-1");
    TEST_ASSERT_EQ("(% -10i -3i)", "-1i");

    // Modulo by zero
    TEST_ASSERT_EQ("(% 10 0)", "0Nl");
    TEST_ASSERT_EQ("(% 10i 0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% -10 0)", "0Nl");
    TEST_ASSERT_EQ("(% 0 0)", "0Nl");

    // Modulo with exact divisibility
    TEST_ASSERT_EQ("(% 12 3)", "0");
    TEST_ASSERT_EQ("(% 12 4)", "0");
    TEST_ASSERT_EQ("(% -12 3)", "0");
    TEST_ASSERT_EQ("(% 12i 3i)", "0i");

    // Modulo with null
    TEST_ASSERT_EQ("(% 0Nl 5)", "0Nl");
    TEST_ASSERT_EQ("(% 5 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(% 0Ni 5i)", "0Ni");
    TEST_ASSERT_EQ("(% 5i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(% 0Nf 5.0)", "0Nf");
    TEST_ASSERT_EQ("(% 5.0 0Nf)", "0Nf");

    // Vector modulo
    TEST_ASSERT_EQ("(% [10 11 12] 5)", "[0 1 2]");
    TEST_ASSERT_EQ("(% [10 11 12] [3 4 5])", "[1 3 2]");
    TEST_ASSERT_EQ("(% [-10 -11 -12] 5)", "[0 4 3]");

    // i16 modulo
    TEST_ASSERT_EQ("(% 10h 3h)", "1h");
    TEST_ASSERT_EQ("(% -10h 3h)", "2h");
    TEST_ASSERT_EQ("(% [10h 11h] [3h 4h])", "[1h 3h]");

    // Float modulo
    TEST_ASSERT_EQ("(% 10.5 3.0)", "1.5");
    TEST_ASSERT_EQ("(% 18.4 5.1)", "3.1");

    // Large number modulo
    TEST_ASSERT_EQ("(% 100000000001 7)", "6");
    TEST_ASSERT_EQ("(% 100000000001 [7])", "[6]");

    PASS();
}

// ==================== MATH FUNCTIONS: NEG, ROUND, FLOOR, CEIL ====================
test_result_t test_math_abs_neg() {
    // neg on various types
    TEST_ASSERT_EQ("(neg 5)", "-5");
    TEST_ASSERT_EQ("(neg -5)", "5");
    TEST_ASSERT_EQ("(neg 0)", "0");
    TEST_ASSERT_EQ("(neg 5i)", "-5i");
    TEST_ASSERT_EQ("(neg -5i)", "5i");
    TEST_ASSERT_EQ("(neg 5h)", "-5h");
    TEST_ASSERT_EQ("(neg -5h)", "5h");
    TEST_ASSERT_EQ("(neg 5.0)", "-5.0");
    TEST_ASSERT_EQ("(neg -5.0)", "5.0");
    TEST_ASSERT_EQ("(neg 0.0)", "0.00");

    // neg on vectors
    TEST_ASSERT_EQ("(neg [1 -2 3 -4])", "[-1 2 -3 4]");
    TEST_ASSERT_EQ("(neg [1i -2i 3i -4i])", "[-1i 2i -3i 4i]");
    TEST_ASSERT_EQ("(neg [1h -2h 3h -4h])", "[-1h 2h -3h 4h]");
    TEST_ASSERT_EQ("(neg [1.0 -2.0 3.0 -4.0])", "[-1.0 2.0 -3.0 4.0]");

    // neg with null
    TEST_ASSERT_EQ("(neg 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(neg 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(neg 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(neg [1 0Nl -3])", "[-1 0Nl 3]");

    PASS();
}

// ==================== MATH FUNCTIONS: FLOOR, CEIL ====================
test_result_t test_math_sqrt_floor_ceil() {
    // floor
    TEST_ASSERT_EQ("(floor 3.7)", "3.0");
    TEST_ASSERT_EQ("(floor 3.0)", "3.0");
    TEST_ASSERT_EQ("(floor -3.7)", "-4.0");
    TEST_ASSERT_EQ("(floor -3.0)", "-3.0");
    TEST_ASSERT_EQ("(floor 0.0)", "0.0");
    TEST_ASSERT_EQ("(floor 0.5)", "0.0");
    TEST_ASSERT_EQ("(floor -0.5)", "-1.0");

    // floor on vectors
    TEST_ASSERT_EQ("(floor [1.2 2.8 -3.1 -4.9])", "[1.0 2.0 -4.0 -5.0]");

    // floor with null
    TEST_ASSERT_EQ("(floor 0Nf)", "0Nf");

    // ceil
    TEST_ASSERT_EQ("(ceil 3.2)", "4.0");
    TEST_ASSERT_EQ("(ceil 3.0)", "3.0");
    TEST_ASSERT_EQ("(ceil -3.2)", "-3.0");
    TEST_ASSERT_EQ("(ceil -3.0)", "-3.0");
    TEST_ASSERT_EQ("(ceil 0.0)", "0.0");
    TEST_ASSERT_EQ("(ceil 0.5)", "1.0");
    TEST_ASSERT_EQ("(ceil -0.5)", "0.0");

    // ceil on vectors
    TEST_ASSERT_EQ("(ceil [1.2 2.8 -3.1 -4.9])", "[2.0 3.0 -3.0 -4.0]");

    // ceil with null
    TEST_ASSERT_EQ("(ceil 0Nf)", "0Nf");

    PASS();
}

// ==================== MATH FUNCTIONS: ROUND ====================
test_result_t test_math_log_exp() {
    // round function
    TEST_ASSERT_EQ("(round 3.7)", "4.0");
    TEST_ASSERT_EQ("(round 3.2)", "3.0");
    TEST_ASSERT_EQ("(round -3.7)", "-4.0");
    TEST_ASSERT_EQ("(round -3.2)", "-3.0");
    TEST_ASSERT_EQ("(round 0.5)", "1.0");
    TEST_ASSERT_EQ("(round 0.0)", "0.0");

    // round on vectors
    TEST_ASSERT_EQ("(round [1.2 2.8 -3.1 -4.9])", "[1.0 3.0 -3.0 -5.0]");

    // round with null
    TEST_ASSERT_EQ("(round 0Nf)", "0Nf");

    PASS();
}

// ==================== VECTOR-VECTOR, SCALAR-VECTOR COMPREHENSIVE ====================
test_result_t test_math_vector_ops() {
    // Empty vector operations
    TEST_ASSERT_EQ("(+ [] [])", "[]");
    TEST_ASSERT_EQ("(- [] [])", "[]");
    TEST_ASSERT_EQ("(* [] [])", "[]");

    // Single element vectors
    TEST_ASSERT_EQ("(+ [5] [3])", "[8]");
    TEST_ASSERT_EQ("(- [5] [3])", "[2]");
    TEST_ASSERT_EQ("(* [5] [3])", "[15]");
    TEST_ASSERT_EQ("(/ [15] [3])", "[5]");

    // Scalar left, vector right - all ops
    TEST_ASSERT_EQ("(+ 10 [1 2 3])", "[11 12 13]");
    TEST_ASSERT_EQ("(- 10 [1 2 3])", "[9 8 7]");
    TEST_ASSERT_EQ("(* 10 [1 2 3])", "[10 20 30]");
    TEST_ASSERT_EQ("(/ 10 [1 2 5])", "[10 5 2]");
    TEST_ASSERT_EQ("(% 10 [3 4 5])", "[1 2 0]");

    // Vector left, scalar right - all ops
    TEST_ASSERT_EQ("(+ [1 2 3] 10)", "[11 12 13]");
    TEST_ASSERT_EQ("(- [10 20 30] 5)", "[5 15 25]");
    TEST_ASSERT_EQ("(* [1 2 3] 10)", "[10 20 30]");
    TEST_ASSERT_EQ("(/ [10 20 30] 10)", "[1 2 3]");
    TEST_ASSERT_EQ("(% [10 11 12] 5)", "[0 1 2]");

    // Vector-vector same type
    TEST_ASSERT_EQ("(+ [1 2 3] [4 5 6])", "[5 7 9]");
    TEST_ASSERT_EQ("(- [10 20 30] [1 2 3])", "[9 18 27]");
    TEST_ASSERT_EQ("(* [2 3 4] [5 6 7])", "[10 18 28]");
    TEST_ASSERT_EQ("(/ [10 20 30] [2 4 5])", "[5 5 6]");
    TEST_ASSERT_EQ("(% [10 11 12] [3 4 5])", "[1 3 2]");

    // Mixed type vector ops
    TEST_ASSERT_EQ("(+ [1i 2i] [3 4])", "[4 6]");
    TEST_ASSERT_EQ("(+ [1i 2i] [3.0 4.0])", "[4.0 6.0]");
    TEST_ASSERT_EQ("(+ [1 2] [3.0 4.0])", "[4.0 6.0]");
    TEST_ASSERT_EQ("(* [2i 3i] [1.5 2.5])", "[3.0 7.5]");

    PASS();
}

// ==================== LARGE NUMBER ARITHMETIC ====================
test_result_t test_math_large_numbers() {
    // Large i64 values
    TEST_ASSERT_EQ("(+ 1000000000 1000000000)", "2000000000");
    TEST_ASSERT_EQ("(* 1000000 1000000)", "1000000000000");
    TEST_ASSERT_EQ("(- 1000000000000 1)", "999999999999");
    TEST_ASSERT_EQ("(/ 1000000000000 1000000)", "1000000");

    // Large number modulo
    TEST_ASSERT_EQ("(% 1000000000007 1000000)", "7");
    TEST_ASSERT_EQ("(% 100000000001 5)", "1");

    // Large vector operations
    TEST_ASSERT_EQ("(+ [1000000000 2000000000] [3000000000 4000000000])",
                   "[4000000000 6000000000]");
    TEST_ASSERT_EQ("(* [1000000 2000000] [1000000 1000000])",
                   "[1000000000000 2000000000000]");

    PASS();
}

// ==================== UNARY AGGREGATION FUNCTIONS ====================
test_result_t test_math_unary_aggregations() {
    // sum
    TEST_ASSERT_EQ("(sum [1 2 3 4 5])", "15");
    TEST_ASSERT_EQ("(sum [1i 2i 3i])", "6");
    TEST_ASSERT_EQ("(sum [1h 2h 3h])", "6");
    TEST_ASSERT_EQ("(sum [1.0 2.0 3.0])", "6.0");
    TEST_ASSERT_EQ("(sum [])", "0");
    TEST_ASSERT_EQ("(sum [0Nl 1 2])", "3");

    // avg
    TEST_ASSERT_EQ("(avg [2 4 6])", "4.0");
    TEST_ASSERT_EQ("(avg [1.0 2.0 3.0])", "2.0");
    TEST_ASSERT_EQ("(avg [10i 20i 30i])", "20.0");
    TEST_ASSERT_EQ("(avg [0Nl 2 4])", "3.0");

    // min / max
    TEST_ASSERT_EQ("(min [3 1 4 1 5])", "1");
    TEST_ASSERT_EQ("(max [3 1 4 1 5])", "5");
    TEST_ASSERT_EQ("(min [3i 1i 4i])", "1i");
    TEST_ASSERT_EQ("(max [3i 1i 4i])", "4i");
    TEST_ASSERT_EQ("(min [3h 1h 4h])", "1h");
    TEST_ASSERT_EQ("(max [3h 1h 4h])", "4h");
    TEST_ASSERT_EQ("(min [3.0 1.0 4.0])", "1.0");
    TEST_ASSERT_EQ("(max [3.0 1.0 4.0])", "4.0");
    TEST_ASSERT_EQ("(min [0Nl 1 2])", "1");
    TEST_ASSERT_EQ("(max [0Nl 1 2])", "2");

    // count
    TEST_ASSERT_EQ("(count [1 2 3])", "3");
    TEST_ASSERT_EQ("(count [])", "0");
    TEST_ASSERT_EQ("(count [0Nl 1 2])", "3");

    // first / last
    TEST_ASSERT_EQ("(first [10 20 30])", "10");
    TEST_ASSERT_EQ("(last [10 20 30])", "30");
    TEST_ASSERT_EQ("(first [10i 20i])", "10i");
    TEST_ASSERT_EQ("(last [10i 20i])", "20i");
    TEST_ASSERT_EQ("(first [])", "0Nl");
    TEST_ASSERT_EQ("(last [])", "0Nl");

    // med (median)
    TEST_ASSERT_EQ("(med [1 2 3 4 5])", "3.0");
    TEST_ASSERT_EQ("(med [1 2 3 4])", "2.5");
    TEST_ASSERT_EQ("(med [5 1 3])", "3.0");

    // dev (standard deviation)
    TEST_ASSERT_EQ("(dev [2 4 4 4 5 5 7 9])", "2.0");

    PASS();
}

// ==================== CHAINED / NESTED ARITHMETIC ====================
test_result_t test_math_chained_ops() {
    // Nested arithmetic
    TEST_ASSERT_EQ("(+ 1 (+ 2 3))", "6");
    TEST_ASSERT_EQ("(* 2 (+ 3 4))", "14");
    TEST_ASSERT_EQ("(- (* 5 3) (+ 1 2))", "12");
    TEST_ASSERT_EQ("(/ (+ 10 20) (- 10 5))", "6");

    // Chained with vectors
    TEST_ASSERT_EQ("(+ [1 2 3] (+ [4 5 6] [7 8 9]))", "[12 15 18]");
    TEST_ASSERT_EQ("(* [2 3] (+ [1 1] [1 1]))", "[4 6]");

    // Nested with type promotions
    TEST_ASSERT_EQ("(+ 1i (+ 2 3.0))", "6.0");
    TEST_ASSERT_EQ("(* 2h (+ 3i 4))", "14");

    // Aggregate of arithmetic
    TEST_ASSERT_EQ("(sum (+ [1 2 3] [4 5 6]))", "21");
    TEST_ASSERT_EQ("(avg (* [2 4 6] 2))", "8.0");
    TEST_ASSERT_EQ("(max (- [10 20 30] 5))", "25");
    TEST_ASSERT_EQ("(min (+ [1 2 3] 10))", "11");

    // Arithmetic on function results
    TEST_ASSERT_EQ("(+ (sum [1 2 3]) (sum [4 5 6]))", "21");
    TEST_ASSERT_EQ("(- (max [1 2 3]) (min [1 2 3]))", "2");

    PASS();
}

// ==================== U8 (BYTE) ARITHMETIC ====================
test_result_t test_math_u8_arithmetic() {
    // u8 basic ops
    TEST_ASSERT_EQ("(+ 0x01 0x02)", "0x03");
    TEST_ASSERT_EQ("(- 0x05 0x02)", "0x03");
    TEST_ASSERT_EQ("(* 0x03 0x04)", "0x0c");
    TEST_ASSERT_EQ("(/ 0x0a 0x02)", "0x05");

    // u8 with zero
    TEST_ASSERT_EQ("(+ 0x00 0x00)", "0x00");
    TEST_ASSERT_EQ("(+ 0x05 0x00)", "0x05");
    TEST_ASSERT_EQ("(* 0x05 0x00)", "0x00");

    // u8 vector ops
    TEST_ASSERT_EQ("(+ [0x01 0x02 0x03] [0x04 0x05 0x06])", "[0x05 0x07 0x09]");
    TEST_ASSERT_EQ("(- [0x05 0x06 0x07] [0x01 0x02 0x03])", "[0x04 0x04 0x04]");
    TEST_ASSERT_EQ("(* [0x02 0x03] [0x04 0x05])", "[0x08 0x0f]");

    // u8 scalar-vector
    TEST_ASSERT_EQ("(+ 0x0a [0x01 0x02 0x03])", "[0x0b 0x0c 0x0d]");
    TEST_ASSERT_EQ("(* 0x02 [0x01 0x02 0x03])", "[0x02 0x04 0x06]");

    // u8 promotion to wider types
    TEST_ASSERT_EQ("(+ 0x05 10h)", "15h");
    TEST_ASSERT_EQ("(+ 0x05 10i)", "15i");
    TEST_ASSERT_EQ("(+ 0x05 10)", "15");
    TEST_ASSERT_EQ("(+ 0x05 10.0)", "15.0");

    PASS();
}

// ==================== TYPE ERROR CASES ====================
test_result_t test_math_type_errors() {
    // Arithmetic on incompatible types
    TEST_ASSERT_ER("(+ 0Nf 2024.03.20)", "type");
    TEST_ASSERT_ER("(* 02:15:07.000 02:15:07.000)", "type");
    TEST_ASSERT_ER("(- 2025.03.04D15:41:47.087221025 2025.12.13)", "type");
    TEST_ASSERT_ER("(+ null 1)", "type");
    TEST_ASSERT_ER("(* null 1)", "type");
    TEST_ASSERT_ER("(sum null)", "type");

    // Arithmetic on booleans
    TEST_ASSERT_EQ("(sum [true true false true])", "3");

    // Arithmetic on symbols should error
    TEST_ASSERT_ER("(+ 'abc 1)", "type");
    TEST_ASSERT_ER("(* 'abc 2)", "type");

    PASS();
}

// ==================== F64 SPECIAL VALUES ====================
test_result_t test_math_f64_special() {
    // Negative zero
    TEST_ASSERT_EQ("(+ -0.0 0.0)", "0.00");
    TEST_ASSERT_EQ("(- -0.0 0.0)", "0.00");
    TEST_ASSERT_EQ("(* -0.0 0.0)", "0.00");
    TEST_ASSERT_EQ("(- -0.00 0.00)", "0.00");

    // Very small values
    TEST_ASSERT_EQ("(+ 0.001 0.002)", "0.003");
    TEST_ASSERT_EQ("(* 0.1 0.1)", "0.01");

    // Division producing float (floor division)
    TEST_ASSERT_EQ("(/ 1.0 3.0)", "0.00");
    TEST_ASSERT_EQ("(/ 2.0 3.0)", "0.00");
    TEST_ASSERT_EQ("(/ 10.0 3.0)", "3.00");

    // Mixed operations with float
    TEST_ASSERT_EQ("(+ (* 3.0 4.0) 2.0)", "14.0");
    TEST_ASSERT_EQ("(- (/ 10.0 2.0) 1.0)", "4.0");

    // Float null
    TEST_ASSERT_EQ("(+ 0Nf 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(- 0Nf 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(* 0Nf 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0Nf)", "0Nf");

    // Float vector with null
    TEST_ASSERT_EQ("(+ [1.0 0Nf 3.0] 1.0)", "[2.0 0Nf 4.0]");
    TEST_ASSERT_EQ("(* [1.0 0Nf 3.0] 2.0)", "[2.0 0Nf 6.0]");

    PASS();
}

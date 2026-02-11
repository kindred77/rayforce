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

// ==================== DATE LITERAL & FORMATTING TESTS ====================
test_result_t test_temporal_date_literals() {
    // Basic date literals
    TEST_ASSERT_EQ("2000.01.01", "2000.01.01");
    TEST_ASSERT_EQ("2024.12.31", "2024.12.31");
    TEST_ASSERT_EQ("1999.06.15", "1999.06.15");
    TEST_ASSERT_EQ("2100.01.01", "2100.01.01");

    // Epoch date (day 0)
    TEST_ASSERT_EQ("(as 'date 0)", "2000.01.01");
    TEST_ASSERT_EQ("(as 'date 1)", "2000.01.02");
    TEST_ASSERT_EQ("(as 'date -1)", "1999.12.31");

    // Pre-epoch dates
    TEST_ASSERT_EQ("(as 'date -366)", "1998.12.31");
    TEST_ASSERT_EQ("1970.01.01", "1970.01.01");

    // Null date
    TEST_ASSERT_EQ("0Nd", "0Nd");
    TEST_ASSERT_EQ("(type 0Nd)", "'date");
    TEST_ASSERT_EQ("(nil? 0Nd)", "false");

    // Type checking
    TEST_ASSERT_EQ("(type 2024.01.01)", "'date");
    TEST_ASSERT_EQ("(type [2024.01.01 2024.01.02])", "'DATE");

    // Date vector literal
    TEST_ASSERT_EQ("[2024.01.01 2024.06.15 2024.12.31]", "[2024.01.01 2024.06.15 2024.12.31]");

    PASS();
}

// ==================== TIME LITERAL & FORMATTING TESTS ====================
test_result_t test_temporal_time_literals() {
    // Basic time literals
    TEST_ASSERT_EQ("00:00:00.000", "00:00:00.000");
    TEST_ASSERT_EQ("12:30:45.123", "12:30:45.123");
    TEST_ASSERT_EQ("23:59:59.999", "23:59:59.999");

    // Midnight and boundaries
    TEST_ASSERT_EQ("(as 'time 0)", "00:00:00.000");
    TEST_ASSERT_EQ("(as 'time 1)", "00:00:00.001");
    TEST_ASSERT_EQ("(as 'time 999)", "00:00:00.999");
    TEST_ASSERT_EQ("(as 'time 1000)", "00:00:01.000");
    TEST_ASSERT_EQ("(as 'time 60000)", "00:01:00.000");
    TEST_ASSERT_EQ("(as 'time 3600000)", "01:00:00.000");

    // Null time
    TEST_ASSERT_EQ("0Nt", "0Nt");
    TEST_ASSERT_EQ("(type 0Nt)", "'time");
    TEST_ASSERT_EQ("(nil? 0Nt)", "false");

    // Type checking
    TEST_ASSERT_EQ("(type 12:00:00.000)", "'time");
    TEST_ASSERT_EQ("(type [12:00:00.000 13:00:00.000])", "'TIME");

    // Fractional second parsing (1-3 digits)
    TEST_ASSERT_EQ("10:00:00.1", "10:00:00.001");
    TEST_ASSERT_EQ("10:00:00.12", "10:00:00.012");
    TEST_ASSERT_EQ("10:00:00.123", "10:00:00.123");

    // Time vector literal
    TEST_ASSERT_EQ("[10:00:00.000 12:30:00.000 15:45:00.000]", "[10:00:00.000 12:30:00.000 15:45:00.000]");

    PASS();
}

// ==================== TIMESTAMP LITERAL & FORMATTING TESTS ====================
test_result_t test_temporal_timestamp_literals() {
    // Basic timestamp literals
    TEST_ASSERT_EQ("2000.01.01D00:00:00.000000000", "2000.01.01D00:00:00.000000000");
    TEST_ASSERT_EQ("2024.06.15D12:30:45.123456789", "2024.06.15D12:30:45.123456789");

    // Epoch timestamp (ns 0)
    TEST_ASSERT_EQ("(as 'timestamp 0)", "2000.01.01D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp 1)", "2000.01.01D00:00:00.000000001");
    TEST_ASSERT_EQ("(as 'timestamp 1000000000)", "2000.01.01D00:00:01.000000000");

    // Null timestamp
    TEST_ASSERT_EQ("0Np", "0Np");
    TEST_ASSERT_EQ("(type 0Np)", "'timestamp");
    TEST_ASSERT_EQ("(nil? 0Np)", "true");

    // Type checking
    TEST_ASSERT_EQ("(type 2024.01.01D12:00:00.000000000)", "'timestamp");
    TEST_ASSERT_EQ("(type [2024.01.01D00:00:00.000000000 2024.01.02D00:00:00.000000000])", "'TIMESTAMP");

    // Nanosecond precision
    TEST_ASSERT_EQ("2024.01.01D00:00:00.000000001", "2024.01.01D00:00:00.000000001");
    TEST_ASSERT_EQ("2024.01.01D00:00:00.999999999", "2024.01.01D00:00:00.999999999");

    // ISO string parsing (already covered in lang.c, just verify edge cases)
    TEST_ASSERT_EQ("(as 'timestamp \"2000-01-01\")", "2000.01.01D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2000-01-01T00:00:00Z\")", "2000.01.01D00:00:00.000000000");

    PASS();
}

// ==================== DATE ARITHMETIC EDGE CASES ====================
test_result_t test_temporal_date_arithmetic() {
    // Month boundary crossings
    TEST_ASSERT_EQ("(+ 2024.01.31 1)", "2024.02.01");
    TEST_ASSERT_EQ("(+ 2024.02.28 1)", "2024.02.29");  // 2024 is leap year
    TEST_ASSERT_EQ("(+ 2024.02.29 1)", "2024.03.01");
    TEST_ASSERT_EQ("(+ 2023.02.28 1)", "2023.03.01");  // 2023 is not leap year
    TEST_ASSERT_EQ("(+ 2024.03.31 1)", "2024.04.01");
    TEST_ASSERT_EQ("(+ 2024.12.31 1)", "2025.01.01");  // Year boundary

    // Subtraction crossing boundaries
    TEST_ASSERT_EQ("(- 2024.03.01 1)", "2024.02.29");  // Leap year
    TEST_ASSERT_EQ("(- 2023.03.01 1)", "2023.02.28");  // Non-leap year
    TEST_ASSERT_EQ("(- 2025.01.01 1)", "2024.12.31");  // Year boundary

    // Date differences
    TEST_ASSERT_EQ("(- 2024.03.01 2024.02.01)", "29");  // Feb in leap year
    TEST_ASSERT_EQ("(- 2023.03.01 2023.02.01)", "28");  // Feb in non-leap year
    TEST_ASSERT_EQ("(- 2025.01.01 2024.01.01)", "366"); // Leap year has 366 days
    TEST_ASSERT_EQ("(- 2024.01.01 2023.01.01)", "365"); // Non-leap year

    // Large date offsets
    TEST_ASSERT_EQ("(+ 2000.01.01 365)", "2000.12.31"); // 2000 is leap year (366 days, day 365 = Dec 31)
    TEST_ASSERT_EQ("(+ 2000.01.01 366)", "2001.01.01");

    // Negative offsets before epoch
    TEST_ASSERT_EQ("(- 2000.01.01 1)", "1999.12.31");
    TEST_ASSERT_EQ("(- 2000.01.01 365)", "1999.01.01");

    // Leap year edge cases
    TEST_ASSERT_EQ("(+ 2000.02.28 1)", "2000.02.29");  // 2000 is leap (divisible by 400)
    TEST_ASSERT_EQ("(+ 2000.02.29 1)", "2000.03.01");
    TEST_ASSERT_EQ("(+ 2100.02.28 1)", "2100.03.01");  // 2100 is NOT leap (divisible by 100 but not 400)

    // Date comparison
    TEST_ASSERT_EQ("(< 2024.01.01 2024.01.02)", "true");
    TEST_ASSERT_EQ("(> 2024.12.31 2024.01.01)", "true");
    TEST_ASSERT_EQ("(== 2024.06.15 2024.06.15)", "true");
    TEST_ASSERT_EQ("(<= 2024.01.01 2024.01.01)", "true");
    TEST_ASSERT_EQ("(>= 2024.12.31 2024.06.15)", "true");

    // Vector date arithmetic
    TEST_ASSERT_EQ("(+ [2024.01.31 2024.02.28 2024.03.31] 1)", "[2024.02.01 2024.02.29 2024.04.01]");
    TEST_ASSERT_EQ("(- [2024.03.01 2024.01.01] [2024.02.01 2023.01.01])", "[29 365]");

    PASS();
}

// ==================== TIME ARITHMETIC EDGE CASES ====================
test_result_t test_temporal_time_arithmetic() {
    // Millisecond operations
    TEST_ASSERT_EQ("(+ 00:00:00.000 1)", "00:00:00.001");
    TEST_ASSERT_EQ("(+ 00:00:00.999 1)", "00:00:01.000");
    TEST_ASSERT_EQ("(+ 00:00:59.000 1000)", "00:01:00.000");
    TEST_ASSERT_EQ("(+ 00:59:00.000 60000)", "01:00:00.000");

    // Hour boundaries
    TEST_ASSERT_EQ("(+ 23:00:00.000 3600000)", "24:00:00.000");
    TEST_ASSERT_EQ("(+ 23:59:59.999 1)", "24:00:00.000");

    // Subtraction
    TEST_ASSERT_EQ("(- 01:00:00.000 1)", "00:59:59.999");
    TEST_ASSERT_EQ("(- 01:00:00.000 1000)", "00:59:59.000");
    TEST_ASSERT_EQ("(- 01:00:00.000 60000)", "00:59:00.000");
    TEST_ASSERT_EQ("(- 01:00:00.000 3600000)", "00:00:00.000");

    // Time differences
    TEST_ASSERT_EQ("(- 12:00:00.000 06:00:00.000)", "06:00:00.000");
    TEST_ASSERT_EQ("(- 23:59:59.999 00:00:00.000)", "23:59:59.999");
    TEST_ASSERT_EQ("(- 00:00:01.000 00:00:00.001)", "00:00:00.999");

    // Time comparisons
    TEST_ASSERT_EQ("(< 10:00:00.000 12:00:00.000)", "true");
    TEST_ASSERT_EQ("(> 23:59:59.999 00:00:00.000)", "true");
    TEST_ASSERT_EQ("(== 12:30:00.000 12:30:00.000)", "true");
    TEST_ASSERT_EQ("(!= 12:30:00.000 12:30:00.001)", "true");

    // Vector time arithmetic
    TEST_ASSERT_EQ("(+ [10:00:00.000 12:00:00.000] 1000)", "[10:00:01.000 12:00:01.000]");
    TEST_ASSERT_EQ("(- [10:00:01.000 12:00:01.000] [10:00:00.000 12:00:00.000])", "[00:00:01.000 00:00:01.000]");

    PASS();
}

// ==================== TIMESTAMP ARITHMETIC EDGE CASES ====================
test_result_t test_temporal_timestamp_arithmetic() {
    // Nanosecond operations
    TEST_ASSERT_EQ("(+ 2024.01.01D00:00:00.000000000 1)", "2024.01.01D00:00:00.000000001");
    TEST_ASSERT_EQ("(+ 2024.01.01D00:00:00.999999999 1)", "2024.01.01D00:00:01.000000000");

    // Second boundary
    TEST_ASSERT_EQ("(+ 2024.01.01D00:00:59.000000000 1000000000)", "2024.01.01D00:01:00.000000000");

    // Minute boundary
    TEST_ASSERT_EQ("(+ 2024.01.01D00:59:00.000000000 60000000000)", "2024.01.01D01:00:00.000000000");

    // Hour boundary
    TEST_ASSERT_EQ("(+ 2024.01.01D23:00:00.000000000 3600000000000)", "2024.01.02D00:00:00.000000000");

    // Day boundary crossing
    TEST_ASSERT_EQ("(+ 2024.01.01D23:59:59.999999999 1)", "2024.01.02D00:00:00.000000000");

    // Subtraction
    TEST_ASSERT_EQ("(- 2024.01.02D00:00:00.000000000 1)", "2024.01.01D23:59:59.999999999");

    // Timestamp differences
    TEST_ASSERT_EQ("(- 2024.01.01D00:00:01.000000000 2024.01.01D00:00:00.000000000)", "1000000000");
    TEST_ASSERT_EQ("(- 2024.01.02D00:00:00.000000000 2024.01.01D00:00:00.000000000)", "86400000000000");

    // Timestamp comparisons
    TEST_ASSERT_EQ("(< 2024.01.01D00:00:00.000000000 2024.01.01D00:00:00.000000001)", "true");
    TEST_ASSERT_EQ("(> 2024.12.31D23:59:59.999999999 2024.01.01D00:00:00.000000000)", "true");
    TEST_ASSERT_EQ("(== 2024.06.15D12:00:00.000000000 2024.06.15D12:00:00.000000000)", "true");

    // Vector timestamp arithmetic
    TEST_ASSERT_EQ("(+ [2024.01.01D00:00:00.000000000 2024.01.02D00:00:00.000000000] 1000000000)",
                   "[2024.01.01D00:00:01.000000000 2024.01.02D00:00:01.000000000]");

    PASS();
}

// ==================== TEMPORAL NULL PROPAGATION ====================
test_result_t test_temporal_null_propagation() {
    // Null date propagation
    TEST_ASSERT_EQ("(+ 0Nd 1)", "0Nd");
    TEST_ASSERT_EQ("(- 0Nd 1)", "0Nd");
    TEST_ASSERT_EQ("(- 0Nd 2024.01.01)", "0Ni");
    TEST_ASSERT_EQ("(- 2024.01.01 0Nd)", "0Ni");

    // Null time propagation
    TEST_ASSERT_EQ("(+ 0Nt 1000)", "0Nt");
    TEST_ASSERT_EQ("(- 0Nt 1000)", "0Nt");
    TEST_ASSERT_EQ("(- 0Nt 10:00:00.000)", "0Nt");
    TEST_ASSERT_EQ("(- 10:00:00.000 0Nt)", "0Nt");

    // Null timestamp propagation
    TEST_ASSERT_EQ("(+ 0Np 1000000000)", "0Np");
    TEST_ASSERT_EQ("(- 0Np 1000000000)", "0Np");

    // Null in vectors
    TEST_ASSERT_EQ("(+ [2024.01.01 0Nd 2024.01.03] 1)", "[2024.01.02 0Nd 2024.01.04]");
    TEST_ASSERT_EQ("(+ [10:00:00.000 0Nt 12:00:00.000] 1000)", "[10:00:01.000 0Nt 12:00:01.000]");

    // Null comparisons
    TEST_ASSERT_EQ("(== 0Nd 0Nd)", "true");
    TEST_ASSERT_EQ("(== 0Nt 0Nt)", "true");
    TEST_ASSERT_EQ("(== 0Np 0Np)", "true");

    PASS();
}

// ==================== TEMPORAL VECTOR SORTING ====================
test_result_t test_temporal_sorting() {
    // Date sorting
    TEST_ASSERT_EQ("(asc [2024.12.31 2024.01.01 2024.06.15])", "[2024.01.01 2024.06.15 2024.12.31]");
    TEST_ASSERT_EQ("(desc [2024.01.01 2024.12.31 2024.06.15])", "[2024.12.31 2024.06.15 2024.01.01]");

    // Time sorting
    TEST_ASSERT_EQ("(asc [23:00:00.000 01:00:00.000 12:00:00.000])", "[01:00:00.000 12:00:00.000 23:00:00.000]");
    TEST_ASSERT_EQ("(desc [01:00:00.000 23:00:00.000 12:00:00.000])", "[23:00:00.000 12:00:00.000 01:00:00.000]");

    // Timestamp sorting
    TEST_ASSERT_EQ("(asc [2024.12.31D23:59:59.000000000 2024.01.01D00:00:00.000000000 2024.06.15D12:00:00.000000000])",
                   "[2024.01.01D00:00:00.000000000 2024.06.15D12:00:00.000000000 2024.12.31D23:59:59.000000000]");

    // Min/max on temporal vectors
    TEST_ASSERT_EQ("(min [2024.06.15 2024.01.01 2024.12.31])", "2024.01.01");
    TEST_ASSERT_EQ("(max [2024.06.15 2024.01.01 2024.12.31])", "2024.12.31");
    TEST_ASSERT_EQ("(min [10:00:00.000 05:00:00.000 20:00:00.000])", "05:00:00.000");
    TEST_ASSERT_EQ("(max [10:00:00.000 05:00:00.000 20:00:00.000])", "20:00:00.000");

    // First/last on temporal vectors
    TEST_ASSERT_EQ("(first [2024.01.01 2024.06.15 2024.12.31])", "2024.01.01");
    TEST_ASSERT_EQ("(last [2024.01.01 2024.06.15 2024.12.31])", "2024.12.31");

    PASS();
}

// ==================== TEMPORAL FILTERING ====================
test_result_t test_temporal_filtering() {
    // Date filtering
    TEST_ASSERT_EQ("(filter [2024.01.01 2024.06.15 2024.12.31] [true false true])",
                   "[2024.01.01 2024.12.31]");
    TEST_ASSERT_EQ("(filter [2024.01.01 2024.06.15 2024.12.31] (> [2024.01.01 2024.06.15 2024.12.31] 2024.03.01))",
                   "[2024.06.15 2024.12.31]");

    // Time filtering
    TEST_ASSERT_EQ("(filter [08:00:00.000 12:00:00.000 18:00:00.000] (> [08:00:00.000 12:00:00.000 18:00:00.000] 10:00:00.000))",
                   "[12:00:00.000 18:00:00.000]");

    // Count on temporal vectors
    TEST_ASSERT_EQ("(count [2024.01.01 2024.06.15 2024.12.31])", "3");
    TEST_ASSERT_EQ("(count [10:00:00.000 12:00:00.000])", "2");

    PASS();
}

// ==================== TEMPORAL TYPE CONVERSIONS ====================
test_result_t test_temporal_conversions() {
    // Date to integer (days since epoch)
    TEST_ASSERT_EQ("(as 'i64 2000.01.01)", "0");
    TEST_ASSERT_EQ("(as 'i64 2000.01.02)", "1");
    TEST_ASSERT_EQ("(as 'i32 2000.01.01)", "0i");

    // Integer to date
    TEST_ASSERT_EQ("(as 'date 0)", "2000.01.01");
    TEST_ASSERT_EQ("(as 'date 365)", "2000.12.31");

    // Time to integer (milliseconds since midnight)
    TEST_ASSERT_EQ("(as 'i64 00:00:00.000)", "0");
    TEST_ASSERT_EQ("(as 'i64 00:00:01.000)", "1000");
    TEST_ASSERT_EQ("(as 'i64 01:00:00.000)", "3600000");

    // Integer to time
    TEST_ASSERT_EQ("(as 'time 0)", "00:00:00.000");
    TEST_ASSERT_EQ("(as 'time 3600000)", "01:00:00.000");

    // Timestamp to integer (nanoseconds since epoch)
    TEST_ASSERT_EQ("(as 'i64 2000.01.01D00:00:00.000000000)", "0");
    TEST_ASSERT_EQ("(as 'i64 2000.01.01D00:00:00.000000001)", "1");

    // Integer to timestamp
    TEST_ASSERT_EQ("(as 'timestamp 0)", "2000.01.01D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp 86400000000000)", "2000.01.02D00:00:00.000000000");

    // Date to/from string
    TEST_ASSERT_EQ("(as 'C8 2024.03.20)", "\"2024.03.20\"");
    TEST_ASSERT_EQ("(as 'date \"2024.03.20\")", "2024.03.20");

    // Time to/from string
    TEST_ASSERT_EQ("(as 'C8 12:30:45.123)", "\"12:30:45.123\"");
    TEST_ASSERT_EQ("(as 'time \"12:30:45.123\")", "12:30:45.123");
    TEST_ASSERT_EQ("(as 'time \"00:00:00.000\")", "00:00:00.000");

    // Timestamp to/from string
    TEST_ASSERT_EQ("(as 'timestamp \"2024.03.20D12:30:45.123456789\")", "2024.03.20D12:30:45.123456789");

    // Date to/from float
    TEST_ASSERT_EQ("(as 'f64 2000.01.01)", "0.00");
    TEST_ASSERT_EQ("(as 'f64 2000.01.02)", "1.00");
    TEST_ASSERT_EQ("(as 'date 0.0)", "2000.01.01");

    // Cross-temporal: date -> timestamp
    TEST_ASSERT_EQ("(type (as 'timestamp 2024.01.01))", "'timestamp");

    // Cross-temporal: timestamp -> date
    TEST_ASSERT_EQ("(type (as 'date 2024.01.01D12:30:00.000000000))", "'date");

    // Date/time vector casts
    TEST_ASSERT_EQ("(as 'DATE [0 1 2])", "[2000.01.01 2000.01.02 2000.01.03]");
    TEST_ASSERT_EQ("(as 'TIME [0 1000 2000])", "[00:00:00.000 00:00:01.000 00:00:02.000]");

    PASS();
}

// ==================== TEMPORAL EDGE CASES ====================
test_result_t test_temporal_edge_cases() {
    // Leap year Feb 29
    TEST_ASSERT_EQ("2024.02.29", "2024.02.29");
    TEST_ASSERT_EQ("2000.02.29", "2000.02.29");  // Divisible by 400 -> leap

    // Century non-leap
    TEST_ASSERT_EQ("(+ 2100.02.28 1)", "2100.03.01");  // 2100 not leap

    // Time at end of day
    TEST_ASSERT_EQ("23:59:59.999", "23:59:59.999");

    // Large integer date offsets (10 years)
    TEST_ASSERT_EQ("(+ 2000.01.01 3652)", "2009.12.31");  // ~10 years

    // Timestamp near midnight rollover
    TEST_ASSERT_EQ("(+ 2024.01.31D23:59:59.999999999 1)", "2024.02.01D00:00:00.000000000");

    // Month lengths
    TEST_ASSERT_EQ("(- 2024.02.01 2024.01.01)", "31");  // Jan = 31 days
    TEST_ASSERT_EQ("(- 2024.03.01 2024.02.01)", "29");  // Feb 2024 = 29 days (leap)
    TEST_ASSERT_EQ("(- 2023.03.01 2023.02.01)", "28");  // Feb 2023 = 28 days
    TEST_ASSERT_EQ("(- 2024.04.01 2024.03.01)", "31");  // Mar = 31 days
    TEST_ASSERT_EQ("(- 2024.05.01 2024.04.01)", "30");  // Apr = 30 days

    // Scalar on left of temporal vector arithmetic
    TEST_ASSERT_EQ("(- 2024.01.10 [2024.01.01 2024.01.05 2024.01.09])", "[9 5 1]");

    PASS();
}

// ==================== TEMPORAL IN TABLES ====================
test_result_t test_temporal_in_tables() {
    // Table with date column
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.02 2024.01.03] [10 20 30])))"
        "(count t)",
        "3");

    // Table with time column
    TEST_ASSERT_EQ(
        "(set t2 (table [tm val] (list [09:30:00.000 10:00:00.000 10:30:00.000] [100 200 300])))"
        "(at (at t2 'tm) 0)",
        "09:30:00.000");

    // Table with timestamp column
    TEST_ASSERT_EQ(
        "(set t3 (table [ts val] (list [2024.01.01D09:30:00.000000000 2024.01.01D10:00:00.000000000] [1 2])))"
        "(at (at t3 'ts) 1)",
        "2024.01.01D10:00:00.000000000");

    // Distinct on temporal
    TEST_ASSERT_EQ("(distinct [2024.01.01 2024.01.01 2024.01.02 2024.01.02 2024.01.03])",
                   "[2024.01.01 2024.01.02 2024.01.03]");

    PASS();
}

// ==================== TEMPORAL STRING PARSING EDGE CASES ====================
test_result_t test_temporal_string_parsing() {
    // ISO format with various timezone offsets
    TEST_ASSERT_EQ("(as 'timestamp \"2024-06-15T12:00:00+00:00\")", "2024.06.15D12:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2024-06-15T12:00:00-12:00\")", "2024.06.16D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2024-06-15T00:00:00+05:30\")", "2024.06.14D18:30:00.000000000");

    // ISO format with millisecond precision
    TEST_ASSERT_EQ("(as 'timestamp \"2024-01-01 00:00:00.000\")", "2024.01.01D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2024-01-01 00:00:00.001\")", "2024.01.01D00:00:00.001000000");

    // Rayforce native format roundtrip
    TEST_ASSERT_EQ("(as 'timestamp \"2024.06.15D12:30:45.123456789\")", "2024.06.15D12:30:45.123456789");

    // Time string parsing with missing millis
    TEST_ASSERT_EQ("(as 'time \"12:00:00\")", "12:00:00.000");
    TEST_ASSERT_EQ("(as 'time \"23:59:59\")", "23:59:59.000");
    TEST_ASSERT_EQ("(as 'time \"00:00:00.\")", "00:00:00.000");
    TEST_ASSERT_EQ("(as 'time \"00:00:00.0\")", "00:00:00.000");

    // Date string parsing
    TEST_ASSERT_EQ("(as 'date \"2024.01.01\")", "2024.01.01");
    TEST_ASSERT_EQ("(as 'date \"2000.01.01\")", "2000.01.01");

    PASS();
}

// ==================== CROSS-TYPE TEMPORAL COMPARISONS ====================
test_result_t test_temporal_cross_type() {
    // Date equality via integer roundtrip
    TEST_ASSERT_EQ("(== (as 'i64 2024.01.01) (as 'i64 2024.01.01))", "true");
    TEST_ASSERT_EQ("(!= (as 'i64 2024.01.01) (as 'i64 2024.01.02))", "true");

    // Time equality via integer roundtrip
    TEST_ASSERT_EQ("(== (as 'i64 12:00:00.000) (as 'i64 12:00:00.000))", "true");
    TEST_ASSERT_EQ("(== (as 'i64 12:00:00.000) 43200000)", "true");  // 12h = 43200000ms

    // Timestamp equality via integer roundtrip
    TEST_ASSERT_EQ("(== (as 'i64 2000.01.01D00:00:01.000000000) 1000000000)", "true");

    // Temporal to symbol
    TEST_ASSERT_EQ("(type (as 'symbol 2024.01.01))", "'symbol");
    TEST_ASSERT_EQ("(type (as 'symbol 12:00:00.000))", "'symbol");
    TEST_ASSERT_EQ("(type (as 'symbol 2024.01.01D12:00:00.000000000))", "'symbol");

    // Temporal to boolean
    TEST_ASSERT_EQ("(as 'b8 2000.01.01)", "false");  // Epoch = 0 = false
    TEST_ASSERT_EQ("(as 'b8 2000.01.02)", "true");   // Day 1 = true
    TEST_ASSERT_EQ("(as 'b8 00:00:00.000)", "false"); // Midnight = 0 = false
    TEST_ASSERT_EQ("(as 'b8 00:00:00.001)", "true");  // 1ms = true

    PASS();
}

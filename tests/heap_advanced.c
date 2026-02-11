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

// ==================== SLAB-SIZED ALLOCATIONS ====================
test_result_t test_heap_slab_sizes() {
    // Slab cache orders 5-9: 32, 64, 128, 256, 512 bytes
    nil_t *p32 = heap_alloc(32);
    TEST_ASSERT(p32 != NULL, "32-byte alloc failed");

    nil_t *p64 = heap_alloc(64);
    TEST_ASSERT(p64 != NULL, "64-byte alloc failed");

    nil_t *p128 = heap_alloc(128);
    TEST_ASSERT(p128 != NULL, "128-byte alloc failed");

    nil_t *p256 = heap_alloc(256);
    TEST_ASSERT(p256 != NULL, "256-byte alloc failed");

    nil_t *p512 = heap_alloc(512);
    TEST_ASSERT(p512 != NULL, "512-byte alloc failed");

    // All pointers should be distinct
    TEST_ASSERT(p32 != p64, "32 and 64 should differ");
    TEST_ASSERT(p64 != p128, "64 and 128 should differ");
    TEST_ASSERT(p128 != p256, "128 and 256 should differ");
    TEST_ASSERT(p256 != p512, "256 and 512 should differ");

    // Write pattern to verify no overlap
    memset(p32, 0xAA, 32);
    memset(p64, 0xBB, 64);
    memset(p128, 0xCC, 128);
    memset(p256, 0xDD, 256);
    memset(p512, 0xEE, 512);

    // Verify patterns intact
    TEST_ASSERT(*((u8_t *)p32) == 0xAA, "32-byte pattern corrupted");
    TEST_ASSERT(*((u8_t *)p64) == 0xBB, "64-byte pattern corrupted");
    TEST_ASSERT(*((u8_t *)p128) == 0xCC, "128-byte pattern corrupted");
    TEST_ASSERT(*((u8_t *)p256) == 0xDD, "256-byte pattern corrupted");
    TEST_ASSERT(*((u8_t *)p512) == 0xEE, "512-byte pattern corrupted");

    heap_free(p32);
    heap_free(p64);
    heap_free(p128);
    heap_free(p256);
    heap_free(p512);

    PASS();
}

// ==================== BOUNDARY ALLOCATIONS ====================
test_result_t test_heap_boundary_sizes() {
    // Just below slab boundaries
    nil_t *p31 = heap_alloc(31);
    TEST_ASSERT(p31 != NULL, "31-byte alloc failed");

    nil_t *p63 = heap_alloc(63);
    TEST_ASSERT(p63 != NULL, "63-byte alloc failed");

    nil_t *p127 = heap_alloc(127);
    TEST_ASSERT(p127 != NULL, "127-byte alloc failed");

    nil_t *p255 = heap_alloc(255);
    TEST_ASSERT(p255 != NULL, "255-byte alloc failed");

    nil_t *p511 = heap_alloc(511);
    TEST_ASSERT(p511 != NULL, "511-byte alloc failed");

    // Just above slab boundaries
    nil_t *p33 = heap_alloc(33);
    TEST_ASSERT(p33 != NULL, "33-byte alloc failed");

    nil_t *p65 = heap_alloc(65);
    TEST_ASSERT(p65 != NULL, "65-byte alloc failed");

    nil_t *p129 = heap_alloc(129);
    TEST_ASSERT(p129 != NULL, "129-byte alloc failed");

    nil_t *p257 = heap_alloc(257);
    TEST_ASSERT(p257 != NULL, "257-byte alloc failed");

    nil_t *p513 = heap_alloc(513);
    TEST_ASSERT(p513 != NULL, "513-byte alloc failed");

    // Write and verify
    memset(p31, 0x11, 31);
    memset(p33, 0x22, 33);
    TEST_ASSERT(*((u8_t *)p31) == 0x11, "31-byte pattern corrupted");
    TEST_ASSERT(*((u8_t *)p33) == 0x22, "33-byte pattern corrupted");

    heap_free(p31);
    heap_free(p63);
    heap_free(p127);
    heap_free(p255);
    heap_free(p511);
    heap_free(p33);
    heap_free(p65);
    heap_free(p129);
    heap_free(p257);
    heap_free(p513);

    PASS();
}

// ==================== RAPID ALLOC/FREE CYCLES ====================
test_result_t test_heap_rapid_cycles() {
    i64_t i;
    // Rapid alloc/free of same size to stress slab cache
    for (i = 0; i < 10000; i++) {
        nil_t *p = heap_alloc(64);
        TEST_ASSERT(p != NULL, "rapid 64-byte alloc failed");
        memset(p, 0xFF, 64);
        heap_free(p);
    }

    // Rapid alloc/free of slab boundary size
    for (i = 0; i < 10000; i++) {
        nil_t *p = heap_alloc(32);
        TEST_ASSERT(p != NULL, "rapid 32-byte alloc failed");
        heap_free(p);
    }

    // Rapid alloc/free of large slab size
    for (i = 0; i < 5000; i++) {
        nil_t *p = heap_alloc(512);
        TEST_ASSERT(p != NULL, "rapid 512-byte alloc failed");
        heap_free(p);
    }

    // Rapid alloc/free above slab range
    for (i = 0; i < 1000; i++) {
        nil_t *p = heap_alloc(1024);
        TEST_ASSERT(p != NULL, "rapid 1024-byte alloc failed");
        heap_free(p);
    }

    PASS();
}

// ==================== REALLOC CROSSING SIZE BOUNDARIES ====================
test_result_t test_heap_realloc_boundaries() {
    // Realloc from slab to slab (32 -> 128)
    nil_t *p = heap_alloc(32);
    TEST_ASSERT(p != NULL, "initial 32-byte alloc failed");
    memset(p, 0xAA, 32);

    nil_t *p2 = heap_realloc(p, 128);
    TEST_ASSERT(p2 != NULL, "realloc 32->128 failed");
    // First 32 bytes should be preserved
    TEST_ASSERT(*((u8_t *)p2) == 0xAA, "data not preserved after realloc 32->128");

    // Realloc from slab to above slab (128 -> 1024)
    nil_t *p3 = heap_realloc(p2, 1024);
    TEST_ASSERT(p3 != NULL, "realloc 128->1024 failed");
    TEST_ASSERT(*((u8_t *)p3) == 0xAA, "data not preserved after realloc 128->1024");

    // Realloc back down (1024 -> 64)
    nil_t *p4 = heap_realloc(p3, 64);
    TEST_ASSERT(p4 != NULL, "realloc 1024->64 failed");
    TEST_ASSERT(*((u8_t *)p4) == 0xAA, "data not preserved after realloc 1024->64");

    // Realloc from small to very large
    nil_t *p5 = heap_realloc(p4, 4096);
    TEST_ASSERT(p5 != NULL, "realloc 64->4096 failed");
    TEST_ASSERT(*((u8_t *)p5) == 0xAA, "data not preserved after realloc 64->4096");

    // Realloc to slightly larger (within same slab order)
    nil_t *p6 = heap_alloc(30);
    TEST_ASSERT(p6 != NULL, "30-byte alloc failed");
    memset(p6, 0xBB, 30);
    nil_t *p7 = heap_realloc(p6, 31);
    TEST_ASSERT(p7 != NULL, "realloc 30->31 failed");
    TEST_ASSERT(*((u8_t *)p7) == 0xBB, "data not preserved after realloc 30->31");

    heap_free(p5);
    heap_free(p7);

    PASS();
}

// ==================== ZERO-SIZE ALLOCATION ====================
test_result_t test_heap_zero_size() {
    nil_t *p = heap_alloc(0);
    TEST_ASSERT(p == NULL, "zero-size alloc should return NULL");

    // Size 1 should work
    nil_t *p1 = heap_alloc(1);
    TEST_ASSERT(p1 != NULL, "1-byte alloc should succeed");
    *((u8_t *)p1) = 42;
    TEST_ASSERT(*((u8_t *)p1) == 42, "1-byte write/read failed");
    heap_free(p1);

    // Size 2 should work
    nil_t *p2 = heap_alloc(2);
    TEST_ASSERT(p2 != NULL, "2-byte alloc should succeed");
    heap_free(p2);

    PASS();
}

// ==================== ALIGNMENT VERIFICATION ====================
test_result_t test_heap_alignment() {
    i64_t i;
    i64_t sizes[] = {1, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65,
                     127, 128, 255, 256, 511, 512, 1023, 1024, 2048, 4096};
    i64_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (i = 0; i < num_sizes; i++) {
        nil_t *p = heap_alloc(sizes[i]);
        if (sizes[i] == 0) {
            TEST_ASSERT(p == NULL, "zero alloc not NULL");
        } else {
            TEST_ASSERT(p != NULL, "alloc failed for alignment test");
            // Check 8-byte alignment (minimum expected)
            TEST_ASSERT(((uintptr_t)p % 8) == 0, "pointer not 8-byte aligned");
            heap_free(p);
        }
    }

    PASS();
}

// ==================== OBJECT ALLOCATION PATTERNS ====================
test_result_t test_heap_obj_patterns() {
    // I64 object
    obj_p v1 = I64(42);
    TEST_ASSERT(v1 != NULL, "I64 alloc failed");
    drop_obj(v1);

    // Multiple I64 objects
    obj_p v2 = I64(100);
    obj_p v3 = I64(200);
    obj_p v4 = I64(300);
    TEST_ASSERT(v2 != NULL, "I64 v2 alloc failed");
    TEST_ASSERT(v3 != NULL, "I64 v3 alloc failed");
    TEST_ASSERT(v4 != NULL, "I64 v4 alloc failed");
    drop_obj(v2);
    drop_obj(v3);
    drop_obj(v4);

    // LIST creation and push
    obj_p lst = LIST(0);
    TEST_ASSERT(lst != NULL, "LIST alloc failed");
    push_obj(&lst, i64(1));
    push_obj(&lst, i64(2));
    push_obj(&lst, i64(3));
    drop_obj(lst);

    // Larger list
    obj_p big_lst = LIST(0);
    i64_t i;
    for (i = 0; i < 100; i++) {
        push_obj(&big_lst, i64(i));
    }
    drop_obj(big_lst);

    // F64 object
    obj_p f = f64(3.14);
    TEST_ASSERT(f != NULL, "f64 alloc failed");
    drop_obj(f);

    PASS();
}

// ==================== STRESS TEST: MIXED SIZES ====================
test_result_t test_heap_stress_mixed() {
    i64_t i, j, n = 5000;
    nil_t *ptrs[5000];
    i64_t sizes[5000];

    // Allocate mixed sizes
    for (i = 0; i < n; i++) {
        // Mix of slab-sized and non-slab-sized allocations
        switch (i % 7) {
        case 0: sizes[i] = 32; break;
        case 1: sizes[i] = 64; break;
        case 2: sizes[i] = 128; break;
        case 3: sizes[i] = 256; break;
        case 4: sizes[i] = 512; break;
        case 5: sizes[i] = 1024; break;
        case 6: sizes[i] = 47; break;  // non-power-of-two
        }
        ptrs[i] = heap_alloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "stress alloc failed");
        memset(ptrs[i], (u8_t)(i & 0xFF), sizes[i]);
    }

    // Verify patterns
    for (i = 0; i < n; i++) {
        TEST_ASSERT(*((u8_t *)ptrs[i]) == (u8_t)(i & 0xFF), "stress pattern check failed");
    }

    // Free in random order (Fisher-Yates shuffle of indices)
    i64_t indices[5000];
    for (i = 0; i < n; i++)
        indices[i] = i;
    for (i = n - 1; i > 0; i--) {
        j = rand() % (i + 1);
        i64_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }
    for (i = 0; i < n; i++) {
        heap_free(ptrs[indices[i]]);
    }

    PASS();
}

// ==================== ALTERNATING ALLOC/FREE ====================
test_result_t test_heap_alternating() {
    i64_t i;
    // Alternating alloc/free of different sizes
    nil_t *prev = NULL;
    for (i = 0; i < 1000; i++) {
        i64_t size = 32 + (i % 10) * 32;  // 32, 64, ..., 320, 32, 64, ...
        nil_t *p = heap_alloc(size);
        TEST_ASSERT(p != NULL, "alternating alloc failed");
        memset(p, 0xFF, size);

        if (prev != NULL)
            heap_free(prev);
        prev = p;
    }
    if (prev != NULL)
        heap_free(prev);

    // Batch alloc, partial free, more alloc, full free
    nil_t *batch1[100];
    nil_t *batch2[100];

    for (i = 0; i < 100; i++) {
        batch1[i] = heap_alloc(128);
        TEST_ASSERT(batch1[i] != NULL, "batch1 alloc failed");
    }

    // Free even indices
    for (i = 0; i < 100; i += 2) {
        heap_free(batch1[i]);
        batch1[i] = NULL;
    }

    // Allocate second batch (may reuse freed slots)
    for (i = 0; i < 100; i++) {
        batch2[i] = heap_alloc(128);
        TEST_ASSERT(batch2[i] != NULL, "batch2 alloc failed");
    }

    // Free everything
    for (i = 0; i < 100; i++) {
        if (batch1[i] != NULL)
            heap_free(batch1[i]);
        heap_free(batch2[i]);
    }

    PASS();
}

// ==================== OBJECT LIFECYCLE STRESS ====================
test_result_t test_heap_obj_lifecycle() {
    i64_t i;

    // Create and destroy many I64 objects
    for (i = 0; i < 1000; i++) {
        obj_p v = I64(i);
        TEST_ASSERT(v != NULL, "I64 lifecycle alloc failed");
        drop_obj(v);
    }

    // Create lists of increasing size
    for (i = 1; i <= 50; i++) {
        obj_p lst = LIST(0);
        i64_t j;
        for (j = 0; j < i; j++) {
            push_obj(&lst, i64(j));
        }
        drop_obj(lst);
    }

    // Interleaved I64 and list creation
    obj_p vals[100];
    for (i = 0; i < 100; i++) {
        if (i % 2 == 0) {
            vals[i] = I64(i);
        } else {
            vals[i] = LIST(0);
            push_obj(&vals[i], i64(i));
        }
        TEST_ASSERT(vals[i] != NULL, "interleaved alloc failed");
    }

    // Drop all in reverse order
    for (i = 99; i >= 0; i--) {
        drop_obj(vals[i]);
    }

    PASS();
}

// ==================== LARGE ALLOCATION ====================
test_result_t test_heap_large_alloc() {
    // Allocate progressively larger blocks
    nil_t *p1 = heap_alloc(1024);
    TEST_ASSERT(p1 != NULL, "1KB alloc failed");
    memset(p1, 0xAA, 1024);

    nil_t *p2 = heap_alloc(4096);
    TEST_ASSERT(p2 != NULL, "4KB alloc failed");
    memset(p2, 0xBB, 4096);

    nil_t *p3 = heap_alloc(65536);
    TEST_ASSERT(p3 != NULL, "64KB alloc failed");
    memset(p3, 0xCC, 65536);

    nil_t *p4 = heap_alloc(1048576);
    TEST_ASSERT(p4 != NULL, "1MB alloc failed");
    memset(p4, 0xDD, 1048576);

    // Verify patterns
    TEST_ASSERT(*((u8_t *)p1) == 0xAA, "1KB pattern corrupted");
    TEST_ASSERT(*((u8_t *)p2) == 0xBB, "4KB pattern corrupted");
    TEST_ASSERT(*((u8_t *)p3) == 0xCC, "64KB pattern corrupted");
    TEST_ASSERT(*((u8_t *)p4) == 0xDD, "1MB pattern corrupted");

    // Verify last byte of each
    TEST_ASSERT(*((u8_t *)p1 + 1023) == 0xAA, "1KB last byte corrupted");
    TEST_ASSERT(*((u8_t *)p2 + 4095) == 0xBB, "4KB last byte corrupted");
    TEST_ASSERT(*((u8_t *)p3 + 65535) == 0xCC, "64KB last byte corrupted");
    TEST_ASSERT(*((u8_t *)p4 + 1048575) == 0xDD, "1MB last byte corrupted");

    heap_free(p1);
    heap_free(p2);
    heap_free(p3);
    heap_free(p4);

    // Very large alloc should fail gracefully
    nil_t *p5 = heap_alloc(1ull << 39);
    TEST_ASSERT(p5 == NULL, "absurdly large alloc should return NULL");

    PASS();
}

// ==================== REALLOC DATA PRESERVATION ====================
test_result_t test_heap_realloc_data() {
    // Allocate and fill with sequential bytes
    i64_t i;
    i64_t init_size = 64;
    u8_t *p = (u8_t *)heap_alloc(init_size);
    TEST_ASSERT(p != NULL, "initial alloc failed");

    for (i = 0; i < init_size; i++)
        p[i] = (u8_t)(i & 0xFF);

    // Realloc to larger
    i64_t new_size = 256;
    u8_t *p2 = (u8_t *)heap_realloc(p, new_size);
    TEST_ASSERT(p2 != NULL, "realloc larger failed");

    // Verify original data preserved
    for (i = 0; i < init_size; i++) {
        TEST_ASSERT(p2[i] == (u8_t)(i & 0xFF), "data corrupted after realloc larger");
    }

    // Fill the extended region
    for (i = init_size; i < new_size; i++)
        p2[i] = (u8_t)((i + 0x80) & 0xFF);

    // Realloc to smaller
    i64_t small_size = 32;
    u8_t *p3 = (u8_t *)heap_realloc(p2, small_size);
    TEST_ASSERT(p3 != NULL, "realloc smaller failed");

    // Verify data preserved up to new size
    for (i = 0; i < small_size; i++) {
        TEST_ASSERT(p3[i] == (u8_t)(i & 0xFF), "data corrupted after realloc smaller");
    }

    // Realloc same size returns same pointer
    u8_t *p4 = (u8_t *)heap_realloc(p3, small_size);
    TEST_ASSERT(p4 != NULL, "realloc same size failed");
    TEST_ASSERT(p4 == p3, "realloc same size should return same ptr");

    heap_free(p4);

    PASS();
}

// ==================== POWER-OF-TWO ALLOCATION SWEEP ====================
test_result_t test_heap_power_of_two() {
    i64_t i;
    // Allocate every power of 2 from 1 to 2^16
    nil_t *ptrs[17];  // 2^0 through 2^16
    for (i = 0; i <= 16; i++) {
        i64_t size = 1ull << i;
        ptrs[i] = heap_alloc(size);
        TEST_ASSERT(ptrs[i] != NULL, "power-of-two alloc failed");
        memset(ptrs[i], (u8_t)i, size);
    }

    // Verify patterns
    for (i = 0; i <= 16; i++) {
        TEST_ASSERT(*((u8_t *)ptrs[i]) == (u8_t)i, "power-of-two pattern check failed");
    }

    // Free in forward order
    for (i = 0; i <= 16; i++) {
        heap_free(ptrs[i]);
    }

    // Re-allocate in reverse order to stress allocator
    for (i = 16; i >= 0; i--) {
        i64_t size = 1ull << i;
        ptrs[i] = heap_alloc(size);
        TEST_ASSERT(ptrs[i] != NULL, "reverse power-of-two alloc failed");
    }

    // Free all
    for (i = 0; i <= 16; i++) {
        heap_free(ptrs[i]);
    }

    PASS();
}

/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

/*
 * test_agg_engine.c — unit tests for the v2 aggregation engine admission
 * gate (agg_v2_can_handle).  These build group DAG nodes via the C API
 * (mirroring test_group_extra.c) and assert the gate verdict directly,
 * without executing the node.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "ops/agg_engine.h"
#include "table/sym.h"
#include <string.h>

/* Build a single-column table with the given type, name, n rows = 8. */
static ray_t* make_typed_table(int8_t type, const char* col, int64_t n) {
    ray_t* vec = ray_vec_new(type, n);
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    vec->len = n;
    int64_t name = ray_sym_intern(col, (int32_t)strlen(col));
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);
    return tbl;
}

/* Build a 2-column table: key column "k" of ktype + value column "v" of vtype. */
static ray_t* make_kv_table(int8_t ktype, int8_t vtype, int64_t n) {
    ray_t* kvec = ray_vec_new(ktype, n);
    ray_t* vvec = ray_vec_new(vtype, n);
    if (!kvec || RAY_IS_ERR(kvec) || !vvec || RAY_IS_ERR(vvec)) return NULL;
    kvec->len = n;
    vvec->len = n;
    int64_t kname = ray_sym_intern("k", 1);
    int64_t vname = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, kname, kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, vname, vvec); ray_release(vvec);
    return tbl;
}

/* Case 1: single I64 key + one OP_SUM over an I64 column → admit. */
static test_result_t test_gate_admits_i64_key_sum_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_kv_table(RAY_I64, RAY_I64, 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    ray_op_t* scan_v = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 2: two keys → defer. */
static test_result_t test_gate_defers_two_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a 3-col table: k1, k2 (I64), v (I64). */
    ray_t* k1 = ray_vec_new(RAY_I64, 8); k1->len = 8;
    ray_t* k2 = ray_vec_new(RAY_I64, 8); k2->len = 8;
    ray_t* vv = ray_vec_new(RAY_I64, 8); vv->len = 8;
    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, s_k2, k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, s_v,  vv); ray_release(vv);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k1, scan_k2 };
    ray_op_t* grp = ray_group(g, keys, 2, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_FALSE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 3: single I64 key + OP_SUM over an I32 column (not registered) → defer. */
static test_result_t test_gate_defers_sum_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_kv_table(RAY_I64, RAY_I32, 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    ray_op_t* scan_v = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_FALSE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 4: single I64 key + OP_COUNT → admit. */
static test_result_t test_gate_admits_count(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_typed_table(RAY_I64, "k", 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    uint16_t  ops[]  = { OP_COUNT };
    ray_op_t* ins[]  = { scan_k };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t agg_engine_entries[] = {
    { "gate_admits_i64_key_sum_i64", test_gate_admits_i64_key_sum_i64, NULL, NULL },
    { "gate_defers_two_keys",        test_gate_defers_two_keys,        NULL, NULL },
    { "gate_defers_sum_i32",         test_gate_defers_sum_i32,         NULL, NULL },
    { "gate_admits_count",           test_gate_admits_count,           NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

/* test/test_expr_null.c — null-aware fused expression tests */
#include "test.h"
#include <rayforce.h>
#include "ops/ops.h"
#include "ops/internal.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <math.h>

/* Build an I64 column with nulls at given indices. */
static ray_t* vec_i64_with_nulls(const int64_t* vals, int64_t n,
                                 const int64_t* null_idx, int64_t n_nulls) {
    ray_t* v = ray_vec_from_raw(RAY_I64, (void*)vals, n);
    for (int64_t i = 0; i < n_nulls; i++)
        ray_vec_set_null(v, null_idx[i], true);
    return v;
}

static test_result_t test_expr_bail_counter_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    int64_t nidx[] = {2, 5};
    ray_t* col = vec_i64_with_nulls(vals, 8, nidx, 2);
    TEST_ASSERT(col->attrs & RAY_ATTR_HAS_NULLS, "attr set");

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    uint64_t before = ray_expr_bail_counts[EXPR_BAIL_NULLS];
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* e = ray_add(g, x, ray_const_i64(g, 1));
    ray_t* r = ray_execute(g, e);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Pre-null-support: nullable column must bail to fallback. */
    TEST_ASSERT(ray_expr_bail_counts[EXPR_BAIL_NULLS] > before,
                "nullable scan counted as EXPR_BAIL_NULLS");

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

const test_entry_t expr_null_entries[] = {
    { "expr_null/bail_counter", test_expr_bail_counter_nulls, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

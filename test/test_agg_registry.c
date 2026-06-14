/* test/test_agg_registry.c — differential: vtable accumulator ≡ current ray_*_fn.
 * Phase 0 oracle = the existing reduction functions. */
#include "test.h"
#include <rayforce.h>
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "lang/eval.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <stdlib.h>

static ray_t* vec_i64(const int64_t* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    memcpy(ray_data(v), xs, (size_t)n * sizeof(int64_t));
    return v;
}

/* Run a vtable accumulator over the whole vector as a single group (gid 0). */
static ray_t* run_single_group(const agg_vtable_t* vt, ray_t* col) {
    void* state = calloc(1, vt->state_size);
    vt->init(state);
    uint32_t* gids = calloc((size_t)col->len, sizeof(uint32_t));  /* all zero */
    ray_valid_t valid = { ray_data(col), col->type,
                          (col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    vt->update_batch(state, vt->state_size, gids, ray_data(col), &valid, col->len, NULL);
    ray_t* out = vt->finalize(state, NULL);
    free(gids); free(state);
    return out;
}

static test_result_t test_sum_i64_matches_reduction(void) {
    /* ray_sum_fn(RAY_I64) reduces via the parallel morsel/DAG executor,
     * which needs the heap + runtime up — mirror the other exec tests. */
    ray_heap_init();
    (void)ray_sym_init();

    int64_t xs[] = { 10, 20, 30, -5, 7 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);
    const agg_vtable_t* vt = agg_resolve(OP_SUM, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);
    ray_t* got = run_single_group(vt, col);
    /* ray_sum_fn on an I64 vector returns a lazy DAG wrapper — force it to a
     * concrete scalar atom before comparing. */
    ray_t* want = ray_lazy_materialize(ray_sum_fn(col));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_FALSE(RAY_IS_ERR(want));
    TEST_ASSERT_EQ_I(got->i64, want->i64);
    ray_release(got); ray_release(want); ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_minmax_count_i64_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t xs[] = { 10, 20, 30, -5, 7 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);
    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_MIN, ray_min_fn }, { OP_MAX, ray_max_fn },
    };
    for (size_t c = 0; c < 2; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        /* ray_min_fn/ray_max_fn on an I64 vector return a lazy DAG wrapper —
         * force to a concrete scalar atom before comparing. */
        ray_t* want = ray_lazy_materialize(cases[c].ref(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_I(got->i64, want->i64);
        ray_release(got); ray_release(want);
    }
    const agg_vtable_t* vt = agg_resolve(OP_COUNT, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);
    ray_t* got = run_single_group(vt, col);
    TEST_ASSERT_EQ_I(got->i64, 5);     /* 5 live rows, no nulls */
    ray_release(got); ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static ray_t* vec_f64(const double* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_F64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n; memcpy(ray_data(v), xs, (size_t)n * sizeof(double));
    return v;
}

static test_result_t test_f64_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double xs[] = { 1.5, 2.0, -3.25, 10.0 };
    ray_t* col = vec_f64(xs, 4);
    TEST_ASSERT_NOT_NULL(col);
    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_SUM, ray_sum_fn }, { OP_MIN, ray_min_fn },
        { OP_MAX, ray_max_fn }, { OP_AVG, ray_avg_fn },
    };
    for (size_t c = 0; c < 4; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(cases[c].ref(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-12);
        ray_release(got); ray_release(want);
    }
    ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t agg_registry_entries[] = {
    { "agg_registry/sum_i64_matches_reduction", test_sum_i64_matches_reduction, NULL, NULL },
    { "agg_registry/minmax_count_i64_match", test_minmax_count_i64_match, NULL, NULL },
    { "agg_registry/f64_match", test_f64_match, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

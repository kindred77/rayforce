/* Multi-thread allocator stress.  Each worker owns its heap and only frees
 * its OWN blocks (no cross-thread reference), so destroy is race-free.  Runs
 * under the global parallel flag to exercise the same-pool coalescing path in
 * heap_coalesce.  TSan/ASan target for the parallel-coalescing change. */
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include <pthread.h>
#include <stdatomic.h>

extern void ray_parallel_begin(void);
extern void ray_parallel_end(void);
extern RAY_TLS bool ray_rc_sync;

#define HP_WORKERS 4
#define HP_ROUNDS  4000
#define HP_LIVE    64

static void* hp_worker(void* _) {
    (void)_;
    ray_heap_init();
    ray_rc_sync = true;
    ray_t* live[HP_LIVE] = {0};
    for (long r = 0; r < HP_ROUNDS; r++) {
        for (int i = 0; i < HP_LIVE; i++) {
            /* mix slab-range (96B) and morsel-range (8KB) sizes */
            size_t sz = (i & 1) ? 8192 : 96;
            if (live[i]) ray_free(live[i]);
            live[i] = ray_alloc(sz);
        }
    }
    for (int i = 0; i < HP_LIVE; i++)
        if (live[i]) ray_free(live[i]);
    ray_heap_destroy();
    return NULL;
}

/* N workers churn their own blocks concurrently under the parallel flag. */
static test_result_t test_parallel_concurrent_churn(void) {
    ray_parallel_begin();
    pthread_t th[HP_WORKERS];
    for (int i = 0; i < HP_WORKERS; i++)
        pthread_create(&th[i], NULL, hp_worker, NULL);
    for (int i = 0; i < HP_WORKERS; i++)
        pthread_join(th[i], NULL);
    ray_parallel_end();
    PASS();   /* success = no crash, no sanitizer report */
}

/* Single thread: alloc then free a batch of small blocks while the parallel
 * flag is set; the frees take the same-pool coalescing path.  parallel_end
 * runs GC.  No crash = pass. */
static test_result_t test_parallel_same_pool_coalesce(void) {
    ray_parallel_begin();
    ray_t* b[128];
    for (int i = 0; i < 128; i++) b[i] = ray_alloc(96);
    for (int i = 0; i < 128; i++) ray_free(b[i]);
    ray_parallel_end();
    PASS();
}

const test_entry_t heap_parallel_entries[] = {
    { "heap_parallel/concurrent_churn",   test_parallel_concurrent_churn,  NULL, NULL },
    { "heap_parallel/same_pool_coalesce", test_parallel_same_pool_coalesce, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

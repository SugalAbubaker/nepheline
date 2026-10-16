// SPDX-License-Identifier: Apache-2.0
// Tracked allocators: counts through alloc, realloc, and free, peaks,
// failures, concurrency, and the profiler's registry.
#include <nepheline/core/arena.h>

#include "test_profile_support.h"

NTEST(memory, counts_follow_allocations)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    NphTrackedAllocator t;
    nph_tracked_allocator_init(&t, nph_debug_allocator_allocator(&da), "test");
    NphAllocator *a = nph_tracked_allocator(&t);
    NTEST_ASSERT_EQ_STR(a->name, "test");

    void *x = NPH_ALLOC(a, 100U, 16U);
    void *y = NPH_ALLOC(a, 50U, 16U);
    NTEST_ASSERT_NOT_NULL(x);
    NTEST_ASSERT_NOT_NULL(y);
    NphMemoryStats s;
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_STR(s.tag, "test");
    NTEST_ASSERT_EQ_UINT(s.bytes, 150U);
    NTEST_ASSERT_EQ_UINT(s.allocations, 2U);
    NTEST_ASSERT_EQ_UINT(s.peak_bytes, 150U);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 2U);

    // Growth and shrinking by reallocation.
    x = NPH_REALLOC(a, x, 100U, 300U, 16U);
    NTEST_ASSERT_NOT_NULL(x);
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.bytes, 350U);
    NTEST_ASSERT_EQ_UINT(s.peak_bytes, 350U);
    NTEST_ASSERT_EQ_UINT(s.allocations, 2U);
    NTEST_ASSERT_EQ_UINT(s.total_bytes, 350U);
    x = NPH_REALLOC(a, x, 300U, 20U, 16U);
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.bytes, 70U);
    NTEST_ASSERT_EQ_UINT(s.peak_bytes, 350U);
    // A reallocation of NULL is an allocation.
    void *z = NPH_REALLOC(a, NULL, 0U, 8U, 16U);
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.allocations, 3U);
    NTEST_ASSERT_EQ_UINT(s.peak_allocations, 3U);
    NTEST_ASSERT_EQ_UINT(s.bytes, 78U);

    NPH_FREE(a, x, 20U, 16U);
    NPH_FREE(a, y, 50U, 16U);
    NPH_FREE(a, z, 8U, 16U);
    NPH_FREE(a, NULL, 8U, 16U); // ignored
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.bytes, 0U);
    NTEST_ASSERT_EQ_UINT(s.allocations, 0U);
    NTEST_ASSERT_EQ_UINT(s.peak_bytes, 350U);
    NTEST_ASSERT_EQ_UINT(s.peak_allocations, 3U);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 3U);
    nph_tracked_allocator_reset_peaks(&t);
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.peak_bytes, 0U);
    NTEST_ASSERT_EQ_UINT(s.peak_allocations, 0U);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
}

NTEST(memory, failures_change_nothing)
{
    u8 block[256];
    NphArena arena;
    nph_arena_init(&arena, block, sizeof block);
    NphTrackedAllocator t;
    nph_tracked_allocator_init(&t, nph_arena_allocator(&arena), "arena");
    NphAllocator *a = nph_tracked_allocator(&t);
    void *x = NPH_ALLOC(a, 64U, 16U);
    NTEST_ASSERT_NOT_NULL(x);
    NTEST_ASSERT_NULL(NPH_ALLOC(a, 4096U, 16U));
    NphMemoryStats s;
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.bytes, 64U);
    NTEST_ASSERT_EQ_UINT(s.allocations, 1U);
    NTEST_ASSERT_NULL(NPH_REALLOC(a, x, 64U, 4096U, 16U));
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.bytes, 64U); // the block is untouched and still counted
    NPH_FREE(a, x, 64U, 16U);
    nph_arena_destroy(&arena);
}

typedef struct Churn {
    NphAllocator *a;
    u32 rounds;
} Churn;

static void churn_main(void *user)
{
    const Churn *c = (const Churn *)user;
    void *blocks[16];
    for (u32 r = 0; r < c->rounds; ++r) {
        for (u32 i = 0; i < 16U; ++i) {
            blocks[i] = NPH_ALLOC(c->a, 32U + i, 16U);
        }
        for (u32 i = 0; i < 16U; ++i) {
            NPH_FREE(c->a, blocks[i], 32U + i, 16U);
        }
    }
}

NTEST(memory, counts_are_exact_under_concurrency)
{
    NphTrackedAllocator t;
    nph_tracked_allocator_init(&t, nph_allocator_system(), "shared");
    Churn churn = {nph_tracked_allocator(&t), 500U};
    NphThread threads[4];
    for (u32 i = 0; i < 4U; ++i) {
        NphThreadDesc td;
        memset(&td, 0, sizeof td);
        td.fn = churn_main;
        td.user = &churn;
        td.name = "churn";
        NTEST_ASSERT_EQ_INT(nph_thread_create(&threads[i], &td), NPH_RESULT_OK);
    }
    for (u32 i = 0; i < 4U; ++i) {
        nph_thread_join(&threads[i]);
    }
    NphMemoryStats s;
    nph_tracked_allocator_stats(&t, &s);
    NTEST_ASSERT_EQ_UINT(s.bytes, 0U);
    NTEST_ASSERT_EQ_UINT(s.allocations, 0U);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 4U * 500U * 16U);
    NTEST_ASSERT_LE_UINT(s.peak_allocations, 4U * 16U);
    NTEST_ASSERT_GE_UINT(s.peak_allocations, 16U);
}

NTEST(memory, the_profiler_lists_tracked_allocators)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.max_memory = 2U;
    NphProfiler *p = test_profiler(nph_debug_allocator_allocator(&da), &d);
    NphTrackedAllocator a;
    NphTrackedAllocator b;
    NphTrackedAllocator c;
    nph_tracked_allocator_init(&a, nph_allocator_system(), "a");
    nph_tracked_allocator_init(&b, nph_allocator_system(), "b");
    nph_tracked_allocator_init(&c, nph_allocator_system(), "c");
    NTEST_ASSERT_EQ_INT(nph_profiler_track_memory(p, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_profiler_track_memory(p, &a), NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_INT(nph_profiler_track_memory(p, &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_profiler_track_memory(p, &c), NPH_RESULT_FULL);
    void *x = NPH_ALLOC(nph_tracked_allocator(&b), 10U, 16U);
    NphMemoryStats s[4];
    NTEST_ASSERT_EQ_UINT(nph_profiler_memory_stats(p, s, 4U), 2U);
    NTEST_ASSERT_EQ_STR(s[0].tag, "a");
    NTEST_ASSERT_EQ_STR(s[1].tag, "b");
    NTEST_ASSERT_EQ_UINT(s[1].bytes, 10U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_memory_stats(p, s, 1U), 1U);
    nph_profiler_untrack_memory(p, &a);
    nph_profiler_untrack_memory(p, &c); // not registered: ignored
    NTEST_ASSERT_EQ_UINT(nph_profiler_memory_stats(p, s, 4U), 1U);
    NTEST_ASSERT_EQ_STR(s[0].tag, "b");
    NTEST_ASSERT_EQ_INT(nph_profiler_track_memory(p, &c), NPH_RESULT_OK);
    NPH_FREE(nph_tracked_allocator(&b), x, 10U, 16U);
    nph_profiler_destroy(p);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
}

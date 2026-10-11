// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>

#include "test_support.h"

#include <string.h>

typedef struct DebugFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} DebugFixture;

NTEST_SETUP(DebugFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(DebugFixture)
{
    nph_debug_allocator_destroy(&f->da, NULL, NULL);
}

typedef struct LeakList {
    u32 count;
    NphDebugAllocInfo first;
} LeakList;

static void record_leak(const NphDebugAllocInfo *info, void *user)
{
    LeakList *l = (LeakList *)user;
    if (l->count == 0U) {
        l->first = *info;
    }
    l->count += 1U;
}

NTEST_F(DebugFixture, alloc_free_and_stats)
{
    NTEST_ASSERT_EQ_STR(f->a->name, "debug");
    u8 *p = (u8 *)NPH_ALLOC(f->a, 100U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 16U));
    // Fresh memory carries the fresh-poison pattern.
    for (u32 i = 0; i < 100U; ++i) {
        NTEST_ASSERT_EQ_UINT(p[i], NPH_DEBUG_ALLOC_POISON_FRESH);
    }
    NphDebugAllocStats s;
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.live_bytes, 100U);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 1U);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(p), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check(&f->da, NULL), NPH_RESULT_OK);
    NPH_FREE(f->a, p, 100U, 16U);
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
    NTEST_ASSERT_EQ_UINT(s.live_bytes, 0U);
    NTEST_ASSERT_EQ_UINT(s.total_frees, 1U);
    NTEST_ASSERT_EQ_UINT(s.peak_live_bytes, 100U);
    NTEST_ASSERT_EQ_UINT(s.peak_live_count, 1U);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_report(&f->da, NULL, NULL), 0U);
}

NTEST_F(DebugFixture, honours_large_alignment)
{
    const u32 aligns[] = {16U, 64U, 256U, 4096U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(aligns); ++i) {
        void *p = NPH_ALLOC(f->a, 10U, aligns[i]);
        NTEST_ASSERT_NOT_NULL(p);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, aligns[i]));
        NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(p), NPH_RESULT_OK);
        NPH_FREE(f->a, p, 10U, aligns[i]);
    }
}

NTEST_F(DebugFixture, reports_leaks_with_callsites)
{
    void *leak = NPH_ALLOC(f->a, 42U, 16U);
    const u32 line = (u32)__LINE__ - 1U;
    void *leak2 = NPH_ALLOC(f->a, 8U, 16U);
    LeakList l;
    memset(&l, 0, sizeof l);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_report(&f->da, record_leak, &l), 2U);
    NTEST_ASSERT_EQ_UINT(l.count, 2U);
    // Most recent first.
    NTEST_ASSERT_EQ_PTR(l.first.ptr, leak2);
    NTEST_ASSERT_EQ_UINT(l.first.size, 8U);
    NTEST_ASSERT_EQ_UINT(l.first.sequence, 1U);
    NPH_FREE(f->a, leak2, 8U, 16U);
    memset(&l, 0, sizeof l);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_report(&f->da, record_leak, &l), 1U);
    NTEST_ASSERT_EQ_PTR(l.first.ptr, leak);
    NTEST_ASSERT_EQ_UINT(l.first.size, 42U);
    NTEST_ASSERT_EQ_UINT(l.first.align, 16U);
    NTEST_ASSERT_EQ_UINT(l.first.site.line, line);
    NTEST_ASSERT_NOT_NULL(strstr(l.first.site.file, "test_alloc_debug.c"));
    NPH_FREE(f->a, leak, 42U, 16U);
}

NTEST(alloc_debug, destroy_reports_leaks_and_counts_them)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    void *p = NPH_ALLOC(&da.allocator, 16U, 16U);
    void *q = NPH_ALLOC(&da.allocator, 16U, 16U);
    LeakList l;
    memset(&l, 0, sizeof l);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, record_leak, &l), 2U);
    NTEST_ASSERT_EQ_UINT(l.count, 2U);
    NTEST_ASSERT_NULL(da.allocator.vtable);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, record_leak, &l), 0U);
    // Return the memory to the system directly; the wrapper is gone.
    NPH_FREE(nph_allocator_system(), (u8 *)p - 96, 16U + 96U + 16U, 16U);
    NPH_FREE(nph_allocator_system(), (u8 *)q - 96, 16U + 96U + 16U, 16U);
}

NTEST_F(DebugFixture, detects_overrun_and_underrun)
{
    u8 *p = (u8 *)NPH_ALLOC(f->a, 32U, 16U);
    p[32] = 0U; // one past the end
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(p), NPH_RESULT_CORRUPT);
    NphDebugAllocInfo info;
    memset(&info, 0, sizeof info);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check(&f->da, &info), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_PTR(info.ptr, p);
    NTEST_ASSERT_EQ_UINT(info.size, 32U);
    p[32] = NPH_DEBUG_ALLOC_GUARD_PATTERN; // repair
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(p), NPH_RESULT_OK);
    p[-1] = 0U; // one before the start
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(p), NPH_RESULT_CORRUPT);
    p[-1] = NPH_DEBUG_ALLOC_GUARD_PATTERN;
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check(&f->da, NULL), NPH_RESULT_OK);
    // Freeing a corrupted block asserts, then still releases it.
    p[32] = 0U;
    NPH_TEST_EXPECT_ASSERT(NPH_FREE(f->a, p, 32U, 16U));
    NphDebugAllocStats s;
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
}

NTEST_F(DebugFixture, detects_size_and_alignment_mismatch)
{
    void *p = NPH_ALLOC(f->a, 32U, 16U);
    NPH_TEST_EXPECT_ASSERT(NPH_FREE(f->a, p, 31U, 16U));
    void *q = NPH_ALLOC(f->a, 32U, 64U);
    NPH_TEST_EXPECT_ASSERT(NPH_FREE(f->a, q, 32U, 16U));
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_report(&f->da, NULL, NULL), 0U);
}

NTEST(alloc_debug, check_block_rejects_foreign_pointers)
{
    NPH_ALIGNAS(16) u8 buffer[256];
    memset(buffer, 0, sizeof buffer);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(buffer + 128), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check_block(NULL), NPH_RESULT_CORRUPT);
}

NTEST(alloc_debug, poisons_freed_memory_and_detects_double_free)
{
    // An arena as the inner allocator keeps freed memory readable, so the
    // poison pattern can be observed.
    NphArena arena;
    NTEST_ASSERT_EQ_INT(nph_arena_init_from(&arena, nph_allocator_system(), NPH_KIB(16)),
                        NPH_RESULT_OK);
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_arena_allocator(&arena));
    NphAllocator *a = &da.allocator;
    u8 *p = (u8 *)NPH_ALLOC(a, 64U, 16U);
    void *blocker = NPH_ALLOC(a, 16U, 16U); // keeps p from being the arena's top
    memset(p, 0x11, 64U);
    NPH_FREE(a, p, 64U, 16U);
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_EQ_UINT(p[i], NPH_DEBUG_ALLOC_POISON_FREED);
    }
    NPH_TEST_EXPECT_ASSERT(NPH_FREE(a, p, 64U, 16U));
    NPH_FREE(a, blocker, 16U, 16U);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
    nph_arena_destroy(&arena);
}

NTEST_F(DebugFixture, realloc_moves_and_poisons_the_old_block)
{
    u8 *p = (u8 *)NPH_ALLOC(f->a, 16U, 16U);
    test_fill_pattern(p, 16U, 1U);
    u8 *q = (u8 *)NPH_REALLOC(f->a, p, 16U, 64U, 16U);
    NTEST_ASSERT_NOT_NULL(q);
    NTEST_ASSERT_NE_PTR(q, p);
    NTEST_ASSERT_TRUE(test_check_pattern(q, 16U, 1U));
    for (u32 i = 16U; i < 64U; ++i) {
        NTEST_ASSERT_EQ_UINT(q[i], NPH_DEBUG_ALLOC_POISON_FRESH);
    }
    NphDebugAllocStats s;
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.live_bytes, 64U);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 2U);
    NTEST_ASSERT_EQ_UINT(s.total_frees, 1U);
    NPH_FREE(f->a, q, 64U, 16U);
}

NTEST(alloc_debug, inner_failure_propagates_as_null)
{
    // A pool too small for the header makes every request fail.
    NphDebugAllocator da;
    NphArena tiny;
    NPH_ALIGNAS(16) u8 memory[64];
    nph_arena_init(&tiny, memory, sizeof memory);
    nph_debug_allocator_init(&da, nph_arena_allocator(&tiny));
    NTEST_ASSERT_NULL(NPH_ALLOC(&da.allocator, 16U, 16U));
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
    nph_arena_destroy(&tiny); // stack memory must be unpoisoned before the frame is reused
}

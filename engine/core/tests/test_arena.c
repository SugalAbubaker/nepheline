// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/arena.h>

#include "test_support.h"

#include <string.h>

// The block comes from the system allocator with an explicit alignment: a
// fixture lives on the heap, where NPH_ALIGNAS on a member is not honoured.
typedef struct ArenaFixture {
    u8 *memory;
    NphArena arena;
} ArenaFixture;

NTEST_SETUP(ArenaFixture)
{
    f->memory = (u8 *)NPH_ALLOC(nph_allocator_system(), 1024U, 64U);
    NTEST_ASSERT_NOT_NULL(f->memory);
    nph_arena_init(&f->arena, f->memory, 1024U);
}

NTEST_TEARDOWN(ArenaFixture)
{
    nph_arena_destroy(&f->arena);
    NPH_FREE(nph_allocator_system(), f->memory, 1024U, 64U);
}

NTEST_F(ArenaFixture, starts_empty)
{
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 0U);
    NTEST_ASSERT_EQ_UINT(nph_arena_remaining(&f->arena), 1024U);
    NTEST_ASSERT_EQ_UINT(nph_arena_capacity(&f->arena), 1024U);
    NTEST_ASSERT_EQ_UINT(nph_arena_peak(&f->arena), 0U);
    NTEST_ASSERT_EQ_STR(f->arena.allocator.name, "arena");
    NTEST_ASSERT_EQ_PTR(nph_arena_allocator(&f->arena), (NphAllocator *)&f->arena);
}

NTEST_F(ArenaFixture, allocations_are_sequential_and_aligned)
{
    u8 *a = (u8 *)nph_arena_alloc(&f->arena, 10U, 1U);
    u8 *b = (u8 *)nph_arena_alloc(&f->arena, 10U, 16U);
    u8 *c = (u8 *)nph_arena_alloc(&f->arena, 1U, 64U);
    NTEST_ASSERT_EQ_PTR(a, f->memory);
    NTEST_ASSERT_EQ_PTR(b, f->memory + 16);
    NTEST_ASSERT_EQ_PTR(c, f->memory + 64);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 65U);
    NTEST_ASSERT_EQ_UINT(nph_arena_peak(&f->arena), 65U);
    NTEST_ASSERT_TRUE(nph_arena_owns(&f->arena, c));
    NTEST_ASSERT_FALSE(nph_arena_owns(&f->arena, f->memory + 1024));
    NTEST_ASSERT_FALSE(nph_arena_owns(&f->arena, &f->arena));
}

NTEST_F(ArenaFixture, exhaustion_returns_null_and_keeps_state)
{
    NTEST_ASSERT_NOT_NULL(nph_arena_alloc(&f->arena, 1000U, 16U));
    NTEST_ASSERT_NULL(nph_arena_alloc(&f->arena, 25U, 16U));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 1000U);
    NTEST_ASSERT_NOT_NULL(nph_arena_alloc(&f->arena, 24U, 1U));
    NTEST_ASSERT_NULL(nph_arena_alloc(&f->arena, 1U, 1U));
    NTEST_ASSERT_EQ_UINT(nph_arena_remaining(&f->arena), 0U);
    // A request whose padding alone exceeds the space must not overflow.
    nph_arena_reset(&f->arena);
    NTEST_ASSERT_NOT_NULL(nph_arena_alloc(&f->arena, 1023U, 1U));
    NTEST_ASSERT_NULL(nph_arena_alloc(&f->arena, 1U, 64U));
    NTEST_ASSERT_NULL(nph_arena_alloc(&f->arena, NPH_U64_MAX, 1U));
}

NTEST_F(ArenaFixture, markers_restore_position)
{
    NTEST_ASSERT_NOT_NULL(nph_arena_alloc(&f->arena, 100U, 1U));
    const NphArenaMarker m = nph_arena_mark(&f->arena);
    NTEST_ASSERT_NOT_NULL(nph_arena_alloc(&f->arena, 100U, 1U));
    NTEST_ASSERT_NOT_NULL(nph_arena_alloc(&f->arena, 100U, 1U));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 300U);
    nph_arena_reset_to(&f->arena, m);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 100U);
    NTEST_ASSERT_EQ_UINT(nph_arena_peak(&f->arena), 300U);
    nph_arena_reset(&f->arena);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 0U);
    // Resetting to a marker past the current offset is rejected.
    const NphArenaMarker bogus = {500U};
    NPH_TEST_EXPECT_ASSERT(nph_arena_reset_to(&f->arena, bogus));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 0U);
}

NTEST_F(ArenaFixture, zeroed_allocation)
{
    u8 *p = (u8 *)nph_arena_alloc(&f->arena, 64U, 1U);
    memset(p, 0xFF, 64U);
    nph_arena_reset(&f->arena);
    const u8 *z = (const u8 *)nph_arena_alloc_zeroed(&f->arena, 64U, 1U);
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_EQ_UINT(z[i], 0U);
    }
    typedef struct Item {
        u64 a;
        u32 b;
    } Item;
    Item *item = NPH_ARENA_ALLOC_TYPE(&f->arena, Item);
    NTEST_ASSERT_NOT_NULL(item);
    NTEST_ASSERT_EQ_UINT(item->a + item->b, 0U);
    Item *items = NPH_ARENA_ALLOC_ARRAY(&f->arena, Item, 4U);
    NTEST_ASSERT_NOT_NULL(items);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(items, NPH_ALIGNOF(Item)));
}

NTEST_F(ArenaFixture, vtable_free_reclaims_only_the_top)
{
    NphAllocator *a = nph_arena_allocator(&f->arena);
    void *p1 = NPH_ALLOC(a, 32U, 16U);
    void *p2 = NPH_ALLOC(a, 32U, 16U);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 64U);
    NPH_FREE(a, p1, 32U, 16U); // not the top: nothing happens
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 64U);
    NPH_FREE(a, p2, 32U, 16U); // the top: reclaimed
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 32U);
    NPH_FREE(a, p1, 32U, 16U); // now the top
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 0U);
    NPH_FREE(a, NULL, 32U, 16U);
}

NTEST_F(ArenaFixture, vtable_realloc_grows_top_in_place_and_copies_otherwise)
{
    NphAllocator *a = nph_arena_allocator(&f->arena);
    u8 *p = (u8 *)NPH_ALLOC(a, 32U, 16U);
    test_fill_pattern(p, 32U, 1U);
    u8 *q = (u8 *)NPH_REALLOC(a, p, 32U, 128U, 16U);
    NTEST_ASSERT_EQ_PTR(q, p);
    NTEST_ASSERT_TRUE(test_check_pattern(q, 32U, 1U));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 128U);
    // Shrinking the top also happens in place.
    q = (u8 *)NPH_REALLOC(a, q, 128U, 16U, 16U);
    NTEST_ASSERT_EQ_PTR(q, p);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 16U);
    // Not the top: a copy is made.
    void *blocker = NPH_ALLOC(a, 16U, 16U);
    NTEST_ASSERT_NOT_NULL(blocker);
    u8 *r = (u8 *)NPH_REALLOC(a, q, 16U, 64U, 16U);
    NTEST_ASSERT_NE_PTR(r, q);
    NTEST_ASSERT_TRUE(test_check_pattern(r, 16U, 1U));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 96U);
    // Growing the top past the capacity fails and leaves the block valid.
    NTEST_ASSERT_NULL(NPH_REALLOC(a, r, 64U, 2000U, 16U));
    NTEST_ASSERT_TRUE(test_check_pattern(r, 16U, 1U));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(&f->arena), 96U);
}

NTEST(arena, init_from_backing_allocator)
{
    NphArena arena;
    NTEST_ASSERT_EQ_INT(nph_arena_init_from(&arena, nph_allocator_system(), NPH_KIB(64)),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_arena_capacity(&arena), NPH_KIB(64));
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(arena.base, NPH_ALLOC_MAX_ALIGN));
    void *p = nph_arena_alloc(&arena, NPH_KIB(64), 1U);
    NTEST_ASSERT_NOT_NULL(p);
    memset(p, 1, (size_t)NPH_KIB(64));
    nph_arena_destroy(&arena);
    NTEST_ASSERT_NULL(arena.base);
    nph_arena_destroy(&arena); // idempotent

    NTEST_ASSERT_EQ_INT(nph_arena_init_from(&arena, nph_allocator_system(), 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(arena, frame_arena_rotates_and_resets)
{
    NphFrameArena fa;
    NTEST_ASSERT_EQ_INT(nph_frame_arena_init(&fa, nph_allocator_system(), 256U, 3U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(fa.frame_count, 3U);
    NTEST_ASSERT_EQ_STR(nph_frame_arena_allocator(&fa)->name, "frame-arena");

    NphArena *first = nph_frame_arena_current(&fa);
    u8 *p0 = (u8 *)nph_arena_alloc(first, 100U, 1U);
    NTEST_ASSERT_NOT_NULL(p0);
    test_fill_pattern(p0, 100U, 7U);

    nph_frame_arena_begin_frame(&fa);
    NTEST_ASSERT_NE_PTR(nph_frame_arena_current(&fa), first);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(nph_frame_arena_current(&fa)), 0U);
    NTEST_ASSERT_EQ_UINT(fa.frame_index, 1U);
    // Frame 0's data is still intact two frames later...
    nph_frame_arena_begin_frame(&fa);
    NTEST_ASSERT_TRUE(test_check_pattern(p0, 100U, 7U));
    NTEST_ASSERT_EQ_UINT(nph_arena_used(first), 100U);
    // ...and is released when its arena comes around again.
    nph_frame_arena_begin_frame(&fa);
    NTEST_ASSERT_EQ_PTR(nph_frame_arena_current(&fa), first);
    NTEST_ASSERT_EQ_UINT(nph_arena_used(first), 0U);

    nph_frame_arena_destroy(&fa);
    NTEST_ASSERT_EQ_UINT(fa.frame_count, 0U);

    NTEST_ASSERT_EQ_INT(nph_frame_arena_init(&fa, nph_allocator_system(), 256U, 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        nph_frame_arena_init(&fa, nph_allocator_system(), 256U, NPH_FRAME_ARENA_MAX_FRAMES + 1U),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_frame_arena_init(&fa, nph_allocator_system(), 0U, 2U),
                        NPH_RESULT_INVALID_ARGUMENT);
}

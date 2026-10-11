// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/array.h>

#include "test_support.h"

#include <string.h>

typedef struct Point {
    i32 x;
    i32 y;
} Point;

typedef NPH_ARRAY(Point) PointArray;

typedef struct ArrayFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} ArrayFixture;

NTEST_SETUP(ArrayFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(ArrayFixture)
{
    // Every test must free its arrays: the wrapper reports leaks.
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(ArrayFixture, starts_empty_without_allocating)
{
    NphArrayU32 arr;
    NPH_ARRAY_INIT(&arr, f->a);
    NTEST_ASSERT_NULL(arr.data);
    NTEST_ASSERT_EQ_UINT(arr.len, 0U);
    NTEST_ASSERT_EQ_UINT(arr.cap, 0U);
    NTEST_ASSERT_TRUE(NPH_ARRAY_IS_EMPTY(&arr));
    NTEST_ASSERT_EQ_UINT(NPH_ARRAY_BYTES(&arr), 0U);
    NphDebugAllocStats s;
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 0U);
    NPH_ARRAY_FREE(&arr); // freeing an empty array is fine
}

NTEST_F(ArrayFixture, push_grows_and_preserves_order)
{
    NphArrayU32 arr;
    NPH_ARRAY_INIT(&arr, f->a);
    for (u32 i = 0; i < 1000U; ++i) {
        NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&arr, i * 3U), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(arr.len, i + 1U);
        NTEST_ASSERT_GE_UINT(arr.cap, arr.len);
    }
    for (u32 i = 0; i < 1000U; ++i) {
        NTEST_ASSERT_EQ_UINT(arr.data[i], i * 3U);
    }
    NTEST_ASSERT_EQ_UINT(NPH_ARRAY_BYTES(&arr), 4000U);
    NTEST_ASSERT_EQ_UINT(*NPH_ARRAY_LAST(&arr), 999U * 3U);
    // Capacity doubled from 8: 8, 16, ..., 1024.
    NTEST_ASSERT_EQ_UINT(arr.cap, 1024U);
    NPH_ARRAY_FREE(&arr);
    NTEST_ASSERT_NULL(arr.data);
    NTEST_ASSERT_EQ_UINT(arr.cap, 0U);
}

NTEST_F(ArrayFixture, push_slot_and_struct_elements)
{
    PointArray pts;
    NPH_ARRAY_INIT(&pts, f->a);
    Point *p = NPH_ARRAY_PUSH_SLOT(&pts);
    NTEST_ASSERT_NOT_NULL(p);
    p->x = 1;
    p->y = 2;
    const Point q = {3, 4};
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&pts, q), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pts.len, 2U);
    NTEST_ASSERT_EQ_INT(pts.data[0].y, 2);
    NTEST_ASSERT_EQ_INT(pts.data[1].x, 3);
    i32 sum = 0;
    NPH_ARRAY_FOREACH (Point, it, &pts) {
        sum += it->x + it->y;
    }
    NTEST_ASSERT_EQ_INT(sum, 10);
    const Point popped = NPH_ARRAY_POP(&pts);
    NTEST_ASSERT_EQ_INT(popped.x, 3);
    NTEST_ASSERT_EQ_UINT(pts.len, 1U);
    NPH_ARRAY_FREE(&pts);
}

NTEST_F(ArrayFixture, reserve_resize_shrink)
{
    NphArrayU64 arr;
    NPH_ARRAY_INIT(&arr, f->a);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESERVE(&arr, 100U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(arr.cap, 100U);
    NTEST_ASSERT_EQ_UINT(arr.len, 0U);
    const u64 *before = arr.data;
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESERVE(&arr, 50U), NPH_RESULT_OK); // no-op
    NTEST_ASSERT_EQ_PTR(arr.data, before);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESIZE(&arr, 10U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(arr.len, 10U);
    for (u32 i = 0; i < 10U; ++i) {
        NTEST_ASSERT_EQ_UINT(arr.data[i], 0U); // zero-filled
        arr.data[i] = i + 1U;
    }
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESIZE(&arr, 300U), NPH_RESULT_OK); // grows
    NTEST_ASSERT_EQ_UINT(arr.data[9], 10U);
    NTEST_ASSERT_EQ_UINT(arr.data[299], 0U);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESIZE(&arr, 5U), NPH_RESULT_OK); // shrinks length only
    NTEST_ASSERT_GE_UINT(arr.cap, 300U);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_SHRINK_TO_FIT(&arr), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(arr.cap, 5U);
    NTEST_ASSERT_EQ_UINT(arr.data[4], 5U);
    NPH_ARRAY_CLEAR(&arr);
    NTEST_ASSERT_EQ_UINT(arr.len, 0U);
    NTEST_ASSERT_EQ_UINT(arr.cap, 5U);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_SHRINK_TO_FIT(&arr), NPH_RESULT_OK); // frees
    NTEST_ASSERT_NULL(arr.data);
    NPH_ARRAY_FREE(&arr);
}

NTEST_F(ArrayFixture, push_n_insert_remove)
{
    NphArrayI32 arr;
    NPH_ARRAY_INIT(&arr, f->a);
    const i32 values[] = {1, 2, 3, 4, 5};
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&arr, values, 5U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&arr, values, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(arr.len, 5U);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_INSERT(&arr, 0U, 0), NPH_RESULT_OK);  // front
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_INSERT(&arr, 3U, 99), NPH_RESULT_OK); // middle
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_INSERT(&arr, 7U, 6), NPH_RESULT_OK);  // end
    const i32 expected[] = {0, 1, 2, 99, 3, 4, 5, 6};
    NTEST_ASSERT_EQ_UINT(arr.len, 8U);
    NTEST_ASSERT_EQ_MEM(arr.data, expected, sizeof expected);

    NPH_ARRAY_REMOVE_AT(&arr, 3U); // ordered
    const i32 expected2[] = {0, 1, 2, 3, 4, 5, 6};
    NTEST_ASSERT_EQ_MEM(arr.data, expected2, sizeof expected2);
    NPH_ARRAY_REMOVE_RANGE(&arr, 1U, 3U);
    const i32 expected3[] = {0, 4, 5, 6};
    NTEST_ASSERT_EQ_UINT(arr.len, 4U);
    NTEST_ASSERT_EQ_MEM(arr.data, expected3, sizeof expected3);
    NPH_ARRAY_REMOVE_SWAP(&arr, 0U); // last moves into index 0
    const i32 expected4[] = {6, 4, 5};
    NTEST_ASSERT_EQ_UINT(arr.len, 3U);
    NTEST_ASSERT_EQ_MEM(arr.data, expected4, sizeof expected4);
    NPH_ARRAY_REMOVE_SWAP(&arr, 2U); // removing the last is a plain pop
    NTEST_ASSERT_EQ_UINT(arr.len, 2U);
    NPH_ARRAY_REMOVE_RANGE(&arr, 0U, 2U);
    NTEST_ASSERT_TRUE(NPH_ARRAY_IS_EMPTY(&arr));
    NPH_ARRAY_FREE(&arr);
}

NTEST_F(ArrayFixture, bounds_are_asserted_and_ignored)
{
    NphArrayU32 arr;
    NPH_ARRAY_INIT(&arr, f->a);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&arr, 1U), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(NPH_ARRAY_REMOVE_AT(&arr, 5U));
    NTEST_ASSERT_EQ_UINT(arr.len, 1U);
    NPH_TEST_EXPECT_ASSERT(NPH_ARRAY_REMOVE_SWAP(&arr, 1U));
    NTEST_ASSERT_EQ_UINT(arr.len, 1U);
    NphResult r = NPH_RESULT_OK;
    NPH_TEST_EXPECT_ASSERT(r = NPH_ARRAY_INSERT(&arr, 2U, 7U));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_UINT(arr.len, 1U);
    NPH_ARRAY_CLEAR(&arr);
    u32 popped = 5U;
    NPH_TEST_EXPECT_ASSERT(popped = NPH_ARRAY_POP(&arr));
    NTEST_ASSERT_EQ_UINT(popped, arr.data[0]); // index 0 is returned after the failed pop
    NPH_TEST_EXPECT_ASSERT((void)NPH_ARRAY_LAST(&arr));
    NPH_ARRAY_FREE(&arr);
}

NTEST(array, allocation_failure_leaves_the_array_intact)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[256];
    nph_arena_init(&arena, memory, sizeof memory);
    NphArrayU64 arr;
    NPH_ARRAY_INIT(&arr, nph_arena_allocator(&arena));
    u32 pushed = 0U;
    while (NPH_ARRAY_PUSH(&arr, (u64)pushed) == NPH_RESULT_OK) {
        ++pushed;
        NTEST_ASSERT_LT_UINT(pushed, 100U);
    }
    NTEST_ASSERT_GT_UINT(pushed, 8U);
    NTEST_ASSERT_EQ_UINT(arr.len, pushed);
    for (u32 i = 0; i < pushed; ++i) {
        NTEST_ASSERT_EQ_UINT(arr.data[i], (u64)i);
    }
    NTEST_ASSERT_NULL(NPH_ARRAY_PUSH_SLOT(&arr));
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESERVE(&arr, 1000U), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_RESIZE(&arr, 1000U), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_UINT(arr.len, pushed);
    NPH_ARRAY_FREE(&arr);
    nph_arena_destroy(&arena);
}

NTEST(array, element_alignment_rule)
{
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(1U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(8U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(24U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(32U), 32U);
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(48U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(64U), 64U);
    NTEST_ASSERT_EQ_UINT(nph_array_elem_align(128U), 16U);
}

typedef struct Wide {
    NPH_ALIGNAS(32) f32 lanes[8];
} Wide;

NTEST(array, wide_elements_are_aligned)
{
    typedef NPH_ARRAY(Wide) WideArray;
    WideArray arr;
    NPH_ARRAY_INIT(&arr, nph_allocator_system());
    for (u32 i = 0; i < 100U; ++i) {
        Wide *w = NPH_ARRAY_PUSH_SLOT(&arr);
        NTEST_ASSERT_NOT_NULL(w);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(w, 32U));
        memset(w, 0, sizeof *w);
    }
    NPH_ARRAY_FREE(&arr);
}

// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc.h>

#include "test_support.h"

#include <string.h>

typedef struct Vec4 {
    f32 x, y, z, w;
} Vec4;

NTEST(alloc, system_allocator_is_a_singleton_with_a_name)
{
    NphAllocator *a = nph_allocator_system();
    NTEST_ASSERT_NOT_NULL(a);
    NTEST_ASSERT_EQ_PTR(a, nph_allocator_system());
    NTEST_ASSERT_EQ_STR(a->name, "system");
    NTEST_ASSERT_NOT_NULL(a->vtable);
}

NTEST(alloc, system_alloc_honours_alignment)
{
    NphAllocator *a = nph_allocator_system();
    const u32 aligns[] = {1U, 8U, 16U, 32U, 64U, 128U, 256U, 1024U, 4096U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(aligns); ++i) {
        void *p = NPH_ALLOC(a, 100U, aligns[i]);
        NTEST_ASSERT_NOT_NULL(p);
        // Everything below the default alignment is promoted to it.
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, nph_max_u32(aligns[i], NPH_ALLOC_DEFAULT_ALIGN)));
        memset(p, 0xAB, 100U);
        NPH_FREE(a, p, 100U, aligns[i]);
    }
}

NTEST(alloc, zeroed_and_typed)
{
    NphAllocator *a = nph_allocator_system();
    u8 *bytes = (u8 *)NPH_ALLOC_ZEROED(a, 64U, 16U);
    NTEST_ASSERT_NOT_NULL(bytes);
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_EQ_UINT(bytes[i], 0U);
    }
    NPH_FREE(a, bytes, 64U, 16U);

    Vec4 *v = NPH_ALLOC_TYPE(a, Vec4);
    NTEST_ASSERT_NOT_NULL(v);
    NTEST_ASSERT_TRUE(v->x == 0.0f && v->w == 0.0f);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(v, NPH_ALIGNOF(Vec4)));
    NPH_FREE_TYPE(a, Vec4, v);

    Vec4 *u = NPH_ALLOC_TYPE_UNINIT(a, Vec4);
    NTEST_ASSERT_NOT_NULL(u);
    u->x = 1.0f;
    NPH_FREE_TYPE(a, Vec4, u);

    Vec4 *arr = NPH_ALLOC_ARRAY_ZEROED(a, Vec4, 10U);
    NTEST_ASSERT_NOT_NULL(arr);
    NTEST_ASSERT_TRUE(arr[9].z == 0.0f);
    NPH_FREE_ARRAY(a, Vec4, arr, 10U);

    u32 *ints = NPH_ALLOC_ARRAY(a, u32, 3U);
    NTEST_ASSERT_NOT_NULL(ints);
    ints[2] = 7U;
    NPH_FREE_ARRAY(a, u32, ints, 3U);
}

NTEST(alloc, realloc_preserves_contents)
{
    NphAllocator *a = nph_allocator_system();
    u8 *p = (u8 *)NPH_ALLOC(a, 32U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    test_fill_pattern(p, 32U, 1U);
    p = (u8 *)NPH_REALLOC(a, p, 32U, 4096U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(test_check_pattern(p, 32U, 1U));
    test_fill_pattern(p, 4096U, 2U);
    p = (u8 *)NPH_REALLOC(a, p, 4096U, 8U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(test_check_pattern(p, 8U, 2U));
    NPH_FREE(a, p, 8U, 16U);

    // Over-aligned realloc keeps the alignment and the data.
    u8 *q = (u8 *)NPH_ALLOC(a, 100U, 256U);
    NTEST_ASSERT_NOT_NULL(q);
    test_fill_pattern(q, 100U, 3U);
    q = (u8 *)NPH_REALLOC(a, q, 100U, 10000U, 256U);
    NTEST_ASSERT_NOT_NULL(q);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(q, 256U));
    NTEST_ASSERT_TRUE(test_check_pattern(q, 100U, 3U));
    NPH_FREE(a, q, 10000U, 256U);

    // realloc(NULL) allocates.
    void *r = NPH_REALLOC(a, NULL, 0U, 24U, 16U);
    NTEST_ASSERT_NOT_NULL(r);
    NPH_FREE(a, r, 24U, 16U);
}

NTEST(alloc, free_null_is_a_no_op)
{
    NPH_FREE(nph_allocator_system(), NULL, 16U, 16U);
}

NTEST(alloc, invalid_requests_return_null)
{
    NphAllocator *a = nph_allocator_system();
    void *p = NULL;
    NPH_TEST_EXPECT_ASSERT(p = NPH_ALLOC(a, 0U, 16U));
    NTEST_ASSERT_NULL(p);
    NPH_TEST_EXPECT_ASSERT(p = NPH_ALLOC(a, 16U, 24U)); // not a power of two
    NTEST_ASSERT_NULL(p);
    NPH_TEST_EXPECT_ASSERT(p = NPH_ALLOC_ARRAY(a, u8, 0U));
    NTEST_ASSERT_NULL(p);
    NPH_TEST_EXPECT_ASSERT(p = NPH_ALLOC_ARRAY(a, u64, NPH_U64_MAX / 4U)); // overflow
    NTEST_ASSERT_NULL(p);
    NPH_TEST_EXPECT_ASSERT(p = NPH_REALLOC(a, NULL, 0U, 0U, 16U));
    NTEST_ASSERT_NULL(p);
}

NTEST(alloc, callsite_records_file_and_line)
{
    const NphCallsite site = NPH_CALLSITE_HERE();
    NTEST_ASSERT_EQ_UINT(site.line, (u32)__LINE__ - 1U);
    NTEST_ASSERT_NOT_NULL(strstr(site.file, "test_alloc.c"));
}

NTEST(alloc, normalize_align)
{
    NTEST_ASSERT_EQ_UINT(nph_alloc_normalize_align(1U), NPH_ALLOC_DEFAULT_ALIGN);
    NTEST_ASSERT_EQ_UINT(nph_alloc_normalize_align(8U), NPH_ALLOC_DEFAULT_ALIGN);
    NTEST_ASSERT_EQ_UINT(nph_alloc_normalize_align(16U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_alloc_normalize_align(64U), 64U);
}

// A counting allocator that fails on demand, to exercise fallback paths.
typedef struct FailingAllocator {
    NphAllocator allocator;
    NphAllocator *inner;
    u32 fail_after; // allocations to allow before failing
    u32 allocs;
    u32 frees;
} FailingAllocator;

static void *failing_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    FailingAllocator *f = (FailingAllocator *)self;
    if (f->allocs >= f->fail_after) {
        return NULL;
    }
    f->allocs += 1U;
    return f->inner->vtable->alloc(f->inner, size, align, site);
}

static void failing_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    FailingAllocator *f = (FailingAllocator *)self;
    f->frees += 1U;
    f->inner->vtable->free(f->inner, ptr, size, align, site);
}

static const NphAllocatorVtable g_failing_vtable = {failing_alloc, nph_allocator_realloc_fallback,
                                                    failing_free};

NTEST(alloc, realloc_fallback_copies_and_frees_old_block)
{
    FailingAllocator f = {{&g_failing_vtable, "failing"}, nph_allocator_system(), 100U, 0U, 0U};
    u8 *p = (u8 *)NPH_ALLOC(&f.allocator, 16U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    test_fill_pattern(p, 16U, 9U);
    u8 *q = (u8 *)NPH_REALLOC(&f.allocator, p, 16U, 64U, 16U);
    NTEST_ASSERT_NOT_NULL(q);
    NTEST_ASSERT_TRUE(test_check_pattern(q, 16U, 9U));
    NTEST_ASSERT_EQ_UINT(f.allocs, 2U);
    NTEST_ASSERT_EQ_UINT(f.frees, 1U);
    NPH_FREE(&f.allocator, q, 64U, 16U);
    NTEST_ASSERT_EQ_UINT(f.frees, 2U);
}

NTEST(alloc, realloc_fallback_leaves_block_intact_on_failure)
{
    FailingAllocator f = {{&g_failing_vtable, "failing"}, nph_allocator_system(), 1U, 0U, 0U};
    u8 *p = (u8 *)NPH_ALLOC(&f.allocator, 16U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    test_fill_pattern(p, 16U, 4U);
    NTEST_ASSERT_NULL(NPH_REALLOC(&f.allocator, p, 16U, 64U, 16U));
    NTEST_ASSERT_TRUE(test_check_pattern(p, 16U, 4U));
    NTEST_ASSERT_EQ_UINT(f.frees, 0U);
    NPH_FREE(&f.allocator, p, 16U, 16U);
}

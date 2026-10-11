// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/pool.h>

#include "test_support.h"

#include <string.h>

typedef struct PoolFixture {
    NPH_ALIGNAS(64) u8 memory[1024];
    NphPool pool;
} PoolFixture;

NTEST_SETUP(PoolFixture)
{
    nph_pool_init(&f->pool, f->memory, sizeof f->memory, 24U, 8U);
}

NTEST_TEARDOWN(PoolFixture)
{
    nph_pool_destroy(&f->pool);
}

NTEST_F(PoolFixture, geometry)
{
    NTEST_ASSERT_EQ_UINT(f->pool.slot_size, 24U);
    NTEST_ASSERT_EQ_UINT(f->pool.slot_align, 8U);
    NTEST_ASSERT_EQ_UINT(nph_pool_capacity(&f->pool), 1024U / 24U);
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), 0U);
    NTEST_ASSERT_EQ_STR(f->pool.allocator.name, "pool");
    NTEST_ASSERT_EQ_PTR(nph_pool_allocator(&f->pool), (NphAllocator *)&f->pool);
}

NTEST_F(PoolFixture, exhausts_then_refuses)
{
    const u32 cap = nph_pool_capacity(&f->pool);
    void *slots[64];
    NTEST_ASSERT_LE_UINT(cap, NPH_ARRAY_COUNT(slots));
    for (u32 i = 0; i < cap; ++i) {
        slots[i] = nph_pool_alloc(&f->pool);
        NTEST_ASSERT_NOT_NULL(slots[i]);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(slots[i], 8U));
        NTEST_ASSERT_TRUE(nph_pool_owns(&f->pool, slots[i]));
        memset(slots[i], (int)i, 24U);
    }
    NTEST_ASSERT_NULL(nph_pool_alloc(&f->pool));
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), cap);
    NTEST_ASSERT_EQ_UINT(nph_pool_peak(&f->pool), cap);
    // Slots are distinct and do not overlap.
    for (u32 i = 0; i < cap; ++i) {
        for (u32 j = i + 1U; j < cap; ++j) {
            NTEST_ASSERT_NE_PTR(slots[i], slots[j]);
        }
        NTEST_ASSERT_EQ_UINT(((const u8 *)slots[i])[23], (u8)i);
    }
    for (u32 i = 0; i < cap; ++i) {
        nph_pool_free(&f->pool, slots[i]);
    }
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), 0U);
}

NTEST_F(PoolFixture, free_slots_are_reused_most_recent_first)
{
    void *a = nph_pool_alloc(&f->pool);
    void *b = nph_pool_alloc(&f->pool);
    void *c = nph_pool_alloc(&f->pool);
    nph_pool_free(&f->pool, b);
    nph_pool_free(&f->pool, a);
    NTEST_ASSERT_EQ_PTR(nph_pool_alloc(&f->pool), a);
    NTEST_ASSERT_EQ_PTR(nph_pool_alloc(&f->pool), b);
    void *d = nph_pool_alloc(&f->pool);
    NTEST_ASSERT_NE_PTR(d, c);
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), 4U);
}

NTEST_F(PoolFixture, zeroed_and_null_free)
{
    u8 *p = (u8 *)nph_pool_alloc(&f->pool);
    memset(p, 0xFF, 24U);
    nph_pool_free(&f->pool, p);
    const u8 *z = (const u8 *)nph_pool_alloc_zeroed(&f->pool);
    NTEST_ASSERT_EQ_PTR(z, p);
    for (u32 i = 0; i < 24U; ++i) {
        NTEST_ASSERT_EQ_UINT(z[i], 0U);
    }
    nph_pool_free(&f->pool, NULL);
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), 1U);
}

NTEST_F(PoolFixture, owns_rejects_foreign_and_interior_pointers)
{
    void *a = nph_pool_alloc(&f->pool);
    NTEST_ASSERT_TRUE(nph_pool_owns(&f->pool, a));
    NTEST_ASSERT_FALSE(nph_pool_owns(&f->pool, (u8 *)a + 1));
    NTEST_ASSERT_FALSE(nph_pool_owns(&f->pool, &f->pool));
    NTEST_ASSERT_FALSE(nph_pool_owns(&f->pool, f->memory + 1024));
    int local = 0;
    NPH_TEST_EXPECT_ASSERT(nph_pool_free(&f->pool, &local));
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), 1U);
}

NTEST_F(PoolFixture, vtable_refuses_alignment_the_slots_lack)
{
    // The fixture's slots are 8-byte aligned; the generic interface promises
    // 16, so every request through it is refused.
    NphAllocator *a = nph_pool_allocator(&f->pool);
    void *p = NULL;
    NPH_TEST_EXPECT_ASSERT(p = NPH_ALLOC(a, 8U, 8U));
    NTEST_ASSERT_NULL(p);
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&f->pool), 0U);
}

NTEST(pool, vtable_checks_size_and_alignment)
{
    NPH_ALIGNAS(64) u8 memory[1024];
    NphPool pool;
    nph_pool_init(&pool, memory, sizeof memory, 24U, 16U);
    NTEST_ASSERT_EQ_UINT(pool.slot_size, 32U);
    NphAllocator *a = nph_pool_allocator(&pool);
    void *p = NPH_ALLOC(a, 24U, 8U);
    NTEST_ASSERT_NOT_NULL(p);
    void *q = NULL;
    NPH_TEST_EXPECT_ASSERT(q = NPH_ALLOC(a, 33U, 8U));
    NTEST_ASSERT_NULL(q);
    NPH_TEST_EXPECT_ASSERT(q = NPH_ALLOC(a, 8U, 64U));
    NTEST_ASSERT_NULL(q);
    // Realloc within the slot keeps the pointer; beyond it fails.
    NTEST_ASSERT_EQ_PTR(NPH_REALLOC(a, p, 24U, 32U, 8U), p);
    NTEST_ASSERT_NULL(NPH_REALLOC(a, p, 32U, 64U, 8U));
    NPH_FREE(a, p, 32U, 8U);
    NTEST_ASSERT_EQ_UINT(nph_pool_live(&pool), 0U);
    nph_pool_destroy(&pool);
}

NTEST(pool, slots_are_at_least_pointer_sized_and_aligned)
{
    NPH_ALIGNAS(64) u8 memory[256];
    NphPool pool;
    nph_pool_init(&pool, memory, sizeof memory, 1U, 1U);
    NTEST_ASSERT_EQ_UINT(pool.slot_size, (u32)sizeof(void *));
    NTEST_ASSERT_EQ_UINT(pool.slot_align, (u32)sizeof(void *));
    NTEST_ASSERT_EQ_UINT(nph_pool_capacity(&pool), 256U / 8U);
    nph_pool_destroy(&pool); // stack memory must be unpoisoned before reuse

    nph_pool_init(&pool, memory, sizeof memory, 20U, 64U);
    NTEST_ASSERT_EQ_UINT(pool.slot_size, 64U);
    NTEST_ASSERT_EQ_UINT(nph_pool_capacity(&pool), 4U);
    void *p = nph_pool_alloc(&pool);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 64U));
    nph_pool_destroy(&pool);

    // An unaligned block is trimmed to the first aligned slot.
    nph_pool_init(&pool, memory + 1, 255U, 20U, 64U);
    NTEST_ASSERT_EQ_PTR(pool.base, memory + 64);
    NTEST_ASSERT_EQ_UINT(nph_pool_capacity(&pool), 3U);
    nph_pool_destroy(&pool);

    // Too small for one slot: zero capacity, allocation refused.
    nph_pool_init(&pool, memory, 4U, 20U, 8U);
    NTEST_ASSERT_EQ_UINT(nph_pool_capacity(&pool), 0U);
    NTEST_ASSERT_NULL(nph_pool_alloc(&pool));
    nph_pool_destroy(&pool);
    nph_pool_init(&pool, NULL, 0U, 20U, 8U);
    NTEST_ASSERT_NULL(nph_pool_alloc(&pool));
    nph_pool_destroy(&pool);
}

NTEST(pool, init_from_backing_allocator)
{
    NphPool pool;
    NTEST_ASSERT_EQ_INT(nph_pool_init_from(&pool, nph_allocator_system(), 100U, 32U, 10U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pool_capacity(&pool), 10U);
    NTEST_ASSERT_EQ_UINT(pool.slot_size, 128U);
    for (u32 i = 0; i < 10U; ++i) {
        void *p = nph_pool_alloc(&pool);
        NTEST_ASSERT_NOT_NULL(p);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 32U));
        memset(p, 0, 100U);
    }
    NTEST_ASSERT_NULL(nph_pool_alloc(&pool));
    nph_pool_destroy(&pool);
    NTEST_ASSERT_NULL(pool.base);
    nph_pool_destroy(&pool);

    NTEST_ASSERT_EQ_INT(nph_pool_init_from(&pool, nph_allocator_system(), 100U, 32U, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_pool_init_from(&pool, nph_allocator_system(), 100U, 3U, 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
}

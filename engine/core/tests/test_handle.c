// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/handle.h>

#include "test_support.h"

typedef struct HandleFixture {
    NphDebugAllocator da;
    NphHandlePool pool;
} HandleFixture;

NTEST_SETUP(HandleFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    // 8 index bits, 24 generation bits, 100 slots, reuse after 4 releases.
    NTEST_ASSERT_EQ_INT(
        nph_handle_pool_init(&f->pool, nph_debug_allocator_allocator(&f->da), 100U, 8U, 4U),
        NPH_RESULT_OK);
}

NTEST_TEARDOWN(HandleFixture)
{
    nph_handle_pool_free(&f->pool);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(HandleFixture, null_handle_is_never_valid)
{
    const NphHandle null = NPH_HANDLE_NULL;
    NTEST_ASSERT_TRUE(nph_handle_is_null(null));
    NTEST_ASSERT_FALSE(nph_handle_pool_is_valid(&f->pool, null));
    NTEST_ASSERT_FALSE(nph_handle_pool_release(&f->pool, null));
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_live(&f->pool), 0U);
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_capacity(&f->pool), 100U);
}

NTEST_F(HandleFixture, handles_are_distinct_and_valid_until_released)
{
    NphHandle handles[100];
    for (u32 i = 0; i < 100U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&f->pool, &handles[i]), NPH_RESULT_OK);
        NTEST_ASSERT_FALSE(nph_handle_is_null(handles[i]));
        NTEST_ASSERT_TRUE(nph_handle_pool_is_valid(&f->pool, handles[i]));
        NTEST_ASSERT_EQ_UINT(nph_handle_pool_index(&f->pool, handles[i]), i);
        NTEST_ASSERT_EQ_UINT(nph_handle_pool_generation(&f->pool, handles[i]), 1U);
        for (u32 j = 0; j < i; ++j) {
            NTEST_ASSERT_FALSE(nph_handle_equals(handles[i], handles[j]));
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_live(&f->pool), 100U);
    NphHandle extra;
    NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&f->pool, &extra), NPH_RESULT_FULL);
    NTEST_ASSERT_TRUE(nph_handle_is_null(extra));

    NTEST_ASSERT_TRUE(nph_handle_pool_release(&f->pool, handles[42]));
    NTEST_ASSERT_FALSE(nph_handle_pool_is_valid(&f->pool, handles[42]));
    NTEST_ASSERT_FALSE(nph_handle_pool_release(&f->pool, handles[42])); // double release
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_live(&f->pool), 99U);
    // The slot is reused (the pool is exhausted) with a new generation.
    NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&f->pool, &extra), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_index(&f->pool, extra), 42U);
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_generation(&f->pool, extra), 2U);
    NTEST_ASSERT_FALSE(nph_handle_equals(extra, handles[42]));
    NTEST_ASSERT_FALSE(nph_handle_pool_is_valid(&f->pool, handles[42])); // stale
    NTEST_ASSERT_TRUE(nph_handle_pool_is_valid(&f->pool, extra));
}

NTEST_F(HandleFixture, released_slots_wait_before_reuse)
{
    NphHandle h[8];
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&f->pool, &h[i]), NPH_RESULT_OK);
    }
    // Four releases do not yet trigger reuse (threshold is "more than 4").
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_TRUE(nph_handle_pool_release(&f->pool, h[i]));
    }
    NphHandle fresh;
    NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&f->pool, &fresh), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_index(&f->pool, fresh), 8U); // never-used slot
    NTEST_ASSERT_TRUE(nph_handle_pool_release(&f->pool, h[4]));
    // Five queued: the oldest released slot (0) comes back first.
    NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&f->pool, &fresh), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_index(&f->pool, fresh), 0U);
    NTEST_ASSERT_EQ_UINT(nph_handle_pool_generation(&f->pool, fresh), 2U);
}

NTEST(handle, generation_wraps_to_one_and_skips_zero)
{
    NphHandlePool pool;
    // 4 generation bits: generations 1..15.
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, nph_allocator_system(), 2U, 28U, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pool.generation_max, 15U);
    NphHandle h;
    for (u32 round = 1; round <= 40U; ++round) {
        NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&pool, &h), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(nph_handle_pool_index(&pool, h), 0U);
        const u32 expected = ((round - 1U) % 15U) + 1U;
        NTEST_ASSERT_EQ_UINT(nph_handle_pool_generation(&pool, h), expected);
        NTEST_ASSERT_FALSE(nph_handle_is_null(h));
        NTEST_ASSERT_TRUE(nph_handle_pool_release(&pool, h));
    }
    nph_handle_pool_free(&pool);
    nph_handle_pool_free(&pool);
}

NTEST(handle, init_rejects_bad_parameters)
{
    NphHandlePool pool;
    NphAllocator *a = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 0U, 8U, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 257U, 8U, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 1U, 0U, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 1U, 29U, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 256U, 8U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pool.index_mask, 0xFFU);
    nph_handle_pool_free(&pool);
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 1U, 24U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pool.generation_max, 255U);
    NTEST_ASSERT_EQ_UINT(pool.index_mask, 0xFFFFFFU);
    nph_handle_pool_free(&pool);
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, a, 1U, 28U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pool.generation_max, 15U);
    nph_handle_pool_free(&pool);
}

NTEST(handle, randomized_use_matches_a_reference)
{
    NphHandlePool pool;
    NTEST_ASSERT_EQ_INT(nph_handle_pool_init(&pool, nph_allocator_system(), 64U, 6U, 8U),
                        NPH_RESULT_OK);
    NphHandle live[64];
    bool is_live[64];
    NphHandle stale[64];
    u32 stale_count = 0U;
    for (u32 i = 0; i < 64U; ++i) {
        is_live[i] = false;
    }
    u64 rng = 0xAB1EULL;
    u32 live_count = 0U;
    for (u32 op = 0; op < 20000U; ++op) {
        const u32 slot = (u32)test_rng_below(&rng, 64U);
        if (is_live[slot] && test_rng_below(&rng, 2U) == 0U) {
            NTEST_ASSERT_TRUE(nph_handle_pool_release(&pool, live[slot]));
            if (stale_count < 64U) {
                stale[stale_count++] = live[slot];
            }
            is_live[slot] = false;
            --live_count;
        } else if (!is_live[slot]) {
            NphHandle h;
            NTEST_ASSERT_EQ_INT(nph_handle_pool_alloc(&pool, &h), NPH_RESULT_OK);
            live[slot] = h;
            is_live[slot] = true;
            ++live_count;
        }
        NTEST_ASSERT_EQ_UINT(nph_handle_pool_live(&pool), live_count);
        if (op % 64U == 0U) {
            for (u32 i = 0; i < 64U; ++i) {
                if (is_live[i]) {
                    NTEST_ASSERT_TRUE(nph_handle_pool_is_valid(&pool, live[i]));
                }
            }
            for (u32 i = 0; i < stale_count; ++i) {
                NTEST_ASSERT_FALSE(nph_handle_pool_is_valid(&pool, stale[i]));
            }
            stale_count = 0U;
        }
    }
    nph_handle_pool_free(&pool);
}

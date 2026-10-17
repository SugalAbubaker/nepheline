// SPDX-License-Identifier: Apache-2.0
// The range allocator (core/range_alloc.h): size classes, allocation,
// coalescing, exhaustion, refusals, and a randomized run against a model
// of the space that checks every live range is disjoint and inside it.
#include <nepheline/core/range_alloc.h>

#include "test_support.h"

#include <string.h>

#define VALIDATE(ra)                                                                               \
    do {                                                                                           \
        const char *reason_ = NULL;                                                                \
        if (nph_range_alloc_validate((ra), &reason_) != NPH_RESULT_OK) {                           \
            NTEST_FAIL("range allocator invariant violated: %s", reason_);                         \
        }                                                                                          \
    } while (0)

NTEST(range_alloc, size_classes_bracket_every_size)
{
    // Exponent-0 classes are exact; from 8 up each class covers
    // [class_size(c), class_size(c + 1)) and the sizes rise strictly.
    for (u32 c = 0; c + 1U < NPH_RANGE_CLASS_COUNT; ++c) {
        NTEST_ASSERT_LT_UINT(nph_range_class_size(c), nph_range_class_size(c + 1U));
        if (c >= 8U) {
            // A class spans at most 1/8 of its smallest size.
            const u64 lo = nph_range_class_size(c);
            const u64 hi = nph_range_class_size(c + 1U);
            NTEST_ASSERT_LE_UINT((hi - lo) * 8U, lo);
        }
    }
    u64 s = 0x1234ULL;
    for (u32 i = 0; i < 200000U; ++i) {
        // Every size up to 2^16, then random sizes over the whole u32 range.
        u32 count = i + 1U;
        if (i >= 65536U) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            count = (u32)(s >> 32U) | 1U;
        }
        const u32 down = nph_range_class_round_down(count);
        const u32 up = nph_range_class_round_up(count);
        NTEST_ASSERT_LE_UINT(nph_range_class_size(down), count);
        NTEST_ASSERT_LT_UINT(count, nph_range_class_size(down + 1U));
        NTEST_ASSERT_GE_UINT(nph_range_class_size(up), count);
        // Rounding up moves at most one class, and only for sizes that are
        // not a class's smallest.
        const bool exact = nph_range_class_size(down) == count;
        NTEST_ASSERT_EQ_UINT(up, exact ? down : down + 1U);
    }
    NTEST_EXPECT_EQ_UINT(nph_range_class_round_down(7U), 7U);
    NTEST_EXPECT_EQ_UINT(nph_range_class_round_down(8U), 8U);
    NTEST_EXPECT_EQ_UINT(nph_range_class_size(16U), 16U);
    NTEST_EXPECT_EQ_UINT(nph_range_class_round_up(17U), 17U); // class 17 starts at 18
    NTEST_EXPECT_EQ_UINT(nph_range_class_size(17U), 18U);
    NTEST_EXPECT_EQ_UINT(nph_range_class_size(240U), 1ULL << 32U);
}

NTEST(range_alloc, a_fresh_allocator_is_one_free_range)
{
    NphRangeAllocator ra;
    NTEST_ASSERT_EQ_INT(nph_range_alloc_init(&ra, nph_allocator_system(), 1000U, 16U),
                        NPH_RESULT_OK);
    VALIDATE(&ra);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_free_total(&ra), 1000U);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_largest_free(&ra), 1000U);
    NTEST_EXPECT_EQ_UINT(ra.live, 0U);
    nph_range_alloc_destroy(&ra);
    nph_range_alloc_destroy(&ra); // safe twice
    nph_range_alloc_destroy(NULL);
}

NTEST(range_alloc, allocations_are_packed_and_coalesce_when_freed)
{
    NphRangeAllocator ra;
    NTEST_ASSERT_EQ_INT(nph_range_alloc_init(&ra, nph_allocator_system(), 100U, 8U), NPH_RESULT_OK);
    NphRange r[4];
    const u32 sizes[4] = {10U, 20U, 30U, 40U};
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_range_alloc(&ra, sizes[i], &r[i]), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(nph_range_size(&ra, r[i]), sizes[i]);
        VALIDATE(&ra);
    }
    // First fit from the bottom of the space: packed in order.
    NTEST_EXPECT_EQ_UINT(r[0].offset, 0U);
    NTEST_EXPECT_EQ_UINT(r[1].offset, 10U);
    NTEST_EXPECT_EQ_UINT(r[2].offset, 30U);
    NTEST_EXPECT_EQ_UINT(r[3].offset, 60U);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_free_total(&ra), 0U);
    NphRange none;
    NTEST_EXPECT_EQ_INT(nph_range_alloc(&ra, 1U, &none), NPH_RESULT_FULL);
    NTEST_EXPECT_TRUE(nph_range_is_null(none));

    // Freeing the middle two leaves one 50-unit hole: they merged.
    nph_range_free(&ra, r[1]);
    VALIDATE(&ra);
    nph_range_free(&ra, r[2]);
    VALIDATE(&ra);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_largest_free(&ra), 50U);
    NphRange big;
    NTEST_ASSERT_EQ_INT(nph_range_alloc(&ra, 48U, &big), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(big.offset, 10U);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_free_total(&ra), 2U);
    nph_range_free(&ra, big);
    // Freeing the outer ones merges everything back into one range.
    nph_range_free(&ra, r[0]);
    nph_range_free(&ra, r[3]);
    VALIDATE(&ra);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_largest_free(&ra), 100U);
    NTEST_EXPECT_EQ_UINT(nph_range_size(&ra, r[0]), 0U); // dead
    nph_range_alloc_destroy(&ra);
}

NTEST(range_alloc, refusals)
{
    NphRangeAllocator ra;
    NTEST_EXPECT_EQ_INT(nph_range_alloc_init(&ra, nph_allocator_system(), 0U, 4U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_range_alloc_init(&ra, nph_allocator_system(), 10U, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_range_alloc_init(&ra, nph_allocator_system(), 64U, 3U), NPH_RESULT_OK);
    NphRange r;
    NTEST_EXPECT_EQ_INT(nph_range_alloc(&ra, 0U, &r), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_range_alloc(&ra, 65U, &r), NPH_RESULT_FULL);
    // The allocation count is a limit of its own.
    NphRange a[3];
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_range_alloc(&ra, 1U, &a[i]), NPH_RESULT_OK);
    }
    NTEST_EXPECT_EQ_INT(nph_range_alloc(&ra, 1U, &r), NPH_RESULT_FULL);
    // A double free and a forged range are refused; nothing changes.
    nph_range_free(&ra, a[1]);
    NPH_TEST_EXPECT_ASSERT(nph_range_free(&ra, a[1]));
    const NphRange forged = {a[2].offset + 1U, a[2].node};
    NPH_TEST_EXPECT_ASSERT(nph_range_free(&ra, forged));
    const NphRange wild = {0U, 1000000U};
    NPH_TEST_EXPECT_ASSERT(nph_range_free(&ra, wild));
    nph_range_free(&ra, NPH_RANGE_NULL); // ignored
    VALIDATE(&ra);
    NTEST_EXPECT_EQ_UINT(ra.live, 2U);
    nph_range_alloc_reset(&ra);
    VALIDATE(&ra);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_largest_free(&ra), 64U);
    nph_range_alloc_destroy(&ra);
}

NTEST(range_alloc, a_refusal_means_no_free_range_of_the_rounded_size)
{
    // O(1) search rounds the request up to its class, so a refusal promises
    // only that no free range reaches the class above the request.
    NphRangeAllocator ra;
    NTEST_ASSERT_EQ_INT(nph_range_alloc_init(&ra, nph_allocator_system(), 4096U, 256U),
                        NPH_RESULT_OK);
    NphRange r[200];
    u32 n = 0U;
    // Fill with a pattern of sizes, then free every other one.
    while (n < 200U && nph_range_alloc(&ra, 13U + (n % 7U), &r[n]) == NPH_RESULT_OK) {
        ++n;
    }
    for (u32 i = 0; i < n; i += 2U) {
        nph_range_free(&ra, r[i]);
    }
    VALIDATE(&ra);
    const u32 largest = nph_range_alloc_largest_free(&ra);
    for (u32 count = 1U; count < 64U; ++count) {
        NphRange t;
        const NphResult res = nph_range_alloc(&ra, count, &t);
        if (res == NPH_RESULT_OK) {
            NTEST_ASSERT_GE_UINT(largest, count);
            nph_range_free(&ra, t);
        } else {
            NTEST_ASSERT_EQ_INT(res, NPH_RESULT_FULL);
            NTEST_ASSERT_LT_UINT(largest, nph_range_class_size(nph_range_class_round_up(count)));
        }
    }
    VALIDATE(&ra);
    nph_range_alloc_destroy(&ra);
}

// A model of the space: owner[i] is the index of the live range holding
// unit i, or NPH_U32_MAX.
#define MODEL_SIZE  8192U
#define MODEL_SLOTS 128U

NTEST(range_alloc, randomized_against_a_model)
{
    NphAllocator *a = nph_allocator_system();
    NphRangeAllocator ra;
    NTEST_ASSERT_EQ_INT(nph_range_alloc_init(&ra, a, MODEL_SIZE, MODEL_SLOTS), NPH_RESULT_OK);
    u32 *owner = NPH_ALLOC_ARRAY(a, u32, MODEL_SIZE);
    NTEST_ASSERT_NOT_NULL(owner);
    memset(owner, 0xFF, sizeof(u32) * MODEL_SIZE);
    NphRange live[MODEL_SLOTS];
    u32 sizes[MODEL_SLOTS];
    for (u32 i = 0; i < MODEL_SLOTS; ++i) {
        live[i] = NPH_RANGE_NULL;
        sizes[i] = 0U;
    }
    u64 s = 0xC0FFEEULL;
    u32 refused = 0U;
    u32 granted = 0U;
    for (u32 op = 0; op < 50000U; ++op) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const u32 slot = (u32)(s >> 40U) % MODEL_SLOTS;
        if (nph_range_is_null(live[slot])) {
            // Sizes skewed small with an occasional large one.
            const u32 roll = (u32)(s >> 20U) & 0xFFU;
            const u32 count = roll < 240U ? 1U + roll % 64U : 64U + (u32)(s >> 8U) % 900U;
            const u32 before = nph_range_alloc_free_total(&ra);
            const NphResult res = nph_range_alloc(&ra, count, &live[slot]);
            if (res != NPH_RESULT_OK) {
                NTEST_ASSERT_EQ_INT(res, NPH_RESULT_FULL);
                NTEST_ASSERT_EQ_UINT(nph_range_alloc_free_total(&ra), before);
                ++refused;
                continue;
            }
            ++granted;
            sizes[slot] = count;
            NTEST_ASSERT_LE_UINT(live[slot].offset + count, MODEL_SIZE);
            for (u32 u = 0; u < count; ++u) {
                NTEST_ASSERT_EQ_UINT(owner[live[slot].offset + u], NPH_U32_MAX);
                owner[live[slot].offset + u] = slot;
            }
        } else {
            for (u32 u = 0; u < sizes[slot]; ++u) {
                NTEST_ASSERT_EQ_UINT(owner[live[slot].offset + u], slot);
                owner[live[slot].offset + u] = NPH_U32_MAX;
            }
            nph_range_free(&ra, live[slot]);
            live[slot] = NPH_RANGE_NULL;
        }
        if (op % 997U == 0U) {
            VALIDATE(&ra);
            u32 free_units = 0U;
            for (u32 u = 0; u < MODEL_SIZE; ++u) {
                free_units += owner[u] == NPH_U32_MAX ? 1U : 0U;
            }
            NTEST_ASSERT_EQ_UINT(nph_range_alloc_free_total(&ra), free_units);
        }
    }
    NTEST_LOG("%u granted, %u refused", granted, refused);
    NTEST_ASSERT_GT_UINT(granted, 10000U);
    for (u32 i = 0; i < MODEL_SLOTS; ++i) {
        nph_range_free(&ra, live[i]);
    }
    VALIDATE(&ra);
    NTEST_EXPECT_EQ_UINT(nph_range_alloc_largest_free(&ra), MODEL_SIZE);
    NPH_FREE_ARRAY(a, u32, owner, MODEL_SIZE);
    nph_range_alloc_destroy(&ra);
}

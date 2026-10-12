// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/tlsf.h>
#include <nepheline/platform/locked_alloc.h>

#include "test_platform_support.h"

#include <string.h>

NTEST(locked_alloc, forwards_to_the_inner_allocator)
{
    NphTlsf tlsf;
    NTEST_ASSERT_EQ_INT(nph_tlsf_init_from(&tlsf, nph_allocator_system(), NPH_MIB(1)),
                        NPH_RESULT_OK);
    NphLockedAllocator la;
    nph_locked_allocator_init(&la, nph_tlsf_allocator(&tlsf));
    NphAllocator *a = nph_locked_allocator_allocator(&la);
    NTEST_ASSERT_EQ_STR(a->name, nph_tlsf_allocator(&tlsf)->name);

    u8 *p = (u8 *)NPH_ALLOC(a, 100U, 64U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 64U));
    NTEST_ASSERT_TRUE(nph_tlsf_owns(&tlsf, p));
    memset(p, 7, 100U);
    p = (u8 *)NPH_REALLOC(a, p, 100U, 10000U, 64U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_EQ_UINT(p[99], 7U);
    NPH_FREE(a, p, 10000U, 64U);

    u32 *arr = NPH_ALLOC_ARRAY_ZEROED(a, u32, 16U);
    NTEST_ASSERT_NOT_NULL(arr);
    NTEST_ASSERT_EQ_UINT(arr[15], 0U);
    NPH_FREE_ARRAY(a, u32, arr, 16U);

    NphTlsfStats stats;
    nph_tlsf_get_stats(&tlsf, &stats);
    NTEST_ASSERT_EQ_UINT(stats.live_count, 0U);

    // Exhaustion propagates as NULL, like the inner allocator.
    NTEST_ASSERT_NULL(NPH_ALLOC(a, NPH_MIB(4), 16U));

    nph_locked_allocator_destroy(&la);
    nph_tlsf_destroy(&tlsf);
}

typedef struct Shared {
    NphAllocator *alloc;
    u32 iterations;
    NphAtomicU32 failures;
} Shared;

static void hammer(u32 index, void *shared)
{
    Shared *s = (Shared *)shared;
    u64 rng = 0x1234U + index;
    void *slots[32];
    u64 sizes[32];
    memset(slots, 0, sizeof slots);
    memset(sizes, 0, sizeof sizes);
    for (u32 i = 0; i < s->iterations; ++i) {
        const u32 slot = (u32)(test_rng_next(&rng) % 32U);
        if (slots[slot] != NULL) {
            // Verify the fill before freeing: a cross-thread corruption
            // would show up here.
            const u8 *bytes = (const u8 *)slots[slot];
            const u8 expected = (u8)(sizes[slot] + index);
            for (u64 k = 0; k < sizes[slot]; k += 61U) {
                if (bytes[k] != expected) {
                    nph_atomic_fetch_add_u32(&s->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
                    break;
                }
            }
            NPH_FREE(s->alloc, slots[slot], sizes[slot], 16U);
            slots[slot] = NULL;
        } else {
            const u64 size = 16U + test_rng_next(&rng) % 2000U;
            void *p = NPH_ALLOC(s->alloc, size, 16U);
            if (p == NULL) {
                nph_atomic_fetch_add_u32(&s->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
                continue;
            }
            memset(p, (int)(u8)(size + index), (size_t)size);
            slots[slot] = p;
            sizes[slot] = size;
        }
    }
    for (u32 slot = 0; slot < 32U; ++slot) {
        if (slots[slot] != NULL) {
            NPH_FREE(s->alloc, slots[slot], sizes[slot], 16U);
        }
    }
}

NTEST(locked_alloc, tlsf_survives_concurrent_traffic)
{
    NphTlsf tlsf;
    NTEST_ASSERT_EQ_INT(nph_tlsf_init_from(&tlsf, nph_allocator_system(), NPH_MIB(16)),
                        NPH_RESULT_OK);
    NphLockedAllocator la;
    nph_locked_allocator_init(&la, nph_tlsf_allocator(&tlsf));
    Shared s;
    s.alloc = nph_locked_allocator_allocator(&la);
    s.iterations = 5000U * TEST_SCALE;
    nph_atomic_store_u32(&s.failures, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker workers[TEST_MAX_WORKERS];
    const u32 n = test_worker_count(8U);
    test_threads_run(workers, n, hammer, &s);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
    const char *reason = NULL;
    NTEST_ASSERT_EQ_INT(nph_tlsf_validate(&tlsf, &reason), NPH_RESULT_OK);
    NphTlsfStats stats;
    nph_tlsf_get_stats(&tlsf, &stats);
    NTEST_ASSERT_EQ_UINT(stats.live_count, 0U);
    NTEST_ASSERT_EQ_UINT(stats.live_bytes, 0U);
    nph_locked_allocator_destroy(&la);
    nph_tlsf_destroy(&tlsf);
}

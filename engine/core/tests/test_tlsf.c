// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/tlsf.h>

#include "test_support.h"

#include <string.h>

#define VALIDATE(tlsf)                                                                             \
    do {                                                                                           \
        const char *reason_ = NULL;                                                                \
        if (nph_tlsf_validate((tlsf), &reason_) != NPH_RESULT_OK) {                                \
            NTEST_FAIL("heap invariant violated: %s", reason_);                                    \
        }                                                                                          \
    } while (0)

typedef struct TlsfFixture {
    NphTlsf tlsf;
    u8 *memory;
    u64 size;
} TlsfFixture;

NTEST_SETUP(TlsfFixture)
{
    f->size = NPH_MIB(1);
    f->memory = (u8 *)NPH_ALLOC(nph_allocator_system(), f->size, 4096U);
    NTEST_ASSERT_NOT_NULL(f->memory);
    NTEST_ASSERT_EQ_INT(nph_tlsf_init(&f->tlsf, f->memory, f->size), NPH_RESULT_OK);
}

NTEST_TEARDOWN(TlsfFixture)
{
    nph_tlsf_destroy(&f->tlsf);
    NPH_FREE(nph_allocator_system(), f->memory, f->size, 4096U);
}

NTEST_F(TlsfFixture, fresh_heap_is_one_free_block)
{
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.pool_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.pool_bytes, f->size);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
    NTEST_ASSERT_EQ_UINT(s.live_bytes, 0U);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.free_bytes, f->size - 32U); // first header + sentinel
    NTEST_ASSERT_EQ_STR(f->tlsf.allocator.name, "tlsf");
    NTEST_ASSERT_EQ_PTR(nph_tlsf_allocator(&f->tlsf), (NphAllocator *)&f->tlsf);
}

NTEST_F(TlsfFixture, alloc_free_round_trip)
{
    void *p = nph_tlsf_alloc(&f->tlsf, 100U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 16U));
    NTEST_ASSERT_TRUE(nph_tlsf_owns(&f->tlsf, p));
    NTEST_ASSERT_EQ_UINT(nph_tlsf_usable_size(p), 112U);
    memset(p, 0xAA, 112U);
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.live_bytes, 112U);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
    nph_tlsf_free(&f->tlsf, p);
    VALIDATE(&f->tlsf);
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.free_bytes, f->size - 32U);
    NTEST_ASSERT_EQ_UINT(s.peak_live_bytes, 112U);
    nph_tlsf_free(&f->tlsf, NULL);
}

NTEST_F(TlsfFixture, small_requests_round_to_the_minimum_block)
{
    void *p = nph_tlsf_alloc(&f->tlsf, 1U, 1U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_EQ_UINT(nph_tlsf_usable_size(p), 16U);
    void *q = nph_tlsf_alloc(&f->tlsf, 17U, 1U);
    NTEST_ASSERT_EQ_UINT(nph_tlsf_usable_size(q), 32U);
    NTEST_ASSERT_EQ_PTR(q, (u8 *)p + 32); // 16 payload + 16 header
    nph_tlsf_free(&f->tlsf, p);
    nph_tlsf_free(&f->tlsf, q);
    VALIDATE(&f->tlsf);
}

NTEST_F(TlsfFixture, coalesces_in_every_direction)
{
    void *a = nph_tlsf_alloc(&f->tlsf, 256U, 16U);
    void *b = nph_tlsf_alloc(&f->tlsf, 256U, 16U);
    void *c = nph_tlsf_alloc(&f->tlsf, 256U, 16U);
    void *d = nph_tlsf_alloc(&f->tlsf, 256U, 16U);
    NTEST_ASSERT_NOT_NULL(d);
    VALIDATE(&f->tlsf);
    NphTlsfStats s;

    nph_tlsf_free(&f->tlsf, b); // free between used neighbours: no merge
    VALIDATE(&f->tlsf);
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.free_count, 2U);

    nph_tlsf_free(&f->tlsf, a); // merge with the free successor b
    VALIDATE(&f->tlsf);
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.free_count, 2U);

    nph_tlsf_free(&f->tlsf, d); // merge with the free remainder after it
    VALIDATE(&f->tlsf);
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.free_count, 2U);

    nph_tlsf_free(&f->tlsf, c); // merges with both neighbours: back to one block
    VALIDATE(&f->tlsf);
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.free_bytes, f->size - 32U);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
}

NTEST_F(TlsfFixture, freed_memory_is_reused)
{
    void *a = nph_tlsf_alloc(&f->tlsf, 1000U, 16U);
    nph_tlsf_free(&f->tlsf, a);
    void *b = nph_tlsf_alloc(&f->tlsf, 1000U, 16U);
    NTEST_ASSERT_EQ_PTR(a, b);
    nph_tlsf_free(&f->tlsf, b);
    VALIDATE(&f->tlsf);
}

NTEST_F(TlsfFixture, over_aligned_allocations)
{
    const u32 aligns[] = {32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U};
    void *ptrs[NPH_ARRAY_COUNT(aligns) * 3U];
    u32 n = 0U;
    // Interleave small blocks so the aligned requests rarely start aligned.
    for (u32 i = 0; i < NPH_ARRAY_COUNT(aligns); ++i) {
        ptrs[n++] = nph_tlsf_alloc(&f->tlsf, 16U, 16U);
        void *p = nph_tlsf_alloc(&f->tlsf, 100U, aligns[i]);
        NTEST_ASSERT_NOT_NULL(p);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, aligns[i]));
        NTEST_ASSERT_GE_UINT(nph_tlsf_usable_size(p), 100U);
        memset(p, 0x5A, 100U);
        ptrs[n++] = p;
        ptrs[n++] = nph_tlsf_alloc(&f->tlsf, 48U, 16U);
        VALIDATE(&f->tlsf);
    }
    for (u32 i = 0; i < n; ++i) {
        nph_tlsf_free(&f->tlsf, ptrs[i]);
        VALIDATE(&f->tlsf);
    }
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
}

NTEST_F(TlsfFixture, over_aligned_covers_every_gap_size)
{
    // The pool is 4096-aligned, so the first payload sits at +16 and a
    // leading block of `lead` bytes puts the next payload at 32 + lead
    // (mod 64). lead = 16: gap 16 (too small for a block, the request is
    // bumped to the next boundary); 32: gap 0; 48: gap 48 (split off);
    // 64: gap 32 (exactly the minimum block); 80: gap 16 again.
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(f->memory, 4096U));
    const u64 leads[] = {16U, 32U, 48U, 64U, 80U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(leads); ++i) {
        void *lead = nph_tlsf_alloc(&f->tlsf, leads[i], 16U);
        NTEST_ASSERT_NOT_NULL(lead);
        void *p = nph_tlsf_alloc(&f->tlsf, 64U, 64U);
        NTEST_ASSERT_NOT_NULL(p);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 64U));
        NTEST_ASSERT_GE_UINT(nph_tlsf_usable_size(p), 64U);
        memset(p, 0x77, 64U);
        VALIDATE(&f->tlsf);
        // The block after p must still be allocatable and the heap tidy.
        void *after = nph_tlsf_alloc(&f->tlsf, 16U, 16U);
        NTEST_ASSERT_NOT_NULL(after);
        VALIDATE(&f->tlsf);
        nph_tlsf_free(&f->tlsf, p);
        VALIDATE(&f->tlsf);
        nph_tlsf_free(&f->tlsf, after);
        nph_tlsf_free(&f->tlsf, lead);
        VALIDATE(&f->tlsf);
        NphTlsfStats s;
        nph_tlsf_get_stats(&f->tlsf, &s);
        NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
        NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
    }
}

NTEST_F(TlsfFixture, realloc_grows_in_place_when_the_next_block_is_free)
{
    u8 *p = (u8 *)nph_tlsf_alloc(&f->tlsf, 64U, 16U);
    test_fill_pattern(p, 64U, 1U);
    u8 *q = (u8 *)nph_tlsf_realloc(&f->tlsf, p, 4096U, 16U);
    NTEST_ASSERT_EQ_PTR(q, p);
    NTEST_ASSERT_TRUE(test_check_pattern(q, 64U, 1U));
    NTEST_ASSERT_EQ_UINT(nph_tlsf_usable_size(q), 4096U);
    VALIDATE(&f->tlsf);
    // Shrink in place.
    test_fill_pattern(q, 4096U, 2U);
    u8 *r = (u8 *)nph_tlsf_realloc(&f->tlsf, q, 100U, 16U);
    NTEST_ASSERT_EQ_PTR(r, p);
    NTEST_ASSERT_EQ_UINT(nph_tlsf_usable_size(r), 112U);
    NTEST_ASSERT_TRUE(test_check_pattern(r, 100U, 2U));
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_bytes, 112U);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
    nph_tlsf_free(&f->tlsf, r);
    VALIDATE(&f->tlsf);
}

NTEST_F(TlsfFixture, realloc_moves_when_blocked)
{
    u8 *p = (u8 *)nph_tlsf_alloc(&f->tlsf, 64U, 16U);
    void *blocker = nph_tlsf_alloc(&f->tlsf, 64U, 16U);
    test_fill_pattern(p, 64U, 3U);
    u8 *q = (u8 *)nph_tlsf_realloc(&f->tlsf, p, 1024U, 16U);
    NTEST_ASSERT_NOT_NULL(q);
    NTEST_ASSERT_NE_PTR(q, p);
    NTEST_ASSERT_TRUE(test_check_pattern(q, 64U, 3U));
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 2U);
    // Shrinking a block that is followed by a used block still splits.
    u8 *r = (u8 *)nph_tlsf_realloc(&f->tlsf, q, 16U, 16U);
    NTEST_ASSERT_EQ_PTR(r, q);
    VALIDATE(&f->tlsf);
    // realloc(NULL) allocates; realloc to an unsatisfiable size fails cleanly.
    void *fresh = nph_tlsf_realloc(&f->tlsf, NULL, 32U, 16U);
    NTEST_ASSERT_NOT_NULL(fresh);
    NTEST_ASSERT_NULL(nph_tlsf_realloc(&f->tlsf, r, NPH_MIB(2), 16U));
    NTEST_ASSERT_TRUE(test_check_pattern(r, 16U, 3U));
    nph_tlsf_free(&f->tlsf, fresh);
    nph_tlsf_free(&f->tlsf, r);
    nph_tlsf_free(&f->tlsf, blocker);
    VALIDATE(&f->tlsf);
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
}

NTEST_F(TlsfFixture, realloc_with_higher_alignment_moves)
{
    u8 *p = (u8 *)nph_tlsf_alloc(&f->tlsf, 64U, 16U);
    test_fill_pattern(p, 64U, 4U);
    u8 *q = (u8 *)nph_tlsf_realloc(&f->tlsf, p, 64U, 1024U);
    NTEST_ASSERT_NOT_NULL(q);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(q, 1024U));
    NTEST_ASSERT_TRUE(test_check_pattern(q, 64U, 4U));
    VALIDATE(&f->tlsf);
    nph_tlsf_free(&f->tlsf, q);
    VALIDATE(&f->tlsf);
}

NTEST_F(TlsfFixture, exhaustion_returns_null_and_heap_stays_valid)
{
    NTEST_ASSERT_NULL(nph_tlsf_alloc(&f->tlsf, f->size, 16U));
    NTEST_ASSERT_NULL(nph_tlsf_alloc(&f->tlsf, NPH_TLSF_BLOCK_SIZE_MAX, 16U));
    NTEST_ASSERT_NULL(nph_tlsf_alloc(&f->tlsf, NPH_U64_MAX, 16U));
    void *all = nph_tlsf_alloc(&f->tlsf, f->size - 32U, 16U);
    NTEST_ASSERT_NOT_NULL(all);
    NTEST_ASSERT_NULL(nph_tlsf_alloc(&f->tlsf, 1U, 1U));
    VALIDATE(&f->tlsf);
    nph_tlsf_free(&f->tlsf, all);
    VALIDATE(&f->tlsf);
    // Fill with many small blocks until it refuses, then release them all.
    void *blocks[4096];
    u32 n = 0U;
    while (n < NPH_ARRAY_COUNT(blocks)) {
        blocks[n] = nph_tlsf_alloc(&f->tlsf, 1000U, 16U);
        if (blocks[n] == NULL) {
            break;
        }
        ++n;
    }
    NTEST_ASSERT_GT_UINT(n, 900U);
    NTEST_ASSERT_LT_UINT(n, 1050U);
    VALIDATE(&f->tlsf);
    for (u32 i = 0; i < n; i += 2U) {
        nph_tlsf_free(&f->tlsf, blocks[i]);
    }
    for (u32 i = 1; i < n; i += 2U) {
        nph_tlsf_free(&f->tlsf, blocks[i]);
    }
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.free_count, 1U);
}

NTEST_F(TlsfFixture, double_free_is_detected)
{
    void *p = nph_tlsf_alloc(&f->tlsf, 64U, 16U);
    nph_tlsf_free(&f->tlsf, p);
    NPH_TEST_EXPECT_ASSERT(nph_tlsf_free(&f->tlsf, p));
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
}

typedef struct WalkRecorder {
    u32 blocks;
    u32 used;
    u64 bytes;
} WalkRecorder;

static void record_walk(void *payload, u64 size, bool used, void *user)
{
    WalkRecorder *w = (WalkRecorder *)user;
    NTEST_ASSERT_NOT_NULL(payload);
    w->blocks += 1U;
    w->used += used ? 1U : 0U;
    w->bytes += size;
}

NTEST_F(TlsfFixture, walk_visits_every_block)
{
    void *a = nph_tlsf_alloc(&f->tlsf, 100U, 16U);
    void *b = nph_tlsf_alloc(&f->tlsf, 200U, 16U);
    void *c = nph_tlsf_alloc(&f->tlsf, 300U, 16U);
    nph_tlsf_free(&f->tlsf, b);
    WalkRecorder w = {0U, 0U, 0U};
    nph_tlsf_walk(&f->tlsf, record_walk, &w);
    NTEST_ASSERT_EQ_UINT(w.blocks, 4U); // a, free b, c, free tail
    NTEST_ASSERT_EQ_UINT(w.used, 2U);
    NTEST_ASSERT_EQ_UINT(w.bytes, f->size - 32U - 3U * 16U); // minus 4 headers... one is the first
    nph_tlsf_free(&f->tlsf, a);
    nph_tlsf_free(&f->tlsf, c);
    VALIDATE(&f->tlsf);
}

NTEST_F(TlsfFixture, vtable_interface)
{
    NphAllocator *a = nph_tlsf_allocator(&f->tlsf);
    u8 *p = (u8 *)NPH_ALLOC(a, 40U, 32U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 32U));
    test_fill_pattern(p, 40U, 8U);
    p = (u8 *)NPH_REALLOC(a, p, 40U, 400U, 32U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, 32U));
    NTEST_ASSERT_TRUE(test_check_pattern(p, 40U, 8U));
    NPH_FREE(a, p, 400U, 32U);
    VALIDATE(&f->tlsf);
    NphTlsfStats s;
    nph_tlsf_get_stats(&f->tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 0U);
}

NTEST(tlsf, init_rejects_bad_regions)
{
    NphTlsf tlsf;
    NPH_ALIGNAS(16) u8 small[NPH_TLSF_MIN_POOL_SIZE - 1U];
    NTEST_ASSERT_EQ_INT(nph_tlsf_init(&tlsf, small, sizeof small), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_tlsf_init(&tlsf, NULL, 4096U), NPH_RESULT_INVALID_ARGUMENT);
    NPH_ALIGNAS(16) u8 ok[NPH_TLSF_MIN_POOL_SIZE];
    NTEST_ASSERT_EQ_INT(nph_tlsf_init(&tlsf, ok, sizeof ok), NPH_RESULT_OK);
    void *p = nph_tlsf_alloc(&tlsf, 16U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    nph_tlsf_free(&tlsf, p);
    nph_tlsf_destroy(&tlsf);
    nph_tlsf_destroy(&tlsf);
}

NTEST(tlsf, unaligned_region_is_trimmed)
{
    NphTlsf tlsf;
    NPH_ALIGNAS(16) u8 memory[1024];
    NTEST_ASSERT_EQ_INT(nph_tlsf_init(&tlsf, memory + 3, 1000U), NPH_RESULT_OK);
    const char *reason = NULL;
    NTEST_ASSERT_EQ_INT(nph_tlsf_validate(&tlsf, &reason), NPH_RESULT_OK);
    NphTlsfStats s;
    nph_tlsf_get_stats(&tlsf, &s);
    // 1000 - 13 slack = 987 -> 976 usable -> 944 payload
    NTEST_ASSERT_EQ_UINT(s.free_bytes, 944U);
    void *p = nph_tlsf_alloc(&tlsf, 944U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_NULL(nph_tlsf_alloc(&tlsf, 1U, 1U));
    nph_tlsf_free(&tlsf, p);
    nph_tlsf_destroy(&tlsf);
}

NTEST(tlsf, init_from_and_pools)
{
    NphTlsf tlsf;
    NphAllocator *sys = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_tlsf_init_from(&tlsf, sys, NPH_KIB(64)), NPH_RESULT_OK);
    void *big = nph_tlsf_alloc(&tlsf, NPH_KIB(60), 16U);
    NTEST_ASSERT_NOT_NULL(big);
    NTEST_ASSERT_NULL(nph_tlsf_alloc(&tlsf, NPH_KIB(8), 16U));

    // Growth by adding a second pool, from the backing allocator.
    NTEST_ASSERT_EQ_INT(nph_tlsf_add_pool_from(&tlsf, sys, NPH_KIB(16)), NPH_RESULT_OK);
    void *more = nph_tlsf_alloc(&tlsf, NPH_KIB(8), 16U);
    NTEST_ASSERT_NOT_NULL(more);
    NTEST_ASSERT_TRUE(nph_tlsf_owns(&tlsf, more));
    const char *reason = NULL;
    NTEST_ASSERT_EQ_INT(nph_tlsf_validate(&tlsf, &reason), NPH_RESULT_OK);
    NphTlsfStats s;
    nph_tlsf_get_stats(&tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.pool_count, 2U);
    NTEST_ASSERT_EQ_UINT(s.pool_bytes, NPH_KIB(80));

    // A caller-owned third pool can be removed once it is empty.
    NPH_ALIGNAS(16) static u8 region[4096];
    NTEST_ASSERT_EQ_INT(nph_tlsf_add_pool(&tlsf, region, sizeof region), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_tlsf_remove_pool(&tlsf, region + 1), NPH_RESULT_NOT_FOUND);
    void *in_region = NULL;
    // Force an allocation into the region by exhausting the others' space.
    for (u32 i = 0; i < 64U; ++i) {
        void *p = nph_tlsf_alloc(&tlsf, 3000U, 16U);
        if (p == NULL) {
            break;
        }
        if ((u8 *)p >= region && (u8 *)p < region + sizeof region) {
            in_region = p;
            break;
        }
    }
    NTEST_ASSERT_NOT_NULL(in_region);
    NTEST_ASSERT_EQ_INT(nph_tlsf_remove_pool(&tlsf, region), NPH_RESULT_INVALID_ARGUMENT);
    nph_tlsf_free(&tlsf, in_region);
    NTEST_ASSERT_EQ_INT(nph_tlsf_remove_pool(&tlsf, region), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_tlsf_owns(&tlsf, region));
    NTEST_ASSERT_EQ_INT(nph_tlsf_validate(&tlsf, &reason), NPH_RESULT_OK);

    // Destroy returns the backing pools; the live blocks are abandoned by design.
    nph_tlsf_destroy(&tlsf);
    NTEST_ASSERT_EQ_UINT(tlsf.pool_count, 0U);

    NTEST_ASSERT_EQ_INT(nph_tlsf_init_from(&tlsf, sys, 16U), NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(tlsf, pool_limit)
{
    NphTlsf tlsf;
    static NPH_ALIGNAS(16) u8 regions[NPH_TLSF_MAX_POOLS + 1U][NPH_TLSF_MIN_POOL_SIZE];
    NTEST_ASSERT_EQ_INT(nph_tlsf_init(&tlsf, regions[0], NPH_TLSF_MIN_POOL_SIZE), NPH_RESULT_OK);
    for (u32 i = 1; i < NPH_TLSF_MAX_POOLS; ++i) {
        NTEST_ASSERT_EQ_INT(nph_tlsf_add_pool(&tlsf, regions[i], NPH_TLSF_MIN_POOL_SIZE),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(
        nph_tlsf_add_pool(&tlsf, regions[NPH_TLSF_MAX_POOLS], NPH_TLSF_MIN_POOL_SIZE),
        NPH_RESULT_FULL);
    const char *reason = NULL;
    NTEST_ASSERT_EQ_INT(nph_tlsf_validate(&tlsf, &reason), NPH_RESULT_OK);
    NphTlsfStats s;
    nph_tlsf_get_stats(&tlsf, &s);
    NTEST_ASSERT_EQ_UINT(s.pool_count, NPH_TLSF_MAX_POOLS);
    nph_tlsf_destroy(&tlsf);
}

// Randomised model check: random allocate/free/realloc with pattern-filled
// payloads, validating the heap after every operation.
typedef struct Slot {
    u8 *ptr;
    u64 size;
    u32 align;
} Slot;

static void stress(NphTlsf *tlsf, u64 seed, u32 ops, u32 slot_count, u64 max_size,
                   u32 validate_every)
{
    Slot slots[256];
    NTEST_ASSERT_LE_UINT(slot_count, NPH_ARRAY_COUNT(slots));
    memset(slots, 0, sizeof slots);
    u64 rng = seed;
    u32 allocated = 0U;
    for (u32 op = 0; op < ops; ++op) {
        Slot *s = &slots[test_rng_below(&rng, slot_count)];
        const u32 action = (u32)test_rng_below(&rng, 10U);
        if (s->ptr == NULL) {
            s->size = 1U + test_rng_below(&rng, max_size);
            s->align = 1U << (u32)test_rng_below(&rng, 9U); // 1..256
            s->ptr = (u8 *)nph_tlsf_alloc(tlsf, s->size, s->align);
            if (s->ptr != NULL) {
                NTEST_ASSERT_TRUE(nph_is_ptr_aligned(s->ptr, s->align));
                NTEST_ASSERT_GE_UINT(nph_tlsf_usable_size(s->ptr), s->size);
                test_fill_pattern(s->ptr, s->size, (u64)op);
                s->size |= (u64)op << 40U; // stash the pattern seed in the high bits
                ++allocated;
            }
        } else {
            const u64 size = s->size & 0xFFFFFFFFFFULL;
            const u64 pattern = s->size >> 40U;
            NTEST_ASSERT_TRUE(test_check_pattern(s->ptr, size, pattern));
            if (action < 7U) {
                nph_tlsf_free(tlsf, s->ptr);
                s->ptr = NULL;
            } else {
                const u64 new_size = 1U + test_rng_below(&rng, max_size);
                u8 *moved = (u8 *)nph_tlsf_realloc(tlsf, s->ptr, new_size, s->align);
                if (moved != NULL) {
                    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(moved, s->align));
                    NTEST_ASSERT_TRUE(
                        test_check_pattern(moved, nph_min_u64(size, new_size), pattern));
                    test_fill_pattern(moved, new_size, (u64)op);
                    s->ptr = moved;
                    s->size = new_size | ((u64)op << 40U);
                } else {
                    NTEST_ASSERT_TRUE(test_check_pattern(s->ptr, size, pattern));
                }
            }
        }
        if (op % validate_every == 0U) {
            VALIDATE(tlsf);
        }
    }
    NTEST_ASSERT_GT_UINT(allocated, 0U);
    for (u32 i = 0; i < slot_count; ++i) {
        if (slots[i].ptr != NULL) {
            NTEST_ASSERT_TRUE(test_check_pattern(slots[i].ptr, slots[i].size & 0xFFFFFFFFFFULL,
                                                 slots[i].size >> 40U));
            nph_tlsf_free(tlsf, slots[i].ptr);
        }
    }
    VALIDATE(tlsf);
}

NTEST_F(TlsfFixture, randomized_small_blocks)
{
    stress(&f->tlsf, 0x1234U, 20000U, 256U, 512U, 16U);
}

NTEST_F(TlsfFixture, randomized_large_blocks_with_exhaustion)
{
    // Leave 128 KiB: 64 slots x up to 16 KiB in it means allocations fail
    // regularly and every size class up to the pool size is exercised.
    void *blocker = nph_tlsf_alloc(&f->tlsf, f->size - NPH_KIB(128), 16U);
    NTEST_ASSERT_NOT_NULL(blocker);
    stress(&f->tlsf, 0xBEEFU, 6000U, 64U, NPH_KIB(16), 64U);
    nph_tlsf_free(&f->tlsf, blocker);
    VALIDATE(&f->tlsf);
}

NTEST_F(TlsfFixture, randomized_mixed_sizes)
{
    stress(&f->tlsf, 0xC0FFEEU, 10000U, 64U, NPH_KIB(4), 32U);
}

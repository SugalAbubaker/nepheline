// SPDX-License-Identifier: Apache-2.0
// Single-threaded semantics of the rings: ordering, capacity, wrap-around.
// Concurrent stress tests arrive with platform threads in Phase 3.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/ring.h>

#include "test_support.h"

#include <string.h>

typedef struct Msg {
    u64 id;
    u32 payload[5];
} Msg;

static Msg make_msg(u64 id)
{
    Msg m;
    memset(&m, 0, sizeof m); // the ring copies padding bytes too; keep them defined
    m.id = id;
    for (u32 i = 0; i < 5U; ++i) {
        m.payload[i] = (u32)(id * 31U + i);
    }
    return m;
}

typedef struct RingFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} RingFixture;

NTEST_SETUP(RingFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(RingFixture)
{
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(RingFixture, spsc_fifo_order_capacity_and_wraparound)
{
    NphSpscRing ring;
    NTEST_ASSERT_EQ_INT(nph_spsc_ring_init(&ring, f->a, sizeof(Msg), 8U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_spsc_ring_capacity(&ring), 8U);
    NTEST_ASSERT_EQ_UINT(nph_spsc_ring_count(&ring), 0U);
    Msg out;
    NTEST_ASSERT_FALSE(nph_spsc_ring_pop(&ring, &out));
    NTEST_ASSERT_NULL(nph_spsc_ring_peek(&ring));

    for (u32 i = 0; i < 8U; ++i) {
        const Msg m = make_msg(i);
        NTEST_ASSERT_TRUE(nph_spsc_ring_push(&ring, &m));
        NTEST_ASSERT_EQ_UINT(nph_spsc_ring_count(&ring), i + 1U);
    }
    const Msg overflow = make_msg(99U);
    NTEST_ASSERT_FALSE(nph_spsc_ring_push(&ring, &overflow));
    NTEST_ASSERT_EQ_UINT(((const Msg *)nph_spsc_ring_peek(&ring))->id, 0U);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_TRUE(nph_spsc_ring_pop(&ring, &out));
        const Msg expected = make_msg(i);
        NTEST_ASSERT_EQ_MEM(&out, &expected, sizeof out);
    }
    NTEST_ASSERT_FALSE(nph_spsc_ring_pop(&ring, &out));

    // Many cycles so the 32-bit positions wrap the 8-slot buffer repeatedly,
    // with a varying fill level.
    u64 next_push = 100U;
    u64 next_pop = 100U;
    u64 rng = 7U;
    for (u32 step = 0; step < 100000U; ++step) {
        if (test_rng_below(&rng, 3U) != 0U) {
            const Msg m = make_msg(next_push);
            if (nph_spsc_ring_push(&ring, &m)) {
                ++next_push;
            } else {
                NTEST_ASSERT_EQ_UINT(next_push - next_pop, 8U);
            }
        } else if (nph_spsc_ring_pop(&ring, &out)) {
            NTEST_ASSERT_EQ_UINT(out.id, next_pop);
            ++next_pop;
        } else {
            NTEST_ASSERT_EQ_UINT(next_push, next_pop);
        }
        NTEST_ASSERT_EQ_UINT(nph_spsc_ring_count(&ring), (u32)(next_push - next_pop));
    }
    nph_spsc_ring_free(&ring);
    nph_spsc_ring_free(&ring);
}

NTEST_F(RingFixture, spsc_position_counter_wraps_safely)
{
    NphSpscRing ring;
    NTEST_ASSERT_EQ_INT(nph_spsc_ring_init(&ring, f->a, sizeof(u32), 4U), NPH_RESULT_OK);
    // Start both positions near the u32 limit.
    nph_atomic_store_u32(&ring.head, 0xFFFFFFFEU, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&ring.tail, 0xFFFFFFFEU, NPH_MEMORY_ORDER_RELAXED);
    ring.cached_head = 0xFFFFFFFEU;
    ring.cached_tail = 0xFFFFFFFEU;
    for (u32 i = 0; i < 20U; ++i) {
        NTEST_ASSERT_TRUE(nph_spsc_ring_push(&ring, &i));
        NTEST_ASSERT_TRUE(nph_spsc_ring_push(&ring, &i));
        u32 a;
        u32 b;
        NTEST_ASSERT_TRUE(nph_spsc_ring_pop(&ring, &a));
        NTEST_ASSERT_TRUE(nph_spsc_ring_pop(&ring, &b));
        NTEST_ASSERT_EQ_UINT(a, i);
        NTEST_ASSERT_EQ_UINT(b, i);
        NTEST_ASSERT_EQ_UINT(nph_spsc_ring_count(&ring), 0U);
    }
    nph_spsc_ring_free(&ring);
}

NTEST_F(RingFixture, spsc_init_rejects_bad_parameters)
{
    NphSpscRing ring;
    NTEST_ASSERT_EQ_INT(nph_spsc_ring_init(&ring, f->a, 0U, 8U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_spsc_ring_init(&ring, f->a, 4U, 1U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_spsc_ring_init(&ring, f->a, 4U, 6U), NPH_RESULT_INVALID_ARGUMENT);
    nph_spsc_ring_free(&ring); // zeroed by the failed init: harmless
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(&ring.head, NPH_CACHE_LINE_SIZE) ==
                      nph_is_ptr_aligned(&ring, NPH_CACHE_LINE_SIZE));
    NTEST_ASSERT_GE_UINT((uptr)&ring.head - (uptr)&ring.tail, NPH_CACHE_LINE_SIZE);
}

NTEST_F(RingFixture, mpmc_fifo_order_capacity_and_wraparound)
{
    NphMpmcRing ring;
    NTEST_ASSERT_EQ_INT(nph_mpmc_ring_init(&ring, f->a, sizeof(Msg), 16U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_mpmc_ring_capacity(&ring), 16U);
    NTEST_ASSERT_EQ_UINT(ring.elem_offset, 16U);
    NTEST_ASSERT_EQ_UINT(ring.stride, 48U);
    Msg out;
    NTEST_ASSERT_FALSE(nph_mpmc_ring_pop(&ring, &out));
    for (u32 i = 0; i < 16U; ++i) {
        const Msg m = make_msg(i);
        NTEST_ASSERT_TRUE(nph_mpmc_ring_push(&ring, &m));
    }
    const Msg overflow = make_msg(99U);
    NTEST_ASSERT_FALSE(nph_mpmc_ring_push(&ring, &overflow));
    NTEST_ASSERT_EQ_UINT(nph_mpmc_ring_count(&ring), 16U);
    for (u32 i = 0; i < 16U; ++i) {
        NTEST_ASSERT_TRUE(nph_mpmc_ring_pop(&ring, &out));
        NTEST_ASSERT_EQ_UINT(out.id, i);
    }
    NTEST_ASSERT_FALSE(nph_mpmc_ring_pop(&ring, &out));

    u64 next_push = 1000U;
    u64 next_pop = 1000U;
    u64 rng = 11U;
    for (u32 step = 0; step < 100000U; ++step) {
        if (test_rng_below(&rng, 3U) != 0U) {
            const Msg m = make_msg(next_push);
            if (nph_mpmc_ring_push(&ring, &m)) {
                ++next_push;
            } else {
                NTEST_ASSERT_EQ_UINT(next_push - next_pop, 16U);
            }
        } else if (nph_mpmc_ring_pop(&ring, &out)) {
            NTEST_ASSERT_EQ_UINT(out.id, next_pop);
            ++next_pop;
        } else {
            NTEST_ASSERT_EQ_UINT(next_push, next_pop);
        }
        NTEST_ASSERT_EQ_UINT(nph_mpmc_ring_count(&ring), (u32)(next_push - next_pop));
    }
    nph_mpmc_ring_free(&ring);
    nph_mpmc_ring_free(&ring);
}

NTEST_F(RingFixture, mpmc_small_elements_and_sequence_wrap)
{
    NphMpmcRing ring;
    NTEST_ASSERT_EQ_INT(nph_mpmc_ring_init(&ring, f->a, sizeof(u8), 2U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(ring.elem_offset, 8U);
    NTEST_ASSERT_EQ_UINT(ring.stride, 16U);
    // Push the positions close to the u32 limit so sequences wrap.
    nph_atomic_store_u32(&ring.enqueue_pos, 0xFFFFFFF0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&ring.dequeue_pos, 0xFFFFFFF0U, NPH_MEMORY_ORDER_RELAXED);
    for (u32 i = 0; i < 2U; ++i) {
        NphAtomicU32 *seq = (NphAtomicU32 *)(ring.cells + (u64)i * ring.stride);
        nph_atomic_store_u32(seq, 0xFFFFFFF0U + i, NPH_MEMORY_ORDER_RELAXED);
    }
    for (u32 i = 0; i < 64U; ++i) {
        const u8 v = (u8)i;
        NTEST_ASSERT_TRUE(nph_mpmc_ring_push(&ring, &v));
        u8 out = 0xFFU;
        NTEST_ASSERT_TRUE(nph_mpmc_ring_pop(&ring, &out));
        NTEST_ASSERT_EQ_UINT(out, (u8)i);
    }
    NTEST_ASSERT_EQ_UINT(nph_mpmc_ring_count(&ring), 0U);
    nph_mpmc_ring_free(&ring);

    NTEST_ASSERT_EQ_INT(nph_mpmc_ring_init(&ring, f->a, 0U, 2U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_mpmc_ring_init(&ring, f->a, 1U, 3U), NPH_RESULT_INVALID_ARGUMENT);
}

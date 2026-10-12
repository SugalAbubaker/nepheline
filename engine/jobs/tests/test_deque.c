// SPDX-License-Identifier: Apache-2.0
// The work-stealing deque: single-threaded semantics, then the owner
// racing thieves with every element accounted for exactly once.
#include <nepheline/jobs/deque.h>

#include "test_jobs_support.h"

// ---------------------------------------------------------------------------
// Single-threaded
// ---------------------------------------------------------------------------

NTEST(deque, init_rejects_bad_capacity)
{
    NphJobDeque d;
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 24U),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_job_deque_free(&d); // safe on the zero-filled result
}

NTEST(deque, owner_take_is_lifo_and_steal_is_fifo)
{
    NphJobDeque d;
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 8U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_job_deque_capacity(&d), 8U);
    u32 v = 0U;
    NTEST_ASSERT_FALSE(nph_job_deque_take(&d, &v));
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal(&d, &v), NPH_JOB_DEQUE_EMPTY);
    NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 0U);

    for (u32 i = 1; i <= 4U; ++i) {
        NTEST_ASSERT_TRUE(nph_job_deque_push(&d, i * 10U));
    }
    NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 4U);
    NTEST_ASSERT_TRUE(nph_job_deque_take(&d, &v));
    NTEST_ASSERT_EQ_UINT(v, 40U);
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal(&d, &v), NPH_JOB_DEQUE_STOLEN);
    NTEST_ASSERT_EQ_UINT(v, 10U);
    NTEST_ASSERT_TRUE(nph_job_deque_take(&d, &v));
    NTEST_ASSERT_EQ_UINT(v, 30U);
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal(&d, &v), NPH_JOB_DEQUE_STOLEN);
    NTEST_ASSERT_EQ_UINT(v, 20U);
    NTEST_ASSERT_FALSE(nph_job_deque_take(&d, &v));
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal(&d, &v), NPH_JOB_DEQUE_EMPTY);
    nph_job_deque_free(&d);
}

NTEST(deque, push_refuses_when_full_and_recovers)
{
    NphJobDeque d;
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 4U), NPH_RESULT_OK);
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_TRUE(nph_job_deque_push(&d, i));
    }
    NTEST_ASSERT_FALSE(nph_job_deque_push(&d, 99U));
    NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 4U);
    u32 v = 0U;
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal(&d, &v), NPH_JOB_DEQUE_STOLEN);
    NTEST_ASSERT_EQ_UINT(v, 0U);
    NTEST_ASSERT_TRUE(nph_job_deque_push(&d, 99U));
    NTEST_ASSERT_TRUE(nph_job_deque_take(&d, &v));
    NTEST_ASSERT_EQ_UINT(v, 99U);
    nph_job_deque_free(&d);
}

// Many pushes and takes beyond the capacity: the positions keep growing
// and the buffer wraps.
NTEST(deque, wraps_around_the_buffer)
{
    NphJobDeque d;
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 16U), NPH_RESULT_OK);
    u32 next = 0U;
    for (u32 round = 0; round < 1000U; ++round) {
        for (u32 i = 0; i < 12U; ++i) {
            NTEST_ASSERT_TRUE(nph_job_deque_push(&d, next++));
        }
        // Mix takes and steals; the set drained must be exactly what went in.
        u32 v = 0U;
        for (u32 i = 0; i < 6U; ++i) {
            NTEST_ASSERT_TRUE(nph_job_deque_take(&d, &v));
            NTEST_ASSERT_EQ_UINT(v, next - 1U - i);
        }
        for (u32 i = 0; i < 6U; ++i) {
            NTEST_ASSERT_EQ_INT(nph_job_deque_steal(&d, &v), NPH_JOB_DEQUE_STOLEN);
            NTEST_ASSERT_EQ_UINT(v, next - 12U + i);
        }
        NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 0U);
    }
    nph_job_deque_free(&d);
}

static bool is_even(u32 value, void *ctx)
{
    NPH_UNUSED(ctx);
    return (value & 1U) == 0U;
}

static bool above(u32 value, void *ctx)
{
    return value > *(const u32 *)ctx;
}

// take_if and steal_if show the element they would remove to the
// predicate and leave it in place when it declines.
NTEST(deque, conditional_take_and_steal_leave_refused_elements)
{
    NphJobDeque d;
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&d, nph_allocator_system(), 8U), NPH_RESULT_OK);
    u32 v = 0U;
    NTEST_ASSERT_FALSE(nph_job_deque_take_if(&d, is_even, NULL, &v));
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal_if(&d, is_even, NULL, &v), NPH_JOB_DEQUE_EMPTY);
    for (u32 i = 1; i <= 5U; ++i) {
        NTEST_ASSERT_TRUE(nph_job_deque_push(&d, i)); // 1 2 3 4 5, 5 at the bottom
    }
    // The owner sees 5 (odd) and refuses; the deque is untouched.
    NTEST_ASSERT_FALSE(nph_job_deque_take_if(&d, is_even, NULL, &v));
    NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 5U);
    // A thief sees 1 (odd) and refuses.
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal_if(&d, is_even, NULL, &v), NPH_JOB_DEQUE_REFUSED);
    NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 5U);
    u32 limit = 4U;
    NTEST_ASSERT_TRUE(nph_job_deque_take_if(&d, above, &limit, &v));
    NTEST_ASSERT_EQ_UINT(v, 5U);
    NTEST_ASSERT_FALSE(nph_job_deque_take_if(&d, above, &limit, &v)); // 4 is not above 4
    NTEST_ASSERT_TRUE(nph_job_deque_take_if(&d, is_even, NULL, &v));
    NTEST_ASSERT_EQ_UINT(v, 4U);
    limit = 0U;
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal_if(&d, above, &limit, &v), NPH_JOB_DEQUE_STOLEN);
    NTEST_ASSERT_EQ_UINT(v, 1U);
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal_if(&d, is_even, NULL, &v), NPH_JOB_DEQUE_STOLEN);
    NTEST_ASSERT_EQ_UINT(v, 2U);
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal_if(&d, is_even, NULL, &v), NPH_JOB_DEQUE_REFUSED);
    NTEST_ASSERT_TRUE(nph_job_deque_take(&d, &v)); // the plain take is unconditional
    NTEST_ASSERT_EQ_UINT(v, 3U);
    NTEST_ASSERT_EQ_UINT(nph_job_deque_count(&d), 0U);
    NTEST_ASSERT_EQ_INT(nph_job_deque_steal_if(&d, is_even, NULL, &v), NPH_JOB_DEQUE_EMPTY);
    nph_job_deque_free(&d);
}

// ---------------------------------------------------------------------------
// Owner versus thieves
// ---------------------------------------------------------------------------

#define DEQUE_ITEMS_MAX (1U << 20U)

typedef struct Contended {
    NphJobDeque deque;
    u32 items;               // values 1..items are pushed, in order
    NphAtomicU32 owner_done; // the owner has pushed everything and drained
    NphAtomicU32 stop;       // thieves may exit
    NphAtomicU32 seen_count; // elements taken by anyone
    NphAtomicU32 duplicates; // an element taken twice
    NphAtomicU32 owner_taken;
    NphAtomicU32 stolen;
    NphAtomicU32 aborts;
    NphAtomicU32 order_violations; // a thief saw values go backwards
    u8 *seen;                      // one byte per value
} Contended;

static void deque_thief(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Contended *c = (Contended *)shared;
    u32 last = 0U;
    for (;;) {
        u32 v = 0U;
        const NphJobDequeSteal r = nph_job_deque_steal(&c->deque, &v);
        if (r == NPH_JOB_DEQUE_STOLEN) {
            NTEST_UNUSED(v);
            if (c->seen[v] != 0U) {
                nph_atomic_fetch_add_u32(&c->duplicates, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            c->seen[v] = 1U; // each value is written by its one taker
            // Steals come from the top: a thief's successive values increase.
            if (v <= last) {
                nph_atomic_fetch_add_u32(&c->order_violations, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            last = v;
            nph_atomic_fetch_add_u32(&c->stolen, 1U, NPH_MEMORY_ORDER_RELAXED);
            nph_atomic_fetch_add_u32(&c->seen_count, 1U, NPH_MEMORY_ORDER_RELAXED);
        } else if (r == NPH_JOB_DEQUE_ABORT) {
            nph_atomic_fetch_add_u32(&c->aborts, 1U, NPH_MEMORY_ORDER_RELAXED);
        } else if (nph_atomic_load_u32(&c->stop, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
            return;
        } else {
            nph_cpu_relax();
        }
    }
}

NTEST(deque, owner_and_thieves_take_every_element_once)
{
    Contended c;
    memset(&c, 0, sizeof c);
    c.items = 200000U * TEST_SCALE;
    if (c.items > DEQUE_ITEMS_MAX) {
        c.items = DEQUE_ITEMS_MAX;
    }
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&c.deque, nph_allocator_system(), 256U), NPH_RESULT_OK);
    c.seen = (u8 *)NPH_ALLOC_ZEROED(nph_allocator_system(), c.items + 1U, 16U);
    NTEST_ASSERT_NOT_NULL(c.seen);

    const u32 thieves = test_jobs_worker_count(7U);
    TestThread threads[8];
    test_threads_start(threads, thieves, deque_thief, &c);

    // The owner pushes in bursts and takes some back itself, so the
    // last-element race between take and steal is exercised constantly.
    u64 rng = 7U;
    u32 next = 1U;
    while (next <= c.items) {
        const u32 burst = (u32)(test_rng_next(&rng) % 16U) + 1U;
        for (u32 i = 0; i < burst && next <= c.items; ++i) {
            while (!nph_job_deque_push(&c.deque, next)) {
                nph_cpu_relax(); // thieves will make room
            }
            ++next;
        }
        const u32 takes = (u32)(test_rng_next(&rng) % 8U);
        for (u32 i = 0; i < takes; ++i) {
            u32 v = 0U;
            if (!nph_job_deque_take(&c.deque, &v)) {
                break;
            }
            if (c.seen[v] != 0U) {
                nph_atomic_fetch_add_u32(&c.duplicates, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            c.seen[v] = 1U;
            nph_atomic_fetch_add_u32(&c.owner_taken, 1U, NPH_MEMORY_ORDER_RELAXED);
            nph_atomic_fetch_add_u32(&c.seen_count, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
    }
    // Drain whatever is left, racing the thieves for it.
    for (;;) {
        u32 v = 0U;
        if (!nph_job_deque_take(&c.deque, &v)) {
            if (nph_job_deque_count(&c.deque) == 0U) {
                break;
            }
            continue;
        }
        if (c.seen[v] != 0U) {
            nph_atomic_fetch_add_u32(&c.duplicates, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
        c.seen[v] = 1U;
        nph_atomic_fetch_add_u32(&c.owner_taken, 1U, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_fetch_add_u32(&c.seen_count, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_atomic_store_u32(&c.stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    test_threads_join(threads, thieves);

    NTEST_LOG("items %u: owner took %u, thieves stole %u, aborts %u", c.items,
              nph_atomic_load_u32(&c.owner_taken, NPH_MEMORY_ORDER_RELAXED),
              nph_atomic_load_u32(&c.stolen, NPH_MEMORY_ORDER_RELAXED),
              nph_atomic_load_u32(&c.aborts, NPH_MEMORY_ORDER_RELAXED));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.seen_count, NPH_MEMORY_ORDER_RELAXED), c.items);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.duplicates, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.order_violations, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_GT_UINT(nph_atomic_load_u32(&c.stolen, NPH_MEMORY_ORDER_RELAXED), 0U);
    for (u32 v = 1; v <= c.items; ++v) {
        if (c.seen[v] != 1U) {
            NTEST_FAIL("value %u was never taken", v);
        }
    }
    NPH_FREE(nph_allocator_system(), c.seen, c.items + 1U, 16U);
    nph_job_deque_free(&c.deque);
}

// The owner keeps the deque at one element so that nearly every take races
// a steal for the last element: the compare-exchange must give it to
// exactly one side.
NTEST(deque, last_element_race_never_duplicates)
{
    Contended c;
    memset(&c, 0, sizeof c);
    c.items = 100000U * TEST_SCALE;
    if (c.items > DEQUE_ITEMS_MAX) {
        c.items = DEQUE_ITEMS_MAX;
    }
    NTEST_ASSERT_EQ_INT(nph_job_deque_init(&c.deque, nph_allocator_system(), 4U), NPH_RESULT_OK);
    c.seen = (u8 *)NPH_ALLOC_ZEROED(nph_allocator_system(), c.items + 1U, 16U);
    NTEST_ASSERT_NOT_NULL(c.seen);
    const u32 thieves = test_jobs_worker_count(3U);
    TestThread threads[4];
    test_threads_start(threads, thieves, deque_thief, &c);
    for (u32 v = 1; v <= c.items; ++v) {
        while (!nph_job_deque_push(&c.deque, v)) {
            nph_cpu_relax();
        }
        u32 got = 0U;
        if (nph_job_deque_take(&c.deque, &got)) {
            if (c.seen[got] != 0U) {
                nph_atomic_fetch_add_u32(&c.duplicates, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            c.seen[got] = 1U;
            nph_atomic_fetch_add_u32(&c.owner_taken, 1U, NPH_MEMORY_ORDER_RELAXED);
            nph_atomic_fetch_add_u32(&c.seen_count, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
    }
    while (nph_atomic_load_u32(&c.seen_count, NPH_MEMORY_ORDER_ACQUIRE) != c.items) {
        nph_thread_yield();
    }
    nph_atomic_store_u32(&c.stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    test_threads_join(threads, thieves);
    NTEST_LOG("owner took %u, thieves stole %u",
              nph_atomic_load_u32(&c.owner_taken, NPH_MEMORY_ORDER_RELAXED),
              nph_atomic_load_u32(&c.stolen, NPH_MEMORY_ORDER_RELAXED));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.duplicates, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.order_violations, NPH_MEMORY_ORDER_RELAXED), 0U);
    NPH_FREE(nph_allocator_system(), c.seen, c.items + 1U, 16U);
    nph_job_deque_free(&c.deque);
}

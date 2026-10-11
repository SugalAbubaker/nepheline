// SPDX-License-Identifier: Apache-2.0
// Single-threaded semantics of the atomic wrappers. Multi-threaded stress
// tests arrive with the platform threads in Phase 3.
#include <nepheline/core/atomic.h>

#include <ntest/ntest.h>

NTEST(atomic, u32_load_store_every_order)
{
    NphAtomicU32 a = NPH_ATOMIC_U32_INIT(7U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 7U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_ACQUIRE), 7U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_SEQ_CST), 7U);
    nph_atomic_store_u32(&a, 8U, NPH_MEMORY_ORDER_RELAXED);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 8U);
    nph_atomic_store_u32(&a, 9U, NPH_MEMORY_ORDER_RELEASE);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_ACQUIRE), 9U);
    nph_atomic_store_u32(&a, 0xFFFFFFFFU, NPH_MEMORY_ORDER_SEQ_CST);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_SEQ_CST), 0xFFFFFFFFU);
}

NTEST(atomic, u32_rmw)
{
    NphAtomicU32 a = NPH_ATOMIC_U32_INIT(10U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_exchange_u32(&a, 20U, NPH_MEMORY_ORDER_ACQ_REL), 10U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_add_u32(&a, 5U, NPH_MEMORY_ORDER_RELAXED), 20U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_sub_u32(&a, 3U, NPH_MEMORY_ORDER_RELAXED), 25U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_or_u32(&a, 0x100U, NPH_MEMORY_ORDER_RELAXED), 22U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_and_u32(&a, 0x10FU, NPH_MEMORY_ORDER_RELAXED), 0x116U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 0x106U);
    // Wrap-around is defined (modular).
    nph_atomic_store_u32(&a, 0xFFFFFFFFU, NPH_MEMORY_ORDER_RELAXED);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_add_u32(&a, 2U, NPH_MEMORY_ORDER_RELAXED), 0xFFFFFFFFU);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 1U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_sub_u32(&a, 2U, NPH_MEMORY_ORDER_RELAXED), 1U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 0xFFFFFFFFU);
}

NTEST(atomic, u32_compare_exchange)
{
    NphAtomicU32 a = NPH_ATOMIC_U32_INIT(1U);
    u32 expected = 1U;
    NTEST_ASSERT_TRUE(nph_atomic_compare_exchange_u32(&a, &expected, 2U, NPH_MEMORY_ORDER_ACQ_REL,
                                                      NPH_MEMORY_ORDER_ACQUIRE));
    NTEST_ASSERT_EQ_UINT(expected, 1U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 2U);
    expected = 1U;
    NTEST_ASSERT_FALSE(nph_atomic_compare_exchange_u32(&a, &expected, 3U, NPH_MEMORY_ORDER_SEQ_CST,
                                                       NPH_MEMORY_ORDER_RELAXED));
    NTEST_ASSERT_EQ_UINT(expected, 2U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&a, NPH_MEMORY_ORDER_RELAXED), 2U);
}

NTEST(atomic, u64_operations)
{
    NphAtomicU64 a = NPH_ATOMIC_U64_INIT(0x100000000ULL);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&a, NPH_MEMORY_ORDER_ACQUIRE), 0x100000000ULL);
    nph_atomic_store_u64(&a, 0xFFFFFFFFFFFFFFFFULL, NPH_MEMORY_ORDER_RELEASE);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_add_u64(&a, 1U, NPH_MEMORY_ORDER_ACQ_REL),
                         0xFFFFFFFFFFFFFFFFULL);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&a, NPH_MEMORY_ORDER_SEQ_CST), 0U);
    nph_atomic_store_u64(&a, 0x0F0F0F0F0F0F0F0FULL, NPH_MEMORY_ORDER_SEQ_CST);
    NTEST_ASSERT_EQ_UINT(
        nph_atomic_fetch_or_u64(&a, 0xF000000000000000ULL, NPH_MEMORY_ORDER_RELAXED),
        0x0F0F0F0F0F0F0F0FULL);
    NTEST_ASSERT_EQ_UINT(
        nph_atomic_fetch_and_u64(&a, 0xFF00000000000000ULL, NPH_MEMORY_ORDER_RELAXED),
        0xFF0F0F0F0F0F0F0FULL);
    NTEST_ASSERT_EQ_UINT(nph_atomic_exchange_u64(&a, 5U, NPH_MEMORY_ORDER_RELAXED),
                         0xFF00000000000000ULL);
    NTEST_ASSERT_EQ_UINT(nph_atomic_fetch_sub_u64(&a, 6U, NPH_MEMORY_ORDER_RELAXED), 5U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&a, NPH_MEMORY_ORDER_RELAXED), 0xFFFFFFFFFFFFFFFFULL);
    u64 expected = 0xFFFFFFFFFFFFFFFFULL;
    NTEST_ASSERT_TRUE(nph_atomic_compare_exchange_u64(&a, &expected, 9U, NPH_MEMORY_ORDER_ACQ_REL,
                                                      NPH_MEMORY_ORDER_ACQUIRE));
    expected = 0U;
    NTEST_ASSERT_FALSE(nph_atomic_compare_exchange_u64(&a, &expected, 10U, NPH_MEMORY_ORDER_ACQ_REL,
                                                       NPH_MEMORY_ORDER_ACQUIRE));
    NTEST_ASSERT_EQ_UINT(expected, 9U);
}

NTEST(atomic, pointer_operations)
{
    int x = 0;
    int y = 0;
    NphAtomicPtr p = NPH_ATOMIC_PTR_INIT(&x);
    NTEST_ASSERT_EQ_PTR(nph_atomic_load_ptr(&p, NPH_MEMORY_ORDER_ACQUIRE), &x);
    nph_atomic_store_ptr(&p, &y, NPH_MEMORY_ORDER_RELEASE);
    NTEST_ASSERT_EQ_PTR(nph_atomic_load_ptr(&p, NPH_MEMORY_ORDER_RELAXED), &y);
    nph_atomic_store_ptr(&p, NULL, NPH_MEMORY_ORDER_SEQ_CST);
    NTEST_ASSERT_NULL(nph_atomic_load_ptr(&p, NPH_MEMORY_ORDER_SEQ_CST));
    NTEST_ASSERT_NULL(nph_atomic_exchange_ptr(&p, &x, NPH_MEMORY_ORDER_ACQ_REL));
    void *expected = &y;
    NTEST_ASSERT_FALSE(nph_atomic_compare_exchange_ptr(&p, &expected, &y, NPH_MEMORY_ORDER_ACQ_REL,
                                                       NPH_MEMORY_ORDER_ACQUIRE));
    NTEST_ASSERT_EQ_PTR(expected, &x);
    NTEST_ASSERT_TRUE(nph_atomic_compare_exchange_ptr(&p, &expected, &y, NPH_MEMORY_ORDER_ACQ_REL,
                                                      NPH_MEMORY_ORDER_ACQUIRE));
    NTEST_ASSERT_EQ_PTR(nph_atomic_load_ptr(&p, NPH_MEMORY_ORDER_RELAXED), &y);
}

NTEST(atomic, fences_and_relax_are_callable)
{
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_ACQUIRE);
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_RELEASE);
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_ACQ_REL);
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_SEQ_CST);
    nph_atomic_signal_fence(NPH_MEMORY_ORDER_SEQ_CST);
    nph_cpu_relax();
}

NTEST(atomic, spin_lock)
{
    NphSpinLock lock = NPH_SPIN_LOCK_INIT;
    NTEST_ASSERT_TRUE(nph_spin_lock_try_acquire(&lock));
    NTEST_ASSERT_FALSE(nph_spin_lock_try_acquire(&lock));
    nph_spin_lock_release(&lock);
    NTEST_ASSERT_TRUE(nph_spin_lock_try_acquire(&lock));
    nph_spin_lock_release(&lock);
    nph_spin_lock_acquire(&lock);
    NTEST_ASSERT_FALSE(nph_spin_lock_try_acquire(&lock));
    nph_spin_lock_release(&lock);

    NphSpinLock other;
    nph_spin_lock_init(&other);
    NTEST_ASSERT_TRUE(nph_spin_lock_try_acquire(&other));
    nph_spin_lock_release(&other);
}

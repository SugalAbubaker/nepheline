// SPDX-License-Identifier: Apache-2.0
#pragma once

// Atomic operations with explicit memory orders, over MSVC intrinsics and the
// GCC/Clang __atomic builtins (CLAUDE.md §8.3; lives in core by
// docs/adr/0005-atomics-in-core.md because the lock-free containers and the
// logger's per-thread rings need them and core may use compiler intrinsics).
//
// The semantics follow C11 <stdatomic.h>, which MSVC does not ship for C.
// Only the operations the engine uses are provided; add more here, never
// use raw intrinsics elsewhere.
//
// Atomic values are wrapped in structs so a plain read or write of the
// payload is a visible type error rather than a silent data race.
//
// Thread-safety: every function here is NPH_THREADSAFE by definition.

#include <nepheline/core/base.h>

#if NPH_COMPILER_MSVC
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphMemoryOrder {
    NPH_MEMORY_ORDER_RELAXED,
    NPH_MEMORY_ORDER_ACQUIRE,
    NPH_MEMORY_ORDER_RELEASE,
    NPH_MEMORY_ORDER_ACQ_REL,
    NPH_MEMORY_ORDER_SEQ_CST,
} NphMemoryOrder;

typedef struct NphAtomicU32 {
    u32 value; // never read or written directly
} NphAtomicU32;

typedef struct NphAtomicU64 {
    u64 value;
} NphAtomicU64;

typedef struct NphAtomicPtr {
    void *value;
} NphAtomicPtr;

// Static initialisers.
#define NPH_ATOMIC_U32_INIT(v) {(v)}
#define NPH_ATOMIC_U64_INIT(v) {(v)}
#define NPH_ATOMIC_PTR_INIT(p) {(p)}

NPH_STATIC_ASSERT(sizeof(NphAtomicU32) == 4, "NphAtomicU32 must be lock-free sized");
NPH_STATIC_ASSERT(sizeof(NphAtomicU64) == 8, "NphAtomicU64 must be lock-free sized");
NPH_STATIC_ASSERT(sizeof(NphAtomicPtr) == 8, "NphAtomicPtr must be lock-free sized");

// Hint to the core that the thread is spinning (PAUSE on x86).
NPH_INLINE void nph_cpu_relax(void)
{
#if NPH_ARCH_X64
#if NPH_COMPILER_MSVC
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif NPH_ARCH_ARM64
#if NPH_COMPILER_MSVC
    __yield();
#else
    __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
}

#if NPH_COMPILER_MSVC

// ---------------------------------------------------------------------------
// MSVC, x86-64 only. On x86 TSO every load is an acquire load and every store
// is a release store at the hardware level, so acquire/release orders need
// only a compiler barrier; seq_cst stores use a locked exchange; all
// Interlocked operations are full barriers. This mirrors the MSVC STL's
// <atomic> implementation. ARM64 support (with _acq/_rel intrinsics and DMB
// fences) is added when that platform arrives.
// ---------------------------------------------------------------------------

#if !NPH_ARCH_X64
#error "nepheline/core/atomic.h: MSVC atomics are implemented for x86-64 only"
#endif

#pragma warning(push)
#pragma warning(                                                                                   \
    disable : 4996) // _ReadWriteBarrier is deprecated in favour of <atomic>, which C lacks

#define NPH_COMPILER_BARRIER_() _ReadWriteBarrier()

NPH_INLINE void nph_atomic_thread_fence(NphMemoryOrder order)
{
    if (order == NPH_MEMORY_ORDER_RELAXED) {
        return;
    }
    NPH_COMPILER_BARRIER_();
    if (order == NPH_MEMORY_ORDER_SEQ_CST) {
        _mm_mfence();
        NPH_COMPILER_BARRIER_();
    }
}

NPH_INLINE void nph_atomic_signal_fence(NphMemoryOrder order)
{
    NPH_UNUSED(order);
    NPH_COMPILER_BARRIER_();
}

// --- u32 ---

NPH_INLINE u32 nph_atomic_load_u32(const NphAtomicU32 *a, NphMemoryOrder order)
{
    const u32 v = (u32)__iso_volatile_load32((const volatile __int32 *)&a->value);
    if (order != NPH_MEMORY_ORDER_RELAXED) {
        NPH_COMPILER_BARRIER_();
    }
    return v;
}

NPH_INLINE void nph_atomic_store_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    if (order == NPH_MEMORY_ORDER_SEQ_CST) {
        (void)_InterlockedExchange((volatile long *)&a->value, (long)v);
        return;
    }
    if (order != NPH_MEMORY_ORDER_RELAXED) {
        NPH_COMPILER_BARRIER_();
    }
    __iso_volatile_store32((volatile __int32 *)&a->value, (__int32)v);
}

NPH_INLINE u32 nph_atomic_exchange_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u32)_InterlockedExchange((volatile long *)&a->value, (long)v);
}

NPH_INLINE u32 nph_atomic_fetch_add_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u32)_InterlockedExchangeAdd((volatile long *)&a->value, (long)v);
}

NPH_INLINE u32 nph_atomic_fetch_sub_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u32)_InterlockedExchangeAdd((volatile long *)&a->value, -(long)v);
}

NPH_INLINE u32 nph_atomic_fetch_or_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u32)_InterlockedOr((volatile long *)&a->value, (long)v);
}

NPH_INLINE u32 nph_atomic_fetch_and_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u32)_InterlockedAnd((volatile long *)&a->value, (long)v);
}

// Compares *a with *expected; on match stores `desired` and returns true,
// otherwise writes the observed value to *expected and returns false. The
// strong form never fails spuriously.
NPH_INLINE bool nph_atomic_compare_exchange_u32(NphAtomicU32 *a, u32 *expected, u32 desired,
                                                NphMemoryOrder success, NphMemoryOrder failure)
{
    NPH_UNUSED(success);
    NPH_UNUSED(failure);
    const u32 prev = (u32)_InterlockedCompareExchange((volatile long *)&a->value, (long)desired,
                                                      (long)*expected);
    if (prev == *expected) {
        return true;
    }
    *expected = prev;
    return false;
}

// --- u64 ---

NPH_INLINE u64 nph_atomic_load_u64(const NphAtomicU64 *a, NphMemoryOrder order)
{
    const u64 v = (u64)__iso_volatile_load64((const volatile __int64 *)&a->value);
    if (order != NPH_MEMORY_ORDER_RELAXED) {
        NPH_COMPILER_BARRIER_();
    }
    return v;
}

NPH_INLINE void nph_atomic_store_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    if (order == NPH_MEMORY_ORDER_SEQ_CST) {
        (void)_InterlockedExchange64((volatile __int64 *)&a->value, (__int64)v);
        return;
    }
    if (order != NPH_MEMORY_ORDER_RELAXED) {
        NPH_COMPILER_BARRIER_();
    }
    __iso_volatile_store64((volatile __int64 *)&a->value, (__int64)v);
}

NPH_INLINE u64 nph_atomic_exchange_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u64)_InterlockedExchange64((volatile __int64 *)&a->value, (__int64)v);
}

NPH_INLINE u64 nph_atomic_fetch_add_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u64)_InterlockedExchangeAdd64((volatile __int64 *)&a->value, (__int64)v);
}

NPH_INLINE u64 nph_atomic_fetch_sub_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u64)_InterlockedExchangeAdd64((volatile __int64 *)&a->value, -(__int64)v);
}

NPH_INLINE u64 nph_atomic_fetch_or_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u64)_InterlockedOr64((volatile __int64 *)&a->value, (__int64)v);
}

NPH_INLINE u64 nph_atomic_fetch_and_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return (u64)_InterlockedAnd64((volatile __int64 *)&a->value, (__int64)v);
}

NPH_INLINE bool nph_atomic_compare_exchange_u64(NphAtomicU64 *a, u64 *expected, u64 desired,
                                                NphMemoryOrder success, NphMemoryOrder failure)
{
    NPH_UNUSED(success);
    NPH_UNUSED(failure);
    const u64 prev = (u64)_InterlockedCompareExchange64((volatile __int64 *)&a->value,
                                                        (__int64)desired, (__int64)*expected);
    if (prev == *expected) {
        return true;
    }
    *expected = prev;
    return false;
}

// --- pointers ---

NPH_INLINE void *nph_atomic_load_ptr(const NphAtomicPtr *a, NphMemoryOrder order)
{
    void *const v = (void *)(uptr)__iso_volatile_load64((const volatile __int64 *)&a->value);
    if (order != NPH_MEMORY_ORDER_RELAXED) {
        NPH_COMPILER_BARRIER_();
    }
    return v;
}

NPH_INLINE void nph_atomic_store_ptr(NphAtomicPtr *a, void *v, NphMemoryOrder order)
{
    if (order == NPH_MEMORY_ORDER_SEQ_CST) {
        (void)_InterlockedExchangePointer((void *volatile *)&a->value, v);
        return;
    }
    if (order != NPH_MEMORY_ORDER_RELAXED) {
        NPH_COMPILER_BARRIER_();
    }
    __iso_volatile_store64((volatile __int64 *)&a->value, (__int64)(uptr)v);
}

NPH_INLINE void *nph_atomic_exchange_ptr(NphAtomicPtr *a, void *v, NphMemoryOrder order)
{
    NPH_UNUSED(order);
    return _InterlockedExchangePointer((void *volatile *)&a->value, v);
}

NPH_INLINE bool nph_atomic_compare_exchange_ptr(NphAtomicPtr *a, void **expected, void *desired,
                                                NphMemoryOrder success, NphMemoryOrder failure)
{
    NPH_UNUSED(success);
    NPH_UNUSED(failure);
    void *const prev =
        _InterlockedCompareExchangePointer((void *volatile *)&a->value, desired, *expected);
    if (prev == *expected) {
        return true;
    }
    *expected = prev;
    return false;
}

#pragma warning(pop)

#else

// ---------------------------------------------------------------------------
// GCC / Clang: the __atomic builtins take the order directly.
// ---------------------------------------------------------------------------

NPH_INLINE int nph_atomic_builtin_order(NphMemoryOrder order)
{
    switch (order) {
    case NPH_MEMORY_ORDER_RELAXED:
        return __ATOMIC_RELAXED;
    case NPH_MEMORY_ORDER_ACQUIRE:
        return __ATOMIC_ACQUIRE;
    case NPH_MEMORY_ORDER_RELEASE:
        return __ATOMIC_RELEASE;
    case NPH_MEMORY_ORDER_ACQ_REL:
        return __ATOMIC_ACQ_REL;
    case NPH_MEMORY_ORDER_SEQ_CST:
        break;
    }
    return __ATOMIC_SEQ_CST;
}

// A failure order may not be release or acq_rel, and may not be stronger
// than the success order.
NPH_INLINE int nph_atomic_builtin_failure_order(NphMemoryOrder order)
{
    switch (order) {
    case NPH_MEMORY_ORDER_RELAXED:
        return __ATOMIC_RELAXED;
    case NPH_MEMORY_ORDER_ACQUIRE:
    case NPH_MEMORY_ORDER_RELEASE:
    case NPH_MEMORY_ORDER_ACQ_REL:
        return __ATOMIC_ACQUIRE;
    case NPH_MEMORY_ORDER_SEQ_CST:
        break;
    }
    return __ATOMIC_SEQ_CST;
}

NPH_INLINE void nph_atomic_thread_fence(NphMemoryOrder order)
{
    __atomic_thread_fence(nph_atomic_builtin_order(order));
}

NPH_INLINE void nph_atomic_signal_fence(NphMemoryOrder order)
{
    __atomic_signal_fence(nph_atomic_builtin_order(order));
}

// --- u32 ---

NPH_INLINE u32 nph_atomic_load_u32(const NphAtomicU32 *a, NphMemoryOrder order)
{
    return __atomic_load_n(&a->value, nph_atomic_builtin_order(order));
}
NPH_INLINE void nph_atomic_store_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    __atomic_store_n(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u32 nph_atomic_exchange_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    return __atomic_exchange_n(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u32 nph_atomic_fetch_add_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    return __atomic_fetch_add(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u32 nph_atomic_fetch_sub_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    return __atomic_fetch_sub(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u32 nph_atomic_fetch_or_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    return __atomic_fetch_or(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u32 nph_atomic_fetch_and_u32(NphAtomicU32 *a, u32 v, NphMemoryOrder order)
{
    return __atomic_fetch_and(&a->value, v, nph_atomic_builtin_order(order));
}
// The builtin writes the observed value through `expected`; clang-tidy
// cannot see that and suggests const.
// NOLINTNEXTLINE(readability-non-const-parameter)
NPH_INLINE bool nph_atomic_compare_exchange_u32(NphAtomicU32 *a, u32 *expected, u32 desired,
                                                NphMemoryOrder success, NphMemoryOrder failure)
{
    return __atomic_compare_exchange_n(&a->value, expected, desired, false,
                                       nph_atomic_builtin_order(success),
                                       nph_atomic_builtin_failure_order(failure));
}

// --- u64 ---

NPH_INLINE u64 nph_atomic_load_u64(const NphAtomicU64 *a, NphMemoryOrder order)
{
    return __atomic_load_n(&a->value, nph_atomic_builtin_order(order));
}
NPH_INLINE void nph_atomic_store_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    __atomic_store_n(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u64 nph_atomic_exchange_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    return __atomic_exchange_n(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u64 nph_atomic_fetch_add_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    return __atomic_fetch_add(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u64 nph_atomic_fetch_sub_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    return __atomic_fetch_sub(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u64 nph_atomic_fetch_or_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    return __atomic_fetch_or(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE u64 nph_atomic_fetch_and_u64(NphAtomicU64 *a, u64 v, NphMemoryOrder order)
{
    return __atomic_fetch_and(&a->value, v, nph_atomic_builtin_order(order));
}
// NOLINTNEXTLINE(readability-non-const-parameter)
NPH_INLINE bool nph_atomic_compare_exchange_u64(NphAtomicU64 *a, u64 *expected, u64 desired,
                                                NphMemoryOrder success, NphMemoryOrder failure)
{
    return __atomic_compare_exchange_n(&a->value, expected, desired, false,
                                       nph_atomic_builtin_order(success),
                                       nph_atomic_builtin_failure_order(failure));
}

// --- pointers ---

NPH_INLINE void *nph_atomic_load_ptr(const NphAtomicPtr *a, NphMemoryOrder order)
{
    return __atomic_load_n(&a->value, nph_atomic_builtin_order(order));
}
NPH_INLINE void nph_atomic_store_ptr(NphAtomicPtr *a, void *v, NphMemoryOrder order)
{
    __atomic_store_n(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE void *nph_atomic_exchange_ptr(NphAtomicPtr *a, void *v, NphMemoryOrder order)
{
    return __atomic_exchange_n(&a->value, v, nph_atomic_builtin_order(order));
}
NPH_INLINE bool nph_atomic_compare_exchange_ptr(NphAtomicPtr *a, void **expected, void *desired,
                                                NphMemoryOrder success, NphMemoryOrder failure)
{
    return __atomic_compare_exchange_n(&a->value, expected, desired, false,
                                       nph_atomic_builtin_order(success),
                                       nph_atomic_builtin_failure_order(failure));
}

#endif

// ---------------------------------------------------------------------------
// Spin lock. For very short critical sections in diagnostic code (the debug
// allocator's live-allocation list). Production paths use the job system's
// primitives; a spinning thread burns a core and is never the right tool on
// a hot path (CLAUDE.md §1.4: locking must be visible to the caller).
// ---------------------------------------------------------------------------

typedef struct NphSpinLock {
    NphAtomicU32 locked;
} NphSpinLock;

#define NPH_SPIN_LOCK_INIT {NPH_ATOMIC_U32_INIT(0U)}

NPH_INLINE void nph_spin_lock_init(NphSpinLock *lock)
{
    nph_atomic_store_u32(&lock->locked, 0U, NPH_MEMORY_ORDER_RELAXED);
}

NPH_INLINE bool nph_spin_lock_try_acquire(NphSpinLock *lock)
{
    return nph_atomic_exchange_u32(&lock->locked, 1U, NPH_MEMORY_ORDER_ACQUIRE) == 0U;
}

NPH_INLINE void nph_spin_lock_acquire(NphSpinLock *lock)
{
    for (;;) {
        if (nph_atomic_exchange_u32(&lock->locked, 1U, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
            return;
        }
        // Test-and-test-and-set: spin on a plain load so the cache line is
        // shared rather than bounced between cores by repeated exchanges.
        while (nph_atomic_load_u32(&lock->locked, NPH_MEMORY_ORDER_RELAXED) != 0U) {
            nph_cpu_relax();
        }
    }
}

NPH_INLINE void nph_spin_lock_release(NphSpinLock *lock)
{
    nph_atomic_store_u32(&lock->locked, 0U, NPH_MEMORY_ORDER_RELEASE);
}

#ifdef __cplusplus
}
#endif

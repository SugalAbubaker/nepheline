// SPDX-License-Identifier: Apache-2.0
// The OS-independent halves of thread.h: the semaphore's user-space count
// and nph_once.
#include <nepheline/core/assert.h>
#include <nepheline/platform/thread.h>

#include "platform_internal.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Semaphore (Preshing, "Semaphores are Surprisingly Versatile", 2015; the
// LightweightSemaphore of Desrochers' concurrentqueue). The count holds
// BIAS + permits when no one waits and BIAS - waiters when it is drained.
// A waiter spins on the fast path before committing to the kernel, because
// a job worker is usually woken within a microsecond of parking.
// ---------------------------------------------------------------------------

#define SEMAPHORE_SPIN_ITERATIONS 1024U

NphResult nph_semaphore_init(NphSemaphore *s, u32 initial)
{
    NPH_ASSERT(s != NULL);
    NPH_ASSERT(initial < NPH_SEMAPHORE_BIAS);
    nph_atomic_store_u32(&s->count, NPH_SEMAPHORE_BIAS + initial, NPH_MEMORY_ORDER_RELAXED);
    return nph_semaphore_os_init(s);
}

void nph_semaphore_destroy(NphSemaphore *s)
{
    NPH_ASSERT(s != NULL);
    NPH_ASSERT_MSG(nph_atomic_load_u32(&s->count, NPH_MEMORY_ORDER_RELAXED) >= NPH_SEMAPHORE_BIAS,
                   "destroying a semaphore with waiters");
    nph_semaphore_os_destroy(s);
}

// Takes a permit if the count shows one. Acquire on success so the work the
// poster published is visible.
static bool try_take(NphSemaphore *s)
{
    u32 old = nph_atomic_load_u32(&s->count, NPH_MEMORY_ORDER_RELAXED);
    while (old > NPH_SEMAPHORE_BIAS) {
        if (nph_atomic_compare_exchange_u32(&s->count, &old, old - 1U, NPH_MEMORY_ORDER_ACQUIRE,
                                            NPH_MEMORY_ORDER_RELAXED)) {
            return true;
        }
    }
    return false;
}

static bool wait_spinning(NphSemaphore *s)
{
    for (u32 i = 0; i < SEMAPHORE_SPIN_ITERATIONS; ++i) {
        if (try_take(s)) {
            return true;
        }
        nph_cpu_relax();
    }
    return false;
}

bool nph_semaphore_try_wait(NphSemaphore *s)
{
    NPH_ASSERT(s != NULL);
    return try_take(s);
}

void nph_semaphore_wait(NphSemaphore *s)
{
    NPH_ASSERT(s != NULL);
    if (wait_spinning(s)) {
        return;
    }
    // Register as a waiter. If a permit arrived between the spin and here
    // the old count is above the bias and it is ours.
    const u32 old = nph_atomic_fetch_sub_u32(&s->count, 1U, NPH_MEMORY_ORDER_ACQ_REL);
    if (old > NPH_SEMAPHORE_BIAS) {
        return;
    }
    nph_semaphore_os_wait(s);
}

bool nph_semaphore_wait_timeout(NphSemaphore *s, u64 timeout_ns)
{
    NPH_ASSERT(s != NULL);
    if (timeout_ns == 0U) {
        return try_take(s); // a poll: no spinning, no kernel
    }
    if (wait_spinning(s)) {
        return true;
    }
    const u32 old = nph_atomic_fetch_sub_u32(&s->count, 1U, NPH_MEMORY_ORDER_ACQ_REL);
    if (old > NPH_SEMAPHORE_BIAS) {
        return true;
    }
    if (nph_semaphore_os_wait_timeout(s, timeout_ns)) {
        return true;
    }
    // Timed out with this thread still counted as a waiter. Withdraw, unless
    // a post has meanwhile paid for us: then the kernel semaphore holds a
    // permit that must be consumed, or it would wake a future waiter early.
    for (;;) {
        u32 now = nph_atomic_load_u32(&s->count, NPH_MEMORY_ORDER_RELAXED);
        if (now >= NPH_SEMAPHORE_BIAS && nph_semaphore_os_try_wait(s)) {
            return true;
        }
        if (now < NPH_SEMAPHORE_BIAS &&
            nph_atomic_compare_exchange_u32(&s->count, &now, now + 1U, NPH_MEMORY_ORDER_ACQ_REL,
                                            NPH_MEMORY_ORDER_RELAXED)) {
            return false;
        }
    }
}

void nph_semaphore_post_n(NphSemaphore *s, u32 n)
{
    NPH_ASSERT(s != NULL);
    NPH_ASSERT(n > 0U && n < NPH_SEMAPHORE_BIAS);
    // Release so the work done before the post is visible to the taker.
    const u32 old = nph_atomic_fetch_add_u32(&s->count, n, NPH_MEMORY_ORDER_ACQ_REL);
    if (old < NPH_SEMAPHORE_BIAS) {
        const u32 waiters = NPH_SEMAPHORE_BIAS - old;
        nph_semaphore_os_post(s, waiters < n ? waiters : n);
    }
}

void nph_semaphore_post(NphSemaphore *s)
{
    nph_semaphore_post_n(s, 1U);
}

// ---------------------------------------------------------------------------
// Once
// ---------------------------------------------------------------------------

void nph_once(NphOnce *once, NphOnceFn fn, void *user)
{
    NPH_ASSERT(once != NULL && fn != NULL);
    if (nph_atomic_load_u32(&once->state, NPH_MEMORY_ORDER_ACQUIRE) == 2U) {
        return;
    }
    u32 expected = 0U;
    if (nph_atomic_compare_exchange_u32(&once->state, &expected, 1U, NPH_MEMORY_ORDER_ACQ_REL,
                                        NPH_MEMORY_ORDER_ACQUIRE)) {
        fn(user);
        nph_atomic_store_u32(&once->state, 2U, NPH_MEMORY_ORDER_RELEASE);
        return;
    }
    // Another thread is running fn; initialisers are short, so spin. A
    // recursive call from inside fn would spin forever and is a caller bug.
    u32 spins = 0U;
    while (nph_atomic_load_u32(&once->state, NPH_MEMORY_ORDER_ACQUIRE) != 2U) {
        if (++spins < 64U) {
            nph_cpu_relax();
        } else {
            nph_thread_yield();
        }
    }
}

// ---------------------------------------------------------------------------
// Shared helper for the thread implementations
// ---------------------------------------------------------------------------

void nph_thread_copy_name(char *dst, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    u32 i = 0U;
    for (; i + 1U < NPH_THREAD_NAME_MAX && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

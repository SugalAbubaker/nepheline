// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the platform tests. ntest assertions may only be made
// on the main thread, so worker functions record into shared state (through
// atomics) and the test asserts after joining.

#include <nepheline/core/atomic.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/thread.h>

#include <ntest/ntest.h>

#define TEST_MAX_WORKERS 16U

// Work counts scale down in Debug, where the sanitizers make every atomic
// several times slower, so a test stays well under a second per toolchain.
#if NPH_CONFIG_DEBUG
#define TEST_SCALE 1U
#else
#define TEST_SCALE 4U
#endif

typedef void (*TestWorkerFn)(u32 index, void *shared);

typedef struct TestWorker {
    NphThread thread;
    u32 index;
    void *shared;
    TestWorkerFn fn;
} TestWorker;

static inline void test_worker_main(void *user)
{
    TestWorker *w = (TestWorker *)user;
    w->fn(w->index, w->shared);
}

// Starts `count` workers running fn(index, shared). Asserts on failure.
static inline void test_threads_start(TestWorker *workers, u32 count, TestWorkerFn fn, void *shared)
{
    for (u32 i = 0; i < count; ++i) {
        workers[i].index = i;
        workers[i].shared = shared;
        workers[i].fn = fn;
        NphThreadDesc desc;
        desc.fn = test_worker_main;
        desc.user = &workers[i];
        desc.name = "test-worker";
        desc.stack_size = 0U;
        NTEST_ASSERT_EQ_INT(nph_thread_create(&workers[i].thread, &desc), NPH_RESULT_OK);
    }
}

static inline void test_threads_join(TestWorker *workers, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        nph_thread_join(&workers[i].thread);
    }
}

static inline void test_threads_run(TestWorker *workers, u32 count, TestWorkerFn fn, void *shared)
{
    test_threads_start(workers, count, fn, shared);
    test_threads_join(workers, count);
}

// A worker count that exercises real parallelism without oversubscribing
// a small CI runner: between 2 and `max`, from the logical core count.
static inline u32 test_worker_count(u32 max)
{
    const u32 cores = nph_cpu_logical_count();
    const u32 n = cores < 2U ? 2U : cores;
    return n > max ? max : n;
}

// Spin barrier for lockstep phases between workers.
typedef struct TestBarrier {
    NphAtomicU32 arrived;
    NphAtomicU32 generation;
    u32 total;
} TestBarrier;

static inline void test_barrier_init(TestBarrier *b, u32 total)
{
    nph_atomic_store_u32(&b->arrived, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&b->generation, 0U, NPH_MEMORY_ORDER_RELAXED);
    b->total = total;
}

static inline void test_barrier_wait(TestBarrier *b)
{
    const u32 gen = nph_atomic_load_u32(&b->generation, NPH_MEMORY_ORDER_ACQUIRE);
    if (nph_atomic_fetch_add_u32(&b->arrived, 1U, NPH_MEMORY_ORDER_ACQ_REL) + 1U == b->total) {
        nph_atomic_store_u32(&b->arrived, 0U, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_store_u32(&b->generation, gen + 1U, NPH_MEMORY_ORDER_RELEASE);
        return;
    }
    while (nph_atomic_load_u32(&b->generation, NPH_MEMORY_ORDER_ACQUIRE) == gen) {
        nph_cpu_relax();
    }
}

// Deterministic PRNG for randomized tests (splitmix64).
static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

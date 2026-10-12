// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the job system tests. ntest assertions may only be
// made on the main thread; jobs record into shared state through atomics
// and the test asserts after waiting.

#include <nepheline/core/atomic.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include <ntest/ntest.h>

#include <string.h>

// Work counts scale down where the sanitizers make everything slower.
#if NPH_TSAN_ENABLED
#define TEST_SCALE 1U
#elif NPH_CONFIG_DEBUG
#define TEST_SCALE 2U
#else
#define TEST_SCALE 8U
#endif

// Creates a system with `workers` workers (0: the default count) and
// otherwise default settings, asserting success.
static inline NphJobSystem *test_jobs_create(u32 workers)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.worker_count = workers;
    NphJobSystem *js = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(js);
    return js;
}

static inline NphJobSystem *test_jobs_create_desc(const NphJobSystemDesc *in)
{
    NphJobSystemDesc desc = *in;
    if (desc.alloc == NULL) {
        desc.alloc = nph_allocator_system();
    }
    NphJobSystem *js = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(js);
    return js;
}

// A worker count that exercises real parallelism without oversubscribing
// a small CI runner: between 2 and `max`.
static inline u32 test_jobs_worker_count(u32 max)
{
    const u32 cores = nph_cpu_logical_count();
    const u32 n = cores < 2U ? 2U : cores;
    return n > max ? max : n;
}

// Spins until `a` reads `expected`, or `timeout_ns` passes. Returns
// whether the value was seen.
static inline bool test_wait_atomic_u32(const NphAtomicU32 *a, u32 expected, u64 timeout_ns)
{
    const NphStopwatch sw = nph_stopwatch_start();
    while (nph_atomic_load_u32(a, NPH_MEMORY_ORDER_ACQUIRE) != expected) {
        if (nph_stopwatch_elapsed_ns(&sw) > timeout_ns) {
            return false;
        }
        nph_thread_yield();
    }
    return true;
}

// Deterministic PRNG (splitmix64).
static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Plain threads for tests that need a thread the job system does not own.
typedef void (*TestThreadFn)(u32 index, void *shared);

typedef struct TestThread {
    NphThread thread;
    u32 index;
    void *shared;
    TestThreadFn fn;
} TestThread;

static inline void test_thread_main(void *user)
{
    TestThread *t = (TestThread *)user;
    t->fn(t->index, t->shared);
}

static inline void test_threads_start(TestThread *threads, u32 count, TestThreadFn fn, void *shared)
{
    for (u32 i = 0; i < count; ++i) {
        threads[i].index = i;
        threads[i].shared = shared;
        threads[i].fn = fn;
        NphThreadDesc desc;
        desc.fn = test_thread_main;
        desc.user = &threads[i];
        desc.name = "test-thread";
        desc.stack_size = 0U;
        NTEST_ASSERT_EQ_INT(nph_thread_create(&threads[i].thread, &desc), NPH_RESULT_OK);
    }
}

static inline void test_threads_join(TestThread *threads, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        nph_thread_join(&threads[i].thread);
    }
}

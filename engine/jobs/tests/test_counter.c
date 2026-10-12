// SPDX-License-Identifier: Apache-2.0
// Counters on their own and waits from threads the job system does not
// own (the main thread, plain threads), which block instead of parking a
// fiber.
#include "test_jobs_support.h"

NTEST(counter, init_add_signal_value)
{
    NphJobSystem *js = test_jobs_create(1U);
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 0U);
    nph_job_counter_add(&c, 3U);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 3U);
    nph_job_counter_add(&c, 0U);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 3U);
    nph_job_counter_signal(js, &c);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 2U);
    nph_job_counter_signal(js, &c);
    nph_job_counter_signal(js, &c);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 0U);
    nph_job_counter_init(&c);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 0U);
    nph_job_system_destroy(js);
}

NTEST(counter, wait_on_zero_returns_at_once)
{
    NphJobSystem *js = test_jobs_create(1U);
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    const NphStopwatch sw = nph_stopwatch_start();
    nph_job_wait(js, &c);
    nph_job_wait_value(js, &c, 0U);
    nph_job_counter_add(&c, 2U);
    nph_job_wait_value(js, &c, 2U); // already at or below the target
    nph_job_wait_value(js, &c, 5U);
    NTEST_ASSERT_LT_UINT(nph_stopwatch_elapsed_ns(&sw), NPH_TIME_NS_PER_S);
    nph_job_counter_signal(js, &c);
    nph_job_counter_signal(js, &c);
    nph_job_system_destroy(js);
}

// A plain thread signals the counter the main thread blocks on, after a
// delay long enough that the main thread is asleep in the kernel.
typedef struct Signaller {
    NphJobSystem *js;
    NphJobCounter *counter;
    u32 signals;
    u64 delay_ns;
} Signaller;

static void signaller_fn(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Signaller *s = (Signaller *)shared;
    for (u32 i = 0; i < s->signals; ++i) {
        nph_thread_sleep_ns(s->delay_ns);
        nph_job_counter_signal(s->js, s->counter);
    }
}

NTEST(counter, external_thread_blocks_until_signalled)
{
    NphJobSystem *js = test_jobs_create(1U);
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    nph_job_counter_add(&c, 3U);
    Signaller s = {js, &c, 3U, 20U * NPH_TIME_NS_PER_MS};
    TestThread t;
    test_threads_start(&t, 1U, signaller_fn, &s);
    const NphStopwatch sw = nph_stopwatch_start();
    nph_job_wait_value(js, &c, 1U); // after two signals
    NTEST_ASSERT_LE_UINT(nph_job_counter_value(&c), 1U);
    nph_job_wait(js, &c);
    const u64 elapsed = nph_stopwatch_elapsed_ns(&sw);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&c), 0U);
    // Three 20 ms sleeps, less the one tick of early wake-up Windows allows.
    NTEST_ASSERT_GE_UINT(elapsed, 40U * NPH_TIME_NS_PER_MS);
    test_threads_join(&t, 1U);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_ASSERT_EQ_UINT(stats.waits_external, 2U);
    nph_job_system_destroy(js);
}

// Several external threads wait on the same counter; one signal batch
// releases them all.
typedef struct Waiters {
    NphJobSystem *js;
    NphJobCounter counter;
    NphAtomicU32 released;
    NphAtomicU32 arrived;
} Waiters;

static void waiter_fn(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Waiters *w = (Waiters *)shared;
    nph_atomic_fetch_add_u32(&w->arrived, 1U, NPH_MEMORY_ORDER_RELEASE);
    nph_job_wait(w->js, &w->counter);
    nph_atomic_fetch_add_u32(&w->released, 1U, NPH_MEMORY_ORDER_RELEASE);
}

NTEST(counter, many_external_waiters_are_all_released)
{
    NphJobSystem *js = test_jobs_create(1U);
    Waiters w;
    memset(&w, 0, sizeof w);
    w.js = js;
    nph_job_counter_add(&w.counter, 1U);
    TestThread threads[8];
    test_threads_start(threads, 8U, waiter_fn, &w);
    NTEST_ASSERT_TRUE(test_wait_atomic_u32(&w.arrived, 8U, 10U * NPH_TIME_NS_PER_S));
    nph_thread_sleep_ns(10U * NPH_TIME_NS_PER_MS); // let them all block
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&w.released, NPH_MEMORY_ORDER_ACQUIRE), 0U);
    nph_job_counter_signal(js, &w.counter);
    test_threads_join(threads, 8U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&w.released, NPH_MEMORY_ORDER_ACQUIRE), 8U);
    nph_job_system_destroy(js);
}

// More external waiters than the description allowed slots for: the
// extra ones fall back to polling and still return.
NTEST(counter, external_waiters_beyond_the_slots_poll)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = 1U;
    desc.external_waiter_slots = 2U;
    NphJobSystem *js = test_jobs_create_desc(&desc);
    Waiters w;
    memset(&w, 0, sizeof w);
    w.js = js;
    nph_job_counter_add(&w.counter, 1U);
    TestThread threads[6];
    test_threads_start(threads, 6U, waiter_fn, &w);
    NTEST_ASSERT_TRUE(test_wait_atomic_u32(&w.arrived, 6U, 10U * NPH_TIME_NS_PER_S));
    nph_thread_sleep_ns(20U * NPH_TIME_NS_PER_MS);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&w.released, NPH_MEMORY_ORDER_ACQUIRE), 0U);
    nph_job_counter_signal(js, &w.counter);
    test_threads_join(threads, 6U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&w.released, NPH_MEMORY_ORDER_ACQUIRE), 6U);
    nph_job_system_destroy(js);
}

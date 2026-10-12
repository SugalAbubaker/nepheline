// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <string.h>

typedef struct Shared {
    NphAtomicU64 thread_id;
    NphAtomicU32 ran;
    NphAtomicU32 is_current_matched;
    void *user_seen;
} Shared;

static void record_fn(void *user)
{
    Shared *s = (Shared *)user;
    s->user_seen = user;
    nph_atomic_store_u64(&s->thread_id, nph_thread_current_id(), NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&s->ran, 1U, NPH_MEMORY_ORDER_RELEASE);
}

NTEST(thread, create_runs_fn_and_join_waits)
{
    Shared s;
    memset(&s, 0, sizeof s);
    NphThreadDesc desc;
    desc.fn = record_fn;
    desc.user = &s;
    desc.name = "record";
    desc.stack_size = 0U;
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(t.joinable);
    NTEST_ASSERT_NE_UINT(t.id, 0U);
    NTEST_ASSERT_NE_UINT(t.id, nph_thread_current_id());
    NTEST_ASSERT_FALSE(nph_thread_is_current(&t));
    nph_thread_join(&t);
    NTEST_ASSERT_FALSE(t.joinable);
    NTEST_ASSERT_EQ_UINT(t.native, 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.ran, NPH_MEMORY_ORDER_ACQUIRE), 1U);
    NTEST_ASSERT_EQ_PTR(s.user_seen, &s);
}

NTEST(thread, id_reported_matches_the_threads_own_view)
{
    Shared s;
    memset(&s, 0, sizeof s);
    NphThreadDesc desc;
    desc.fn = record_fn;
    desc.user = &s;
    desc.name = NULL;
    desc.stack_size = 0U;
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    const u64 id = t.id;
    nph_thread_join(&t);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&s.thread_id, NPH_MEMORY_ORDER_RELAXED), id);
}

NTEST(thread, null_fn_is_rejected)
{
    NphThreadDesc desc;
    desc.fn = NULL;
    desc.user = NULL;
    desc.name = NULL;
    desc.stack_size = 0U;
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_FALSE(t.joinable);
}

static void sleep_fn(void *user)
{
    NPH_UNUSED(user);
    nph_thread_sleep_ns(NPH_TIME_NS_PER_MS);
}

NTEST(thread, detach_lets_the_thread_finish_on_its_own)
{
    NphThreadDesc desc;
    desc.fn = sleep_fn;
    desc.user = NULL;
    desc.name = "detached";
    desc.stack_size = 0U;
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    nph_thread_detach(&t);
    NTEST_ASSERT_FALSE(t.joinable);
    // Give it time to exit so the sanitizers do not report it as leaked.
    nph_thread_sleep_ns(20U * NPH_TIME_NS_PER_MS);
}

typedef struct BigStack {
    volatile u8 sink;
    NphAtomicU32 done;
} BigStack;

static void deep_recursion(u32 depth, volatile u8 *sink)
{
    u8 frame[1024];
    frame[0] = (u8)depth;
    frame[1023] = (u8)depth;
    *sink = (u8)(*sink + frame[0] + frame[1023]);
    if (depth > 0U) {
        deep_recursion(depth - 1U, sink);
    }
}

static void big_stack_fn(void *user)
{
    BigStack *b = (BigStack *)user;
    // 2 MiB of frames (several times that under ASan): fits the requested
    // 16 MiB stack, would overflow the 1 MiB Windows default.
    deep_recursion(2U * 1024U, &b->sink);
    nph_atomic_store_u32(&b->done, 1U, NPH_MEMORY_ORDER_RELEASE);
}

NTEST(thread, custom_stack_size_is_honoured)
{
    BigStack b;
    b.sink = 0U;
    nph_atomic_store_u32(&b.done, 0U, NPH_MEMORY_ORDER_RELAXED);
    NphThreadDesc desc;
    desc.fn = big_stack_fn;
    desc.user = &b;
    desc.name = "big-stack";
    desc.stack_size = NPH_MIB(16);
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    nph_thread_join(&t);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&b.done, NPH_MEMORY_ORDER_ACQUIRE), 1U);
}

NTEST(thread, current_handle_is_not_joinable_and_matches_id)
{
    NphThread me;
    nph_thread_current(&me);
    NTEST_ASSERT_FALSE(me.joinable);
    NTEST_ASSERT_EQ_UINT(me.id, nph_thread_current_id());
    NTEST_ASSERT_TRUE(nph_thread_is_current(&me));
}

NTEST(thread, affinity_and_priority_on_current_thread)
{
    NphThread me;
    nph_thread_current(&me);
    NTEST_ASSERT_EQ_INT(nph_thread_set_affinity(&me, 0U), NPH_RESULT_INVALID_ARGUMENT);
    const u32 cores = nph_cpu_logical_count();
    const u64 all = cores >= 64U ? NPH_U64_MAX : (((u64)1U << cores) - 1U);
    // Pin to core 0, then restore every core the process may use.
    NTEST_ASSERT_EQ_INT(nph_thread_set_affinity(&me, 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_thread_set_affinity(&me, all), NPH_RESULT_OK);

    // Lowering is always permitted; raising may be refused on Linux.
    NTEST_ASSERT_EQ_INT(nph_thread_set_priority(&me, NPH_THREAD_PRIORITY_LOW), NPH_RESULT_OK);
    const NphResult normal = nph_thread_set_priority(&me, NPH_THREAD_PRIORITY_NORMAL);
    NTEST_ASSERT_TRUE(normal == NPH_RESULT_OK || normal == NPH_RESULT_ACCESS_DENIED);
    const NphResult high = nph_thread_set_priority(&me, NPH_THREAD_PRIORITY_HIGH);
    NTEST_ASSERT_TRUE(high == NPH_RESULT_OK || high == NPH_RESULT_ACCESS_DENIED);
    const NphResult critical = nph_thread_set_priority(&me, NPH_THREAD_PRIORITY_TIME_CRITICAL);
    NTEST_ASSERT_TRUE(critical == NPH_RESULT_OK || critical == NPH_RESULT_ACCESS_DENIED);
    (void)nph_thread_set_priority(&me, NPH_THREAD_PRIORITY_NORMAL);
}

static void pinned_fn(void *user)
{
    NphAtomicU32 *done = (NphAtomicU32 *)user;
    NphThread me;
    nph_thread_current(&me);
    const NphResult r = nph_thread_set_affinity(&me, 1U);
    nph_atomic_store_u32(done, r == NPH_RESULT_OK ? 1U : 2U, NPH_MEMORY_ORDER_RELEASE);
}

NTEST(thread, affinity_on_a_created_thread)
{
    NphAtomicU32 done = NPH_ATOMIC_U32_INIT(0U);
    NphThreadDesc desc;
    desc.fn = pinned_fn;
    desc.user = &done;
    desc.name = "pinned";
    desc.stack_size = 0U;
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_thread_set_affinity(&t, 1U), NPH_RESULT_OK);
    nph_thread_join(&t);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&done, NPH_MEMORY_ORDER_ACQUIRE), 1U);
}

NTEST(thread, set_current_name_accepts_long_and_unicode_names)
{
    nph_thread_set_current_name("a-very-long-thread-name-that-exceeds-every-limit-there-is");
    nph_thread_set_current_name("wörker-\xC3\xA9"); // UTF-8
    nph_thread_set_current_name("");
    nph_thread_set_current_name("main");
}

NTEST(thread, sleep_waits_at_least_the_requested_time)
{
    const u64 requests[] = {0U, 100U * NPH_TIME_NS_PER_US, NPH_TIME_NS_PER_MS,
                            3U * NPH_TIME_NS_PER_MS};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(requests); ++i) {
        const u64 t0 = nph_time_now_ns();
        nph_thread_sleep_ns(requests[i]);
        const u64 elapsed = nph_time_now_ns() - t0;
        NTEST_ASSERT_GE_UINT(elapsed, requests[i]);
        NTEST_ASSERT_LT_UINT(elapsed, NPH_TIME_NS_PER_S);
    }
    nph_thread_yield();
}

NTEST(thread, short_sleeps_are_not_quantised_to_the_scheduler_tick)
{
    // Ten 1 ms sleeps on a timer with real resolution take ~10-15 ms; on a
    // 15.6 ms tick they would take 156 ms. The bound is loose for CI.
    const u64 t0 = nph_time_now_ns();
    for (u32 i = 0; i < 10U; ++i) {
        nph_thread_sleep_ns(NPH_TIME_NS_PER_MS);
    }
    const u64 elapsed = nph_time_now_ns() - t0;
    NTEST_LOG("ten 1 ms sleeps took %.2f ms", nph_time_ns_to_ms(elapsed));
    NTEST_ASSERT_LT_UINT(elapsed, 100U * NPH_TIME_NS_PER_MS);
}

typedef struct Counter {
    NphAtomicU32 value;
} Counter;

static void count_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    nph_atomic_fetch_add_u32(&((Counter *)shared)->value, 1U, NPH_MEMORY_ORDER_RELAXED);
}

NTEST(thread, many_threads_created_and_joined)
{
    Counter c = {NPH_ATOMIC_U32_INIT(0U)};
    TestWorker workers[TEST_MAX_WORKERS];
    const u32 rounds = 8U * TEST_SCALE;
    for (u32 r = 0; r < rounds; ++r) {
        test_threads_run(workers, TEST_MAX_WORKERS, count_worker, &c);
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.value, NPH_MEMORY_ORDER_RELAXED),
                         rounds * TEST_MAX_WORKERS);
}

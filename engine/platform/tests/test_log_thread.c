// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/log.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_TEST, "test", NPH_LOG_LEVEL_TRACE);

typedef struct CountingSink {
    NphAtomicU32 records;
    NphAtomicU32 dropped;
    NphAtomicU64 flusher_thread;
    NphAtomicU32 flusher_threads_seen; // >1 when records arrive from several threads
} CountingSink;

static void counting_sink(const NphLogRecord *record, void *user)
{
    CountingSink *s = (CountingSink *)user;
    // Drop counts may arrive on a synthetic "core" record when a thread
    // exits with records dropped; count those drops but not the record.
    nph_atomic_fetch_add_u32(&s->dropped, record->dropped_before, NPH_MEMORY_ORDER_RELAXED);
    if (strcmp(record->category, "test") != 0) {
        return;
    }
    nph_atomic_fetch_add_u32(&s->records, 1U, NPH_MEMORY_ORDER_RELAXED);
    const u64 me = nph_thread_current_id();
    u64 prev = nph_atomic_load_u64(&s->flusher_thread, NPH_MEMORY_ORDER_RELAXED);
    if (prev == 0U) {
        nph_atomic_store_u64(&s->flusher_thread, me, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_store_u32(&s->flusher_threads_seen, 1U, NPH_MEMORY_ORDER_RELAXED);
    } else if (prev != me) {
        nph_atomic_store_u32(&s->flusher_threads_seen, 2U, NPH_MEMORY_ORDER_RELAXED);
    }
}

typedef struct LogFixture {
    CountingSink sink;
} LogFixture;

NTEST_SETUP(LogFixture)
{
    NphLogConfig config;
    config.alloc = nph_allocator_system();
    config.ring_capacity = 256U;
    config.max_threads = 32U;
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_OK);
    memset(&f->sink, 0, sizeof f->sink);
    NTEST_ASSERT_EQ_INT(nph_log_add_sink(counting_sink, &f->sink, NPH_LOG_LEVEL_TRACE),
                        NPH_RESULT_OK);
}

NTEST_TEARDOWN(LogFixture)
{
    NTEST_UNUSED(f);
    nph_log_shutdown();
}

NTEST_F(LogFixture, delivers_without_explicit_flush)
{
    NphLogThread lt;
    memset(&lt, 0, sizeof lt);
    NTEST_ASSERT_EQ_INT(nph_log_thread_start(&lt, 1U), NPH_RESULT_OK);
    NPH_LOG_INFO(&LOG_TEST, "hello from the main thread");
    // No flush call here: the logger thread does it within its interval.
    u32 waited = 0U;
    while (nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED) == 0U &&
           waited < 2000U) {
        nph_thread_sleep_ns(NPH_TIME_NS_PER_MS);
        ++waited;
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED), 1U);
    // The sink ran on the logger thread, not here.
    NTEST_ASSERT_NE_UINT(nph_atomic_load_u64(&f->sink.flusher_thread, NPH_MEMORY_ORDER_RELAXED),
                         nph_thread_current_id());
    nph_log_thread_stop(&lt);
    NTEST_ASSERT_FALSE(lt.running);
    nph_log_thread_stop(&lt); // idempotent
}

NTEST_F(LogFixture, stop_flushes_what_was_logged_before_it)
{
    NphLogThread lt;
    memset(&lt, 0, sizeof lt);
    NTEST_ASSERT_EQ_INT(nph_log_thread_start(&lt, 1000U), NPH_RESULT_OK); // long interval
    for (u32 i = 0; i < 50U; ++i) {
        NPH_LOG_INFO(&LOG_TEST, "record %u", i); // INFO: DEBUG compiles out of Shipping
    }
    nph_log_thread_stop(&lt);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED), 50U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f->sink.dropped, NPH_MEMORY_ORDER_RELAXED), 0U);
}

NTEST_F(LogFixture, kick_flushes_early)
{
    NphLogThread lt;
    memset(&lt, 0, sizeof lt);
    NTEST_ASSERT_EQ_INT(nph_log_thread_start(&lt, 5000U), NPH_RESULT_OK);
    NPH_LOG_WARN(&LOG_TEST, "urgent");
    nph_log_thread_kick(&lt);
    u32 waited = 0U;
    while (nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED) == 0U &&
           waited < 2000U) {
        nph_thread_sleep_ns(NPH_TIME_NS_PER_MS);
        ++waited;
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED), 1U);
    NTEST_ASSERT_LT_UINT(waited, 1000U); // well before the 5 s interval
    nph_log_thread_stop(&lt);
}

typedef struct Loggers {
    u32 records_each;
} Loggers;

static void logger_worker(u32 index, void *shared)
{
    const Loggers *l = (const Loggers *)shared;
    for (u32 i = 0; i < l->records_each; ++i) {
        NPH_LOG_INFO(&LOG_TEST, "worker %u record %u", index, i);
        if ((i & 63U) == 0U) {
            nph_thread_yield(); // let the flusher keep up so nothing drops
        }
    }
    // Threads created by nph_thread_create detach their ring on exit.
}

NTEST_F(LogFixture, many_threads_log_concurrently_with_the_flusher)
{
    NphLogThread lt;
    memset(&lt, 0, sizeof lt);
    NTEST_ASSERT_EQ_INT(nph_log_thread_start(&lt, 1U), NPH_RESULT_OK);
    Loggers l;
    l.records_each = 400U * TEST_SCALE;
    TestWorker workers[8];
    test_threads_run(workers, 8U, logger_worker, &l);
    nph_log_thread_stop(&lt);
    const u32 delivered = nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED);
    const u32 dropped = nph_atomic_load_u32(&f->sink.dropped, NPH_MEMORY_ORDER_RELAXED);
    NTEST_LOG("delivered %u, dropped %u", delivered, dropped);
    // Every record is either delivered or accounted for as dropped. A drop
    // is possible when a worker outruns a 256-record ring; it must never
    // be silent.
    NTEST_ASSERT_EQ_UINT(delivered + dropped, 8U * l.records_each);
    NTEST_ASSERT_GT_UINT(delivered, 0U);
}

// nph_log_drain racing the logger thread's flushes: every
// record pushed before each drain has been delivered when it returns
// (what unloading a game module, whose names the records point into,
// relies on).
NTEST_F(LogFixture, drain_delivers_everything_pushed_before_it)
{
    NphLogThread lt;
    memset(&lt, 0, sizeof lt);
    NTEST_ASSERT_EQ_INT(nph_log_thread_start(&lt, 1U), NPH_RESULT_OK);
    u32 pushed = 0U;
    for (u32 round = 0; round < 200U; ++round) {
        const u32 burst = 1U + (round % 7U);
        for (u32 i = 0; i < burst; ++i) {
            NPH_LOG_INFO(&LOG_TEST, "round %u record %u", round, i);
        }
        pushed += burst;
        nph_log_thread_kick(&lt); // often mid-flush when the drain starts
        (void)nph_log_drain();
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f->sink.records, NPH_MEMORY_ORDER_RELAXED),
                             pushed);
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f->sink.dropped, NPH_MEMORY_ORDER_RELAXED), 0U);
    nph_log_thread_stop(&lt);
    NTEST_ASSERT_EQ_UINT(nph_log_drain(), 0U); // nothing left
}

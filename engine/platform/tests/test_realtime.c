// SPDX-License-Identifier: Apache-2.0
// Real-time scopes (core/realtime.h) at the platform's blocking calls: every
// lock, wait, sleep, and file or asynchronous I/O call made inside a scope
// is reported with its kind, and the non-blocking variants are not.

#include <nepheline/core/realtime.h>
#include <nepheline/platform/async_io.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <stdio.h>
#include <string.h>

#if NPH_ASSERTS_ENABLED

// The hook runs on whichever thread violated, so it counts with atomics.
typedef struct RtCounts {
    NphAtomicU32 by_kind[NPH_REALTIME_VIOLATION_COUNT];
    NphAtomicU64 last_thread;
} RtCounts;

static void rt_count(NphRealtimeViolation kind, const char *what, NphCallsite site, void *user)
{
    NPH_UNUSED(what);
    NPH_UNUSED(site);
    RtCounts *c = (RtCounts *)user;
    if ((u32)kind < NPH_REALTIME_VIOLATION_COUNT) {
        (void)nph_atomic_fetch_add_u32(&c->by_kind[kind], 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_atomic_store_u64(&c->last_thread, nph_thread_current_id(), NPH_MEMORY_ORDER_RELAXED);
}

static u32 rt_kind(RtCounts *c, NphRealtimeViolation kind)
{
    return nph_atomic_load_u32(&c->by_kind[kind], NPH_MEMORY_ORDER_RELAXED);
}

static u32 rt_total(RtCounts *c)
{
    u32 n = 0U;
    for (u32 k = 0U; k < NPH_REALTIME_VIOLATION_COUNT; ++k) {
        n += rt_kind(c, (NphRealtimeViolation)k);
    }
    return n;
}

typedef struct RtFixture {
    RtCounts counts;
    NphRealtimeHook previous;
    void *previous_user;
    char dir[1024];
} RtFixture;

static u32 g_rt_dir_counter;

NTEST_SETUP(RtFixture)
{
    nph_realtime_set_hook(rt_count, &f->counts, &f->previous, &f->previous_user);
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    snprintf(f->dir, sizeof f->dir, "%s/nph_realtime_test_%llu_%u", tmp,
             (unsigned long long)nph_platform_process_id(), g_rt_dir_counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
}

NTEST_TEARDOWN(RtFixture)
{
    while (nph_realtime_active()) {
        nph_realtime_exit();
    }
    nph_realtime_set_hook(f->previous, f->previous_user, NULL, NULL);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(f->dir), NPH_RESULT_OK);
}

NTEST_F(RtFixture, locks_are_reported_and_tries_are_not)
{
    NphMutex m = NPH_MUTEX_INIT;
    NphRwLock rw = NPH_RWLOCK_INIT;
    nph_realtime_enter();
    nph_mutex_lock(&m);
    nph_mutex_unlock(&m);
    NTEST_ASSERT_TRUE(nph_mutex_try_lock(&m));
    nph_mutex_unlock(&m);
    nph_rwlock_lock_shared(&rw);
    nph_rwlock_unlock_shared(&rw);
    nph_rwlock_lock_exclusive(&rw);
    nph_rwlock_unlock_exclusive(&rw);
    NTEST_ASSERT_TRUE(nph_rwlock_try_lock_shared(&rw));
    nph_rwlock_unlock_shared(&rw);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_LOCK), 3U);
    NTEST_ASSERT_EQ_UINT(rt_total(&f->counts), 3U);
}

NTEST_F(RtFixture, waits_are_reported_even_when_they_would_not_block)
{
    NphMutex m = NPH_MUTEX_INIT;
    NphCondVar cv = NPH_CONDVAR_INIT;
    NphSemaphore s;
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&s, 2U), NPH_RESULT_OK);
    nph_mutex_lock(&m);
    nph_realtime_enter();
    (void)nph_condvar_wait_timeout(&cv, &m, nph_time_us_to_ns(100U));
    nph_semaphore_wait(&s);
    NTEST_ASSERT_TRUE(nph_semaphore_wait_timeout(&s, 0U));
    nph_semaphore_post(&s); // posting never waits
    NTEST_ASSERT_TRUE(nph_semaphore_try_wait(&s));
    nph_realtime_exit();
    nph_mutex_unlock(&m);
    nph_semaphore_destroy(&s);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_WAIT), 3U);
    NTEST_ASSERT_EQ_UINT(rt_total(&f->counts), 3U);
}

static void rt_nothing(void *user)
{
    NPH_UNUSED(user);
}

static void rt_once_init(void *user)
{
    *(u32 *)user += 1U;
}

NTEST_F(RtFixture, joins_sleeps_and_first_onces_are_reported)
{
    NphThread t;
    const NphThreadDesc desc = {rt_nothing, NULL, "rt-test", 0U};
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    NphOnce once = NPH_ONCE_INIT;
    u32 runs = 0U;
    nph_realtime_enter();
    nph_thread_join(&t);
    nph_thread_sleep_ns(1000U);
    nph_thread_yield();
    nph_once(&once, rt_once_init, &runs);
    nph_once(&once, rt_once_init, &runs); // done: one acquire load, no wait
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(runs, 1U);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_WAIT), 2U);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_SLEEP), 2U);
    NTEST_ASSERT_EQ_UINT(rt_total(&f->counts), 4U);
}

NTEST_F(RtFixture, file_and_async_io_are_reported)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/io.bin", f->dir);
    const u8 bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, bytes, sizeof bytes), NPH_RESULT_OK);
    NphAsyncIo *io = NULL;
    const NphAsyncIoConfig config = {NPH_ASYNC_IO_BACKEND_THREADS, 4U, 1U};
    NTEST_ASSERT_EQ_INT(nph_async_io_create(nph_allocator_system(), &config, &io), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(rt_total(&f->counts), 0U);

    // Each call reports at least once (a sequential read on an overlapped
    // Windows handle is a positional read underneath, which reports too).
    NphFile file;
    u8 got[16];
    u64 n = 0U;
    u64 size = 0U;
    u32 seen = 0U;
    nph_realtime_enter();
    NTEST_ASSERT_EQ_INT(nph_file_open(path, NPH_FILE_READ | NPH_FILE_ASYNC, &file), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO), seen);
    seen = rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO);
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, 0U, got, sizeof got, &n), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO), seen);
    seen = rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO);
    NTEST_ASSERT_EQ_INT(nph_file_read(&file, got, 4U, &n), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO), seen);
    seen = rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO);
    NTEST_ASSERT_EQ_INT(nph_file_size(&file, &size), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO), seen);
    seen = rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO);
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, 0U, got, 16U, 7U), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO), seen);
    nph_realtime_exit();

    // Collect the completion outside the scope, then poll once inside it.
    NphAsyncIoCompletion c;
    NTEST_ASSERT_EQ_UINT(nph_async_io_poll(io, &c, 1U, nph_time_ms_to_ns(2000U)), 1U);
    NTEST_ASSERT_EQ_UINT(c.user, 7U);
    seen = rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO);
    nph_realtime_enter();
    NTEST_ASSERT_EQ_UINT(nph_async_io_poll(io, &c, 1U, 0U), 0U);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_IO), seen + 1U);
    nph_file_close(&file);
    nph_async_io_destroy(io);
    NTEST_ASSERT_EQ_UINT(size, sizeof bytes);
    NTEST_ASSERT_EQ_MEM(got, bytes, 16U);
    // Nothing else was reported but the thread pool's poll, which waits on
    // its completion semaphore (with no timeout here).
    NTEST_ASSERT_LE_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_WAIT), 1U);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_ALLOC), 0U);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_LOCK), 0U);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_SLEEP), 0U);
}

typedef struct RtThreadShared {
    NphAtomicU32 done;
} RtThreadShared;

static void rt_worker_allocates_in_a_scope(u32 index, void *shared)
{
    NPH_UNUSED(index);
    RtThreadShared *s = (RtThreadShared *)shared;
    nph_realtime_enter();
    void *p = NPH_ALLOC(nph_allocator_system(), 64U, 16U);
    NPH_FREE(nph_allocator_system(), p, 64U, 16U);
    nph_realtime_exit();
    nph_atomic_store_u32(&s->done, 1U, NPH_MEMORY_ORDER_RELEASE);
}

NTEST_F(RtFixture, a_scope_belongs_to_its_thread)
{
    RtThreadShared shared;
    nph_atomic_store_u32(&shared.done, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker worker;
    test_threads_start(&worker, 1U, rt_worker_allocates_in_a_scope, &shared);
    // The main thread allocates meanwhile, outside any scope: not reported.
    for (u32 i = 0U; i < 100U; ++i) {
        void *p = NPH_ALLOC(nph_allocator_system(), 32U, 16U);
        NPH_FREE(nph_allocator_system(), p, 32U, 16U);
    }
    const u64 worker_id = worker.thread.id;
    test_threads_join(&worker, 1U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&shared.done, NPH_MEMORY_ORDER_ACQUIRE), 1U);
    NTEST_ASSERT_EQ_UINT(rt_kind(&f->counts, NPH_REALTIME_VIOLATION_ALLOC), 2U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&f->counts.last_thread, NPH_MEMORY_ORDER_RELAXED),
                         worker_id);
}

#else

NTEST(realtime_platform, blocking_calls_are_unchecked_without_asserts)
{
    NphMutex m = NPH_MUTEX_INIT;
    nph_realtime_enter();
    nph_mutex_lock(&m);
    nph_mutex_unlock(&m);
    nph_thread_sleep_ns(0U);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(nph_realtime_violations(), 0U);
}

#endif

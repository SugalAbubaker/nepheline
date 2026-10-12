// SPDX-License-Identifier: Apache-2.0
// Mutex, condition variable, read-write lock, semaphore, once.
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

typedef struct MutexShared {
    NphMutex mutex;
    u64 counter; // plain: protected by the mutex
    u32 iterations;
} MutexShared;

static void mutex_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    MutexShared *s = (MutexShared *)shared;
    for (u32 i = 0; i < s->iterations; ++i) {
        nph_mutex_lock(&s->mutex);
        s->counter += 1U;
        nph_mutex_unlock(&s->mutex);
    }
}

NTEST(mutex, serialises_increments)
{
    MutexShared s;
    nph_mutex_init(&s.mutex);
    s.counter = 0U;
    s.iterations = 20000U * TEST_SCALE;
    TestWorker workers[TEST_MAX_WORKERS];
    const u32 n = test_worker_count(8U);
    test_threads_run(workers, n, mutex_worker, &s);
    NTEST_ASSERT_EQ_UINT(s.counter, (u64)s.iterations * n);
    nph_mutex_destroy(&s.mutex);
}

NTEST(mutex, try_lock_fails_while_held)
{
    NphMutex m = NPH_MUTEX_INIT; // zero-filled is initialised
    NTEST_ASSERT_TRUE(nph_mutex_try_lock(&m));
    // SRW locks and default pthread mutexes are not recursive: a second
    // try_lock from the same thread must fail rather than deadlock or
    // succeed.
    NTEST_ASSERT_FALSE(nph_mutex_try_lock(&m));
    nph_mutex_unlock(&m);
    NTEST_ASSERT_TRUE(nph_mutex_try_lock(&m));
    nph_mutex_unlock(&m);
    nph_mutex_destroy(&m);
}

// ---------------------------------------------------------------------------
// Condition variable: a bounded queue with one producer and N consumers.
// ---------------------------------------------------------------------------

typedef struct Queue {
    NphMutex mutex;
    NphCondVar not_empty;
    NphCondVar not_full;
    u32 items[8];
    u32 head;
    u32 count;
    u32 total;    // items the producer will push
    bool closed;  // producer finished
    u64 consumed; // sum of consumed items, protected by the mutex
    u32 consumed_count;
} Queue;

static void producer(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Queue *q = (Queue *)shared;
    for (u32 i = 1; i <= q->total; ++i) {
        nph_mutex_lock(&q->mutex);
        while (q->count == NPH_ARRAY_COUNT(q->items)) {
            nph_condvar_wait(&q->not_full, &q->mutex);
        }
        q->items[(q->head + q->count) % NPH_ARRAY_COUNT(q->items)] = i;
        q->count += 1U;
        nph_condvar_signal(&q->not_empty);
        nph_mutex_unlock(&q->mutex);
    }
    nph_mutex_lock(&q->mutex);
    q->closed = true;
    nph_condvar_broadcast(&q->not_empty);
    nph_mutex_unlock(&q->mutex);
}

static void consumer(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Queue *q = (Queue *)shared;
    for (;;) {
        nph_mutex_lock(&q->mutex);
        while (q->count == 0U && !q->closed) {
            nph_condvar_wait(&q->not_empty, &q->mutex);
        }
        if (q->count == 0U) {
            nph_mutex_unlock(&q->mutex);
            return;
        }
        const u32 item = q->items[q->head];
        q->head = (q->head + 1U) % NPH_ARRAY_COUNT(q->items);
        q->count -= 1U;
        q->consumed += item;
        q->consumed_count += 1U;
        nph_condvar_signal(&q->not_full);
        nph_mutex_unlock(&q->mutex);
    }
}

NTEST(condvar, bounded_queue_delivers_every_item)
{
    Queue q;
    memset(&q, 0, sizeof q);
    nph_mutex_init(&q.mutex);
    nph_condvar_init(&q.not_empty);
    nph_condvar_init(&q.not_full);
    q.total = 5000U * TEST_SCALE;
    TestWorker consumers[4];
    TestWorker prod;
    test_threads_start(consumers, 4U, consumer, &q);
    test_threads_start(&prod, 1U, producer, &q);
    test_threads_join(&prod, 1U);
    test_threads_join(consumers, 4U);
    NTEST_ASSERT_EQ_UINT(q.consumed_count, q.total);
    NTEST_ASSERT_EQ_UINT(q.consumed, (u64)q.total * (q.total + 1U) / 2U);
    nph_condvar_destroy(&q.not_full);
    nph_condvar_destroy(&q.not_empty);
    nph_mutex_destroy(&q.mutex);
}

NTEST(condvar, wait_timeout_returns_false_when_nobody_signals)
{
    NphMutex m = NPH_MUTEX_INIT;
    NphCondVar cv = NPH_CONDVAR_INIT;
    nph_mutex_lock(&m);
    const u64 t0 = nph_time_now_ns();
    NTEST_ASSERT_FALSE(nph_condvar_wait_timeout(&cv, &m, 5U * NPH_TIME_NS_PER_MS));
    const u64 elapsed = nph_time_now_ns() - t0;
    // The mutex is held again on return.
    NTEST_ASSERT_FALSE(nph_mutex_try_lock(&m));
    nph_mutex_unlock(&m);
    NTEST_ASSERT_GE_UINT(elapsed, 4U * NPH_TIME_NS_PER_MS); // Windows rounds to ms
    NTEST_ASSERT_LT_UINT(elapsed, NPH_TIME_NS_PER_S);
    nph_condvar_destroy(&cv);
    nph_mutex_destroy(&m);
}

typedef struct Flag {
    NphMutex mutex;
    NphCondVar cv;
    bool set;
} Flag;

static void setter(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Flag *f = (Flag *)shared;
    nph_thread_sleep_ns(2U * NPH_TIME_NS_PER_MS);
    nph_mutex_lock(&f->mutex);
    f->set = true;
    nph_condvar_signal(&f->cv);
    nph_mutex_unlock(&f->mutex);
}

NTEST(condvar, wait_timeout_returns_true_when_signalled)
{
    Flag f;
    nph_mutex_init(&f.mutex);
    nph_condvar_init(&f.cv);
    f.set = false;
    TestWorker w;
    test_threads_start(&w, 1U, setter, &f);
    nph_mutex_lock(&f.mutex);
    bool woke = true;
    while (!f.set && woke) {
        woke = nph_condvar_wait_timeout(&f.cv, &f.mutex, 2U * NPH_TIME_NS_PER_S);
    }
    NTEST_ASSERT_TRUE(f.set);
    nph_mutex_unlock(&f.mutex);
    test_threads_join(&w, 1U);
    nph_condvar_destroy(&f.cv);
    nph_mutex_destroy(&f.mutex);
}

// ---------------------------------------------------------------------------
// Read-write lock
// ---------------------------------------------------------------------------

typedef struct RwShared {
    NphRwLock lock;
    u64 a; // kept equal to b under the lock; readers check the invariant
    u64 b;
    u32 iterations;
    NphAtomicU32 violations;
    NphAtomicU32 max_concurrent_readers;
    NphAtomicU32 readers_inside;
} RwShared;

static void rw_worker(u32 index, void *shared)
{
    RwShared *s = (RwShared *)shared;
    const bool writer = (index % 4U) == 0U;
    for (u32 i = 0; i < s->iterations; ++i) {
        if (writer) {
            nph_rwlock_lock_exclusive(&s->lock);
            s->a += 1U;
            // A reader that slips in here would see a != b.
            s->b += 1U;
            nph_rwlock_unlock_exclusive(&s->lock);
        } else {
            nph_rwlock_lock_shared(&s->lock);
            const u32 inside =
                nph_atomic_fetch_add_u32(&s->readers_inside, 1U, NPH_MEMORY_ORDER_RELAXED) + 1U;
            u32 max = nph_atomic_load_u32(&s->max_concurrent_readers, NPH_MEMORY_ORDER_RELAXED);
            while (inside > max && !nph_atomic_compare_exchange_u32(
                                       &s->max_concurrent_readers, &max, inside,
                                       NPH_MEMORY_ORDER_RELAXED, NPH_MEMORY_ORDER_RELAXED)) {
            }
            if (s->a != s->b) {
                nph_atomic_fetch_add_u32(&s->violations, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            nph_atomic_fetch_sub_u32(&s->readers_inside, 1U, NPH_MEMORY_ORDER_RELAXED);
            nph_rwlock_unlock_shared(&s->lock);
        }
    }
}

NTEST(rwlock, writers_are_exclusive_and_readers_overlap)
{
    RwShared s;
    memset(&s, 0, sizeof s);
    nph_rwlock_init(&s.lock);
    s.iterations = 5000U * TEST_SCALE;
    TestWorker workers[8];
    test_threads_run(workers, 8U, rw_worker, &s);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.violations, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(s.a, (u64)s.iterations * 2U); // workers 0 and 4 write
    NTEST_ASSERT_EQ_UINT(s.a, s.b);
    NTEST_LOG("max concurrent readers: %u",
              nph_atomic_load_u32(&s.max_concurrent_readers, NPH_MEMORY_ORDER_RELAXED));
    nph_rwlock_destroy(&s.lock);
}

NTEST(rwlock, try_lock_semantics)
{
    NphRwLock rw = NPH_RWLOCK_INIT;
    NTEST_ASSERT_TRUE(nph_rwlock_try_lock_shared(&rw));
    NTEST_ASSERT_TRUE(nph_rwlock_try_lock_shared(&rw)); // readers share
    NTEST_ASSERT_FALSE(nph_rwlock_try_lock_exclusive(&rw));
    nph_rwlock_unlock_shared(&rw);
    nph_rwlock_unlock_shared(&rw);
    NTEST_ASSERT_TRUE(nph_rwlock_try_lock_exclusive(&rw));
    NTEST_ASSERT_FALSE(nph_rwlock_try_lock_shared(&rw));
    NTEST_ASSERT_FALSE(nph_rwlock_try_lock_exclusive(&rw));
    nph_rwlock_unlock_exclusive(&rw);
    nph_rwlock_destroy(&rw);
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

NTEST(semaphore, permits_and_try_wait)
{
    NphSemaphore s;
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&s, 2U), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_semaphore_try_wait(&s));
    NTEST_ASSERT_TRUE(nph_semaphore_try_wait(&s));
    NTEST_ASSERT_FALSE(nph_semaphore_try_wait(&s));
    nph_semaphore_post(&s);
    NTEST_ASSERT_TRUE(nph_semaphore_try_wait(&s));
    nph_semaphore_post_n(&s, 3U);
    nph_semaphore_wait(&s);
    nph_semaphore_wait(&s);
    NTEST_ASSERT_TRUE(nph_semaphore_wait_timeout(&s, 0U));
    NTEST_ASSERT_FALSE(nph_semaphore_wait_timeout(&s, 0U));
    nph_semaphore_destroy(&s);
}

NTEST(semaphore, wait_timeout_expires)
{
    NphSemaphore s;
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&s, 0U), NPH_RESULT_OK);
    const u64 t0 = nph_time_now_ns();
    NTEST_ASSERT_FALSE(nph_semaphore_wait_timeout(&s, 5U * NPH_TIME_NS_PER_MS));
    const u64 elapsed = nph_time_now_ns() - t0;
    NTEST_ASSERT_GE_UINT(elapsed, 4U * NPH_TIME_NS_PER_MS);
    NTEST_ASSERT_LT_UINT(elapsed, NPH_TIME_NS_PER_S);
    // The timed-out waiter withdrew: a later post is not consumed twice.
    nph_semaphore_post(&s);
    NTEST_ASSERT_TRUE(nph_semaphore_try_wait(&s));
    NTEST_ASSERT_FALSE(nph_semaphore_try_wait(&s));
    nph_semaphore_destroy(&s);
}

typedef struct PingPong {
    NphSemaphore ping;
    NphSemaphore pong;
    u32 rounds;
    u64 counter; // alternately owned: no data race through the handoffs
} PingPong;

static void ponger(u32 index, void *shared)
{
    NPH_UNUSED(index);
    PingPong *p = (PingPong *)shared;
    for (u32 i = 0; i < p->rounds; ++i) {
        nph_semaphore_wait(&p->ping);
        p->counter += 1U;
        nph_semaphore_post(&p->pong);
    }
}

NTEST(semaphore, ping_pong_hands_off_between_threads)
{
    PingPong p;
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&p.ping, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&p.pong, 0U), NPH_RESULT_OK);
    p.rounds = 20000U * TEST_SCALE;
    p.counter = 0U;
    TestWorker w;
    test_threads_start(&w, 1U, ponger, &p);
    for (u32 i = 0; i < p.rounds; ++i) {
        p.counter += 1U;
        nph_semaphore_post(&p.ping);
        nph_semaphore_wait(&p.pong);
    }
    test_threads_join(&w, 1U);
    NTEST_ASSERT_EQ_UINT(p.counter, (u64)p.rounds * 2U);
    nph_semaphore_destroy(&p.pong);
    nph_semaphore_destroy(&p.ping);
}

typedef struct Pool {
    NphSemaphore work;      // one permit per unit of work
    NphSemaphore done;      // one permit per finished unit
    NphAtomicU32 remaining; // units left to hand out
    NphAtomicU32 processed;
} Pool;

static void pool_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Pool *p = (Pool *)shared;
    for (;;) {
        nph_semaphore_wait(&p->work);
        if (nph_atomic_load_u32(&p->remaining, NPH_MEMORY_ORDER_ACQUIRE) == NPH_U32_MAX) {
            return; // shutdown permit
        }
        nph_atomic_fetch_add_u32(&p->processed, 1U, NPH_MEMORY_ORDER_RELAXED);
        nph_semaphore_post(&p->done);
    }
}

NTEST(semaphore, many_waiters_with_post_n)
{
    Pool p;
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&p.work, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&p.done, 0U), NPH_RESULT_OK);
    nph_atomic_store_u32(&p.remaining, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&p.processed, 0U, NPH_MEMORY_ORDER_RELAXED);
    const u32 n = test_worker_count(8U);
    TestWorker workers[TEST_MAX_WORKERS];
    test_threads_start(workers, n, pool_worker, &p);
    const u32 batches = 200U * TEST_SCALE;
    const u32 batch = 16U;
    for (u32 b = 0; b < batches; ++b) {
        nph_semaphore_post_n(&p.work, batch);
        for (u32 i = 0; i < batch; ++i) {
            nph_semaphore_wait(&p.done);
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.processed, NPH_MEMORY_ORDER_RELAXED),
                         batches * batch);
    nph_atomic_store_u32(&p.remaining, NPH_U32_MAX, NPH_MEMORY_ORDER_RELEASE);
    nph_semaphore_post_n(&p.work, n);
    test_threads_join(workers, n);
    nph_semaphore_destroy(&p.done);
    nph_semaphore_destroy(&p.work);
}

typedef struct TimedWaiters {
    NphSemaphore sem;
    NphAtomicU32 got;
    NphAtomicU32 timed_out;
} TimedWaiters;

static void timed_waiter(u32 index, void *shared)
{
    NPH_UNUSED(index);
    TimedWaiters *t = (TimedWaiters *)shared;
    if (nph_semaphore_wait_timeout(&t->sem, 200U * NPH_TIME_NS_PER_MS)) {
        nph_atomic_fetch_add_u32(&t->got, 1U, NPH_MEMORY_ORDER_RELAXED);
    } else {
        nph_atomic_fetch_add_u32(&t->timed_out, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
}

NTEST(semaphore, timed_waiters_and_posts_balance)
{
    // Four waiters, two posts: exactly two get a permit, two time out, and
    // the semaphore ends up empty (no permit lost or duplicated).
    TimedWaiters t;
    NTEST_ASSERT_EQ_INT(nph_semaphore_init(&t.sem, 0U), NPH_RESULT_OK);
    nph_atomic_store_u32(&t.got, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&t.timed_out, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker workers[4];
    test_threads_start(workers, 4U, timed_waiter, &t);
    nph_thread_sleep_ns(5U * NPH_TIME_NS_PER_MS);
    nph_semaphore_post_n(&t.sem, 2U);
    test_threads_join(workers, 4U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.got, NPH_MEMORY_ORDER_RELAXED), 2U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.timed_out, NPH_MEMORY_ORDER_RELAXED), 2U);
    NTEST_ASSERT_FALSE(nph_semaphore_try_wait(&t.sem));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.sem.count, NPH_MEMORY_ORDER_RELAXED),
                         NPH_SEMAPHORE_BIAS);
    nph_semaphore_destroy(&t.sem);
}

// ---------------------------------------------------------------------------
// Once
// ---------------------------------------------------------------------------

typedef struct OnceShared {
    NphOnce once;
    NphAtomicU32 runs;
    NphAtomicU32 observed_done;
} OnceShared;

static void once_init(void *user)
{
    OnceShared *s = (OnceShared *)user;
    nph_thread_sleep_ns(NPH_TIME_NS_PER_MS); // widen the window for racers
    nph_atomic_fetch_add_u32(&s->runs, 1U, NPH_MEMORY_ORDER_RELAXED);
}

static void once_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    OnceShared *s = (OnceShared *)shared;
    nph_once(&s->once, once_init, s);
    // After nph_once returns, the initialiser's effects are visible.
    if (nph_atomic_load_u32(&s->runs, NPH_MEMORY_ORDER_RELAXED) == 1U &&
        nph_once_is_done(&s->once)) {
        nph_atomic_fetch_add_u32(&s->observed_done, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
}

NTEST(once, runs_exactly_once_across_threads)
{
    for (u32 round = 0; round < 4U; ++round) {
        OnceShared s;
        NphOnce init = NPH_ONCE_INIT;
        s.once = init;
        nph_atomic_store_u32(&s.runs, 0U, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_store_u32(&s.observed_done, 0U, NPH_MEMORY_ORDER_RELAXED);
        NTEST_ASSERT_FALSE(nph_once_is_done(&s.once));
        TestWorker workers[8];
        test_threads_run(workers, 8U, once_worker, &s);
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.runs, NPH_MEMORY_ORDER_RELAXED), 1U);
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.observed_done, NPH_MEMORY_ORDER_RELAXED), 8U);
        NTEST_ASSERT_TRUE(nph_once_is_done(&s.once));
        nph_once(&s.once, once_init, &s); // no-op now
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.runs, NPH_MEMORY_ORDER_RELAXED), 1U);
    }
}

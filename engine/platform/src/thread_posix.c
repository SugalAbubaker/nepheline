// SPDX-License-Identifier: Apache-2.0
// Threads and synchronisation on pthreads (Linux).
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "platform_internal.h"

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

NPH_STATIC_ASSERT(sizeof(pthread_mutex_t) <= NPH_MUTEX_SIZE, "NphMutex storage too small");
NPH_STATIC_ASSERT(_Alignof(pthread_mutex_t) <= 8U, "NphMutex storage under-aligned");
NPH_STATIC_ASSERT(sizeof(pthread_cond_t) <= NPH_CONDVAR_SIZE, "NphCondVar storage too small");
NPH_STATIC_ASSERT(_Alignof(pthread_cond_t) <= 8U, "NphCondVar storage under-aligned");
NPH_STATIC_ASSERT(sizeof(pthread_rwlock_t) <= NPH_RWLOCK_SIZE, "NphRwLock storage too small");
NPH_STATIC_ASSERT(_Alignof(pthread_rwlock_t) <= 8U, "NphRwLock storage under-aligned");
NPH_STATIC_ASSERT(sizeof(sem_t) <= NPH_SEMAPHORE_SIZE, "NphSemaphore storage too small");
NPH_STATIC_ASSERT(_Alignof(sem_t) <= 8U, "NphSemaphore storage under-aligned");
NPH_STATIC_ASSERT(sizeof(pthread_t) <= sizeof(u64), "pthread_t must fit NphThread.native");

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

typedef struct ThreadStart {
    NphThreadFn fn;
    void *user;
    char name[NPH_THREAD_NAME_MAX];
    u64 tid; // written by the new thread before `copied`
    NphAtomicU32 copied;
} ThreadStart;

void nph_thread_exit_cleanup(void)
{
    nph_log_thread_detach();
}

static void *thread_main(void *arg)
{
    ThreadStart *start = (ThreadStart *)arg;
    const NphThreadFn fn = start->fn;
    void *const user = start->user;
    char name[NPH_THREAD_NAME_MAX];
    memcpy(name, start->name, sizeof name);
    start->tid = nph_thread_current_id();
    nph_atomic_store_u32(&start->copied, 1U, NPH_MEMORY_ORDER_RELEASE);
    // `start` lives on the creator's stack and is invalid from here on.
    if (name[0] != '\0') {
        nph_thread_set_current_name(name);
    }
    fn(user);
    nph_thread_exit_cleanup();
    return NULL;
}

NphResult nph_thread_create(NphThread *out, const NphThreadDesc *desc)
{
    NPH_ASSERT(out != NULL && desc != NULL);
    memset(out, 0, sizeof *out);
    if (desc->fn == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    ThreadStart start;
    start.fn = desc->fn;
    start.user = desc->user;
    nph_thread_copy_name(start.name, desc->name);
    start.tid = 0U;
    nph_atomic_store_u32(&start.copied, 0U, NPH_MEMORY_ORDER_RELAXED);

    pthread_attr_t attr;
    int err = pthread_attr_init(&attr);
    if (err != 0) {
        return nph_posix_fail("pthread_attr_init", err);
    }
    if (desc->stack_size != 0U) {
        const u64 page = (u64)sysconf(_SC_PAGESIZE);
        u64 size = (desc->stack_size + page - 1U) & ~(page - 1U);
        if (size < (u64)PTHREAD_STACK_MIN) {
            size = (u64)PTHREAD_STACK_MIN;
        }
        err = pthread_attr_setstacksize(&attr, (size_t)size);
        if (err != 0) {
            pthread_attr_destroy(&attr);
            return nph_posix_fail("pthread_attr_setstacksize", err);
        }
    }
    pthread_t handle;
    err = pthread_create(&handle, &attr, thread_main, &start);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        // EAGAIN here means a thread or memory limit, not "try again".
        const NphResult r = nph_posix_fail("pthread_create", err);
        return r == NPH_RESULT_BUSY ? NPH_RESULT_OUT_OF_MEMORY : r;
    }
    while (nph_atomic_load_u32(&start.copied, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
        nph_thread_yield();
    }
    out->native = (u64)handle;
    out->id = start.tid;
    out->joinable = 1;
    return NPH_RESULT_OK;
}

void nph_thread_join(NphThread *t)
{
    NPH_ASSERT(t != NULL && t->joinable);
    NPH_ASSERT_MSG(!nph_thread_is_current(t), "a thread cannot join itself");
    NPH_VERIFY(pthread_join((pthread_t)t->native, NULL) == 0);
    memset(t, 0, sizeof *t);
}

void nph_thread_detach(NphThread *t)
{
    NPH_ASSERT(t != NULL && t->joinable);
    NPH_VERIFY(pthread_detach((pthread_t)t->native) == 0);
    memset(t, 0, sizeof *t);
}

void nph_thread_current(NphThread *out)
{
    NPH_ASSERT(out != NULL);
    out->native = (u64)pthread_self();
    out->id = nph_thread_current_id();
    out->joinable = 0;
}

u64 nph_thread_current_id(void)
{
    return (u64)syscall(SYS_gettid);
}

bool nph_thread_is_current(const NphThread *t)
{
    NPH_ASSERT(t != NULL);
    return t->id == nph_thread_current_id();
}

NphResult nph_thread_set_affinity(const NphThread *t, u64 mask)
{
    NPH_ASSERT(t != NULL);
    if (mask == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (u32 i = 0; i < 64U; ++i) {
        if ((mask & ((u64)1U << i)) != 0U) {
            CPU_SET(i, &set);
        }
    }
    const int err = pthread_setaffinity_np((pthread_t)t->native, sizeof set, &set);
    if (err != 0) {
        return nph_posix_fail("pthread_setaffinity_np", err);
    }
    return NPH_RESULT_OK;
}

NphResult nph_thread_set_priority(const NphThread *t, NphThreadPriority priority)
{
    NPH_ASSERT(t != NULL);
    // Nice values per thread (a Linux extension of setpriority). Raising
    // above 0 needs CAP_SYS_NICE or an RLIMIT_NICE grant; SCHED_FIFO for the
    // time-critical class would need the same and is left to the runtime's
    // configuration.
    int nice_value;
    switch (priority) {
    case NPH_THREAD_PRIORITY_LOW:
        nice_value = 10;
        break;
    case NPH_THREAD_PRIORITY_NORMAL:
        nice_value = 0;
        break;
    case NPH_THREAD_PRIORITY_HIGH:
        nice_value = -5;
        break;
    case NPH_THREAD_PRIORITY_TIME_CRITICAL:
        nice_value = -15;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (setpriority(PRIO_PROCESS, (id_t)t->id, nice_value) != 0) {
        const int err = errno;
        if (err == EACCES || err == EPERM) {
            NPH_LOG_DEBUG(&NPH_LOG_PLATFORM, "thread priority %d refused (unprivileged)",
                          nice_value);
            return NPH_RESULT_ACCESS_DENIED;
        }
        return nph_posix_fail("setpriority", err);
    }
    return NPH_RESULT_OK;
}

void nph_thread_set_current_name(const char *name)
{
    NPH_ASSERT(name != NULL);
    char truncated[16]; // the kernel limit: 15 bytes plus the terminator
    u32 i = 0U;
    for (; i + 1U < sizeof truncated && name[i] != '\0'; ++i) {
        truncated[i] = name[i];
    }
    truncated[i] = '\0';
    (void)pthread_setname_np(pthread_self(), truncated);
}

static struct timespec timespec_from_ns(u64 ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / NPH_TIME_NS_PER_S);
    ts.tv_nsec = (long)(ns % NPH_TIME_NS_PER_S);
    return ts;
}

// Absolute CLOCK_MONOTONIC deadline `timeout_ns` from now, saturating.
static struct timespec deadline_from_now(u64 timeout_ns)
{
    const u64 now = nph_time_now_ns();
    const u64 deadline = timeout_ns > NPH_U64_MAX - now ? NPH_U64_MAX : now + timeout_ns;
    return timespec_from_ns(deadline);
}

void nph_thread_sleep_ns(u64 ns)
{
    if (ns == 0U) {
        nph_thread_yield();
        return;
    }
    struct timespec req = timespec_from_ns(ns);
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &req) == EINTR) {
    }
}

void nph_thread_yield(void)
{
    sched_yield();
}

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

static pthread_mutex_t *mutex_native(NphMutex *m)
{
    return (pthread_mutex_t *)m->storage;
}

void nph_mutex_init(NphMutex *m)
{
    NPH_ASSERT(m != NULL);
    NPH_VERIFY(pthread_mutex_init(mutex_native(m), NULL) == 0);
}

void nph_mutex_destroy(NphMutex *m)
{
    NPH_ASSERT(m != NULL);
    NPH_VERIFY(pthread_mutex_destroy(mutex_native(m)) == 0);
}

void nph_mutex_lock(NphMutex *m)
{
    NPH_VERIFY(pthread_mutex_lock(mutex_native(m)) == 0);
}

bool nph_mutex_try_lock(NphMutex *m)
{
    return pthread_mutex_trylock(mutex_native(m)) == 0;
}

void nph_mutex_unlock(NphMutex *m)
{
    NPH_VERIFY(pthread_mutex_unlock(mutex_native(m)) == 0);
}

// ---------------------------------------------------------------------------
// Condition variable. Timed waits use pthread_cond_clockwait (glibc 2.30)
// against CLOCK_MONOTONIC, so a wall-clock change never cuts a wait short.
// ---------------------------------------------------------------------------

static pthread_cond_t *condvar_native(NphCondVar *cv)
{
    return (pthread_cond_t *)cv->storage;
}

void nph_condvar_init(NphCondVar *cv)
{
    NPH_ASSERT(cv != NULL);
    NPH_VERIFY(pthread_cond_init(condvar_native(cv), NULL) == 0);
}

void nph_condvar_destroy(NphCondVar *cv)
{
    NPH_ASSERT(cv != NULL);
    NPH_VERIFY(pthread_cond_destroy(condvar_native(cv)) == 0);
}

void nph_condvar_wait(NphCondVar *cv, NphMutex *m)
{
    NPH_VERIFY(pthread_cond_wait(condvar_native(cv), mutex_native(m)) == 0);
}

bool nph_condvar_wait_timeout(NphCondVar *cv, NphMutex *m, u64 timeout_ns)
{
    const struct timespec deadline = deadline_from_now(timeout_ns);
    const int err =
        pthread_cond_clockwait(condvar_native(cv), mutex_native(m), CLOCK_MONOTONIC, &deadline);
    NPH_ASSERT(err == 0 || err == ETIMEDOUT);
    return err == 0;
}

void nph_condvar_signal(NphCondVar *cv)
{
    NPH_VERIFY(pthread_cond_signal(condvar_native(cv)) == 0);
}

void nph_condvar_broadcast(NphCondVar *cv)
{
    NPH_VERIFY(pthread_cond_broadcast(condvar_native(cv)) == 0);
}

// ---------------------------------------------------------------------------
// Read-write lock
// ---------------------------------------------------------------------------

static pthread_rwlock_t *rwlock_native(NphRwLock *rw)
{
    return (pthread_rwlock_t *)rw->storage;
}

void nph_rwlock_init(NphRwLock *rw)
{
    NPH_ASSERT(rw != NULL);
    NPH_VERIFY(pthread_rwlock_init(rwlock_native(rw), NULL) == 0);
}

void nph_rwlock_destroy(NphRwLock *rw)
{
    NPH_ASSERT(rw != NULL);
    NPH_VERIFY(pthread_rwlock_destroy(rwlock_native(rw)) == 0);
}

void nph_rwlock_lock_shared(NphRwLock *rw)
{
    NPH_VERIFY(pthread_rwlock_rdlock(rwlock_native(rw)) == 0);
}

bool nph_rwlock_try_lock_shared(NphRwLock *rw)
{
    return pthread_rwlock_tryrdlock(rwlock_native(rw)) == 0;
}

void nph_rwlock_unlock_shared(NphRwLock *rw)
{
    NPH_VERIFY(pthread_rwlock_unlock(rwlock_native(rw)) == 0);
}

void nph_rwlock_lock_exclusive(NphRwLock *rw)
{
    NPH_VERIFY(pthread_rwlock_wrlock(rwlock_native(rw)) == 0);
}

bool nph_rwlock_try_lock_exclusive(NphRwLock *rw)
{
    return pthread_rwlock_trywrlock(rwlock_native(rw)) == 0;
}

void nph_rwlock_unlock_exclusive(NphRwLock *rw)
{
    NPH_VERIFY(pthread_rwlock_unlock(rwlock_native(rw)) == 0);
}

// ---------------------------------------------------------------------------
// POSIX semaphore behind NphSemaphore
// ---------------------------------------------------------------------------

static sem_t *semaphore_native(NphSemaphore *s)
{
    return (sem_t *)s->native;
}

NphResult nph_semaphore_os_init(NphSemaphore *s)
{
    if (sem_init(semaphore_native(s), 0, 0U) != 0) {
        return nph_posix_fail("sem_init", errno);
    }
    return NPH_RESULT_OK;
}

void nph_semaphore_os_destroy(NphSemaphore *s)
{
    NPH_VERIFY(sem_destroy(semaphore_native(s)) == 0);
}

void nph_semaphore_os_wait(NphSemaphore *s)
{
    while (sem_wait(semaphore_native(s)) != 0) {
        NPH_ASSERT(errno == EINTR);
    }
}

bool nph_semaphore_os_wait_timeout(NphSemaphore *s, u64 timeout_ns)
{
    const struct timespec deadline = deadline_from_now(timeout_ns);
    for (;;) {
        if (sem_clockwait(semaphore_native(s), CLOCK_MONOTONIC, &deadline) == 0) {
            return true;
        }
        if (errno == ETIMEDOUT) {
            return false;
        }
        NPH_ASSERT(errno == EINTR);
    }
}

bool nph_semaphore_os_try_wait(NphSemaphore *s)
{
    return sem_trywait(semaphore_native(s)) == 0;
}

void nph_semaphore_os_post(NphSemaphore *s, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        NPH_VERIFY(sem_post(semaphore_native(s)) == 0);
    }
}

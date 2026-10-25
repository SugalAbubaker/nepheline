// SPDX-License-Identifier: Apache-2.0
// Threads, SRW locks, condition variables, and kernel semaphores on Win32.
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/core/realtime.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "platform_internal.h"

#include <limits.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(SRWLOCK) <= NPH_MUTEX_SIZE, "NphMutex storage too small");
NPH_STATIC_ASSERT(sizeof(CONDITION_VARIABLE) <= NPH_CONDVAR_SIZE, "NphCondVar storage too small");
NPH_STATIC_ASSERT(sizeof(SRWLOCK) <= NPH_RWLOCK_SIZE, "NphRwLock storage too small");
NPH_STATIC_ASSERT(sizeof(HANDLE) <= NPH_SEMAPHORE_SIZE, "NphSemaphore storage too small");

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

typedef struct ThreadStart {
    NphThreadFn fn;
    void *user;
    char name[NPH_THREAD_NAME_MAX];
    NphAtomicU32 copied;
} ThreadStart;

// High-resolution timer for nph_thread_sleep_ns, created on first use and
// closed by the trampoline when the thread ends.
static NPH_THREAD_LOCAL HANDLE t_sleep_timer;

void nph_thread_exit_cleanup(void)
{
    if (t_sleep_timer != NULL) {
        CloseHandle(t_sleep_timer);
        t_sleep_timer = NULL;
    }
    nph_log_thread_detach();
}

static DWORD WINAPI thread_main(LPVOID arg)
{
    ThreadStart *start = (ThreadStart *)arg;
    const NphThreadFn fn = start->fn;
    void *const user = start->user;
    char name[NPH_THREAD_NAME_MAX];
    memcpy(name, start->name, sizeof name);
    nph_atomic_store_u32(&start->copied, 1U, NPH_MEMORY_ORDER_RELEASE);
    // `start` lives on the creator's stack and is invalid from here on.
    if (name[0] != '\0') {
        nph_thread_set_current_name(name);
    }
    fn(user);
    nph_thread_exit_cleanup();
    return 0U;
}

static HANDLE thread_handle(const NphThread *t)
{
    return (HANDLE)(uptr)t->native;
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
    nph_atomic_store_u32(&start.copied, 0U, NPH_MEMORY_ORDER_RELAXED);

    DWORD id = 0U;
    const DWORD flags = desc->stack_size != 0U ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0U;
    const HANDLE h = CreateThread(NULL, (SIZE_T)desc->stack_size, thread_main, &start, flags, &id);
    if (h == NULL) {
        return nph_win32_fail("CreateThread", GetLastError());
    }
    while (nph_atomic_load_u32(&start.copied, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
        nph_thread_yield();
    }
    out->native = (u64)(uptr)h;
    out->id = id;
    out->joinable = 1;
    return NPH_RESULT_OK;
}

void nph_thread_join(NphThread *t)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_WAIT, "nph_thread_join");
    NPH_ASSERT(t != NULL && t->joinable);
    NPH_ASSERT_MSG(!nph_thread_is_current(t), "a thread cannot join itself");
    const HANDLE h = thread_handle(t);
    NPH_VERIFY(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(h);
    memset(t, 0, sizeof *t);
}

void nph_thread_detach(NphThread *t)
{
    NPH_ASSERT(t != NULL && t->joinable);
    CloseHandle(thread_handle(t));
    memset(t, 0, sizeof *t);
}

void nph_thread_current(NphThread *out)
{
    NPH_ASSERT(out != NULL);
    // The pseudo-handle is only meaningful on this thread, which is exactly
    // where the setters that take it are used.
    out->native = (u64)(uptr)GetCurrentThread();
    out->id = GetCurrentThreadId();
    out->joinable = 0;
}

u64 nph_thread_current_id(void)
{
    return GetCurrentThreadId();
}

bool nph_thread_is_current(const NphThread *t)
{
    NPH_ASSERT(t != NULL);
    return t->id == (u64)GetCurrentThreadId();
}

NphResult nph_thread_set_affinity(const NphThread *t, u64 mask)
{
    NPH_ASSERT(t != NULL);
    if (mask == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (SetThreadAffinityMask(thread_handle(t), (DWORD_PTR)mask) == 0U) {
        return nph_win32_fail("SetThreadAffinityMask", GetLastError());
    }
    return NPH_RESULT_OK;
}

NphResult nph_thread_set_priority(const NphThread *t, NphThreadPriority priority)
{
    NPH_ASSERT(t != NULL);
    int native;
    switch (priority) {
    case NPH_THREAD_PRIORITY_LOW:
        native = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case NPH_THREAD_PRIORITY_NORMAL:
        native = THREAD_PRIORITY_NORMAL;
        break;
    case NPH_THREAD_PRIORITY_HIGH:
        native = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case NPH_THREAD_PRIORITY_TIME_CRITICAL:
        native = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!SetThreadPriority(thread_handle(t), native)) {
        return nph_win32_fail("SetThreadPriority", GetLastError());
    }
    return NPH_RESULT_OK;
}

void nph_thread_set_current_name(const char *name)
{
    NPH_ASSERT(name != NULL);
    char truncated[NPH_THREAD_NAME_MAX];
    nph_thread_copy_name(truncated, name);
    wchar_t wide[NPH_THREAD_NAME_MAX];
    if (nph_win32_utf8_to_wide(truncated, wide, (u32)NPH_ARRAY_COUNT(wide)) != NPH_RESULT_OK) {
        // Truncation may have cut a multi-byte sequence; keep the ASCII prefix.
        u32 i = 0U;
        for (; truncated[i] != '\0' && (u8)truncated[i] < 0x80U; ++i) {
            wide[i] = (wchar_t)truncated[i];
        }
        wide[i] = L'\0';
    }
    // Names are diagnostics; a failure (pre-1607 Windows) changes nothing.
    (void)SetThreadDescription(GetCurrentThread(), wide);
}

void nph_thread_sleep_ns(u64 ns)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_SLEEP, "nph_thread_sleep_ns");
    if (ns == 0U) {
        nph_thread_yield();
        return;
    }
    if (t_sleep_timer == NULL) {
        // Rejected before Windows 10 1803; a regular waitable timer has the
        // same granularity as Sleep, so there is no point creating one.
        t_sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
    }
    if (t_sleep_timer != NULL) {
        LARGE_INTEGER due;
        const u64 ticks = (ns + 99U) / 100U; // 100 ns units, negative = relative
        due.QuadPart = ticks > (u64)INT64_MAX ? INT64_MIN : -(LONGLONG)ticks;
        if (SetWaitableTimerEx(t_sleep_timer, &due, 0, NULL, NULL, NULL, 0U) &&
            WaitForSingleObject(t_sleep_timer, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }
    const u64 ms = (ns + NPH_TIME_NS_PER_MS - 1U) / NPH_TIME_NS_PER_MS;
    Sleep(ms >= INFINITE ? INFINITE - 1U : (DWORD)ms);
}

void nph_thread_yield(void)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_SLEEP, "nph_thread_yield");
    SwitchToThread();
}

// ---------------------------------------------------------------------------
// Mutex (SRWLOCK, exclusive mode)
// ---------------------------------------------------------------------------

static SRWLOCK *mutex_lock(NphMutex *m)
{
    return (SRWLOCK *)m->storage;
}

void nph_mutex_init(NphMutex *m)
{
    NPH_ASSERT(m != NULL);
    InitializeSRWLock(mutex_lock(m));
}

void nph_mutex_destroy(NphMutex *m)
{
    NPH_ASSERT(m != NULL);
    NPH_UNUSED(m); // no kernel object to release
#if NPH_ASSERTS_ENABLED
    const BOOLEAN unlocked = TryAcquireSRWLockExclusive(mutex_lock(m));
    NPH_ASSERT_MSG(unlocked, "destroying a locked mutex");
    if (unlocked) {
        ReleaseSRWLockExclusive(mutex_lock(m));
    }
#endif
}

void nph_mutex_lock(NphMutex *m)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_LOCK, "nph_mutex_lock");
    AcquireSRWLockExclusive(mutex_lock(m));
}

bool nph_mutex_try_lock(NphMutex *m)
{
    return TryAcquireSRWLockExclusive(mutex_lock(m)) != 0;
}

void nph_mutex_unlock(NphMutex *m)
{
    ReleaseSRWLockExclusive(mutex_lock(m));
}

// ---------------------------------------------------------------------------
// Condition variable
// ---------------------------------------------------------------------------

static CONDITION_VARIABLE *condvar(NphCondVar *cv)
{
    return (CONDITION_VARIABLE *)cv->storage;
}

static DWORD timeout_ms(u64 ns)
{
    const u64 ms = (ns + NPH_TIME_NS_PER_MS - 1U) / NPH_TIME_NS_PER_MS;
    return ms >= INFINITE ? INFINITE - 1U : (DWORD)ms;
}

void nph_condvar_init(NphCondVar *cv)
{
    NPH_ASSERT(cv != NULL);
    InitializeConditionVariable(condvar(cv));
}

void nph_condvar_destroy(NphCondVar *cv)
{
    NPH_UNUSED(cv); // no kernel object to release
}

void nph_condvar_wait(NphCondVar *cv, NphMutex *m)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_WAIT, "nph_condvar_wait");
    NPH_VERIFY(SleepConditionVariableSRW(condvar(cv), mutex_lock(m), INFINITE, 0U));
}

bool nph_condvar_wait_timeout(NphCondVar *cv, NphMutex *m, u64 timeout_ns)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_WAIT, "nph_condvar_wait_timeout");
    if (SleepConditionVariableSRW(condvar(cv), mutex_lock(m), timeout_ms(timeout_ns), 0U)) {
        return true;
    }
    NPH_ASSERT(GetLastError() == ERROR_TIMEOUT);
    return false;
}

void nph_condvar_signal(NphCondVar *cv)
{
    WakeConditionVariable(condvar(cv));
}

void nph_condvar_broadcast(NphCondVar *cv)
{
    WakeAllConditionVariable(condvar(cv));
}

// ---------------------------------------------------------------------------
// Read-write lock (SRWLOCK, both modes)
// ---------------------------------------------------------------------------

static SRWLOCK *rw_lock(NphRwLock *rw)
{
    return (SRWLOCK *)rw->storage;
}

void nph_rwlock_init(NphRwLock *rw)
{
    NPH_ASSERT(rw != NULL);
    InitializeSRWLock(rw_lock(rw));
}

void nph_rwlock_destroy(NphRwLock *rw)
{
    NPH_ASSERT(rw != NULL);
    NPH_UNUSED(rw); // no kernel object to release
#if NPH_ASSERTS_ENABLED
    const BOOLEAN unlocked = TryAcquireSRWLockExclusive(rw_lock(rw));
    NPH_ASSERT_MSG(unlocked, "destroying a held read-write lock");
    if (unlocked) {
        ReleaseSRWLockExclusive(rw_lock(rw));
    }
#endif
}

void nph_rwlock_lock_shared(NphRwLock *rw)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_LOCK, "nph_rwlock_lock_shared");
    AcquireSRWLockShared(rw_lock(rw));
}

bool nph_rwlock_try_lock_shared(NphRwLock *rw)
{
    return TryAcquireSRWLockShared(rw_lock(rw)) != 0;
}

void nph_rwlock_unlock_shared(NphRwLock *rw)
{
    ReleaseSRWLockShared(rw_lock(rw));
}

void nph_rwlock_lock_exclusive(NphRwLock *rw)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_LOCK, "nph_rwlock_lock_exclusive");
    AcquireSRWLockExclusive(rw_lock(rw));
}

bool nph_rwlock_try_lock_exclusive(NphRwLock *rw)
{
    return TryAcquireSRWLockExclusive(rw_lock(rw)) != 0;
}

void nph_rwlock_unlock_exclusive(NphRwLock *rw)
{
    ReleaseSRWLockExclusive(rw_lock(rw));
}

// ---------------------------------------------------------------------------
// Kernel semaphore behind NphSemaphore
// ---------------------------------------------------------------------------

static HANDLE *semaphore_handle(NphSemaphore *s)
{
    return (HANDLE *)s->native;
}

NphResult nph_semaphore_os_init(NphSemaphore *s)
{
    const HANDLE h = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
    if (h == NULL) {
        *semaphore_handle(s) = NULL;
        return nph_win32_fail("CreateSemaphoreW", GetLastError());
    }
    *semaphore_handle(s) = h;
    return NPH_RESULT_OK;
}

void nph_semaphore_os_destroy(NphSemaphore *s)
{
    HANDLE *h = semaphore_handle(s);
    if (*h != NULL) {
        CloseHandle(*h);
        *h = NULL;
    }
}

void nph_semaphore_os_wait(NphSemaphore *s)
{
    NPH_VERIFY(WaitForSingleObject(*semaphore_handle(s), INFINITE) == WAIT_OBJECT_0);
}

bool nph_semaphore_os_wait_timeout(NphSemaphore *s, u64 timeout_ns)
{
    return WaitForSingleObject(*semaphore_handle(s), timeout_ms(timeout_ns)) == WAIT_OBJECT_0;
}

bool nph_semaphore_os_try_wait(NphSemaphore *s)
{
    return WaitForSingleObject(*semaphore_handle(s), 0U) == WAIT_OBJECT_0;
}

void nph_semaphore_os_post(NphSemaphore *s, u32 n)
{
    NPH_VERIFY(ReleaseSemaphore(*semaphore_handle(s), (LONG)n, NULL));
}

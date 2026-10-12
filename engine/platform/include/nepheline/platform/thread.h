// SPDX-License-Identifier: Apache-2.0
#pragma once

// Threads and blocking synchronisation (CLAUDE.md §8.3 "Threads, mutexes,
// condition variables, semaphores, thread affinity, thread names").
//
// These wrap the OS primitives: Win32 threads, SRW locks, condition
// variables and kernel semaphores on Windows; pthreads and POSIX semaphores
// on Linux. The lock-free building blocks are in core (atomic.h, ring.h);
// the job system (Phase 4) is the engine's scheduler and uses these only to
// park and wake worker threads. Game and engine code never blocks on a
// mutex in a frame, so nothing here is tuned for contention beyond what the
// OS provides.
//
// Each primitive is a fixed-size, in-place object: no allocation, and the
// native object lives inside the struct (sized per platform below; the
// implementation checks the sizes with static assertions). A zero-filled
// mutex, condition variable, or read-write lock is a valid initialised
// object on both platforms, so statics can use the *_INIT macros; the
// semaphore owns a kernel object and needs nph_semaphore_init.
//
// Thread-safety: creation and destruction of an object are single-threaded;
// everything else is NPH_THREADSAFE by definition.

#include <nepheline/core/atomic.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

typedef void (*NphThreadFn)(void *user);

// Thread names are shown by debuggers and profilers. Linux truncates them
// to 15 bytes; keep the distinguishing part at the front ("job-worker-3").
#define NPH_THREAD_NAME_MAX 32U

typedef struct NphThreadDesc {
    NphThreadFn fn;   // required
    void *user;       // passed to fn
    const char *name; // optional, copied before nph_thread_create returns
    u64 stack_size;   // 0: the OS default (1 MiB on Windows, 8 MiB on Linux)
} NphThreadDesc;

typedef struct NphThread {
    u64 native;   // HANDLE or pthread_t
    u64 id;       // OS thread id, as nph_thread_current_id reports it
    b32 joinable; // created by nph_thread_create and not yet joined or detached
} NphThread;

typedef enum NphThreadPriority {
    NPH_THREAD_PRIORITY_LOW,           // background work: asset cooking, streaming decompression
    NPH_THREAD_PRIORITY_NORMAL,        // the default
    NPH_THREAD_PRIORITY_HIGH,          // the main thread and job workers
    NPH_THREAD_PRIORITY_TIME_CRITICAL, // the audio thread
} NphThreadPriority;

// Starts a thread running desc->fn(desc->user). Returns once the new thread
// has copied the description, so `desc` may live on the caller's stack. The
// thread's name is applied on the new thread before fn runs. When fn
// returns, the thread's log ring (core/log.h) is detached automatically.
// Every created thread must be joined or detached exactly once.
// NPH_RESULT_OUT_OF_MEMORY when the OS cannot create the thread,
// NPH_RESULT_INVALID_ARGUMENT for a NULL fn.
NPH_MUST_USE NphResult nph_thread_create(NphThread *out, const NphThreadDesc *desc);

// Waits for the thread to finish and releases it. `t` must be joinable
// (asserted) and becomes empty.
void nph_thread_join(NphThread *t);

// Releases the handle; the thread keeps running and cleans up by itself.
// `t` must be joinable (asserted) and becomes empty.
void nph_thread_detach(NphThread *t);

// A non-joinable handle to the calling thread, for the setters below.
void nph_thread_current(NphThread *out);

// The calling thread's OS id. Stable for the thread's lifetime; ids are
// reused by the OS after a thread exits.
u64 nph_thread_current_id(void);

// True when `t` refers to the calling thread.
bool nph_thread_is_current(const NphThread *t);

// Restricts the thread to the logical cores whose bits are set in `mask`
// (bit i is core i, as enumerated by the OS; only the first 64 cores are
// addressable). A zero mask is NPH_RESULT_INVALID_ARGUMENT. Bits beyond
// the machine's core count are ignored; if nothing remains the OS refuses
// and NPH_RESULT_INVALID_ARGUMENT is returned.
NphResult nph_thread_set_affinity(const NphThread *t, u64 mask);

// Sets the scheduling priority. On Linux only lowering is possible without
// privileges; a refused raise reports NPH_RESULT_ACCESS_DENIED and leaves
// the priority unchanged.
NphResult nph_thread_set_priority(const NphThread *t, NphThreadPriority priority);

// Names the calling thread (nph_thread_create does this for you). Longer
// names are truncated to the platform limit.
void nph_thread_set_current_name(const char *name);

// Blocks for at least `ns` nanoseconds. Uses a high-resolution timer on
// Windows 10 1803+ (about 0.5 ms accuracy) and clock_nanosleep on Linux.
// Zero yields.
void nph_thread_sleep_ns(u64 ns);

// Offers the rest of the time slice to another ready thread.
void nph_thread_yield(void);

// ---------------------------------------------------------------------------
// Mutex (non-recursive). Uncontended lock/unlock is ~20 ns.
// ---------------------------------------------------------------------------

#if NPH_PLATFORM_WINDOWS
#define NPH_MUTEX_SIZE     8U
#define NPH_CONDVAR_SIZE   8U
#define NPH_RWLOCK_SIZE    8U
#define NPH_SEMAPHORE_SIZE 8U
#else
#define NPH_MUTEX_SIZE     40U
#define NPH_CONDVAR_SIZE   48U
#define NPH_RWLOCK_SIZE    56U
#define NPH_SEMAPHORE_SIZE 32U
#endif

typedef struct NphMutex {
    NPH_ALIGNAS(8) u8 storage[NPH_MUTEX_SIZE];
} NphMutex;

#define NPH_MUTEX_INIT {{0}}

// Equivalent to zero-filling. Never fails.
void nph_mutex_init(NphMutex *m);
// Must be unlocked. Safe on a zero-filled mutex.
void nph_mutex_destroy(NphMutex *m);
void nph_mutex_lock(NphMutex *m);
bool nph_mutex_try_lock(NphMutex *m);
void nph_mutex_unlock(NphMutex *m);

// ---------------------------------------------------------------------------
// Condition variable. Waits can wake spuriously; always re-check the
// predicate in a loop.
// ---------------------------------------------------------------------------

typedef struct NphCondVar {
    NPH_ALIGNAS(8) u8 storage[NPH_CONDVAR_SIZE];
} NphCondVar;

#define NPH_CONDVAR_INIT {{0}}

void nph_condvar_init(NphCondVar *cv);
void nph_condvar_destroy(NphCondVar *cv);

// Atomically unlocks `m`, waits, and relocks `m` before returning.
void nph_condvar_wait(NphCondVar *cv, NphMutex *m);

// As nph_condvar_wait with a bound; false when the time elapsed without a
// wake-up (the mutex is held again either way).
bool nph_condvar_wait_timeout(NphCondVar *cv, NphMutex *m, u64 timeout_ns);

// Wakes one waiter / every waiter. Calling with the mutex held is allowed
// and avoids a lost wake-up; calling without it is allowed too.
void nph_condvar_signal(NphCondVar *cv);
void nph_condvar_broadcast(NphCondVar *cv);

// ---------------------------------------------------------------------------
// Read-write lock: many readers or one writer. Not recursive; a thread that
// holds it in either mode must not take it again.
// ---------------------------------------------------------------------------

typedef struct NphRwLock {
    NPH_ALIGNAS(8) u8 storage[NPH_RWLOCK_SIZE];
} NphRwLock;

#define NPH_RWLOCK_INIT {{0}}

void nph_rwlock_init(NphRwLock *rw);
void nph_rwlock_destroy(NphRwLock *rw);
void nph_rwlock_lock_shared(NphRwLock *rw);
bool nph_rwlock_try_lock_shared(NphRwLock *rw);
void nph_rwlock_unlock_shared(NphRwLock *rw);
void nph_rwlock_lock_exclusive(NphRwLock *rw);
bool nph_rwlock_try_lock_exclusive(NphRwLock *rw);
void nph_rwlock_unlock_exclusive(NphRwLock *rw);

// ---------------------------------------------------------------------------
// Counting semaphore. A user-space count in front of the kernel object
// (Preshing, "Semaphores are Surprisingly Versatile", 2015): a post that
// finds no waiter and a wait that finds a permit are one atomic operation
// each and never enter the kernel, which is what makes it usable to park
// and wake job workers. Waiters spin briefly before sleeping.
// ---------------------------------------------------------------------------

typedef struct NphSemaphore {
    NphAtomicU32 count; // permits + NPH_SEMAPHORE_BIAS; below the bias: waiters
    NPH_ALIGNAS(8) u8 native[NPH_SEMAPHORE_SIZE]; // kernel semaphore the slow path sleeps on
} NphSemaphore;

// The count is kept unsigned and biased so that "negative" (waiters) and
// "positive" (permits) are both defined arithmetic.
#define NPH_SEMAPHORE_BIAS 0x80000000U

// Creates the kernel object with `initial` permits. NPH_RESULT_OUT_OF_MEMORY
// if the OS refuses. Nothing may be waiting when destroyed.
NPH_MUST_USE NphResult nph_semaphore_init(NphSemaphore *s, u32 initial);
void nph_semaphore_destroy(NphSemaphore *s);

// Adds one permit / `n` permits, waking up to that many waiters.
void nph_semaphore_post(NphSemaphore *s);
void nph_semaphore_post_n(NphSemaphore *s, u32 n);

// Takes a permit, blocking until one is available.
void nph_semaphore_wait(NphSemaphore *s);

// Takes a permit if one is available now.
bool nph_semaphore_try_wait(NphSemaphore *s);

// Takes a permit, waiting at most `timeout_ns`. false on timeout.
bool nph_semaphore_wait_timeout(NphSemaphore *s, u64 timeout_ns);

// ---------------------------------------------------------------------------
// One-time initialisation. The first caller runs fn; concurrent callers
// wait for it to finish; later callers return at the cost of one acquire
// load.
// ---------------------------------------------------------------------------

typedef struct NphOnce {
    NphAtomicU32 state; // 0 not run, 1 running, 2 done
} NphOnce;

#define NPH_ONCE_INIT {NPH_ATOMIC_U32_INIT(0U)}

typedef void (*NphOnceFn)(void *user);

void nph_once(NphOnce *once, NphOnceFn fn, void *user);

NPH_INLINE bool nph_once_is_done(const NphOnce *once)
{
    return nph_atomic_load_u32(&once->state, NPH_MEMORY_ORDER_ACQUIRE) == 2U;
}

#ifdef __cplusplus
}
#endif

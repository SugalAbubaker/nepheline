// SPDX-License-Identifier: Apache-2.0
#pragma once

// Real-time scopes (CLAUDE.md §1.4 "No hidden costs"; §8.10 "Nothing on
// the audio thread allocates or locks"; docs/core.md "Real-time scopes").
//
// A thread inside a real-time scope has promised to finish on time: it
// must not allocate, take a lock, wait, sleep, or do file I/O, any of
// which can stall it for longer than its deadline (the audio thread's mix
// callback has a few milliseconds and no second chance). The checks are
// where those costs live: the allocation entry points (alloc.h), the
// spin lock (atomic.h), and the platform's blocking calls (mutexes,
// read-write locks, condition variables, semaphore waits, sleeps, joins,
// one-time initialisation, file and asynchronous I/O). A call made inside
// a scope is a violation.
//
// Violations are counted per process. With asserts enabled (Debug and
// Development) each one also fails an assertion, unless a hook is
// installed: tests that prove a path real-time-safe install a hook that
// records violations instead, run the path, and check the count. Without
// asserts (Profile, Shipping) scopes and checks compile to nothing.
//
// What the scope cannot see is code that calls the C library or the OS
// directly. The audio module's tests close that gap in the sanitizer
// builds, whose allocator reports every malloc to a hook
// (engine/audio/tests).
//
// Scopes nest: enter and exit are counted per thread.
//
// Thread-safety: NPH_THREADSAFE. The hook is installed with
// nph_realtime_set_hook, NPH_MAIN_THREAD_ONLY, while no scope is active on
// any thread.

#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphRealtimeViolation {
    NPH_REALTIME_VIOLATION_ALLOC = 0, // an allocation, reallocation, or free (alloc.h)
    NPH_REALTIME_VIOLATION_LOCK,      // a mutex, read-write lock, or spin lock acquired
    NPH_REALTIME_VIOLATION_WAIT,      // a condition, semaphore, join, or one-time wait
    NPH_REALTIME_VIOLATION_SLEEP,     // a sleep or a yield
    NPH_REALTIME_VIOLATION_IO,        // a file or asynchronous I/O call
    NPH_REALTIME_VIOLATION_COUNT
} NphRealtimeViolation;

// Records a violation instead of the assertion. `what` names the call
// ("nph_alloc", "nph_mutex_lock"); `site` is the caller's location where
// the call has one (allocations), or the checker's.
typedef void (*NphRealtimeHook)(NphRealtimeViolation kind, const char *what, NphCallsite site,
                                void *user);

// A static name for `kind` ("alloc", "lock", ...; "invalid" otherwise).
const char *nph_realtime_violation_name(NphRealtimeViolation kind);

#if NPH_ASSERTS_ENABLED

// Enters / leaves a real-time scope on the calling thread.
void nph_realtime_enter(void);
void nph_realtime_exit(void);

// True when the calling thread is inside a scope.
bool nph_realtime_active(void);

// Reports a violation if the calling thread is inside a scope: the
// checkers call this. Counts it, then calls the hook, or fails an
// assertion when there is none. The _at form takes the location as two
// values, for checks in inline header code that C++ also compiles (a
// compound literal is not C++).
void nph_realtime_check(NphRealtimeViolation kind, const char *what, NphCallsite site);
void nph_realtime_check_at(NphRealtimeViolation kind, const char *what, const char *file, u32 line);

// Installs `hook` (NULL restores the assertion); returns the previous one
// through the out-parameters (either may be NULL).
void nph_realtime_set_hook(NphRealtimeHook hook, void *user, NphRealtimeHook *out_previous,
                           void **out_previous_user);

// Violations reported since the process started, of every kind.
u64 nph_realtime_violations(void);

#define NPH_REALTIME_CHECK(kind, what, site) nph_realtime_check((kind), (what), (site))
#define NPH_REALTIME_CHECK_HERE(kind, what)                                                        \
    nph_realtime_check_at((kind), (what), __FILE__, (u32)__LINE__)

#else

NPH_INLINE void nph_realtime_enter(void)
{
}
NPH_INLINE void nph_realtime_exit(void)
{
}
NPH_INLINE bool nph_realtime_active(void)
{
    return false;
}
NPH_INLINE void nph_realtime_set_hook(NphRealtimeHook hook, void *user,
                                      NphRealtimeHook *out_previous, void **out_previous_user)
{
    NPH_UNUSED(hook);
    NPH_UNUSED(user);
    if (out_previous != NULL) {
        *out_previous = NULL;
    }
    if (out_previous_user != NULL) {
        *out_previous_user = NULL;
    }
}
NPH_INLINE u64 nph_realtime_violations(void)
{
    return 0U;
}

#define NPH_REALTIME_CHECK(kind, what, site) ((void)0)
#define NPH_REALTIME_CHECK_HERE(kind, what)  ((void)0)

#endif

#ifdef __cplusplus
}
#endif

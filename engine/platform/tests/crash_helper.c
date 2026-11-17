// SPDX-License-Identifier: Apache-2.0
// Fails on purpose, with the crash handler installed, for run_crash.cmake:
//
//     crash_helper <mode> <dir>
//
// It installs the handler into <dir>, logs a line into <dir>/helper.log (the
// handler delivers it after the report), and fails as <mode> says. Where the
// handler does not install (the sanitizers' builds) it prints "skipped: ..."
// and exits 0. Its own failures exit 90 (usage), 91 (no handler), 92 (no
// thread, fiber, or log), and 93 (the mode did not end the process): codes
// that no fault ends with.
#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/crash.h>
#include <nepheline/platform/fiber.h>
#include <nepheline/platform/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if NPH_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if NPH_COMPILER_MSVC
#include <intrin.h>
#endif
// vcruntime's: calls the pure-call handler, then abort(). No C header declares it.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming): its own name
int __cdecl _purecall(void);
#endif

NPH_LOG_CATEGORY_DEFINE(LOG_HELPER, "crash_helper", NPH_LOG_LEVEL_TRACE);

// Volatile, so that no compiler proves the faults below away.
static volatile int *volatile g_bad; // NULL
static volatile int g_zero;
static volatile u64 g_never = ~0ULL;

NPH_NO_INLINE static void fault_access(void)
{
    g_bad[4] = 1; // a write at 0x10
}

NPH_NO_INLINE static int fault_divide(void)
{
    return 1 / g_zero;
}

NPH_NO_INLINE static void fault_illegal(void)
{
#if NPH_COMPILER_MSVC
    __ud2();
#else
    __builtin_trap();
#endif
}

// Recurses until the stack ends: each frame is used after the call, so it is
// not a tail call, and `g_never` keeps compilers from calling it infinite.
NPH_NO_INLINE static u64 recurse(const volatile u8 *prev, u64 depth)
{
    volatile u8 frame[4096];
    frame[0] = (u8)depth;
    frame[1] = 1U;
    frame[sizeof frame - 1U] = (u8)(prev != NULL ? prev[0] : 0U);
    if (depth == g_never) {
        return 0U;
    }
    return recurse(frame, depth + 1U) + frame[1];
}

// Threads and fibers ------------------------------------------------------

static void worker_fault(void *user)
{
    NPH_UNUSED(user);
    fault_access();
}

typedef struct Pair {
    NphAtomicU32 ready;
} Pair;

// Waits for both faulting threads, then faults: two at once.
static void pair_fault(void *user)
{
    Pair *pair = (Pair *)user;
    (void)nph_atomic_fetch_add_u32(&pair->ready, 1U, NPH_MEMORY_ORDER_ACQ_REL);
    while (nph_atomic_load_u32(&pair->ready, NPH_MEMORY_ORDER_ACQUIRE) < 2U) {
    }
    fault_access();
}

static void fiber_fault(void *user)
{
    NPH_UNUSED(user);
    fault_access();
}

static void fiber_overflow(void *user)
{
    NPH_UNUSED(user);
    (void)recurse(NULL, 0U);
}

static void start_thread(NphThread *t, NphThreadFn fn, void *user, const char *name)
{
    NphThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.fn = fn;
    desc.user = user;
    desc.name = name;
    if (nph_thread_create(t, &desc) != NPH_RESULT_OK) {
        fprintf(stderr, "crash_helper: no thread\n");
        exit(92);
    }
}

static void run_on_fiber(NphFiberFn fn)
{
    static NphFiber self;
    static NphFiber fiber;
    nph_fiber_init_thread(&self);
    NphFiberDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.fn = fn;
    desc.name = "crash-fiber";
    if (nph_fiber_create(&fiber, &desc) != NPH_RESULT_OK) {
        fprintf(stderr, "crash_helper: no fiber\n");
        exit(92);
    }
    nph_fiber_switch(&self, &fiber);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: crash_helper <mode> <dir>\n");
        return 90;
    }
    const char *mode = argv[1];

    NphCrashDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.dir = argv[2];
    desc.app_name = "crash_helper";
    const NphResult r = nph_crash_install(&desc);
    if (r == NPH_RESULT_UNSUPPORTED) {
        printf("skipped: the sanitizers own the signals in this build\n");
        return 0;
    }
    if (r != NPH_RESULT_OK) {
        fprintf(stderr, "crash_helper: install: %s\n", nph_result_string(r));
        return 91;
    }

    // A record still in its ring when the process fails: the handler
    // delivers it to the file after the report.
    NphLogConfig log;
    memset(&log, 0, sizeof log);
    log.alloc = nph_allocator_system();
    NphLogFileSink sink;
    char log_path[1200];
    snprintf(log_path, sizeof log_path, "%s/helper.log", argv[2]);
    if (nph_log_init(&log) != NPH_RESULT_OK ||
        nph_log_file_sink_open(&sink, log_path) != NPH_RESULT_OK ||
        nph_log_add_sink(nph_log_file_sink, &sink, NPH_LOG_LEVEL_TRACE) != NPH_RESULT_OK ||
        nph_log_add_sink(nph_crash_log_sink, NULL, NPH_LOG_LEVEL_TRACE) != NPH_RESULT_OK) {
        fprintf(stderr, "crash_helper: no log\n");
        return 92;
    }
    NPH_LOG_INFO(&LOG_HELPER, "the last line before the %s", mode);

    nph_thread_set_current_name("crash-main");
    nph_crash_set_note(0U, "crash helper");
    nph_crash_set_note(5U, mode);

    NphThread t1;
    NphThread t2;
    if (strcmp(mode, "access") == 0) {
        fault_access();
    } else if (strcmp(mode, "uninstalled") == 0) {
        nph_crash_uninstall();
#if NPH_PLATFORM_WINDOWS
        // Uninstalled, nothing keeps Windows from asking about the fault in a
        // dialog, which would hold the test until its timeout.
        (void)SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif
        fault_access();
    } else if (strcmp(mode, "abort") == 0) {
        abort();
    } else if (strcmp(mode, "assert") == 0) {
        // Called directly so that it fails in every build, NPH_ASSERT or not.
        nph_assert_fail(__FILE__, "main", (u32)__LINE__, "1 == 2", "on purpose: %d", 42);
    } else if (strcmp(mode, "divide") == 0) {
        printf("%d\n", fault_divide());
    } else if (strcmp(mode, "illegal") == 0) {
        fault_illegal();
    } else if (strcmp(mode, "overflow") == 0) {
        printf("%llu\n", (unsigned long long)recurse(NULL, 0U));
    } else if (strcmp(mode, "thread") == 0) {
        start_thread(&t1, worker_fault, NULL, "crash-worker");
        nph_thread_join(&t1);
    } else if (strcmp(mode, "twice") == 0) {
        static Pair pair;
        start_thread(&t1, pair_fault, &pair, "crash-pair-1");
        start_thread(&t2, pair_fault, &pair, "crash-pair-2");
        nph_thread_join(&t1);
        nph_thread_join(&t2);
    } else if (strcmp(mode, "fiber") == 0) {
        run_on_fiber(fiber_fault);
    } else if (strcmp(mode, "fiber_overflow") == 0) {
        run_on_fiber(fiber_overflow);
#if NPH_PLATFORM_WINDOWS
    } else if (strcmp(mode, "invalid_parameter") == 0) {
        _invalid_parameter_noinfo();
    } else if (strcmp(mode, "purecall") == 0) {
        (void)_purecall();
#endif
    } else {
        fprintf(stderr, "crash_helper: unknown mode %s\n", mode);
        return 90;
    }
    fprintf(stderr, "crash_helper: %s did not end the process\n", mode);
    return 93;
}

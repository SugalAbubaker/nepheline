// SPDX-License-Identifier: Apache-2.0
#pragma once

// Assertions (CLAUDE.md §2 "Fail loudly in development, gracefully in
// shipping").
//
//   NPH_ASSERT(cond)              checked when NPH_ASSERTS_ENABLED; otherwise
//                                 the condition is not evaluated at all
//   NPH_ASSERT_MSG(cond, fmt...)  same, with a printf-style message
//   NPH_VERIFY(cond)              the condition is always evaluated (it may
//                                 have side effects); checked only when asserts
//                                 are enabled
//   NPH_VERIFY_MSG(cond, fmt...)
//   NPH_UNREACHABLE()             assert failure when enabled, optimiser hint
//                                 otherwise
//
// A failed assertion calls the installed handler with the location, the
// condition text, and the formatted message. The default handler prints to
// stderr and asks for a debugger break, then aborts. Tests install their own
// handler to observe expected failures (it may longjmp out).

#include <nepheline/core/base.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAssertInfo {
    const char *file;
    const char *function;
    u32 line;
    const char *condition; // source text of the failed condition
    const char *message;   // formatted message, or "" when none was given
} NphAssertInfo;

typedef enum NphAssertAction {
    NPH_ASSERT_ACTION_ABORT,    // call abort() (the default handler's choice)
    NPH_ASSERT_ACTION_BREAK,    // trap into the debugger, then continue
    NPH_ASSERT_ACTION_CONTINUE, // return to the failing code (tests only)
} NphAssertAction;

// Called for every failed assertion. May longjmp. Must be reentrant with
// respect to other threads: assertions can fire concurrently.
typedef NphAssertAction (*NphAssertHandler)(const NphAssertInfo *info, void *user);

// Installs `handler` (NULL restores the default) and returns the previous
// handler through the out-parameters (either may be NULL).
// Thread-safety: NPH_MAIN_THREAD_ONLY; install before spawning threads.
void nph_assert_set_handler(NphAssertHandler handler, void *user, NphAssertHandler *out_previous,
                            void **out_previous_user);

// Reports a failed assertion: formats the message, invokes the handler, and
// performs its chosen action. Returns only for NPH_ASSERT_ACTION_CONTINUE
// (and after a BREAK). Called by the macros below; not called directly.
// Thread-safety: NPH_THREADSAFE.
NPH_PRINTF_LIKE(5, 6)
void nph_assert_fail(const char *file, const char *function, u32 line, const char *condition,
                     const char *fmt, ...);

#if NPH_ASSERTS_ENABLED

// MSVC warns (C4127) when an assertion's condition is a constant expression
// such as `sizeof(T) == 8`; that is a legitimate assertion, so the warning is
// silenced inside the macro only.
#if NPH_COMPILER_MSVC
#define NPH_ASSERT_WARNING_PUSH_() __pragma(warning(push)) __pragma(warning(disable : 4127))
#define NPH_ASSERT_WARNING_POP_()  __pragma(warning(pop))
#else
#define NPH_ASSERT_WARNING_PUSH_()
#define NPH_ASSERT_WARNING_POP_()
#endif

#define NPH_ASSERT(cond)                                                                           \
    do {                                                                                           \
        NPH_ASSERT_WARNING_PUSH_()                                                                 \
        if (NPH_UNLIKELY(!(cond))) {                                                               \
            nph_assert_fail(__FILE__, __func__, (u32)__LINE__, #cond, "");                         \
        }                                                                                          \
        NPH_ASSERT_WARNING_POP_()                                                                  \
    } while (0)

#define NPH_ASSERT_MSG(cond, ...)                                                                  \
    do {                                                                                           \
        NPH_ASSERT_WARNING_PUSH_()                                                                 \
        if (NPH_UNLIKELY(!(cond))) {                                                               \
            nph_assert_fail(__FILE__, __func__, (u32)__LINE__, #cond, __VA_ARGS__);                \
        }                                                                                          \
        NPH_ASSERT_WARNING_POP_()                                                                  \
    } while (0)

#define NPH_VERIFY(cond)          NPH_ASSERT(cond)
#define NPH_VERIFY_MSG(cond, ...) NPH_ASSERT_MSG(cond, __VA_ARGS__)

#define NPH_UNREACHABLE()                                                                          \
    nph_assert_fail(__FILE__, __func__, (u32)__LINE__, "unreachable",                              \
                    "control reached code marked NPH_UNREACHABLE")

#else

#define NPH_ASSERT(cond)          ((void)0)
#define NPH_ASSERT_MSG(cond, ...) ((void)0)
#define NPH_VERIFY(cond)          ((void)(cond))
#define NPH_VERIFY_MSG(cond, ...) ((void)(cond))
#define NPH_UNREACHABLE()         NPH_UNREACHABLE_HINT()

#endif

#ifdef __cplusplus
}
#endif

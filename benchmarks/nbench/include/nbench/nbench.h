// SPDX-License-Identifier: Apache-2.0
#pragma once

// nbench: the Nepheline micro-benchmark harness (docs/benchmarks.md).
//
//     NBENCH(alloc_free_64B)
//     {
//         while (nbench_keep_running(state)) {
//             void *p = malloc(64);
//             nbench_do_not_optimize(p);
//             free(p);
//         }
//     }
//
// Each benchmark is run for several repeats; within a repeat the iteration
// count is scaled until the loop runs for at least --min-time seconds. The
// reported figure is nanoseconds per operation, where an operation is one
// iteration divided by nbench_set_items_per_iteration (default 1). The
// median over repeats is the headline number; the minimum is reported too.
//
// Results go to the console and, with --json=FILE, to a JSON document that
// CI archives and compares over time (CLAUDE.md §9 "Performance regression").
//
// Like ntest, nbench depends only on the C library and the OS.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NbenchState NbenchState;
typedef void (*NbenchFn)(NbenchState *state);

typedef struct NbenchCase {
    const char *name;
    const char *file;
    int line;
    NbenchFn fn;
    struct NbenchCase *next;
} NbenchCase;

void nbench_register(NbenchCase *bc);

#if defined(_MSC_VER) && !defined(__clang__)
#pragma section(".CRT$XCU", read)
#define NBENCH_INITIALIZER_(fn)                                                                    \
    static void fn(void);                                                                          \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr_)(void) = fn;                                 \
    __pragma(comment(linker, "/include:" #fn "_ptr_")) static void fn(void)
#elif defined(__GNUC__) || defined(__clang__)
#define NBENCH_INITIALIZER_(fn) __attribute__((constructor)) static void fn(void)
#else
#error "nbench: unsupported compiler"
#endif

// NBENCH(name) { body using NbenchState *state }
#define NBENCH(name)                                                                               \
    static void nbench_body_##name(NbenchState *state);                                            \
    static NbenchCase nbench_case_##name = {#name, __FILE__, __LINE__, nbench_body_##name, NULL};  \
    NBENCH_INITIALIZER_(nbench_reg_##name)                                                         \
    {                                                                                              \
        nbench_register(&nbench_case_##name);                                                      \
    }                                                                                              \
    static void nbench_body_##name(NbenchState *state)

// Loop control. Starts the timer on the first call and stops it when the
// iteration budget is spent. Everything inside the loop is measured.
bool nbench_keep_running(NbenchState *state);

// Declares that one iteration performs `n` operations, so the reported
// ns/op is divided by n. Call before or during the loop; the last value wins.
void nbench_set_items_per_iteration(NbenchState *state, uint64_t n);

// Attaches a static label (e.g. the allocator name) to the result. Optional.
void nbench_set_label(NbenchState *state, const char *label);

// Pauses and resumes the timer, for setup inside the loop that must not count.
void nbench_pause_timing(NbenchState *state);
void nbench_resume_timing(NbenchState *state);

// Prevents the optimiser from discarding a value or hoisting its producer.
void nbench_do_not_optimize(const void *p);
void nbench_do_not_optimize_u64(uint64_t v);

// Clobbers memory so stores before it cannot be elided.
void nbench_clobber_memory(void);

// Monotonic time in nanoseconds (QueryPerformanceCounter / CLOCK_MONOTONIC).
uint64_t nbench_now_ns(void);

// Parses the command line (see --help), runs the selected benchmarks, prints
// a table, optionally writes JSON and checks budgets. Exit codes: 0 ok,
// 2 usage error or unwritable report, 3 no benchmark matched, 4 a budget
// was exceeded.
int nbench_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

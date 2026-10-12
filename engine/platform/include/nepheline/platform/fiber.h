// SPDX-License-Identifier: Apache-2.0
#pragma once

// Fibers: user-space execution contexts with their own stacks, switched
// explicitly by the code that owns them (CLAUDE.md §8.3 "Hand-written
// assembly context switch for the Windows x64 and System V x64 ABIs, with
// guard-paged stacks. Win32 Fibers as a fallback").
//
// The job system (Phase 4; Gyrling, "Parallelizing the Naughty Dog Engine
// Using Fibers", GDC 2015) runs every job on a fiber so that a job waiting
// for a counter can be parked and the worker thread picks up other work
// instead of blocking. A switch saves the callee-saved registers, the
// floating-point control state, and (on Windows) the stack bounds the OS
// keeps in the thread information block, then swaps the stack pointer; it
// costs a few nanoseconds (measured in docs/platform.md), against the
// microseconds of a kernel context switch.
//
// Stacks come from vmem.h: a reservation with a no-access guard page at
// the low end, so an overflow faults immediately instead of corrupting the
// neighbouring memory. Fibers are not bound to a thread: a fiber suspended
// on one thread may be resumed on another, which is what lets the job
// system migrate work.
//
// A switch preserves the callee-saved registers and MXCSR (so the SSE
// rounding and exception masks are per fiber). The x87 control word is
// not switched (ADR-0010 §8): x64 code never uses x87, and the engine never
// changes floating-point modes anyway (ADR-0008). Never cache the address
// of a thread-local across a switch: the fiber may resume on another
// thread (ADR-0010 §4).
//
// Rules:
//   - A thread calls nph_fiber_init_thread once before any switch and
//     nph_fiber_release_thread before it exits. That object represents the
//     thread's original stack.
//   - nph_fiber_switch(from, to): `from` must be the fiber running on the
//     calling thread; it suspends there and resumes when some fiber
//     switches back to it.
//   - A fiber function never returns. When its work is done it switches to
//     another fiber (the job system's scheduler fiber). Returning is a
//     fatal error.
//   - A fiber is destroyed only while suspended, from a different fiber.
//
// Two backends, chosen at build time (cmake -DNPH_FIBERS_WIN32=ON selects
// the Win32 fiber API on Windows; the assembly switch is the default on
// both platforms): same API, same semantics. The assembly backend is
// annotated for AddressSanitizer and ThreadSanitizer so Debug and TSan
// builds understand the stack switches.
//
// Thread-safety: a fiber is used by one thread at a time; switching the
// same fiber from two threads is a caller bug. nph_fiber_current is
// per-thread.

#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NphFiberFn)(void *user);

#define NPH_FIBER_DEFAULT_STACK_SIZE NPH_KIB(64)
#define NPH_FIBER_MIN_STACK_SIZE     NPH_KIB(16)

typedef struct NphFiberDesc {
    NphFiberFn fn;    // required; never returns
    void *user;       // passed to fn
    u64 stack_size;   // usable bytes, rounded up to pages (0: the default)
    const char *name; // optional; static string for diagnostics
} NphFiberDesc;

typedef struct NphFiber {
    void *context;     // saved stack pointer (assembly) or the Win32 fiber handle
    u8 *stack_base;    // reservation start (the guard page), NULL for a thread fiber
    u8 *stack_lo;      // lowest usable stack address
    u64 stack_size;    // usable bytes
    u64 reserved_size; // reservation including the guard page
    NphFiberFn fn;
    void *user;
    const char *name;
    void *sanitizer_fiber; // TSan fiber handle
    void *sanitizer_stack; // ASan fake-stack save slot while suspended
    const void
        *asan_bottom; // ASan: this fiber's stack bottom (thread fibers learn it on first switch)
    u64 asan_size;
    b32 is_thread;
} NphFiber;

// Creates a fiber with its own guard-paged stack. It starts running
// desc->fn(desc->user) the first time something switches to it.
// NPH_RESULT_INVALID_ARGUMENT for a NULL fn, NPH_RESULT_OUT_OF_MEMORY when
// the stack cannot be reserved.
NPH_MUST_USE NphResult nph_fiber_create(NphFiber *fiber, const NphFiberDesc *desc);

// Releases the stack. The fiber must not be running and must not be the
// calling thread's current fiber. Safe on a zero-filled object.
void nph_fiber_destroy(NphFiber *fiber);

// Makes `fiber` represent the calling thread's own context. Must precede
// every other fiber call on the thread; nph_fiber_current() returns it
// afterwards. Never fails.
void nph_fiber_init_thread(NphFiber *fiber);

// Undoes nph_fiber_init_thread. The thread must be running on `fiber`.
void nph_fiber_release_thread(NphFiber *fiber);

// Suspends `from` (the fiber running on this thread) and resumes `to`.
// Returns when another fiber switches back to `from`, possibly on a
// different thread. Never inlined, even under LTO: its thread-local
// accesses must be computed on the thread that makes the call.
void nph_fiber_switch(NphFiber *from, NphFiber *to);

// The fiber running on the calling thread; NULL before nph_fiber_init_thread.
NphFiber *nph_fiber_current(void);

// "asm" or "win32".
const char *nph_fiber_backend_name(void);

#ifdef __cplusplus
}
#endif

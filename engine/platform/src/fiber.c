// SPDX-License-Identifier: Apache-2.0
// Fibers over the assembly context switch (fiber_win64.asm /
// fiber_sysv_x64.S); docs/adr/0010-fibers.md.
//
// Thread-local hazard: the compiler computes the address of a thread-local
// once per function and may keep it across calls. Code that runs after
// nph_fiber_switch_asm returns may be on a different thread, so every
// thread-local access after a switch happens in a separate non-inlined
// function (finish_switch, nph_fiber_current), never in the same function
// as the switch.
#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/fiber.h>
#include <nepheline/platform/vmem.h>

#include "platform_internal.h"

#include <stdlib.h>
#include <string.h>

#if NPH_ASAN_ENABLED
#include <sanitizer/common_interface_defs.h>
#endif
#if NPH_TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
#endif

#if NPH_COMPILER_MSVC
#include <intrin.h>
#else
#include <xmmintrin.h>
#endif

// The assembly entry points.
void nph_fiber_switch_asm(void **from_sp, void *to_sp);
void nph_fiber_trampoline_asm(void);
// Called by the trampoline on a fresh fiber's stack.
void nph_fiber_entry(NphFiber *fiber);

static NPH_THREAD_LOCAL NphFiber *t_current;
static NPH_THREAD_LOCAL NphFiber *t_previous; // what the running fiber was switched from

// Completes a switch on the destination side. Non-inlined so its
// thread-local accesses are computed on the thread that resumed us.
static NPH_NO_INLINE void finish_switch(void)
{
#if NPH_ASAN_ENABLED
    NphFiber *self = t_current;
    NphFiber *prev = t_previous;
    const void *old_bottom = NULL;
    size_t old_size = 0U;
    __sanitizer_finish_switch_fiber(self->sanitizer_stack, &old_bottom, &old_size);
    // The first switch away from a thread's own stack reveals its bounds,
    // which later switches back to it need.
    if (prev != NULL && prev->asan_bottom == NULL) {
        prev->asan_bottom = old_bottom;
        prev->asan_size = old_size;
    }
#endif
}

// Lays out the frame nph_fiber_switch_asm restores on the first switch
// into a fresh fiber (see the frame comment in the assembly). The stack
// grows down from `top`; the frame sits just below it with the zero word
// that terminates stack walks above the trampoline's return slot.
static void *prepare_initial_frame(NphFiber *f)
{
    u8 *top = f->stack_lo + f->stack_size;
#if NPH_PLATFORM_WINDOWS
    const u64 frame = 280U; // 200 fp/TIB + 8 regs + return + zero word
#else
    const u64 frame = 72U; // 8 fp + 6 regs + return + zero word
#endif
    u8 *base = top - frame;
    base -= (uptr)base & 15U; // 16-byte aligned, in pointer arithmetic
    memset(base, 0, (size_t)(top - base));
    // MXCSR as the creating thread has it (round to nearest, all exceptions
    // masked, by ADR-0008); the x87 control word is not part of a fiber's
    // state (ADR-0010 §8).
    const u32 mxcsr = (u32)_mm_getcsr();
#if NPH_PLATFORM_WINDOWS
    memcpy(base + 160U, &mxcsr, sizeof mxcsr);
    // Addresses are stored as integers: the frame is raw memory, not a
    // struct, and pointer values are never read back through it.
    const uptr stack_base_tib = (uptr)top;
    const uptr stack_limit_tib = (uptr)f->stack_lo;
    const uptr dealloc_tib = (uptr)f->stack_base;
    memcpy(base + 168U, &stack_base_tib, sizeof stack_base_tib);
    memcpy(base + 176U, &stack_limit_tib, sizeof stack_limit_tib);
    memcpy(base + 184U, &dealloc_tib, sizeof dealloc_tib);
    const uptr r12 = (uptr)f;
    memcpy(base + 224U, &r12, sizeof r12);
    const uptr entry = (uptr)nph_fiber_trampoline_asm;
    memcpy(base + 264U, &entry, sizeof entry);
#else
    memcpy(base + 0U, &mxcsr, sizeof mxcsr);
    const uptr r12 = (uptr)f;
    memcpy(base + 32U, &r12, sizeof r12);
    const uptr entry = (uptr)nph_fiber_trampoline_asm;
    memcpy(base + 56U, &entry, sizeof entry);
#endif
    return base;
}

NphResult nph_fiber_create(NphFiber *fiber, const NphFiberDesc *desc)
{
    NPH_ASSERT(fiber != NULL && desc != NULL);
    memset(fiber, 0, sizeof *fiber);
    if (desc->fn == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 page = nph_vmem_page_size();
    u64 size = desc->stack_size != 0U ? desc->stack_size : NPH_FIBER_DEFAULT_STACK_SIZE;
    if (size < NPH_FIBER_MIN_STACK_SIZE) {
        size = NPH_FIBER_MIN_STACK_SIZE;
    }
    size = nph_align_up_u64(size, page);
    const u64 reserved = size + page; // one guard page below the stack
    void *base = NULL;
    NPH_TRY(nph_vmem_reserve(reserved, &base));
    const NphResult r = nph_vmem_commit((u8 *)base + page, size);
    if (r != NPH_RESULT_OK) {
        (void)nph_vmem_release(base, reserved);
        return r;
    }
    // A fresh reservation may reuse addresses whose shadow something else
    // left poisoned; the stack must start clean.
    NPH_ASAN_UNPOISON(base, reserved);
    fiber->stack_base = (u8 *)base;
    fiber->stack_lo = (u8 *)base + page;
    fiber->stack_size = size;
    fiber->reserved_size = reserved;
    fiber->fn = desc->fn;
    fiber->user = desc->user;
    fiber->name = desc->name != NULL ? desc->name : "fiber";
    fiber->asan_bottom = fiber->stack_lo;
    fiber->asan_size = size;
    fiber->context = prepare_initial_frame(fiber);
#if NPH_TSAN_ENABLED
    fiber->sanitizer_fiber = __tsan_create_fiber(0U);
    __tsan_set_fiber_name(fiber->sanitizer_fiber, fiber->name);
#endif
    return NPH_RESULT_OK;
}

void nph_fiber_destroy(NphFiber *fiber)
{
    NPH_ASSERT(fiber != NULL);
    if (fiber->stack_base == NULL) {
        return; // zero-filled, already destroyed, or a thread fiber
    }
    NPH_ASSERT_MSG(nph_fiber_current() != fiber, "destroying the running fiber");
#if NPH_TSAN_ENABLED
    __tsan_destroy_fiber(fiber->sanitizer_fiber);
#endif
    (void)nph_vmem_release(fiber->stack_base, fiber->reserved_size);
    memset(fiber, 0, sizeof *fiber);
}

void nph_fiber_init_thread(NphFiber *fiber)
{
    NPH_ASSERT(fiber != NULL);
    NPH_ASSERT_MSG(t_current == NULL, "this thread already has a fiber context");
    memset(fiber, 0, sizeof *fiber);
    fiber->is_thread = 1;
    fiber->name = "thread";
#if NPH_TSAN_ENABLED
    fiber->sanitizer_fiber = __tsan_get_current_fiber();
#endif
    t_current = fiber;
}

void nph_fiber_release_thread(NphFiber *fiber)
{
    NPH_ASSERT(fiber != NULL && fiber->is_thread);
    NPH_ASSERT_MSG(nph_fiber_current() == fiber, "the thread must be running on its own fiber");
    t_current = NULL;
    t_previous = NULL;
    memset(fiber, 0, sizeof *fiber);
}

// Never inlined: with link-time optimisation it would otherwise be
// expanded into a fiber function, whose thread-local base was computed on
// whichever thread first ran it (ADR-0010 §4).
NPH_NO_INLINE void nph_fiber_switch(NphFiber *from, NphFiber *to)
{
    NPH_ASSERT(from != NULL && to != NULL);
    NPH_ASSERT_MSG(from != to, "switching a fiber to itself");
    NPH_ASSERT_MSG(nph_fiber_current() == from, "`from` is not the running fiber");
    NPH_ASSERT_MSG(to->is_thread || to->stack_base != NULL, "`to` was not created");
#if NPH_TSAN_ENABLED
    __tsan_switch_to_fiber(to->sanitizer_fiber, 0U);
#endif
#if NPH_ASAN_ENABLED
    __sanitizer_start_switch_fiber(&from->sanitizer_stack, to->asan_bottom, (size_t)to->asan_size);
#endif
    t_previous = from;
    t_current = to;
    nph_fiber_switch_asm(&from->context, to->context);
    // Resumed on `from`, on whichever thread switched back to it.
    finish_switch();
}

NPH_NO_INLINE NphFiber *nph_fiber_current(void)
{
    return t_current;
}

void nph_fiber_entry(NphFiber *fiber)
{
    finish_switch();
    fiber->fn(fiber->user);
    // There is no frame to return to; a fiber ends by switching away.
    NPH_LOG_FATAL(&NPH_LOG_PLATFORM, "fiber '%s' returned from its function", fiber->name);
    nph_log_flush(0U);
    NPH_ASSERT_MSG(false, "fiber '%s' returned from its function", fiber->name);
    abort();
}

const char *nph_fiber_backend_name(void)
{
    return "asm";
}

// SPDX-License-Identifier: Apache-2.0
// Fibers over the Win32 fiber API (the fallback backend, built with
// -DNPH_FIBERS_WIN32=ON; docs/adr/0010-fibers.md). Same API and semantics
// as fiber.c; the OS owns the stacks and the switch.
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/fiber.h>
#include <nepheline/platform/vmem.h>

#include "platform_internal.h"

#include <stdlib.h>
#include <string.h>

static NPH_THREAD_LOCAL NphFiber *t_current;

static void CALLBACK fiber_proc(void *param)
{
    NphFiber *fiber = (NphFiber *)param;
    fiber->fn(fiber->user);
    NPH_LOG_FATAL(&NPH_LOG_PLATFORM, "fiber '%s' returned from its function", fiber->name);
    nph_log_flush(0U);
    NPH_ASSERT_MSG(false, "fiber '%s' returned from its function", fiber->name);
    abort();
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
    fiber->fn = desc->fn;
    fiber->user = desc->user;
    fiber->name = desc->name != NULL ? desc->name : "fiber";
    fiber->stack_size = size;
    fiber->reserved_size = size + page;
    // Commit the whole stack up front, as the assembly backend does, so the
    // two behave the same under memory pressure; the OS adds its guard page.
    fiber->context = CreateFiberEx((SIZE_T)size, (SIZE_T)(size + page), FIBER_FLAG_FLOAT_SWITCH,
                                   fiber_proc, fiber);
    if (fiber->context == NULL) {
        const NphResult r = nph_win32_fail("CreateFiberEx", GetLastError());
        memset(fiber, 0, sizeof *fiber);
        return r == NPH_RESULT_OK ? NPH_RESULT_OUT_OF_MEMORY : r;
    }
    return NPH_RESULT_OK;
}

void nph_fiber_destroy(NphFiber *fiber)
{
    NPH_ASSERT(fiber != NULL);
    if (fiber->context == NULL || fiber->is_thread) {
        return;
    }
    NPH_ASSERT_MSG(nph_fiber_current() != fiber, "destroying the running fiber");
    DeleteFiber(fiber->context);
    memset(fiber, 0, sizeof *fiber);
}

void nph_fiber_init_thread(NphFiber *fiber)
{
    NPH_ASSERT(fiber != NULL);
    NPH_ASSERT_MSG(t_current == NULL, "this thread already has a fiber context");
    memset(fiber, 0, sizeof *fiber);
    void *context = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (context == NULL) {
        // Already a fiber (an earlier init/release pair on this thread).
        NPH_ASSERT(GetLastError() == ERROR_ALREADY_FIBER);
        context = GetCurrentFiber();
    }
    fiber->context = context;
    fiber->is_thread = 1;
    fiber->name = "thread";
    t_current = fiber;
}

void nph_fiber_release_thread(NphFiber *fiber)
{
    NPH_ASSERT(fiber != NULL && fiber->is_thread);
    NPH_ASSERT_MSG(nph_fiber_current() == fiber, "the thread must be running on its own fiber");
    NPH_VERIFY(ConvertFiberToThread());
    t_current = NULL;
    memset(fiber, 0, sizeof *fiber);
}

// Never inlined, for the same thread-local reason as in fiber.c.
NPH_NO_INLINE void nph_fiber_switch(NphFiber *from, NphFiber *to)
{
    NPH_ASSERT(from != NULL && to != NULL);
    NPH_ASSERT_MSG(from != to, "switching a fiber to itself");
    NPH_ASSERT_MSG(nph_fiber_current() == from, "`from` is not the running fiber");
    NPH_ASSERT_MSG(to->context != NULL, "`to` was not created");
    NPH_UNUSED(from); // the OS remembers the current fiber itself
    t_current = to;
    SwitchToFiber(to->context);
    // Resumed on `from`; whoever switched back set t_current on its thread.
}

NPH_NO_INLINE NphFiber *nph_fiber_current(void)
{
    return t_current;
}

const char *nph_fiber_backend_name(void)
{
    return "win32";
}

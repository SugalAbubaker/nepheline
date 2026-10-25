// SPDX-License-Identifier: Apache-2.0
// Real-time scopes: a per-thread depth, a process-wide violation count, and
// the hook tests install (realtime.h).

#include <nepheline/core/assert.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/realtime.h>

const char *nph_realtime_violation_name(NphRealtimeViolation kind)
{
    switch (kind) {
    case NPH_REALTIME_VIOLATION_ALLOC:
        return "alloc";
    case NPH_REALTIME_VIOLATION_LOCK:
        return "lock";
    case NPH_REALTIME_VIOLATION_WAIT:
        return "wait";
    case NPH_REALTIME_VIOLATION_SLEEP:
        return "sleep";
    case NPH_REALTIME_VIOLATION_IO:
        return "io";
    case NPH_REALTIME_VIOLATION_COUNT:
        break;
    }
    return "invalid";
}

#if NPH_ASSERTS_ENABLED

static NPH_THREAD_LOCAL u32 t_realtime_depth;

// The hook and its user pointer are written only while no scope is active
// (realtime.h), so a scope's reads never race the write; they are atomic
// anyway, so a thread that checks while another installs sees one pair or
// the other rather than a torn pointer.
static NphAtomicPtr g_realtime_hook;
static NphAtomicPtr g_realtime_hook_user;
static NphAtomicU64 g_realtime_violations;

void nph_realtime_enter(void)
{
    ++t_realtime_depth;
}

void nph_realtime_exit(void)
{
    NPH_ASSERT_MSG(t_realtime_depth > 0U, "nph_realtime_exit without a matching enter");
    if (t_realtime_depth > 0U) {
        --t_realtime_depth;
    }
}

bool nph_realtime_active(void)
{
    return t_realtime_depth > 0U;
}

void nph_realtime_check(NphRealtimeViolation kind, const char *what, NphCallsite site)
{
    if (NPH_LIKELY(t_realtime_depth == 0U)) {
        return;
    }
    (void)nph_atomic_fetch_add_u64(&g_realtime_violations, 1U, NPH_MEMORY_ORDER_RELAXED);
    // The hook pointer travels as a data pointer (NphAtomicPtr); a union
    // converts it back without a cast between object and function pointers.
    union {
        void *data;
        NphRealtimeHook fn;
    } hook;
    hook.data = nph_atomic_load_ptr(&g_realtime_hook, NPH_MEMORY_ORDER_ACQUIRE);
    if (hook.fn != NULL) {
        hook.fn(kind, what, site,
                nph_atomic_load_ptr(&g_realtime_hook_user, NPH_MEMORY_ORDER_ACQUIRE));
        return;
    }
    NPH_ASSERT_MSG(false, "%s (%s) inside a real-time scope at %s:%u", what,
                   nph_realtime_violation_name(kind), site.file != NULL ? site.file : "?",
                   site.line);
}

void nph_realtime_check_at(NphRealtimeViolation kind, const char *what, const char *file, u32 line)
{
    if (NPH_LIKELY(t_realtime_depth == 0U)) {
        return;
    }
    NphCallsite site;
    site.file = file;
    site.line = line;
    nph_realtime_check(kind, what, site);
}

void nph_realtime_set_hook(NphRealtimeHook hook, void *user, NphRealtimeHook *out_previous,
                           void **out_previous_user)
{
    union {
        void *data;
        NphRealtimeHook fn;
    } previous, next;
    next.fn = hook;
    previous.data = nph_atomic_load_ptr(&g_realtime_hook, NPH_MEMORY_ORDER_ACQUIRE);
    void *previous_user = nph_atomic_load_ptr(&g_realtime_hook_user, NPH_MEMORY_ORDER_ACQUIRE);
    nph_atomic_store_ptr(&g_realtime_hook_user, user, NPH_MEMORY_ORDER_RELEASE);
    nph_atomic_store_ptr(&g_realtime_hook, next.data, NPH_MEMORY_ORDER_RELEASE);
    if (out_previous != NULL) {
        *out_previous = previous.fn;
    }
    if (out_previous_user != NULL) {
        *out_previous_user = previous_user;
    }
}

u64 nph_realtime_violations(void)
{
    return nph_atomic_load_u64(&g_realtime_violations, NPH_MEMORY_ORDER_RELAXED);
}

#endif

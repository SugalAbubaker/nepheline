// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/time.h>

#include "platform_internal.h"

// QueryPerformanceFrequency never changes while the system runs, so the
// first caller publishes it and everyone else reads it with a relaxed load;
// a racing second computation stores the identical value.
static NphAtomicU64 g_qpc_frequency = NPH_ATOMIC_U64_INIT(0U);

static u64 qpc_frequency(void)
{
    u64 f = nph_atomic_load_u64(&g_qpc_frequency, NPH_MEMORY_ORDER_RELAXED);
    if (f == 0U) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li); // cannot fail on XP and later
        f = (u64)li.QuadPart;
        nph_atomic_store_u64(&g_qpc_frequency, f, NPH_MEMORY_ORDER_RELAXED);
    }
    return f;
}

u64 nph_time_now_ns(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    const u64 ticks = (u64)li.QuadPart;
    const u64 f = qpc_frequency();
    // Split so neither product overflows: the frequency is 10 MHz on
    // current Windows (24 MHz on some older machines), far below the
    // 1.8e10 at which (ticks % f) * 1e9 would wrap.
    return (ticks / f) * NPH_TIME_NS_PER_S + ((ticks % f) * NPH_TIME_NS_PER_S) / f;
}

u64 nph_time_wall_ns(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const u64 ticks_1601 = ((u64)ft.dwHighDateTime << 32U) | ft.dwLowDateTime;
    // FILETIME is 100 ns units since 1601-01-01; 11644473600 s to 1970.
    const u64 ticks_1970 = ticks_1601 - 116444736000000000ULL;
    return ticks_1970 * 100U;
}

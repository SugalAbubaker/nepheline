// SPDX-License-Identifier: Apache-2.0
#pragma once

// Clocks (CLAUDE.md §8.3 "High-resolution timer").
//
// nph_time_now_ns is the engine clock: monotonic, never adjusted, counting
// from an arbitrary point before the process started. It is
// QueryPerformanceCounter on Windows and CLOCK_MONOTONIC on Linux, both of
// which the kernel derives from the invariant TSC on every supported CPU, so
// a reading costs ~20 ns and is consistent across cores. Resolution is
// 100 ns on Windows and 1 ns on Linux.
//
// nph_time_wall_ns is UTC nanoseconds since the Unix epoch for log
// timestamps and file times. It can jump when the clock is set; never
// measure durations with it.
//
// nph_time_cycles reads the CPU timestamp counter directly. It is the
// cheapest clock (~7 ns) and is what the profiler stamps zones with; convert
// to seconds with the frequency from cpu.h. On CPUs without an invariant TSC
// (none that meet the §3 baseline) the counter may differ between cores.
//
// Thread-safety: every function is NPH_THREADSAFE.

#include <nepheline/core/base.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_TIME_NS_PER_US 1000ULL
#define NPH_TIME_NS_PER_MS 1000000ULL
#define NPH_TIME_NS_PER_S  1000000000ULL

// Monotonic nanoseconds. Never fails.
u64 nph_time_now_ns(void);

// UTC nanoseconds since 1970-01-01T00:00:00Z. Never fails.
u64 nph_time_wall_ns(void);

// CPU timestamp counter (RDTSC).
u64 nph_time_cycles(void);

NPH_INLINE f64 nph_time_ns_to_s(u64 ns)
{
    return (f64)ns * 1e-9;
}
NPH_INLINE f64 nph_time_ns_to_ms(u64 ns)
{
    return (f64)ns * 1e-6;
}
NPH_INLINE f64 nph_time_ns_to_us(u64 ns)
{
    return (f64)ns * 1e-3;
}
NPH_INLINE u64 nph_time_ms_to_ns(u64 ms)
{
    return ms * NPH_TIME_NS_PER_MS;
}
NPH_INLINE u64 nph_time_us_to_ns(u64 us)
{
    return us * NPH_TIME_NS_PER_US;
}
// Seconds to nanoseconds; negative or NaN input gives 0.
NPH_INLINE u64 nph_time_s_to_ns(f64 s)
{
    return s > 0.0 ? (u64)(s * 1e9) : 0ULL;
}

// A started stopwatch on the monotonic clock.
typedef struct NphStopwatch {
    u64 start_ns;
} NphStopwatch;

NPH_INLINE NphStopwatch nph_stopwatch_start(void)
{
    const NphStopwatch sw = {nph_time_now_ns()};
    return sw;
}
NPH_INLINE u64 nph_stopwatch_elapsed_ns(const NphStopwatch *sw)
{
    return nph_time_now_ns() - sw->start_ns;
}
NPH_INLINE f64 nph_stopwatch_elapsed_ms(const NphStopwatch *sw)
{
    return nph_time_ns_to_ms(nph_stopwatch_elapsed_ns(sw));
}
// Restarts the stopwatch and returns the elapsed nanoseconds of the lap.
NPH_INLINE u64 nph_stopwatch_lap_ns(NphStopwatch *sw)
{
    const u64 now = nph_time_now_ns();
    const u64 elapsed = now - sw->start_ns;
    sw->start_ns = now;
    return elapsed;
}

#ifdef __cplusplus
}
#endif

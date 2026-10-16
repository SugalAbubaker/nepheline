// SPDX-License-Identifier: Apache-2.0
#pragma once

// The fixed-step clock (CLAUDE.md §2 "Deterministic simulation: a fixed
// timestep"; §8.9 "Fixed 240 Hz timestep ... configurable for other
// targets"; docs/runtime.md "The frame").
//
// Real time accumulates; each frame the simulation takes as many whole
// steps as fit (Fiedler, "Fix Your Timestep!", 2004). The steps a frame may
// take are capped, so a long stall (a debugger break, a loading hitch)
// costs at most `max_steps` steps instead of a spiral where each frame
// falls further behind; the time beyond the cap is dropped and counted.
// A time scale stretches or freezes simulated time without touching the
// step length, so a slowed simulation is still the same deterministic
// sequence of steps.
//
// The clock reads no clock itself: the caller passes the time, which is
// what makes it testable and lets a replay feed recorded times.
//
// Thread-safety: an NphFixedClock belongs to one thread.

#include <nepheline/core/base.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_FIXED_CLOCK_MAX_RATE 10000U

typedef struct NphFixedClock {
    u64 step_ns;        // the fixed step
    u32 max_steps;      // per advance; time beyond is dropped
    u32 rate_hz;        // 1e9 / step_ns, as configured
    f64 time_scale;     // 1: real time; 0: paused
    u64 accumulator_ns; // simulated time owed, below one step after an advance
    u64 last_ns;        // the time passed to the previous advance; 0 before the first
    u64 steps;          // total steps handed out
    u64 dropped_ns;     // total time dropped by the cap
    f64 frame_dt;       // the last advance's real frame time, seconds
} NphFixedClock;

// A clock of `rate_hz` steps per second (1 .. NPH_FIXED_CLOCK_MAX_RATE)
// taking at most `max_steps` (>= 1) per advance, at time scale 1.
void nph_fixed_clock_init(NphFixedClock *c, u32 rate_hz, u32 max_steps);

// Changes the rate, keeping the time owed. Same range as init.
void nph_fixed_clock_set_rate(NphFixedClock *c, u32 rate_hz);

// Sets the time scale (>= 0; 0 pauses: advances hand out no steps).
void nph_fixed_clock_set_time_scale(NphFixedClock *c, f64 scale);

// Feeds the time `now_ns` (monotonic) and returns how many fixed steps to
// run. The first call starts the clock and returns 0. A time earlier than
// the previous one counts as no time passing.
u32 nph_fixed_clock_advance(NphFixedClock *c, u64 now_ns);

// The step length in seconds: every step's dt.
NPH_INLINE f64 nph_fixed_clock_step_seconds(const NphFixedClock *c)
{
    return (f64)c->step_ns * 1e-9;
}

// How far into the next step the owed time reaches, in [0, 1): the factor
// for interpolating between the last two simulated states.
f64 nph_fixed_clock_alpha(const NphFixedClock *c);

// Discards the owed time, keeping the counters: a paused simulation that
// resumes starts on a step boundary.
void nph_fixed_clock_reset(NphFixedClock *c);

#ifdef __cplusplus
}
#endif

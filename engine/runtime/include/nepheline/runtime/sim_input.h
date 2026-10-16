// SPDX-License-Identifier: Apache-2.0
#pragma once

// Input as the simulation sees it (docs/runtime.md "Input").
//
// The window's events reach an NphInput (platform/input.h) once per frame
// on the main thread. The simulation runs in fixed steps, zero or several
// per frame, and in the pipelined frame loop on the job system while the
// main thread already handles the next frame's events. It therefore never
// reads the NphInput: each step gets an immutable NphSimInput.
//
// An accumulator gathers every frame's input until a step consumes it:
//   - held state and values (down, value) are the latest frame's;
//   - edges (pressed, released) are counted across frames, so a press in a
//     frame that runs no step is seen by the next step, once;
//   - mouse and scroll motion add up across frames in the same way.
// The first step of a frame takes the edges and the motion; later steps of
// the same frame see the held state with no edges and no motion. A press
// and release inside one frame shows as both edges with `down` false.
//
// Thread-safety: the accumulator belongs to the main thread; an
// NphSimInput is a value that any thread may read once it is published.

#include <nepheline/platform/input.h>

#ifdef __cplusplus
extern "C" {
#endif

// Actions the simulation sees (the first ones registered with the input).
#define NPH_SIM_INPUT_MAX_ACTIONS 64U

typedef struct NphSimAction {
    b32 down;     // held at the end of the latest frame
    u32 pressed;  // presses since the previous step
    u32 released; // releases since the previous step
    f32 value;    // the latest frame's value
} NphSimAction;

typedef struct NphSimInput {
    u32 action_count;
    u32 reserved;
    u64 step;     // which step this is (counts from 0 over the accumulator's life)
    f64 mouse_dx; // cursor motion since the previous step, screen units
    f64 mouse_dy;
    f64 scroll_dx; // wheel motion since the previous step
    f64 scroll_dy;
    NphSimAction actions[NPH_SIM_INPUT_MAX_ACTIONS];
} NphSimInput;

typedef struct NphSimInputAccumulator {
    NphSimInput pending; // what the next step will receive
    u64 steps;
} NphSimInputAccumulator;

void nph_sim_input_reset(NphSimInputAccumulator *acc);

// Adds one frame of input (after nph_input_end_frame).
void nph_sim_input_accumulate(NphSimInputAccumulator *acc, const NphInput *input);

// The input of the next step: copies what has accumulated into `out`,
// then clears the edges and the motion.
void nph_sim_input_take(NphSimInputAccumulator *acc, NphSimInput *out);

// Queries on a step's input. An action id past the count reads as idle.
NPH_INLINE bool nph_sim_input_down(const NphSimInput *in, NphActionId action)
{
    return action < in->action_count && in->actions[action].down != 0;
}
NPH_INLINE bool nph_sim_input_pressed(const NphSimInput *in, NphActionId action)
{
    return action < in->action_count && in->actions[action].pressed != 0U;
}
NPH_INLINE bool nph_sim_input_released(const NphSimInput *in, NphActionId action)
{
    return action < in->action_count && in->actions[action].released != 0U;
}
NPH_INLINE f32 nph_sim_input_value(const NphSimInput *in, NphActionId action)
{
    return action < in->action_count ? in->actions[action].value : 0.0F;
}

#ifdef __cplusplus
}
#endif

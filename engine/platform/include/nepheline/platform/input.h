// SPDX-License-Identifier: Apache-2.0
#pragma once

// The action layer (CLAUDE.md §8.3 "Input is mapped to an action layer
// (keyboard, mouse, gamepad)"; docs/adr/0012-window-and-input.md).
//
// An NphInput consumes the raw events of a window (window.h) and gamepad
// snapshots, keeps per-frame state for keys, mouse buttons, cursor motion,
// scroll, and typed text, and maps all of that onto named *actions* that
// gameplay and the editor query: "jump" is down, "fire" was pressed this
// frame, "move_x" is 0.7. Bindings are data: a key with modifiers, a mouse
// button, a pair of keys as an axis, a mouse axis, a gamepad button, or a
// gamepad axis with a dead zone; an action may have any number of them
// and the inputs combine (digital bindings contribute ±1, analog ones
// their value; the sum is clamped to [-1, 1] except for mouse deltas,
// which pass through scaled).
//
// Per frame: nph_input_begin_frame, then every event through
// nph_input_process_event (and the gamepads through
// nph_input_poll_gamepads), then nph_input_end_frame, then queries. The
// layer has no dependency on the window system except
// nph_input_poll_gamepads, so it is tested with synthetic events.
//
// Thread-safety: one NphInput belongs to one thread (the main thread).

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/platform/window.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphInput NphInput;

typedef u32 NphActionId;
#define NPH_ACTION_INVALID NPH_U32_MAX

// A binding that ignores modifier state.
#define NPH_MOD_ANY NPH_U32_MAX
// A gamepad binding that accepts input from every connected gamepad.
#define NPH_GAMEPAD_ANY NPH_U32_MAX

#define NPH_INPUT_TEXT_MAX 32U // code points of typed text kept per frame

typedef enum NphMouseAxis {
    NPH_MOUSE_AXIS_X = 0, // cursor delta, screen units per frame
    NPH_MOUSE_AXIS_Y,
    NPH_MOUSE_AXIS_SCROLL_X, // wheel / trackpad offset per frame
    NPH_MOUSE_AXIS_SCROLL_Y,
    NPH_MOUSE_AXIS_COUNT
} NphMouseAxis;

typedef struct NphInputDesc {
    u32 max_actions;  // 0: 64
    u32 max_bindings; // 0: 256
} NphInputDesc;

NPH_MUST_USE NphResult nph_input_create(NphAllocator *alloc, const NphInputDesc *desc,
                                        NphInput **out);
void nph_input_destroy(NphInput *input);

// ---------------------------------------------------------------------------
// Frame cycle
// ---------------------------------------------------------------------------

// Clears the per-frame state: pressed/released edges, mouse deltas, scroll,
// typed text. Held keys and buttons stay held.
void nph_input_begin_frame(NphInput *input);

// Applies one window event. Events the layer does not use are ignored.
void nph_input_process_event(NphInput *input, const NphWindowEvent *event);

// Sets gamepad `index`'s snapshot for this frame (NULL: disconnected).
void nph_input_set_gamepad(NphInput *input, u32 index, const NphGamepadState *state);

// Samples every gamepad from the window system (all disconnected when it
// is not initialised).
void nph_input_poll_gamepads(NphInput *input);

// Evaluates the actions from the raw state gathered since begin_frame.
void nph_input_end_frame(NphInput *input);

// ---------------------------------------------------------------------------
// Raw state (valid after end_frame; keys and buttons also between events)
// ---------------------------------------------------------------------------

bool nph_input_key_down(const NphInput *input, NphKey key);
bool nph_input_key_pressed(const NphInput *input, NphKey key);  // went down this frame
bool nph_input_key_released(const NphInput *input, NphKey key); // went up this frame
bool nph_input_mouse_down(const NphInput *input, NphMouseButton button);
bool nph_input_mouse_pressed(const NphInput *input, NphMouseButton button);
bool nph_input_mouse_released(const NphInput *input, NphMouseButton button);

// Current modifier bits (NphMod), from the latest key or button event.
u32 nph_input_mods(const NphInput *input);

void nph_input_mouse_position(const NphInput *input, f64 *out_x, f64 *out_y);
void nph_input_mouse_delta(const NphInput *input, f64 *out_dx, f64 *out_dy);
void nph_input_scroll(const NphInput *input, f64 *out_dx, f64 *out_dy);

// Code points typed this frame, in order; returns the count (at most
// NPH_INPUT_TEXT_MAX; later ones are dropped).
u32 nph_input_text(const NphInput *input, u32 *out_codepoints, u32 max);

// The frame's snapshot of gamepad `index`, or NULL when disconnected.
const NphGamepadState *nph_input_gamepad(const NphInput *input, u32 index);

// ---------------------------------------------------------------------------
// Actions and bindings
// ---------------------------------------------------------------------------

// Registers a named action. NPH_RESULT_ALREADY_EXISTS for a duplicate
// name, NPH_RESULT_FULL at max_actions. Names are copied (31 bytes max).
NPH_MUST_USE NphResult nph_input_action_register(NphInput *input, const char *name,
                                                 NphActionId *out);
NphResult nph_input_action_find(const NphInput *input, const char *name, NphActionId *out);
const char *nph_input_action_name(const NphInput *input, NphActionId action);
u32 nph_input_action_count(const NphInput *input);

// Every bind returns NPH_RESULT_FULL at max_bindings and
// NPH_RESULT_INVALID_ARGUMENT for a bad action or input id.

// Down while `key` is held with exactly `mods` (of shift, control, alt,
// super; the lock bits are ignored) or any modifiers for NPH_MOD_ANY.
NphResult nph_input_bind_key(NphInput *input, NphActionId action, NphKey key, u32 mods);
NphResult nph_input_bind_mouse_button(NphInput *input, NphActionId action, NphMouseButton button,
                                      u32 mods);

// Value -1 while `negative` is held, +1 while `positive` is held, 0 for
// both or neither; counts as down while either is held.
NphResult nph_input_bind_key_axis(NphInput *input, NphActionId action, NphKey negative,
                                  NphKey positive);

// Value = the frame's mouse delta on `axis` times `scale` (not clamped).
NphResult nph_input_bind_mouse_axis(NphInput *input, NphActionId action, NphMouseAxis axis,
                                    f32 scale);

// Down while the button is held on gamepad `gamepad` (or any, with
// NPH_GAMEPAD_ANY).
NphResult nph_input_bind_gamepad_button(NphInput *input, NphActionId action, u32 gamepad,
                                        NphGamepadButton button);

// Value = axis * scale once |axis| exceeds `deadzone` (rescaled so the
// value starts at 0 at the dead-zone edge); down while that holds.
NphResult nph_input_bind_gamepad_axis(NphInput *input, NphActionId action, u32 gamepad,
                                      NphGamepadAxis axis, f32 scale, f32 deadzone);

// Removes every binding of the action.
void nph_input_unbind_all(NphInput *input, NphActionId action);

// Queries (valid after end_frame). An invalid id reads as inactive.
bool nph_input_action_down(const NphInput *input, NphActionId action);
bool nph_input_action_pressed(const NphInput *input, NphActionId action);
bool nph_input_action_released(const NphInput *input, NphActionId action);
f32 nph_input_action_value(const NphInput *input, NphActionId action);

#ifdef __cplusplus
}
#endif

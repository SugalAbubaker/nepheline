// SPDX-License-Identifier: Apache-2.0
// The action layer, driven by synthetic events (no display needed).
#include <nepheline/platform/input.h>

#include <ntest/ntest.h>

#include <string.h>

typedef struct InputFixture {
    NphInput *input;
} InputFixture;

NTEST_SETUP(InputFixture)
{
    NTEST_ASSERT_EQ_INT(nph_input_create(nph_allocator_system(), NULL, &f->input), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(f->input);
}

NTEST_TEARDOWN(InputFixture)
{
    nph_input_destroy(f->input);
    nph_input_destroy(NULL); // harmless
}

static NphWindowEvent key_event(NphKey key, NphKeyAction action, u32 mods)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_KEY;
    e.u.key.key = (u32)key;
    e.u.key.action = (u32)action;
    e.u.key.mods = mods;
    return e;
}

static NphWindowEvent mouse_button_event(NphMouseButton button, NphKeyAction action, u32 mods)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_MOUSE_BUTTON;
    e.u.mouse_button.button = (u32)button;
    e.u.mouse_button.action = (u32)action;
    e.u.mouse_button.mods = mods;
    return e;
}

static NphWindowEvent mouse_move_event(f64 x, f64 y)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_MOUSE_MOVE;
    e.u.mouse_move.x = x;
    e.u.mouse_move.y = y;
    return e;
}

static NphWindowEvent scroll_event(f64 dx, f64 dy)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_MOUSE_SCROLL;
    e.u.scroll.dx = dx;
    e.u.scroll.dy = dy;
    return e;
}

static NphWindowEvent char_event(u32 codepoint)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_CHAR;
    e.u.ch.codepoint = codepoint;
    return e;
}

static NphWindowEvent focus_event(bool focused)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_FOCUS;
    e.u.focus.focused = focused;
    return e;
}

static void frame(NphInput *input, const NphWindowEvent *events, u32 count)
{
    nph_input_begin_frame(input);
    for (u32 i = 0; i < count; ++i) {
        nph_input_process_event(input, &events[i]);
    }
    nph_input_end_frame(input);
}

NTEST_F(InputFixture, key_edges_across_frames)
{
    NphWindowEvent press = key_event(NPH_KEY_W, NPH_KEY_ACTION_PRESS, 0U);
    frame(f->input, &press, 1U);
    NTEST_ASSERT_TRUE(nph_input_key_down(f->input, NPH_KEY_W));
    NTEST_ASSERT_TRUE(nph_input_key_pressed(f->input, NPH_KEY_W));
    NTEST_ASSERT_FALSE(nph_input_key_released(f->input, NPH_KEY_W));
    NTEST_ASSERT_FALSE(nph_input_key_down(f->input, NPH_KEY_A));

    // Still held next frame: down but not pressed again; repeats change nothing.
    NphWindowEvent repeat = key_event(NPH_KEY_W, NPH_KEY_ACTION_REPEAT, 0U);
    frame(f->input, &repeat, 1U);
    NTEST_ASSERT_TRUE(nph_input_key_down(f->input, NPH_KEY_W));
    NTEST_ASSERT_FALSE(nph_input_key_pressed(f->input, NPH_KEY_W));

    NphWindowEvent release = key_event(NPH_KEY_W, NPH_KEY_ACTION_RELEASE, 0U);
    frame(f->input, &release, 1U);
    NTEST_ASSERT_FALSE(nph_input_key_down(f->input, NPH_KEY_W));
    NTEST_ASSERT_TRUE(nph_input_key_released(f->input, NPH_KEY_W));

    frame(f->input, NULL, 0U);
    NTEST_ASSERT_FALSE(nph_input_key_released(f->input, NPH_KEY_W));

    // Press and release within one frame: both edges visible, not down.
    NphWindowEvent tap[2] = {key_event(NPH_KEY_SPACE, NPH_KEY_ACTION_PRESS, 0U),
                             key_event(NPH_KEY_SPACE, NPH_KEY_ACTION_RELEASE, 0U)};
    frame(f->input, tap, 2U);
    NTEST_ASSERT_TRUE(nph_input_key_pressed(f->input, NPH_KEY_SPACE));
    NTEST_ASSERT_TRUE(nph_input_key_released(f->input, NPH_KEY_SPACE));
    NTEST_ASSERT_FALSE(nph_input_key_down(f->input, NPH_KEY_SPACE));

    // Out-of-range keys are ignored.
    NTEST_ASSERT_FALSE(nph_input_key_down(f->input, (NphKey)NPH_KEY_COUNT));
    NphWindowEvent bogus = key_event((NphKey)9999, NPH_KEY_ACTION_PRESS, 0U);
    frame(f->input, &bogus, 1U);
}

NTEST_F(InputFixture, mouse_buttons_motion_scroll_and_text)
{
    NphWindowEvent events[6] = {
        mouse_button_event(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS, NPH_MOD_SHIFT),
        mouse_move_event(100.0, 50.0), // first position: no delta
        mouse_move_event(110.0, 45.0), // +10, -5
        scroll_event(0.0, 2.0),
        char_event((u32)'h'),
        char_event(0x1F600U),
    };
    frame(f->input, events, 6U);
    NTEST_ASSERT_TRUE(nph_input_mouse_down(f->input, NPH_MOUSE_BUTTON_LEFT));
    NTEST_ASSERT_TRUE(nph_input_mouse_pressed(f->input, NPH_MOUSE_BUTTON_LEFT));
    NTEST_ASSERT_EQ_UINT(nph_input_mods(f->input), NPH_MOD_SHIFT);
    f64 x = 0.0;
    f64 y = 0.0;
    nph_input_mouse_position(f->input, &x, &y);
    NTEST_ASSERT_NEAR_F64(x, 110.0, 1e-12);
    NTEST_ASSERT_NEAR_F64(y, 45.0, 1e-12);
    nph_input_mouse_delta(f->input, &x, &y);
    NTEST_ASSERT_NEAR_F64(x, 10.0, 1e-12);
    NTEST_ASSERT_NEAR_F64(y, -5.0, 1e-12);
    nph_input_scroll(f->input, &x, &y);
    NTEST_ASSERT_NEAR_F64(x, 0.0, 1e-12);
    NTEST_ASSERT_NEAR_F64(y, 2.0, 1e-12);
    u32 text[4];
    NTEST_ASSERT_EQ_UINT(nph_input_text(f->input, text, 4U), 2U);
    NTEST_ASSERT_EQ_UINT(text[0], (u32)'h');
    NTEST_ASSERT_EQ_UINT(text[1], 0x1F600U);
    NTEST_ASSERT_EQ_UINT(nph_input_text(f->input, text, 1U), 1U);

    // Next frame: deltas and text reset, the button stays down, position kept.
    frame(f->input, NULL, 0U);
    nph_input_mouse_delta(f->input, &x, &y);
    NTEST_ASSERT_NEAR_F64(x, 0.0, 1e-12);
    NTEST_ASSERT_EQ_UINT(nph_input_text(f->input, text, 4U), 0U);
    NTEST_ASSERT_TRUE(nph_input_mouse_down(f->input, NPH_MOUSE_BUTTON_LEFT));
    NTEST_ASSERT_FALSE(nph_input_mouse_pressed(f->input, NPH_MOUSE_BUTTON_LEFT));
    nph_input_mouse_position(f->input, &x, &y);
    NTEST_ASSERT_NEAR_F64(x, 110.0, 1e-12);

    // Text beyond the cap is dropped, not overrun.
    NphWindowEvent many[NPH_INPUT_TEXT_MAX + 5U];
    for (u32 i = 0; i < NPH_ARRAY_COUNT(many); ++i) {
        many[i] = char_event('a' + i);
    }
    frame(f->input, many, (u32)NPH_ARRAY_COUNT(many));
    u32 all[NPH_INPUT_TEXT_MAX + 5U];
    NTEST_ASSERT_EQ_UINT(nph_input_text(f->input, all, (u32)NPH_ARRAY_COUNT(all)),
                         NPH_INPUT_TEXT_MAX);
}

NTEST_F(InputFixture, losing_focus_releases_everything)
{
    NphWindowEvent held[3] = {
        key_event(NPH_KEY_W, NPH_KEY_ACTION_PRESS, NPH_MOD_CONTROL),
        mouse_button_event(NPH_MOUSE_BUTTON_RIGHT, NPH_KEY_ACTION_PRESS, NPH_MOD_CONTROL),
        focus_event(false)};
    frame(f->input, held, 3U);
    NTEST_ASSERT_FALSE(nph_input_key_down(f->input, NPH_KEY_W));
    NTEST_ASSERT_TRUE(nph_input_key_released(f->input, NPH_KEY_W));
    NTEST_ASSERT_FALSE(nph_input_mouse_down(f->input, NPH_MOUSE_BUTTON_RIGHT));
    NTEST_ASSERT_TRUE(nph_input_mouse_released(f->input, NPH_MOUSE_BUTTON_RIGHT));
    NTEST_ASSERT_EQ_UINT(nph_input_mods(f->input), 0U);
    NphWindowEvent back = focus_event(true);
    frame(f->input, &back, 1U);
    NTEST_ASSERT_FALSE(nph_input_key_released(f->input, NPH_KEY_W));
}

NTEST_F(InputFixture, actions_register_find_and_limits)
{
    NphActionId jump = NPH_ACTION_INVALID;
    NphActionId fire = NPH_ACTION_INVALID;
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "jump", &jump), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "fire", &fire), NPH_RESULT_OK);
    NTEST_ASSERT_NE_UINT(jump, fire);
    NTEST_ASSERT_EQ_UINT(nph_input_action_count(f->input), 2U);
    NTEST_ASSERT_EQ_STR(nph_input_action_name(f->input, fire), "fire");
    NTEST_ASSERT_EQ_STR(nph_input_action_name(f->input, 99U), "");
    NphActionId found = NPH_ACTION_INVALID;
    NTEST_ASSERT_EQ_INT(nph_input_action_find(f->input, "jump", &found), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(found, jump);
    NTEST_ASSERT_EQ_INT(nph_input_action_find(f->input, "dance", &found), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_UINT(found, NPH_ACTION_INVALID);
    NphActionId dup = 0U;
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "jump", &dup),
                        NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_UINT(dup, NPH_ACTION_INVALID);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "", &dup), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        nph_input_action_register(f->input, "a-name-that-is-far-too-long-for-the-table", &dup),
        NPH_RESULT_INVALID_ARGUMENT);

    // Invalid action ids read as inactive and refuse bindings.
    NTEST_ASSERT_FALSE(nph_input_action_down(f->input, 42U));
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, 42U), 0.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(f->input, 42U, NPH_KEY_A, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(f->input, jump, NPH_KEY_UNKNOWN, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);

    // Capacity limits.
    NphInputDesc small;
    small.max_actions = 2U;
    small.max_bindings = 1U;
    NphInput *tiny = NULL;
    NTEST_ASSERT_EQ_INT(nph_input_create(nph_allocator_system(), &small, &tiny), NPH_RESULT_OK);
    NphActionId a = 0U;
    NphActionId b = 0U;
    NphActionId c = 0U;
    NTEST_ASSERT_EQ_INT(nph_input_action_register(tiny, "a", &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(tiny, "b", &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(tiny, "c", &c), NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(tiny, a, NPH_KEY_A, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(tiny, a, NPH_KEY_B, 0U), NPH_RESULT_FULL);
    nph_input_destroy(tiny);
}

NTEST_F(InputFixture, key_bindings_with_modifiers)
{
    NphActionId save = 0U;
    NphActionId walk = 0U;
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "save", &save), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "walk", &walk), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(f->input, save, NPH_KEY_S, NPH_MOD_CONTROL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(f->input, walk, NPH_KEY_W, NPH_MOD_ANY), NPH_RESULT_OK);

    // S alone: not save. Ctrl+S: save, pressed this frame.
    NphWindowEvent s = key_event(NPH_KEY_S, NPH_KEY_ACTION_PRESS, 0U);
    frame(f->input, &s, 1U);
    NTEST_ASSERT_FALSE(nph_input_action_down(f->input, save));
    NphWindowEvent ctrl_s = key_event(NPH_KEY_S, NPH_KEY_ACTION_REPEAT, NPH_MOD_CONTROL);
    frame(f->input, &ctrl_s, 1U);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, save));
    NTEST_ASSERT_TRUE(nph_input_action_pressed(f->input, save));
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, save), 1.0F, 0.0F);
    // Lock keys are ignored in the comparison.
    NphWindowEvent ctrl_caps =
        key_event(NPH_KEY_S, NPH_KEY_ACTION_REPEAT, NPH_MOD_CONTROL | NPH_MOD_CAPS_LOCK);
    frame(f->input, &ctrl_caps, 1U);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, save));
    NTEST_ASSERT_FALSE(nph_input_action_pressed(f->input, save));
    // Releasing control while S is held ends the action.
    NphWindowEvent no_ctrl = key_event(NPH_KEY_LEFT_CONTROL, NPH_KEY_ACTION_RELEASE, 0U);
    frame(f->input, &no_ctrl, 1U);
    NTEST_ASSERT_FALSE(nph_input_action_down(f->input, save));
    NTEST_ASSERT_TRUE(nph_input_action_released(f->input, save));

    // NPH_MOD_ANY: W with or without shift.
    NphWindowEvent w = key_event(NPH_KEY_W, NPH_KEY_ACTION_PRESS, 0U);
    frame(f->input, &w, 1U);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, walk));
    NphWindowEvent shift_w = key_event(NPH_KEY_W, NPH_KEY_ACTION_REPEAT, NPH_MOD_SHIFT);
    frame(f->input, &shift_w, 1U);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, walk));

    nph_input_unbind_all(f->input, walk);
    frame(f->input, NULL, 0U);
    NTEST_ASSERT_FALSE(nph_input_action_down(f->input, walk));
    NTEST_ASSERT_TRUE(nph_input_action_released(f->input, walk));
}

NTEST_F(InputFixture, axes_combine_and_clamp)
{
    NphActionId move = 0U;
    NphActionId look = 0U;
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "move_x", &move), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "look_x", &look), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key_axis(f->input, move, NPH_KEY_A, NPH_KEY_D),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_gamepad_axis(f->input, move, NPH_GAMEPAD_ANY,
                                                    NPH_GAMEPAD_AXIS_LEFT_X, 1.0F, 0.2F),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_mouse_axis(f->input, look, NPH_MOUSE_AXIS_X, 0.5F),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_input_bind_mouse_button(f->input, look, NPH_MOUSE_BUTTON_LEFT, NPH_MOD_ANY),
        NPH_RESULT_OK);

    // D alone: +1. A and D: 0 but down.
    NphWindowEvent d = key_event(NPH_KEY_D, NPH_KEY_ACTION_PRESS, 0U);
    frame(f->input, &d, 1U);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, move), 1.0F, 0.0F);
    NTEST_ASSERT_TRUE(nph_input_action_pressed(f->input, move));
    NphWindowEvent a = key_event(NPH_KEY_A, NPH_KEY_ACTION_PRESS, 0U);
    frame(f->input, &a, 1U);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, move), 0.0F, 0.0F);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, move));

    // A gamepad stick inside the dead zone adds nothing; outside it adds
    // its rescaled value; the sum is clamped to [-1, 1].
    NphGamepadState pad;
    memset(&pad, 0, sizeof pad);
    pad.connected = 1;
    pad.axes[NPH_GAMEPAD_AXIS_LEFT_X] = 0.1F;
    nph_input_begin_frame(f->input);
    nph_input_set_gamepad(f->input, 1U, &pad);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, move), 0.0F, 0.0F);
    NTEST_ASSERT_NOT_NULL(nph_input_gamepad(f->input, 1U));
    NTEST_ASSERT_NULL(nph_input_gamepad(f->input, 0U));
    pad.axes[NPH_GAMEPAD_AXIS_LEFT_X] = 0.6F; // (0.6 - 0.2) / 0.8 = 0.5
    NphWindowEvent release_a = key_event(NPH_KEY_A, NPH_KEY_ACTION_RELEASE, 0U);
    nph_input_begin_frame(f->input);
    nph_input_process_event(f->input, &release_a);
    nph_input_set_gamepad(f->input, 1U, &pad);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, move), 1.0F, 1e-6F); // 1 + 0.5, clamped
    NphWindowEvent release_d = key_event(NPH_KEY_D, NPH_KEY_ACTION_RELEASE, 0U);
    nph_input_begin_frame(f->input);
    nph_input_process_event(f->input, &release_d);
    nph_input_set_gamepad(f->input, 1U, &pad);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, move), 0.5F, 1e-6F);
    pad.axes[NPH_GAMEPAD_AXIS_LEFT_X] = -1.0F;
    nph_input_begin_frame(f->input);
    nph_input_set_gamepad(f->input, 1U, &pad);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, move), -1.0F, 1e-6F);
    // Disconnecting the pad ends the action.
    nph_input_begin_frame(f->input);
    nph_input_set_gamepad(f->input, 1U, NULL);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_FALSE(nph_input_action_down(f->input, move));
    NTEST_ASSERT_TRUE(nph_input_action_released(f->input, move));

    // Mouse deltas pass through unclamped, scaled; a button adds its clamped 1.
    NphWindowEvent moves[3] = {mouse_move_event(0.0, 0.0), mouse_move_event(10.0, 0.0),
                               mouse_button_event(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS, 0U)};
    frame(f->input, moves, 3U);
    NTEST_ASSERT_NEAR_F32(nph_input_action_value(f->input, look), 5.0F + 1.0F, 1e-6F);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, look));

    // Bad binding arguments.
    NTEST_ASSERT_EQ_INT(
        nph_input_bind_gamepad_axis(f->input, move, 99U, NPH_GAMEPAD_AXIS_LEFT_X, 1.0F, 0.0F),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_input_bind_gamepad_axis(f->input, move, NPH_GAMEPAD_ANY,
                                                    NPH_GAMEPAD_AXIS_LEFT_X, 1.0F, 1.0F),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        nph_input_bind_mouse_axis(f->input, move, (NphMouseAxis)NPH_MOUSE_AXIS_COUNT, 1.0F),
        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST_F(InputFixture, gamepad_buttons_and_specific_pads)
{
    NphActionId fire = 0U;
    NphActionId p2_fire = 0U;
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "fire", &fire), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "p2_fire", &p2_fire), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_input_bind_gamepad_button(f->input, fire, NPH_GAMEPAD_ANY, NPH_GAMEPAD_BUTTON_A),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_gamepad_button(f->input, p2_fire, 1U, NPH_GAMEPAD_BUTTON_A),
                        NPH_RESULT_OK);
    NphGamepadState pad;
    memset(&pad, 0, sizeof pad);
    pad.connected = 1;
    pad.buttons[NPH_GAMEPAD_BUTTON_A] = 1U;
    nph_input_begin_frame(f->input);
    nph_input_set_gamepad(f->input, 0U, &pad);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_TRUE(nph_input_action_pressed(f->input, fire));
    NTEST_ASSERT_FALSE(nph_input_action_down(f->input, p2_fire));
    nph_input_begin_frame(f->input);
    nph_input_set_gamepad(f->input, 0U, NULL);
    nph_input_set_gamepad(f->input, 1U, &pad);
    nph_input_set_gamepad(f->input, NPH_GAMEPAD_MAX + 3U, &pad); // ignored
    nph_input_end_frame(f->input);
    NTEST_ASSERT_TRUE(nph_input_action_down(f->input, fire));
    NTEST_ASSERT_FALSE(nph_input_action_pressed(f->input, fire)); // still down via pad 1
    NTEST_ASSERT_TRUE(nph_input_action_pressed(f->input, p2_fire));
    NTEST_ASSERT_NULL(nph_input_gamepad(f->input, NPH_GAMEPAD_MAX));

    // Polling without a window system: everything disconnected.
    nph_input_begin_frame(f->input);
    nph_input_poll_gamepads(f->input);
    nph_input_end_frame(f->input);
    NTEST_ASSERT_NULL(nph_input_gamepad(f->input, 1U));
    NTEST_ASSERT_TRUE(nph_input_action_released(f->input, fire));
}

NTEST(input, names)
{
    NTEST_ASSERT_EQ_STR(nph_key_name(NPH_KEY_A), "a");
    NTEST_ASSERT_EQ_STR(nph_key_name(NPH_KEY_LEFT_SHIFT), "left shift");
    NTEST_ASSERT_EQ_STR(nph_key_name(NPH_KEY_F11), "f11");
    NTEST_ASSERT_EQ_STR(nph_key_name(NPH_KEY_MENU), "menu");
    NTEST_ASSERT_EQ_STR(nph_key_name(NPH_KEY_UNKNOWN), "unknown");
    NTEST_ASSERT_EQ_STR(nph_key_name((NphKey)NPH_KEY_COUNT), "?");
    for (u32 k = 0; k < NPH_KEY_COUNT; ++k) {
        NTEST_ASSERT_NOT_NULL(nph_key_name((NphKey)k));
        NTEST_ASSERT_NE_STR(nph_key_name((NphKey)k), "?");
    }
    NTEST_ASSERT_EQ_STR(nph_mouse_button_name(NPH_MOUSE_BUTTON_LEFT), "mouse left");
    NTEST_ASSERT_EQ_STR(nph_mouse_button_name(NPH_MOUSE_BUTTON_8), "mouse 8");
    NTEST_ASSERT_EQ_STR(nph_mouse_button_name((NphMouseButton)NPH_MOUSE_BUTTON_COUNT), "?");
    NTEST_ASSERT_EQ_STR(nph_gamepad_button_name(NPH_GAMEPAD_BUTTON_DPAD_LEFT), "dpad left");
    NTEST_ASSERT_EQ_STR(nph_gamepad_axis_name(NPH_GAMEPAD_AXIS_RIGHT_TRIGGER), "right trigger");
    NTEST_ASSERT_EQ_STR(nph_window_event_name(NPH_WINDOW_EVENT_FRAMEBUFFER_RESIZE),
                        "framebuffer resize");
    NTEST_ASSERT_EQ_STR(nph_window_event_name(NPH_WINDOW_EVENT_CLOSE), "close");
    NTEST_ASSERT_EQ_STR(nph_window_event_name((NphWindowEventType)NPH_WINDOW_EVENT_COUNT), "?");
}

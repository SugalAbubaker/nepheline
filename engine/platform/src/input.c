// SPDX-License-Identifier: Apache-2.0
// The action layer, plus the static name tables of window.h (kept here so
// they are available and tested without a display).
#include <nepheline/core/assert.h>
#include <nepheline/platform/input.h>

#include <string.h>

#define DEFAULT_MAX_ACTIONS  64U
#define DEFAULT_MAX_BINDINGS 256U
#define ACTION_NAME_MAX      32U
#define MODS_MASK            (NPH_MOD_SHIFT | NPH_MOD_CONTROL | NPH_MOD_ALT | NPH_MOD_SUPER)

// Per-key / per-button state bits.
#define STATE_DOWN     1U
#define STATE_PRESSED  2U
#define STATE_RELEASED 4U

typedef enum BindingKind {
    BINDING_KEY,
    BINDING_MOUSE_BUTTON,
    BINDING_KEY_AXIS,
    BINDING_MOUSE_AXIS,
    BINDING_GAMEPAD_BUTTON,
    BINDING_GAMEPAD_AXIS,
} BindingKind;

typedef struct Binding {
    u32 action;
    u32 kind; // BindingKind
    u32 a;    // key / button / negative key / axis
    u32 b;    // mods / positive key / gamepad index
    f32 scale;
    f32 deadzone;
} Binding;

typedef struct Action {
    char name[ACTION_NAME_MAX];
    u8 state; // STATE_* bits
    f32 value;
} Action;

struct NphInput {
    NphAllocator *alloc;
    u32 max_actions;
    u32 max_bindings;
    u32 action_count;
    u32 binding_count;
    Action *actions;
    Binding *bindings;

    u8 keys[NPH_KEY_COUNT];
    u8 mouse[NPH_MOUSE_BUTTON_COUNT];
    u32 mods;
    f64 mouse_x;
    f64 mouse_y;
    f64 mouse_dx;
    f64 mouse_dy;
    f64 scroll_dx;
    f64 scroll_dy;
    b32 have_mouse_position; // the first MOVE sets the position without a delta
    u32 text[NPH_INPUT_TEXT_MAX];
    u32 text_count;
    NphGamepadState gamepads[NPH_GAMEPAD_MAX];
};

// ---------------------------------------------------------------------------
// Lifecycle and frame cycle
// ---------------------------------------------------------------------------

NphResult nph_input_create(NphAllocator *alloc, const NphInputDesc *desc, NphInput **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    *out = NULL;
    const u32 max_actions =
        desc != NULL && desc->max_actions != 0U ? desc->max_actions : DEFAULT_MAX_ACTIONS;
    const u32 max_bindings =
        desc != NULL && desc->max_bindings != 0U ? desc->max_bindings : DEFAULT_MAX_BINDINGS;
    NphInput *input = NPH_ALLOC_TYPE(alloc, NphInput);
    if (input == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    input->alloc = alloc;
    input->max_actions = max_actions;
    input->max_bindings = max_bindings;
    input->actions = NPH_ALLOC_ARRAY_ZEROED(alloc, Action, max_actions);
    input->bindings = NPH_ALLOC_ARRAY_ZEROED(alloc, Binding, max_bindings);
    if (input->actions == NULL || input->bindings == NULL) {
        nph_input_destroy(input);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    *out = input;
    return NPH_RESULT_OK;
}

void nph_input_destroy(NphInput *input)
{
    if (input == NULL) {
        return;
    }
    NphAllocator *alloc = input->alloc;
    if (input->actions != NULL) {
        NPH_FREE_ARRAY(alloc, Action, input->actions, input->max_actions);
    }
    if (input->bindings != NULL) {
        NPH_FREE_ARRAY(alloc, Binding, input->bindings, input->max_bindings);
    }
    NPH_FREE_TYPE(alloc, NphInput, input);
}

void nph_input_begin_frame(NphInput *input)
{
    NPH_ASSERT(input != NULL);
    for (u32 i = 0; i < NPH_KEY_COUNT; ++i) {
        input->keys[i] &= STATE_DOWN;
    }
    for (u32 i = 0; i < NPH_MOUSE_BUTTON_COUNT; ++i) {
        input->mouse[i] &= STATE_DOWN;
    }
    input->mouse_dx = 0.0;
    input->mouse_dy = 0.0;
    input->scroll_dx = 0.0;
    input->scroll_dy = 0.0;
    input->text_count = 0U;
}

static void apply_button(u8 *state, u32 action)
{
    if (action == NPH_KEY_ACTION_PRESS) {
        if ((*state & STATE_DOWN) == 0U) {
            *state |= STATE_PRESSED;
        }
        *state |= STATE_DOWN;
    } else if (action == NPH_KEY_ACTION_RELEASE) {
        if ((*state & STATE_DOWN) != 0U) {
            *state |= STATE_RELEASED;
        }
        *state = (u8)(*state & ~STATE_DOWN);
    }
    // REPEAT changes nothing: the key is already down.
}

void nph_input_process_event(NphInput *input, const NphWindowEvent *event)
{
    NPH_ASSERT(input != NULL && event != NULL);
    switch ((NphWindowEventType)event->type) {
    case NPH_WINDOW_EVENT_KEY:
        if (event->u.key.key < NPH_KEY_COUNT) {
            apply_button(&input->keys[event->u.key.key], event->u.key.action);
        }
        input->mods = event->u.key.mods;
        break;
    case NPH_WINDOW_EVENT_CHAR:
        if (input->text_count < NPH_INPUT_TEXT_MAX) {
            input->text[input->text_count++] = event->u.ch.codepoint;
        }
        break;
    case NPH_WINDOW_EVENT_MOUSE_BUTTON:
        if (event->u.mouse_button.button < NPH_MOUSE_BUTTON_COUNT) {
            apply_button(&input->mouse[event->u.mouse_button.button], event->u.mouse_button.action);
        }
        input->mods = event->u.mouse_button.mods;
        break;
    case NPH_WINDOW_EVENT_MOUSE_MOVE:
        if (input->have_mouse_position) {
            input->mouse_dx += event->u.mouse_move.x - input->mouse_x;
            input->mouse_dy += event->u.mouse_move.y - input->mouse_y;
        }
        input->mouse_x = event->u.mouse_move.x;
        input->mouse_y = event->u.mouse_move.y;
        input->have_mouse_position = 1;
        break;
    case NPH_WINDOW_EVENT_MOUSE_SCROLL:
        input->scroll_dx += event->u.scroll.dx;
        input->scroll_dy += event->u.scroll.dy;
        break;
    case NPH_WINDOW_EVENT_FOCUS:
        if (!event->u.focus.focused) {
            // Keys released while unfocused never arrive; drop them all.
            for (u32 i = 0; i < NPH_KEY_COUNT; ++i) {
                if ((input->keys[i] & STATE_DOWN) != 0U) {
                    input->keys[i] = STATE_RELEASED;
                }
            }
            for (u32 i = 0; i < NPH_MOUSE_BUTTON_COUNT; ++i) {
                if ((input->mouse[i] & STATE_DOWN) != 0U) {
                    input->mouse[i] = STATE_RELEASED;
                }
            }
            input->mods = 0U;
        }
        break;
    default:
        break;
    }
}

void nph_input_set_gamepad(NphInput *input, u32 index, const NphGamepadState *state)
{
    NPH_ASSERT(input != NULL);
    if (index >= NPH_GAMEPAD_MAX) {
        return;
    }
    if (state == NULL || !state->connected) {
        memset(&input->gamepads[index], 0, sizeof input->gamepads[index]);
    } else {
        input->gamepads[index] = *state;
        input->gamepads[index].connected = 1;
    }
}

void nph_input_poll_gamepads(NphInput *input)
{
    NPH_ASSERT(input != NULL);
    for (u32 i = 0; i < NPH_GAMEPAD_MAX; ++i) {
        NphGamepadState state;
        if (nph_window_get_gamepad_state(i, &state)) {
            nph_input_set_gamepad(input, i, &state);
        } else {
            nph_input_set_gamepad(input, i, NULL);
        }
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

static bool mods_match(u32 required, u32 current)
{
    return required == NPH_MOD_ANY || (current & MODS_MASK) == (required & MODS_MASK);
}

static f32 clamp_unit(f32 v)
{
    return v < -1.0F ? -1.0F : (v > 1.0F ? 1.0F : v);
}

static f32 apply_deadzone(f32 v, f32 deadzone)
{
    const f32 magnitude = v < 0.0F ? -v : v;
    if (magnitude <= deadzone) {
        return 0.0F;
    }
    // Rescale so the output starts at 0 at the edge and reaches 1 at full.
    const f32 scaled = (magnitude - deadzone) / (1.0F - deadzone);
    return v < 0.0F ? -scaled : scaled;
}

// A raw state that went down and came back up within the frame.
static bool tapped(u8 state)
{
    return (state & (STATE_PRESSED | STATE_RELEASED | STATE_DOWN)) ==
           (STATE_PRESSED | STATE_RELEASED);
}

// Whether a key or button of the binding was pressed and released within
// the frame: a tap shorter than a frame, which the end-of-frame state
// alone would never show.
static bool binding_tapped(const NphInput *input, const Binding *b)
{
    switch ((BindingKind)b->kind) {
    case BINDING_KEY:
        return tapped(input->keys[b->a]) && mods_match(b->b, input->mods);
    case BINDING_MOUSE_BUTTON:
        return tapped(input->mouse[b->a]) && mods_match(b->b, input->mods);
    case BINDING_KEY_AXIS:
        return tapped(input->keys[b->a]) || tapped(input->keys[b->b]);
    default:
        return false; // axes and polled gamepads have no events within a frame
    }
}

// Contribution of one binding: its digital state and its analog value
// (digital bindings contribute ±1 to the clamped sum; mouse axes are
// unclamped deltas).
static void evaluate_binding(const NphInput *input, const Binding *b, bool *out_down,
                             f32 *out_clamped, f32 *out_unclamped)
{
    switch ((BindingKind)b->kind) {
    case BINDING_KEY:
        if ((input->keys[b->a] & STATE_DOWN) != 0U && mods_match(b->b, input->mods)) {
            *out_down = true;
            *out_clamped += 1.0F;
        }
        break;
    case BINDING_MOUSE_BUTTON:
        if ((input->mouse[b->a] & STATE_DOWN) != 0U && mods_match(b->b, input->mods)) {
            *out_down = true;
            *out_clamped += 1.0F;
        }
        break;
    case BINDING_KEY_AXIS: {
        const bool negative = (input->keys[b->a] & STATE_DOWN) != 0U;
        const bool positive = (input->keys[b->b] & STATE_DOWN) != 0U;
        if (negative || positive) {
            *out_down = true;
        }
        *out_clamped += (positive ? 1.0F : 0.0F) - (negative ? 1.0F : 0.0F);
        break;
    }
    case BINDING_MOUSE_AXIS: {
        f64 delta = 0.0;
        switch ((NphMouseAxis)b->a) {
        case NPH_MOUSE_AXIS_X:
            delta = input->mouse_dx;
            break;
        case NPH_MOUSE_AXIS_Y:
            delta = input->mouse_dy;
            break;
        case NPH_MOUSE_AXIS_SCROLL_X:
            delta = input->scroll_dx;
            break;
        case NPH_MOUSE_AXIS_SCROLL_Y:
            delta = input->scroll_dy;
            break;
        default:
            break;
        }
        if (delta != 0.0) {
            *out_down = true;
            *out_unclamped += (f32)delta * b->scale;
        }
        break;
    }
    case BINDING_GAMEPAD_BUTTON:
        for (u32 g = 0; g < NPH_GAMEPAD_MAX; ++g) {
            if (b->b != NPH_GAMEPAD_ANY && b->b != g) {
                continue;
            }
            if (input->gamepads[g].connected && input->gamepads[g].buttons[b->a] != 0U) {
                *out_down = true;
                *out_clamped += 1.0F;
            }
        }
        break;
    case BINDING_GAMEPAD_AXIS:
        for (u32 g = 0; g < NPH_GAMEPAD_MAX; ++g) {
            if (b->b != NPH_GAMEPAD_ANY && b->b != g) {
                continue;
            }
            if (!input->gamepads[g].connected) {
                continue;
            }
            const f32 v = apply_deadzone(input->gamepads[g].axes[b->a], b->deadzone);
            if (v != 0.0F) {
                *out_down = true;
                *out_clamped += v * b->scale;
            }
        }
        break;
    default:
        break;
    }
}

void nph_input_end_frame(NphInput *input)
{
    NPH_ASSERT(input != NULL);
    for (u32 a = 0; a < input->action_count; ++a) {
        bool down = false;
        bool tap = false;
        f32 clamped = 0.0F;
        f32 unclamped = 0.0F;
        for (u32 i = 0; i < input->binding_count; ++i) {
            if (input->bindings[i].action == a) {
                evaluate_binding(input, &input->bindings[i], &down, &clamped, &unclamped);
                tap = tap || binding_tapped(input, &input->bindings[i]);
            }
        }
        Action *action = &input->actions[a];
        const bool was_down = (action->state & STATE_DOWN) != 0U;
        // An action idle at both ends of the frame that a binding tapped in
        // between was pressed and released.
        const bool tapped_through = tap && !down && !was_down;
        action->state = (u8)((down ? STATE_DOWN : 0U) |
                             ((down && !was_down) || tapped_through ? STATE_PRESSED : 0U) |
                             ((!down && was_down) || tapped_through ? STATE_RELEASED : 0U));
        action->value = clamp_unit(clamped) + unclamped;
    }
}

// ---------------------------------------------------------------------------
// Raw queries
// ---------------------------------------------------------------------------

bool nph_input_key_down(const NphInput *input, NphKey key)
{
    return (u32)key < NPH_KEY_COUNT && (input->keys[key] & STATE_DOWN) != 0U;
}

bool nph_input_key_pressed(const NphInput *input, NphKey key)
{
    return (u32)key < NPH_KEY_COUNT && (input->keys[key] & STATE_PRESSED) != 0U;
}

bool nph_input_key_released(const NphInput *input, NphKey key)
{
    return (u32)key < NPH_KEY_COUNT && (input->keys[key] & STATE_RELEASED) != 0U;
}

bool nph_input_mouse_down(const NphInput *input, NphMouseButton button)
{
    return (u32)button < NPH_MOUSE_BUTTON_COUNT && (input->mouse[button] & STATE_DOWN) != 0U;
}

bool nph_input_mouse_pressed(const NphInput *input, NphMouseButton button)
{
    return (u32)button < NPH_MOUSE_BUTTON_COUNT && (input->mouse[button] & STATE_PRESSED) != 0U;
}

bool nph_input_mouse_released(const NphInput *input, NphMouseButton button)
{
    return (u32)button < NPH_MOUSE_BUTTON_COUNT && (input->mouse[button] & STATE_RELEASED) != 0U;
}

u32 nph_input_mods(const NphInput *input)
{
    return input->mods;
}

void nph_input_mouse_position(const NphInput *input, f64 *out_x, f64 *out_y)
{
    if (out_x != NULL) {
        *out_x = input->mouse_x;
    }
    if (out_y != NULL) {
        *out_y = input->mouse_y;
    }
}

void nph_input_mouse_delta(const NphInput *input, f64 *out_dx, f64 *out_dy)
{
    if (out_dx != NULL) {
        *out_dx = input->mouse_dx;
    }
    if (out_dy != NULL) {
        *out_dy = input->mouse_dy;
    }
}

void nph_input_scroll(const NphInput *input, f64 *out_dx, f64 *out_dy)
{
    if (out_dx != NULL) {
        *out_dx = input->scroll_dx;
    }
    if (out_dy != NULL) {
        *out_dy = input->scroll_dy;
    }
}

u32 nph_input_text(const NphInput *input, u32 *out_codepoints, u32 max)
{
    const u32 n = input->text_count < max ? input->text_count : max;
    if (out_codepoints != NULL && n > 0U) {
        memcpy(out_codepoints, input->text, n * sizeof(u32));
    }
    return n;
}

const NphGamepadState *nph_input_gamepad(const NphInput *input, u32 index)
{
    if (index >= NPH_GAMEPAD_MAX || !input->gamepads[index].connected) {
        return NULL;
    }
    return &input->gamepads[index];
}

// ---------------------------------------------------------------------------
// Actions and bindings
// ---------------------------------------------------------------------------

NphResult nph_input_action_register(NphInput *input, const char *name, NphActionId *out)
{
    NPH_ASSERT(input != NULL && name != NULL && out != NULL);
    *out = NPH_ACTION_INVALID;
    const u64 len = strlen(name);
    if (len == 0U || len >= ACTION_NAME_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphActionId existing;
    if (nph_input_action_find(input, name, &existing) == NPH_RESULT_OK) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    if (input->action_count == input->max_actions) {
        return NPH_RESULT_FULL;
    }
    Action *action = &input->actions[input->action_count];
    memset(action, 0, sizeof *action);
    memcpy(action->name, name, len + 1U);
    *out = input->action_count++;
    return NPH_RESULT_OK;
}

NphResult nph_input_action_find(const NphInput *input, const char *name, NphActionId *out)
{
    NPH_ASSERT(input != NULL && name != NULL && out != NULL);
    for (u32 i = 0; i < input->action_count; ++i) {
        if (strcmp(input->actions[i].name, name) == 0) {
            *out = i;
            return NPH_RESULT_OK;
        }
    }
    *out = NPH_ACTION_INVALID;
    return NPH_RESULT_NOT_FOUND;
}

const char *nph_input_action_name(const NphInput *input, NphActionId action)
{
    NPH_ASSERT(input != NULL);
    return action < input->action_count ? input->actions[action].name : "";
}

u32 nph_input_action_count(const NphInput *input)
{
    NPH_ASSERT(input != NULL);
    return input->action_count;
}

static NphResult add_binding(NphInput *input, NphActionId action, BindingKind kind, u32 a, u32 b,
                             f32 scale, f32 deadzone)
{
    NPH_ASSERT(input != NULL);
    if (action >= input->action_count) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (input->binding_count == input->max_bindings) {
        return NPH_RESULT_FULL;
    }
    Binding *binding = &input->bindings[input->binding_count++];
    binding->action = action;
    binding->kind = (u32)kind;
    binding->a = a;
    binding->b = b;
    binding->scale = scale;
    binding->deadzone = deadzone;
    return NPH_RESULT_OK;
}

NphResult nph_input_bind_key(NphInput *input, NphActionId action, NphKey key, u32 mods)
{
    if ((u32)key >= NPH_KEY_COUNT || key == NPH_KEY_UNKNOWN) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return add_binding(input, action, BINDING_KEY, (u32)key, mods, 1.0F, 0.0F);
}

NphResult nph_input_bind_mouse_button(NphInput *input, NphActionId action, NphMouseButton button,
                                      u32 mods)
{
    if ((u32)button >= NPH_MOUSE_BUTTON_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return add_binding(input, action, BINDING_MOUSE_BUTTON, (u32)button, mods, 1.0F, 0.0F);
}

NphResult nph_input_bind_key_axis(NphInput *input, NphActionId action, NphKey negative,
                                  NphKey positive)
{
    if ((u32)negative >= NPH_KEY_COUNT || (u32)positive >= NPH_KEY_COUNT ||
        negative == NPH_KEY_UNKNOWN || positive == NPH_KEY_UNKNOWN) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return add_binding(input, action, BINDING_KEY_AXIS, (u32)negative, (u32)positive, 1.0F, 0.0F);
}

NphResult nph_input_bind_mouse_axis(NphInput *input, NphActionId action, NphMouseAxis axis,
                                    f32 scale)
{
    if ((u32)axis >= NPH_MOUSE_AXIS_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return add_binding(input, action, BINDING_MOUSE_AXIS, (u32)axis, 0U, scale, 0.0F);
}

NphResult nph_input_bind_gamepad_button(NphInput *input, NphActionId action, u32 gamepad,
                                        NphGamepadButton button)
{
    if ((u32)button >= NPH_GAMEPAD_BUTTON_COUNT ||
        (gamepad != NPH_GAMEPAD_ANY && gamepad >= NPH_GAMEPAD_MAX)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return add_binding(input, action, BINDING_GAMEPAD_BUTTON, (u32)button, gamepad, 1.0F, 0.0F);
}

NphResult nph_input_bind_gamepad_axis(NphInput *input, NphActionId action, u32 gamepad,
                                      NphGamepadAxis axis, f32 scale, f32 deadzone)
{
    if ((u32)axis >= NPH_GAMEPAD_AXIS_COUNT ||
        (gamepad != NPH_GAMEPAD_ANY && gamepad >= NPH_GAMEPAD_MAX) || deadzone < 0.0F ||
        deadzone >= 1.0F) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return add_binding(input, action, BINDING_GAMEPAD_AXIS, (u32)axis, gamepad, scale, deadzone);
}

void nph_input_unbind_all(NphInput *input, NphActionId action)
{
    NPH_ASSERT(input != NULL);
    u32 kept = 0U;
    for (u32 i = 0; i < input->binding_count; ++i) {
        if (input->bindings[i].action != action) {
            input->bindings[kept++] = input->bindings[i];
        }
    }
    input->binding_count = kept;
}

bool nph_input_action_down(const NphInput *input, NphActionId action)
{
    return action < input->action_count && (input->actions[action].state & STATE_DOWN) != 0U;
}

bool nph_input_action_pressed(const NphInput *input, NphActionId action)
{
    return action < input->action_count && (input->actions[action].state & STATE_PRESSED) != 0U;
}

bool nph_input_action_released(const NphInput *input, NphActionId action)
{
    return action < input->action_count && (input->actions[action].state & STATE_RELEASED) != 0U;
}

f32 nph_input_action_value(const NphInput *input, NphActionId action)
{
    return action < input->action_count ? input->actions[action].value : 0.0F;
}

// ---------------------------------------------------------------------------
// Names (window.h)
// ---------------------------------------------------------------------------

static const char *const g_key_names[NPH_KEY_COUNT] = {
    "unknown",
    "space",
    "apostrophe",
    "comma",
    "minus",
    "period",
    "slash",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "semicolon",
    "equal",
    "a",
    "b",
    "c",
    "d",
    "e",
    "f",
    "g",
    "h",
    "i",
    "j",
    "k",
    "l",
    "m",
    "n",
    "o",
    "p",
    "q",
    "r",
    "s",
    "t",
    "u",
    "v",
    "w",
    "x",
    "y",
    "z",
    "left bracket",
    "backslash",
    "right bracket",
    "grave accent",
    "world 1",
    "world 2",
    "escape",
    "enter",
    "tab",
    "backspace",
    "insert",
    "delete",
    "right",
    "left",
    "down",
    "up",
    "page up",
    "page down",
    "home",
    "end",
    "caps lock",
    "scroll lock",
    "num lock",
    "print screen",
    "pause",
    "f1",
    "f2",
    "f3",
    "f4",
    "f5",
    "f6",
    "f7",
    "f8",
    "f9",
    "f10",
    "f11",
    "f12",
    "f13",
    "f14",
    "f15",
    "f16",
    "f17",
    "f18",
    "f19",
    "f20",
    "f21",
    "f22",
    "f23",
    "f24",
    "f25",
    "keypad 0",
    "keypad 1",
    "keypad 2",
    "keypad 3",
    "keypad 4",
    "keypad 5",
    "keypad 6",
    "keypad 7",
    "keypad 8",
    "keypad 9",
    "keypad .",
    "keypad /",
    "keypad *",
    "keypad -",
    "keypad +",
    "keypad enter",
    "keypad =",
    "left shift",
    "left control",
    "left alt",
    "left super",
    "right shift",
    "right control",
    "right alt",
    "right super",
    "menu",
};

const char *nph_key_name(NphKey key)
{
    return (u32)key < NPH_KEY_COUNT ? g_key_names[key] : "?";
}

static const char *const g_mouse_button_names[NPH_MOUSE_BUTTON_COUNT] = {
    "mouse left", "mouse right", "mouse middle", "mouse 4",
    "mouse 5",    "mouse 6",     "mouse 7",      "mouse 8",
};

const char *nph_mouse_button_name(NphMouseButton button)
{
    return (u32)button < NPH_MOUSE_BUTTON_COUNT ? g_mouse_button_names[button] : "?";
}

static const char *const g_gamepad_button_names[NPH_GAMEPAD_BUTTON_COUNT] = {
    "a",          "b",         "x",         "y",          "left bumper", "right bumper",
    "back",       "start",     "guide",     "left thumb", "right thumb", "dpad up",
    "dpad right", "dpad down", "dpad left",
};

const char *nph_gamepad_button_name(NphGamepadButton button)
{
    return (u32)button < NPH_GAMEPAD_BUTTON_COUNT ? g_gamepad_button_names[button] : "?";
}

static const char *const g_gamepad_axis_names[NPH_GAMEPAD_AXIS_COUNT] = {
    "left x", "left y", "right x", "right y", "left trigger", "right trigger",
};

const char *nph_gamepad_axis_name(NphGamepadAxis axis)
{
    return (u32)axis < NPH_GAMEPAD_AXIS_COUNT ? g_gamepad_axis_names[axis] : "?";
}

static const char *const g_event_names[NPH_WINDOW_EVENT_COUNT] = {
    "none",         "key",         "char",    "mouse button",       "mouse move",
    "mouse scroll", "mouse enter", "resize",  "framebuffer resize", "content scale",
    "move",         "focus",       "iconify", "maximize",           "close",
    "drop",
};

const char *nph_window_event_name(NphWindowEventType type)
{
    return (u32)type < NPH_WINDOW_EVENT_COUNT ? g_event_names[type] : "?";
}

// SPDX-License-Identifier: Apache-2.0
// Dear ImGui's platform backend on the engine's window. See ui_platform.h.
#include "ui_platform.h"

#include <nepheline/core/assert.h>

#include <float.h>
#include <string.h>

// Modifier keys held, one bit per side, so releasing one side of a pair
// keeps the modifier while the other is down.
enum {
    UI_MOD_LEFT_CTRL = 1U << 0U,
    UI_MOD_RIGHT_CTRL = 1U << 1U,
    UI_MOD_LEFT_SHIFT = 1U << 2U,
    UI_MOD_RIGHT_SHIFT = 1U << 3U,
    UI_MOD_LEFT_ALT = 1U << 4U,
    UI_MOD_RIGHT_ALT = 1U << 5U,
    UI_MOD_LEFT_SUPER = 1U << 6U,
    UI_MOD_RIGHT_SUPER = 1U << 7U,
};

#define UI_MIN_CLIPBOARD 256U

ImGuiKey ui_platform_key(NphKey key)
{
    if (key >= NPH_KEY_0 && key <= NPH_KEY_9) {
        return (ImGuiKey)(ImGuiKey_0 + (int)(key - NPH_KEY_0));
    }
    if (key >= NPH_KEY_A && key <= NPH_KEY_Z) {
        return (ImGuiKey)(ImGuiKey_A + (int)(key - NPH_KEY_A));
    }
    if (key >= NPH_KEY_F1 && key <= NPH_KEY_F24) {
        return (ImGuiKey)(ImGuiKey_F1 + (int)(key - NPH_KEY_F1));
    }
    if (key >= NPH_KEY_KP_0 && key <= NPH_KEY_KP_9) {
        return (ImGuiKey)(ImGuiKey_Keypad0 + (int)(key - NPH_KEY_KP_0));
    }
    switch (key) {
    case NPH_KEY_SPACE:
        return ImGuiKey_Space;
    case NPH_KEY_APOSTROPHE:
        return ImGuiKey_Apostrophe;
    case NPH_KEY_COMMA:
        return ImGuiKey_Comma;
    case NPH_KEY_MINUS:
        return ImGuiKey_Minus;
    case NPH_KEY_PERIOD:
        return ImGuiKey_Period;
    case NPH_KEY_SLASH:
        return ImGuiKey_Slash;
    case NPH_KEY_SEMICOLON:
        return ImGuiKey_Semicolon;
    case NPH_KEY_EQUAL:
        return ImGuiKey_Equal;
    case NPH_KEY_LEFT_BRACKET:
        return ImGuiKey_LeftBracket;
    case NPH_KEY_BACKSLASH:
        return ImGuiKey_Backslash;
    case NPH_KEY_RIGHT_BRACKET:
        return ImGuiKey_RightBracket;
    case NPH_KEY_GRAVE_ACCENT:
        return ImGuiKey_GraveAccent;
    case NPH_KEY_WORLD_1: // the extra key of ISO keyboards, as GLFW's backend maps it
    case NPH_KEY_WORLD_2:
        return ImGuiKey_Oem102;
    case NPH_KEY_ESCAPE:
        return ImGuiKey_Escape;
    case NPH_KEY_ENTER:
        return ImGuiKey_Enter;
    case NPH_KEY_TAB:
        return ImGuiKey_Tab;
    case NPH_KEY_BACKSPACE:
        return ImGuiKey_Backspace;
    case NPH_KEY_INSERT:
        return ImGuiKey_Insert;
    case NPH_KEY_DELETE:
        return ImGuiKey_Delete;
    case NPH_KEY_RIGHT:
        return ImGuiKey_RightArrow;
    case NPH_KEY_LEFT:
        return ImGuiKey_LeftArrow;
    case NPH_KEY_DOWN:
        return ImGuiKey_DownArrow;
    case NPH_KEY_UP:
        return ImGuiKey_UpArrow;
    case NPH_KEY_PAGE_UP:
        return ImGuiKey_PageUp;
    case NPH_KEY_PAGE_DOWN:
        return ImGuiKey_PageDown;
    case NPH_KEY_HOME:
        return ImGuiKey_Home;
    case NPH_KEY_END:
        return ImGuiKey_End;
    case NPH_KEY_CAPS_LOCK:
        return ImGuiKey_CapsLock;
    case NPH_KEY_SCROLL_LOCK:
        return ImGuiKey_ScrollLock;
    case NPH_KEY_NUM_LOCK:
        return ImGuiKey_NumLock;
    case NPH_KEY_PRINT_SCREEN:
        return ImGuiKey_PrintScreen;
    case NPH_KEY_PAUSE:
        return ImGuiKey_Pause;
    case NPH_KEY_KP_DECIMAL:
        return ImGuiKey_KeypadDecimal;
    case NPH_KEY_KP_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case NPH_KEY_KP_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case NPH_KEY_KP_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case NPH_KEY_KP_ADD:
        return ImGuiKey_KeypadAdd;
    case NPH_KEY_KP_ENTER:
        return ImGuiKey_KeypadEnter;
    case NPH_KEY_KP_EQUAL:
        return ImGuiKey_KeypadEqual;
    case NPH_KEY_LEFT_SHIFT:
        return ImGuiKey_LeftShift;
    case NPH_KEY_LEFT_CONTROL:
        return ImGuiKey_LeftCtrl;
    case NPH_KEY_LEFT_ALT:
        return ImGuiKey_LeftAlt;
    case NPH_KEY_LEFT_SUPER:
        return ImGuiKey_LeftSuper;
    case NPH_KEY_RIGHT_SHIFT:
        return ImGuiKey_RightShift;
    case NPH_KEY_RIGHT_CONTROL:
        return ImGuiKey_RightCtrl;
    case NPH_KEY_RIGHT_ALT:
        return ImGuiKey_RightAlt;
    case NPH_KEY_RIGHT_SUPER:
        return ImGuiKey_RightSuper;
    case NPH_KEY_MENU:
        return ImGuiKey_Menu;
    default:
        return ImGuiKey_None;
    }
}

// The UI_MOD_* bit of a modifier key, or 0.
static u32 mod_bit(NphKey key)
{
    switch (key) {
    case NPH_KEY_LEFT_CONTROL:
        return UI_MOD_LEFT_CTRL;
    case NPH_KEY_RIGHT_CONTROL:
        return UI_MOD_RIGHT_CTRL;
    case NPH_KEY_LEFT_SHIFT:
        return UI_MOD_LEFT_SHIFT;
    case NPH_KEY_RIGHT_SHIFT:
        return UI_MOD_RIGHT_SHIFT;
    case NPH_KEY_LEFT_ALT:
        return UI_MOD_LEFT_ALT;
    case NPH_KEY_RIGHT_ALT:
        return UI_MOD_RIGHT_ALT;
    case NPH_KEY_LEFT_SUPER:
        return UI_MOD_LEFT_SUPER;
    case NPH_KEY_RIGHT_SUPER:
        return UI_MOD_RIGHT_SUPER;
    default:
        return 0U;
    }
}

// Sends ImGui the modifiers whose state changed between `before` and `after`.
static void send_mods(ImGuiIO *io, u32 before, u32 after)
{
    static const struct {
        u32 bits;
        ImGuiKey mod;
    } pairs[4] = {
        {UI_MOD_LEFT_CTRL | UI_MOD_RIGHT_CTRL, ImGuiMod_Ctrl},
        {UI_MOD_LEFT_SHIFT | UI_MOD_RIGHT_SHIFT, ImGuiMod_Shift},
        {UI_MOD_LEFT_ALT | UI_MOD_RIGHT_ALT, ImGuiMod_Alt},
        {UI_MOD_LEFT_SUPER | UI_MOD_RIGHT_SUPER, ImGuiMod_Super},
    };
    for (u32 i = 0; i < 4U; ++i) {
        const bool was = (before & pairs[i].bits) != 0U;
        const bool is = (after & pairs[i].bits) != 0U;
        if (was != is) {
            ImGuiIO_AddKeyEvent(io, pairs[i].mod, is);
        }
    }
}

// ---------------------------------------------------------------------------
// The clipboard
// ---------------------------------------------------------------------------

static const char *get_clipboard(ImGuiContext *ctx)
{
    NPH_UNUSED(ctx);
    UiPlatform *p = (UiPlatform *)igGetPlatformIO_Nil()->Platform_ClipboardUserData;
    for (;;) {
        u64 len = 0U;
        const NphResult r = nph_window_get_clipboard(p->window, p->clip, p->clip_cap, &len);
        if (r == NPH_RESULT_OK) {
            return p->clip;
        }
        if (r != NPH_RESULT_OVERFLOW) {
            p->clip[0] = '\0';
            return p->clip;
        }
        // Grow to the text's size (len, when the window system reports it).
        const u64 cap = len + 1U > p->clip_cap * 2U ? len + 1U : p->clip_cap * 2U;
        char *bigger = NPH_ALLOC_ARRAY(p->alloc, char, cap);
        if (bigger == NULL) {
            p->clip[0] = '\0';
            return p->clip;
        }
        NPH_FREE_ARRAY(p->alloc, char, p->clip, p->clip_cap);
        p->clip = bigger;
        p->clip_cap = cap;
    }
}

static void set_clipboard(ImGuiContext *ctx, const char *text)
{
    NPH_UNUSED(ctx);
    UiPlatform *p = (UiPlatform *)igGetPlatformIO_Nil()->Platform_ClipboardUserData;
    nph_window_set_clipboard(p->window, text);
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------

NphResult ui_platform_init(UiPlatform *p, NphAllocator *alloc, NphWindow *window)
{
    NPH_ASSERT(p != NULL && alloc != NULL && igGetCurrentContext() != NULL);
    memset(p, 0, sizeof *p);
    p->alloc = alloc;
    p->window = window;
    p->focused = true;
    ImGuiIO *io = igGetIO_Nil();
    io->BackendPlatformName = "nepheline";
    if (window != NULL) {
        p->clip = NPH_ALLOC_ARRAY(alloc, char, UI_MIN_CLIPBOARD);
        if (p->clip == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        p->clip_cap = UI_MIN_CLIPBOARD;
        ImGuiPlatformIO *pio = igGetPlatformIO_Nil();
        pio->Platform_GetClipboardTextFn = get_clipboard;
        pio->Platform_SetClipboardTextFn = set_clipboard;
        pio->Platform_ClipboardUserData = p;
    }
    return NPH_RESULT_OK;
}

void ui_platform_shutdown(UiPlatform *p)
{
    if (p->clip != NULL) {
        NPH_FREE_ARRAY(p->alloc, char, p->clip, p->clip_cap);
    }
    if (igGetCurrentContext() != NULL) {
        ImGuiPlatformIO *pio = igGetPlatformIO_Nil();
        if (pio->Platform_ClipboardUserData == p) {
            pio->Platform_GetClipboardTextFn = NULL;
            pio->Platform_SetClipboardTextFn = NULL;
            pio->Platform_ClipboardUserData = NULL;
        }
        igGetIO_Nil()->BackendPlatformName = NULL;
    }
    memset(p, 0, sizeof *p);
}

bool ui_platform_event(UiPlatform *p, const NphWindowEvent *ev)
{
    ImGuiIO *io = igGetIO_Nil();
    switch ((NphWindowEventType)ev->type) {
    case NPH_WINDOW_EVENT_KEY: {
        const bool down = ev->u.key.action != (u32)NPH_KEY_ACTION_RELEASE;
        const u32 bit = mod_bit((NphKey)ev->u.key.key);
        if (bit != 0U) {
            const u32 before = p->mods;
            p->mods = down ? p->mods | bit : p->mods & ~bit;
            send_mods(io, before, p->mods);
        }
        const ImGuiKey key = ui_platform_key((NphKey)ev->u.key.key);
        if (key != ImGuiKey_None) {
            ImGuiIO_AddKeyEvent(io, key, down);
        }
        return io->WantCaptureKeyboard;
    }
    case NPH_WINDOW_EVENT_CHAR:
        ImGuiIO_AddInputCharacter(io, ev->u.ch.codepoint);
        return io->WantCaptureKeyboard;
    case NPH_WINDOW_EVENT_MOUSE_BUTTON:
        if (ev->u.mouse_button.button < (u32)ImGuiMouseButton_COUNT) {
            ImGuiIO_AddMouseButtonEvent(io, (int)ev->u.mouse_button.button,
                                        ev->u.mouse_button.action == (u32)NPH_KEY_ACTION_PRESS);
        }
        return io->WantCaptureMouse;
    case NPH_WINDOW_EVENT_MOUSE_MOVE:
        ImGuiIO_AddMousePosEvent(io, (f32)ev->u.mouse_move.x, (f32)ev->u.mouse_move.y);
        return io->WantCaptureMouse;
    case NPH_WINDOW_EVENT_MOUSE_SCROLL:
        ImGuiIO_AddMouseWheelEvent(io, (f32)ev->u.scroll.dx, (f32)ev->u.scroll.dy);
        return io->WantCaptureMouse;
    case NPH_WINDOW_EVENT_MOUSE_ENTER:
        if (!ev->u.mouse_enter.entered) {
            ImGuiIO_AddMousePosEvent(io, -FLT_MAX, -FLT_MAX);
        }
        return false;
    case NPH_WINDOW_EVENT_FOCUS:
        p->focused = ev->u.focus.focused != 0;
        ImGuiIO_AddFocusEvent(io, p->focused);
        if (!p->focused) {
            p->mods = 0U; // ImGui releases every key; the releases may never come
        }
        return false;
    default:
        return false;
    }
}

void ui_platform_set_display_size(UiPlatform *p, u32 width, u32 height)
{
    p->width = width;
    p->height = height;
}

void ui_platform_new_frame(UiPlatform *p, f64 dt_seconds)
{
    ImGuiIO *io = igGetIO_Nil();
    u32 w = p->width;
    u32 h = p->height;
    f32 scale_x = 1.0F;
    f32 scale_y = 1.0F;
    if (p->window != NULL) {
        u32 fb_w = 0U;
        u32 fb_h = 0U;
        nph_window_get_size(p->window, &w, &h);
        nph_window_get_framebuffer_size(p->window, &fb_w, &fb_h);
        if (w > 0U && h > 0U) { // zero while minimised
            scale_x = (f32)fb_w / (f32)w;
            scale_y = (f32)fb_h / (f32)h;
        }
    }
    io->DisplaySize = ui_vec2((f32)w, (f32)h);
    io->DisplayFramebufferScale = ui_vec2(scale_x, scale_y);
    io->DeltaTime = dt_seconds > 1e-5 ? (f32)dt_seconds : 1e-5F;
}

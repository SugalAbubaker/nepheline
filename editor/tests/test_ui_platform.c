// SPDX-License-Identifier: Apache-2.0
// Dear ImGui's platform backend (editor/src/ui_platform.h): the key map,
// and window events becoming ImGui input, without a window or a GPU.
#include <nepheline/platform/time.h>

#include <ntest/ntest.h>

#include "ui_platform.h"

#include <string.h>

NTEST(UiPlatform, every_key_imgui_names_maps_to_its_own_key)
{
    u8 seen[ImGuiKey_NamedKey_END];
    memset(seen, 0, sizeof seen);
    for (u32 k = (u32)NPH_KEY_UNKNOWN + 1U; k < (u32)NPH_KEY_COUNT; ++k) {
        const ImGuiKey key = ui_platform_key((NphKey)k);
        if (k == (u32)NPH_KEY_F25) {
            NTEST_EXPECT_EQ_INT(key, ImGuiKey_None); // ImGui stops at F24
            continue;
        }
        NTEST_ASSERT_GE_INT(key, ImGuiKey_NamedKey_BEGIN);
        NTEST_ASSERT_LT_INT(key, ImGuiKey_NamedKey_END);
        if (k == (u32)NPH_KEY_WORLD_2) {
            NTEST_EXPECT_EQ_INT(key, ui_platform_key(NPH_KEY_WORLD_1)); // both are Oem102
            continue;
        }
        if (seen[key] != 0U) {
            NTEST_ADD_FAILURE("%s maps to an ImGui key another key has", nph_key_name((NphKey)k));
        }
        seen[key] = 1U;
    }
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_UNKNOWN), ImGuiKey_None);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_A), ImGuiKey_A);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_Z), ImGuiKey_Z);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_0), ImGuiKey_0);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_F12), ImGuiKey_F12);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_KP_9), ImGuiKey_Keypad9);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_LEFT_CONTROL), ImGuiKey_LeftCtrl);
    NTEST_EXPECT_EQ_INT(ui_platform_key(NPH_KEY_ESCAPE), ImGuiKey_Escape);
}

// An ImGui context with the backend, offscreen, running frames on demand.
typedef struct PlatformFixture {
    ImGuiContext *ctx;
    UiPlatform platform;
} PlatformFixture;

NTEST_SETUP(PlatformFixture)
{
    f->ctx = igCreateContext(NULL);
    NTEST_ASSERT_NOT_NULL(f->ctx);
    igSetCurrentContext(f->ctx);
    // No renderer here: the font atlas stays a pending texture request.
    igGetIO_Nil()->BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    igGetIO_Nil()->IniFilename = NULL;
    NTEST_ASSERT_EQ_INT(ui_platform_init(&f->platform, nph_allocator_system(), NULL),
                        NPH_RESULT_OK);
    ui_platform_set_display_size(&f->platform, 800U, 600U);
}

NTEST_TEARDOWN(PlatformFixture)
{
    ui_platform_shutdown(&f->platform);
    igDestroyContext(f->ctx);
}

static NphWindowEvent key_event(NphKey key, NphKeyAction action)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_KEY;
    ev.u.key.key = (u32)key;
    ev.u.key.action = (u32)action;
    return ev;
}

static NphWindowEvent mouse_move(f64 x, f64 y)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_MOUSE_MOVE;
    ev.u.mouse_move.x = x;
    ev.u.mouse_move.y = y;
    return ev;
}

static NphWindowEvent mouse_button(NphMouseButton button, NphKeyAction action)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_MOUSE_BUTTON;
    ev.u.mouse_button.button = (u32)button;
    ev.u.mouse_button.action = (u32)action;
    return ev;
}

// A frame's start: NewFrame applies the events fed before it. ImGui spreads
// queued events that interleave over several frames
// (io.ConfigInputTrickleEventQueue), and EndFrame clears the per-frame
// input (the wheel, the text), so the tests feed one kind of event per
// frame and look at per-frame input before the frame ends.
static void begin_frame(PlatformFixture *f)
{
    ui_platform_new_frame(&f->platform, 1.0 / 60.0);
    igNewFrame();
}

static void frame(PlatformFixture *f, void (*ui)(void))
{
    begin_frame(f);
    if (ui != NULL) {
        ui();
    }
    igEndFrame();
}

NTEST_F(PlatformFixture, a_frame_takes_the_display_size_and_the_time_step)
{
    frame(f, NULL);
    const ImGuiIO *io = igGetIO_Nil();
    NTEST_EXPECT_NEAR_F32(io->DisplaySize.x, 800.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(io->DisplaySize.y, 600.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(io->DisplayFramebufferScale.x, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(io->DeltaTime, 1.0F / 60.0F, 1e-7F);
    // A zero step would trip ImGui: it is clamped.
    ui_platform_new_frame(&f->platform, 0.0);
    NTEST_EXPECT_TRUE(io->DeltaTime > 0.0F);
}

NTEST_F(PlatformFixture, keys_text_and_modifiers_reach_imgui)
{
    NphWindowEvent ev = key_event(NPH_KEY_A, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    ev = key_event(NPH_KEY_LEFT_CONTROL, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, NULL);
    const ImGuiIO *io = igGetIO_Nil();
    NTEST_EXPECT_TRUE(igIsKeyDown_Nil(ImGuiKey_A));
    NTEST_EXPECT_TRUE(igIsKeyDown_Nil(ImGuiKey_LeftCtrl));
    NTEST_EXPECT_TRUE(io->KeyCtrl);
    NphWindowEvent ch;
    memset(&ch, 0, sizeof ch);
    ch.type = (u32)NPH_WINDOW_EVENT_CHAR;
    ch.u.ch.codepoint = 0x00E9U; // é
    (void)ui_platform_event(&f->platform, &ch);
    begin_frame(f);
    const int chars = io->InputQueueCharacters.Size;
    const u32 first = chars > 0 ? (u32)io->InputQueueCharacters.Data[0] : 0U;
    igEndFrame();
    NTEST_ASSERT_EQ_INT(chars, 1);
    NTEST_EXPECT_EQ_UINT(first, 0x00E9U);

    // The right control key held keeps Ctrl while the left one goes up.
    ev = key_event(NPH_KEY_RIGHT_CONTROL, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    ev = key_event(NPH_KEY_LEFT_CONTROL, NPH_KEY_ACTION_RELEASE);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, NULL);
    NTEST_EXPECT_TRUE(io->KeyCtrl);
    ev = key_event(NPH_KEY_RIGHT_CONTROL, NPH_KEY_ACTION_RELEASE);
    (void)ui_platform_event(&f->platform, &ev);
    ev = key_event(NPH_KEY_A, NPH_KEY_ACTION_RELEASE);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, NULL);
    NTEST_EXPECT_FALSE(io->KeyCtrl);
    NTEST_EXPECT_FALSE(igIsKeyDown_Nil(ImGuiKey_A));
}

NTEST_F(PlatformFixture, losing_the_focus_releases_everything)
{
    NphWindowEvent ev = key_event(NPH_KEY_LEFT_SHIFT, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    ev = key_event(NPH_KEY_W, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, NULL);
    NTEST_EXPECT_TRUE(igGetIO_Nil()->KeyShift);
    NphWindowEvent focus;
    memset(&focus, 0, sizeof focus);
    focus.type = (u32)NPH_WINDOW_EVENT_FOCUS;
    focus.u.focus.focused = 0;
    NTEST_EXPECT_FALSE(ui_platform_event(&f->platform, &focus));
    frame(f, NULL);
    NTEST_EXPECT_FALSE(igGetIO_Nil()->KeyShift);
    NTEST_EXPECT_FALSE(igIsKeyDown_Nil(ImGuiKey_W));
    // The modifiers start over: a press after the focus comes back counts.
    focus.u.focus.focused = 1;
    (void)ui_platform_event(&f->platform, &focus);
    ev = key_event(NPH_KEY_LEFT_SHIFT, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, NULL);
    NTEST_EXPECT_TRUE(igGetIO_Nil()->KeyShift);
}

static void a_window(void)
{
    igSetNextWindowPos(ui_vec2(10.0F, 10.0F), ImGuiCond_Always, ui_vec2(0.0F, 0.0F));
    igSetNextWindowSize(ui_vec2(200.0F, 150.0F), ImGuiCond_Always);
    (void)igBegin("a window", NULL, ImGuiWindowFlags_None);
    igEnd();
}

NTEST_F(PlatformFixture, the_mouse_reaches_imgui_and_says_when_imgui_wants_it)
{
    frame(f, a_window); // the window first, so the press below lands on it
    NphWindowEvent ev = mouse_move(50.0, 60.0);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, a_window);
    const ImGuiIO *io = igGetIO_Nil();
    NTEST_EXPECT_NEAR_F32(io->MousePos.x, 50.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(io->MousePos.y, 60.0F, 0.0F);
    ev = mouse_button(NPH_MOUSE_BUTTON_RIGHT, NPH_KEY_ACTION_PRESS);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, a_window);
    NTEST_EXPECT_TRUE(igIsMouseDown_Nil(ImGuiMouseButton_Right));
    NphWindowEvent wheel;
    memset(&wheel, 0, sizeof wheel);
    wheel.type = (u32)NPH_WINDOW_EVENT_MOUSE_SCROLL;
    wheel.u.scroll.dy = 2.0;
    (void)ui_platform_event(&f->platform, &wheel);
    begin_frame(f);
    const f32 wheel_y = io->MouseWheel;
    a_window();
    igEndFrame();
    NTEST_EXPECT_NEAR_F32(wheel_y, 2.0F, 0.0F);
    // Over a window, ImGui wants the mouse; the next events say so.
    ev = mouse_move(55.0, 60.0);
    NTEST_EXPECT_TRUE(ui_platform_event(&f->platform, &ev));
    frame(f, a_window);
    // Away from every window, it does not.
    ev = mouse_button(NPH_MOUSE_BUTTON_RIGHT, NPH_KEY_ACTION_RELEASE);
    (void)ui_platform_event(&f->platform, &ev);
    ev = mouse_move(600.0, 500.0);
    (void)ui_platform_event(&f->platform, &ev);
    frame(f, a_window);
    frame(f, a_window);
    ev = mouse_move(601.0, 500.0);
    NTEST_EXPECT_FALSE(ui_platform_event(&f->platform, &ev));
    // Leaving the window puts the mouse nowhere.
    NphWindowEvent leave;
    memset(&leave, 0, sizeof leave);
    leave.type = (u32)NPH_WINDOW_EVENT_MOUSE_ENTER;
    leave.u.mouse_enter.entered = 0;
    (void)ui_platform_event(&f->platform, &leave);
    frame(f, a_window);
    NTEST_EXPECT_FALSE(igIsMousePosValid(NULL));
}

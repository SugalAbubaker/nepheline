// SPDX-License-Identifier: Apache-2.0
#pragma once

// Dear ImGui's platform backend on the engine's window (docs/editor.md
// "UI"; docs/adr/0025-editor-ui.md). Window events (platform/window.h)
// become ImGui input, the clipboard is the window system's, and every
// frame sets the display size, the framebuffer scale, and the time step.
// Without a window (an offscreen editor, in tests) the display size is set
// by the caller, and events still arrive through ui_platform_event.
//
// Modifier keys are tracked from the key events themselves (ImGui wants
// their state as separate events), and forgotten when the window loses
// focus, as ImGui forgets every key then.
//
// Costs: reading the clipboard grows a buffer to the text's size; nothing
// else allocates.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY, with the ImGui context current.

#include <nepheline/platform/window.h>

#include "imgui_c.h"

typedef struct UiPlatform {
    NphAllocator *alloc;
    NphWindow *window; // NULL offscreen
    u32 width;         // offscreen display size
    u32 height;
    u32 mods;     // the modifier keys held, from key events: UI_MOD_* bits
    char *clip;   // the clipboard text last read, for ImGui
    u64 clip_cap; // bytes of `clip`
    bool focused; // the window has the keyboard focus
} UiPlatform;

// The ImGui key of an engine key (the world keys are ImGui's Oem102);
// ImGuiKey_None for keys ImGui does not name (F25).
ImGuiKey ui_platform_key(NphKey key);

// Installs the backend in the current ImGui context. `window` may be NULL
// (offscreen: set the display size with ui_platform_set_display_size).
NPH_MUST_USE NphResult ui_platform_init(UiPlatform *p, NphAllocator *alloc, NphWindow *window);
void ui_platform_shutdown(UiPlatform *p);

// Feeds one event to ImGui. Returns true when ImGui wants it, as of the
// last frame: keys and text while ImGui has the keyboard
// (WantCaptureKeyboard), mouse events while the mouse is over its windows
// (WantCaptureMouse). The caller decides whether the game sees it too.
bool ui_platform_event(UiPlatform *p, const NphWindowEvent *ev);

// The display size without a window, in pixels (the framebuffer scale is 1).
void ui_platform_set_display_size(UiPlatform *p, u32 width, u32 height);

// Before igNewFrame: the display size, the framebuffer scale, and the time
// since the previous frame (clamped to a positive minimum).
void ui_platform_new_frame(UiPlatform *p, f64 dt_seconds);

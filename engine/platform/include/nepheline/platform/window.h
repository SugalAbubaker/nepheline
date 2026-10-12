// SPDX-License-Identifier: Apache-2.0
#pragma once

// Windows, monitors, raw input events, and gamepads (CLAUDE.md §8.3
// "Window/Input: a GLFW wrapper exposing nph_window_* and an event queue";
// docs/adr/0012-window-and-input.md). GLFW is the only OS window library
// the engine uses and this header is its only interface: no engine code
// includes a GLFW header (CLAUDE.md §1.8).
//
// The window system is initialised once per process on the main thread,
// which is also the only thread that creates windows and pumps events
// (an OS rule, not ours). nph_window_poll_events runs the OS message loop
// and appends to each window's event queue; the owner drains the queue
// with nph_window_pop_event, typically feeding input.h. Events are small
// values with no pointers, so a queue overflow (a very slow frame) drops
// the oldest events and counts them rather than failing.
//
// Gamepads are polled, not event driven: nph_window_get_gamepad_state
// returns a snapshot in the standard layout (Xbox-style names), which
// input.h samples once per frame.
//
// Vulkan: the window reports the instance extensions it needs and creates
// a VkSurfaceKHR for an instance; Phase 5 gives it the loader's
// vkGetInstanceProcAddr first. Nothing here depends on Vulkan headers.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY throughout, except the pure helpers
// (key names) and the gamepad query, which GLFW allows from any thread
// after init.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Keys, buttons, modifiers
// ---------------------------------------------------------------------------

// Physical-ish key identities in the US layout, independent of the OS
// (nph_window_* maps the native codes). Text input arrives separately as
// NPH_WINDOW_EVENT_CHAR, already translated by the keyboard layout.
typedef enum NphKey {
    NPH_KEY_UNKNOWN = 0,
    NPH_KEY_SPACE,
    NPH_KEY_APOSTROPHE,
    NPH_KEY_COMMA,
    NPH_KEY_MINUS,
    NPH_KEY_PERIOD,
    NPH_KEY_SLASH,
    NPH_KEY_0,
    NPH_KEY_1,
    NPH_KEY_2,
    NPH_KEY_3,
    NPH_KEY_4,
    NPH_KEY_5,
    NPH_KEY_6,
    NPH_KEY_7,
    NPH_KEY_8,
    NPH_KEY_9,
    NPH_KEY_SEMICOLON,
    NPH_KEY_EQUAL,
    NPH_KEY_A,
    NPH_KEY_B,
    NPH_KEY_C,
    NPH_KEY_D,
    NPH_KEY_E,
    NPH_KEY_F,
    NPH_KEY_G,
    NPH_KEY_H,
    NPH_KEY_I,
    NPH_KEY_J,
    NPH_KEY_K,
    NPH_KEY_L,
    NPH_KEY_M,
    NPH_KEY_N,
    NPH_KEY_O,
    NPH_KEY_P,
    NPH_KEY_Q,
    NPH_KEY_R,
    NPH_KEY_S,
    NPH_KEY_T,
    NPH_KEY_U,
    NPH_KEY_V,
    NPH_KEY_W,
    NPH_KEY_X,
    NPH_KEY_Y,
    NPH_KEY_Z,
    NPH_KEY_LEFT_BRACKET,
    NPH_KEY_BACKSLASH,
    NPH_KEY_RIGHT_BRACKET,
    NPH_KEY_GRAVE_ACCENT,
    NPH_KEY_WORLD_1,
    NPH_KEY_WORLD_2,
    NPH_KEY_ESCAPE,
    NPH_KEY_ENTER,
    NPH_KEY_TAB,
    NPH_KEY_BACKSPACE,
    NPH_KEY_INSERT,
    NPH_KEY_DELETE,
    NPH_KEY_RIGHT,
    NPH_KEY_LEFT,
    NPH_KEY_DOWN,
    NPH_KEY_UP,
    NPH_KEY_PAGE_UP,
    NPH_KEY_PAGE_DOWN,
    NPH_KEY_HOME,
    NPH_KEY_END,
    NPH_KEY_CAPS_LOCK,
    NPH_KEY_SCROLL_LOCK,
    NPH_KEY_NUM_LOCK,
    NPH_KEY_PRINT_SCREEN,
    NPH_KEY_PAUSE,
    NPH_KEY_F1,
    NPH_KEY_F2,
    NPH_KEY_F3,
    NPH_KEY_F4,
    NPH_KEY_F5,
    NPH_KEY_F6,
    NPH_KEY_F7,
    NPH_KEY_F8,
    NPH_KEY_F9,
    NPH_KEY_F10,
    NPH_KEY_F11,
    NPH_KEY_F12,
    NPH_KEY_F13,
    NPH_KEY_F14,
    NPH_KEY_F15,
    NPH_KEY_F16,
    NPH_KEY_F17,
    NPH_KEY_F18,
    NPH_KEY_F19,
    NPH_KEY_F20,
    NPH_KEY_F21,
    NPH_KEY_F22,
    NPH_KEY_F23,
    NPH_KEY_F24,
    NPH_KEY_F25,
    NPH_KEY_KP_0,
    NPH_KEY_KP_1,
    NPH_KEY_KP_2,
    NPH_KEY_KP_3,
    NPH_KEY_KP_4,
    NPH_KEY_KP_5,
    NPH_KEY_KP_6,
    NPH_KEY_KP_7,
    NPH_KEY_KP_8,
    NPH_KEY_KP_9,
    NPH_KEY_KP_DECIMAL,
    NPH_KEY_KP_DIVIDE,
    NPH_KEY_KP_MULTIPLY,
    NPH_KEY_KP_SUBTRACT,
    NPH_KEY_KP_ADD,
    NPH_KEY_KP_ENTER,
    NPH_KEY_KP_EQUAL,
    NPH_KEY_LEFT_SHIFT,
    NPH_KEY_LEFT_CONTROL,
    NPH_KEY_LEFT_ALT,
    NPH_KEY_LEFT_SUPER,
    NPH_KEY_RIGHT_SHIFT,
    NPH_KEY_RIGHT_CONTROL,
    NPH_KEY_RIGHT_ALT,
    NPH_KEY_RIGHT_SUPER,
    NPH_KEY_MENU,
    NPH_KEY_COUNT
} NphKey;

typedef enum NphMouseButton {
    NPH_MOUSE_BUTTON_LEFT = 0,
    NPH_MOUSE_BUTTON_RIGHT,
    NPH_MOUSE_BUTTON_MIDDLE,
    NPH_MOUSE_BUTTON_4,
    NPH_MOUSE_BUTTON_5,
    NPH_MOUSE_BUTTON_6,
    NPH_MOUSE_BUTTON_7,
    NPH_MOUSE_BUTTON_8,
    NPH_MOUSE_BUTTON_COUNT
} NphMouseButton;

// Modifier bits carried by key and mouse-button events.
typedef enum NphMod {
    NPH_MOD_SHIFT = 1U << 0U,
    NPH_MOD_CONTROL = 1U << 1U,
    NPH_MOD_ALT = 1U << 2U,
    NPH_MOD_SUPER = 1U << 3U,
    NPH_MOD_CAPS_LOCK = 1U << 4U,
    NPH_MOD_NUM_LOCK = 1U << 5U,
} NphMod;

typedef enum NphKeyAction {
    NPH_KEY_ACTION_RELEASE = 0,
    NPH_KEY_ACTION_PRESS = 1,
    NPH_KEY_ACTION_REPEAT = 2, // OS key repeat; the key is still down
} NphKeyAction;

// Static names: "a", "left shift", "f11", "mouse left"; "?" for bad values.
const char *nph_key_name(NphKey key);
const char *nph_mouse_button_name(NphMouseButton button);

// ---------------------------------------------------------------------------
// Gamepads (the SDL/GLFW standard layout)
// ---------------------------------------------------------------------------

#define NPH_GAMEPAD_MAX 8U

typedef enum NphGamepadButton {
    NPH_GAMEPAD_BUTTON_A = 0,
    NPH_GAMEPAD_BUTTON_B,
    NPH_GAMEPAD_BUTTON_X,
    NPH_GAMEPAD_BUTTON_Y,
    NPH_GAMEPAD_BUTTON_LEFT_BUMPER,
    NPH_GAMEPAD_BUTTON_RIGHT_BUMPER,
    NPH_GAMEPAD_BUTTON_BACK,
    NPH_GAMEPAD_BUTTON_START,
    NPH_GAMEPAD_BUTTON_GUIDE,
    NPH_GAMEPAD_BUTTON_LEFT_THUMB,
    NPH_GAMEPAD_BUTTON_RIGHT_THUMB,
    NPH_GAMEPAD_BUTTON_DPAD_UP,
    NPH_GAMEPAD_BUTTON_DPAD_RIGHT,
    NPH_GAMEPAD_BUTTON_DPAD_DOWN,
    NPH_GAMEPAD_BUTTON_DPAD_LEFT,
    NPH_GAMEPAD_BUTTON_COUNT
} NphGamepadButton;

typedef enum NphGamepadAxis {
    NPH_GAMEPAD_AXIS_LEFT_X = 0, // -1 left .. +1 right
    NPH_GAMEPAD_AXIS_LEFT_Y,     // -1 up .. +1 down
    NPH_GAMEPAD_AXIS_RIGHT_X,
    NPH_GAMEPAD_AXIS_RIGHT_Y,
    NPH_GAMEPAD_AXIS_LEFT_TRIGGER,  // -1 released .. +1 pressed (GLFW convention)
    NPH_GAMEPAD_AXIS_RIGHT_TRIGGER, // -1 released .. +1 pressed
    NPH_GAMEPAD_AXIS_COUNT
} NphGamepadAxis;

typedef struct NphGamepadState {
    f32 axes[NPH_GAMEPAD_AXIS_COUNT];
    u8 buttons[NPH_GAMEPAD_BUTTON_COUNT]; // 1 pressed, 0 released
    b32 connected;
} NphGamepadState;

const char *nph_gamepad_button_name(NphGamepadButton button);
const char *nph_gamepad_axis_name(NphGamepadAxis axis);

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

typedef enum NphWindowEventType {
    NPH_WINDOW_EVENT_NONE = 0,
    NPH_WINDOW_EVENT_KEY,                // key
    NPH_WINDOW_EVENT_CHAR,               // ch: a Unicode code point of typed text
    NPH_WINDOW_EVENT_MOUSE_BUTTON,       // mouse_button
    NPH_WINDOW_EVENT_MOUSE_MOVE,         // mouse_move: cursor position in window coordinates
    NPH_WINDOW_EVENT_MOUSE_SCROLL,       // scroll: wheel steps (dy) or trackpad offsets
    NPH_WINDOW_EVENT_MOUSE_ENTER,        // mouse_enter: cursor entered (1) or left (0)
    NPH_WINDOW_EVENT_RESIZE,             // size: the window's client area, screen units
    NPH_WINDOW_EVENT_FRAMEBUFFER_RESIZE, // size: the framebuffer, pixels (what the swapchain uses)
    NPH_WINDOW_EVENT_CONTENT_SCALE,      // scale: DPI scale changed
    NPH_WINDOW_EVENT_MOVE,               // position: window moved
    NPH_WINDOW_EVENT_FOCUS,              // focus: gained (1) or lost (0)
    NPH_WINDOW_EVENT_ICONIFY,            // iconify: minimised (1) or restored (0)
    NPH_WINDOW_EVENT_MAXIMIZE,           // maximize: maximised (1) or restored (0)
    NPH_WINDOW_EVENT_CLOSE,              // the user asked to close the window
    NPH_WINDOW_EVENT_COUNT
} NphWindowEventType;

typedef struct NphWindowEvent {
    u32 type; // NphWindowEventType
    u32 pad;
    u64 time_ns; // nph_time_now_ns when the event was queued
    union {
        struct {
            u32 key;      // NphKey
            u32 scancode; // platform scancode, for keys without an NphKey
            u32 action;   // NphKeyAction
            u32 mods;     // NphMod bits
        } key;
        struct {
            u32 codepoint;
        } ch;
        struct {
            u32 button; // NphMouseButton
            u32 action; // NPH_KEY_ACTION_PRESS or RELEASE
            u32 mods;
        } mouse_button;
        struct {
            f64 x;
            f64 y;
        } mouse_move;
        struct {
            f64 dx;
            f64 dy;
        } scroll;
        struct {
            b32 entered;
        } mouse_enter;
        struct {
            u32 width;
            u32 height;
        } size;
        struct {
            f32 x;
            f32 y;
        } scale;
        struct {
            i32 x;
            i32 y;
        } position;
        struct {
            b32 focused;
        } focus;
        struct {
            b32 iconified;
        } iconify;
        struct {
            b32 maximized;
        } maximize;
    } u;
} NphWindowEvent;

NPH_STATIC_ASSERT(sizeof(NphWindowEvent) == 32U, "events are copied by value; keep them small");

// Static name of an event type ("key", "mouse move", ...).
const char *nph_window_event_name(NphWindowEventType type);

// ---------------------------------------------------------------------------
// The window system
// ---------------------------------------------------------------------------

// Initialises the OS window system. NPH_RESULT_UNSUPPORTED when no display
// is available (a headless machine), with the reason logged; everything
// else in this header then returns a failure or does nothing.
// NPH_MAIN_THREAD_ONLY.
NPH_MUST_USE NphResult nph_window_system_init(void);

// Destroys any remaining windows and shuts the window system down. Safe to
// call without init.
void nph_window_system_shutdown(void);

bool nph_window_system_is_initialized(void);

// "win32", "x11", "wayland", "cocoa", or "none" before init.
const char *nph_window_system_backend(void);

// Runs the OS event loop once, queuing events on every window. Returns at
// once when nothing is pending.
void nph_window_poll_events(void);

// As nph_window_poll_events, but blocks until an event arrives or
// `timeout_ns` elapses (0: no limit). For an idle editor.
void nph_window_wait_events(u64 timeout_ns);

// ---------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------

typedef struct NphMonitorInfo {
    char name[64];
    i32 x; // position of the monitor's origin in the virtual screen
    i32 y;
    u32 width; // current video mode, pixels
    u32 height;
    u32 refresh_rate_hz;
    u32 physical_width_mm;
    u32 physical_height_mm;
    f32 scale_x; // content scale (DPI / 96)
    f32 scale_y;
    b32 primary;
} NphMonitorInfo;

// Number of connected monitors (0 before init).
u32 nph_monitor_count(void);

// NPH_RESULT_NOT_FOUND for an index past the count.
NphResult nph_monitor_get_info(u32 index, NphMonitorInfo *out);

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

typedef struct NphWindow NphWindow;

typedef struct NphWindowDesc {
    const char *title; // UTF-8; NULL: "Nepheline"
    u32 width;         // client area in screen units; 0: 1280 x 720
    u32 height;
    b32 resizable;  // default true (a zero-filled desc means "not resizable")
    b32 decorated;  // title bar and border
    b32 visible;    // shown on creation
    b32 maximized;  // start maximised
    b32 fullscreen; // exclusive fullscreen on `monitor`, at its current mode
    u32 monitor;    // monitor index for fullscreen
} NphWindowDesc;

// Fills a description with the defaults above (resizable, decorated,
// visible, windowed, 1280x720).
void nph_window_desc_default(NphWindowDesc *desc);

// Creates a window; its memory comes from `alloc`. NPH_RESULT_UNSUPPORTED
// when the window system is not initialised or the OS refuses (logged).
NPH_MUST_USE NphResult nph_window_create(NphAllocator *alloc, const NphWindowDesc *desc,
                                         NphWindow **out);

void nph_window_destroy(NphWindow *w);

// Takes the oldest queued event. false when the queue is empty.
bool nph_window_pop_event(NphWindow *w, NphWindowEvent *out);

// Number of events dropped because the queue was full since the previous
// call (which resets the count).
u32 nph_window_dropped_events(NphWindow *w);

// The close flag, set by the OS close button (with a CLOSE event) or by
// the application.
bool nph_window_should_close(const NphWindow *w);
void nph_window_set_should_close(NphWindow *w, bool close);

// Sizes: the client area in screen units, the framebuffer in pixels (they
// differ on high-DPI screens), and the content scale between them.
void nph_window_get_size(const NphWindow *w, u32 *out_width, u32 *out_height);
void nph_window_get_framebuffer_size(const NphWindow *w, u32 *out_width, u32 *out_height);
void nph_window_get_content_scale(const NphWindow *w, f32 *out_x, f32 *out_y);
void nph_window_get_position(const NphWindow *w, i32 *out_x, i32 *out_y);

void nph_window_set_size(NphWindow *w, u32 width, u32 height);
void nph_window_set_position(NphWindow *w, i32 x, i32 y);
void nph_window_set_title(NphWindow *w, const char *title);

// Switches between exclusive fullscreen on `monitor` (at its current video
// mode) and the previous windowed placement. NPH_RESULT_NOT_FOUND for a bad
// monitor index.
NphResult nph_window_set_fullscreen(NphWindow *w, bool fullscreen, u32 monitor);
bool nph_window_is_fullscreen(const NphWindow *w);

void nph_window_show(NphWindow *w);
void nph_window_hide(NphWindow *w);
void nph_window_focus(NphWindow *w);
void nph_window_minimize(NphWindow *w);
void nph_window_maximize(NphWindow *w);
void nph_window_restore(NphWindow *w);
bool nph_window_is_visible(const NphWindow *w);
bool nph_window_is_focused(const NphWindow *w);
bool nph_window_is_minimized(const NphWindow *w);
bool nph_window_is_maximized(const NphWindow *w);

typedef enum NphCursorMode {
    NPH_CURSOR_MODE_NORMAL,   // visible, free
    NPH_CURSOR_MODE_HIDDEN,   // hidden while over the window, free
    NPH_CURSOR_MODE_DISABLED, // hidden and captured: unlimited motion for mouse look
} NphCursorMode;

void nph_window_set_cursor_mode(NphWindow *w, NphCursorMode mode);
NphCursorMode nph_window_get_cursor_mode(const NphWindow *w);

// Raw (unaccelerated) mouse motion while the cursor is disabled, where the
// OS supports it.
bool nph_window_raw_mouse_motion_supported(void);
void nph_window_set_raw_mouse_motion(NphWindow *w, bool enabled);

void nph_window_get_cursor_position(const NphWindow *w, f64 *out_x, f64 *out_y);
void nph_window_set_cursor_position(NphWindow *w, f64 x, f64 y);

// The system clipboard as UTF-8. NPH_RESULT_NOT_FOUND when it holds no
// text (or the OS refuses), NPH_RESULT_OVERFLOW when `cap` is too small.
NphResult nph_window_get_clipboard(NphWindow *w, char *buf, u64 cap, u64 *out_len);
void nph_window_set_clipboard(NphWindow *w, const char *text);

// Gamepad `index` (0 .. NPH_GAMEPAD_MAX - 1) as a snapshot; false and a
// zero-filled state when nothing is connected there or the device has no
// standard mapping.
bool nph_window_get_gamepad_state(u32 index, NphGamepadState *out);

// The device name of a connected gamepad, or NULL.
const char *nph_window_gamepad_name(u32 index);

// ---------------------------------------------------------------------------
// Vulkan (used by Phase 5; no Vulkan types here)
// ---------------------------------------------------------------------------

typedef void (*NphVulkanProcFn)(void);
typedef NphVulkanProcFn (*NphVulkanGetInstanceProcAddrFn)(void *vk_instance, const char *name);

// Hands the window system the loader's vkGetInstanceProcAddr (volk's)
// before any of the functions below are used. Without it the window
// system looks for the Vulkan loader library itself.
void nph_window_vulkan_set_loader(NphVulkanGetInstanceProcAddrFn get_instance_proc_addr);

// The instance extensions required to create surfaces on this platform.
// The array is owned by the window system and valid until shutdown.
// NPH_RESULT_UNSUPPORTED when no Vulkan loader is available.
NphResult nph_window_vulkan_extensions(const char *const **out_names, u32 *out_count);

// Creates a VkSurfaceKHR (returned as its 64-bit handle) for `vk_instance`
// (a VkInstance). `vk_allocator` is a const VkAllocationCallbacks * or NULL.
// NPH_RESULT_UNSUPPORTED when Vulkan is unavailable, NPH_RESULT_UNKNOWN
// with the VkResult logged otherwise.
NphResult nph_window_vulkan_create_surface(NphWindow *w, void *vk_instance,
                                           const void *vk_allocator, u64 *out_vk_surface);

#ifdef __cplusplus
}
#endif

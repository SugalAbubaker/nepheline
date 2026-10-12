// SPDX-License-Identifier: Apache-2.0
// The GLFW wrapper: the one translation unit in the engine that includes a
// GLFW header (CLAUDE.md §1.8; docs/adr/0012-window-and-input.md).
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>

#include "platform_internal.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <string.h>

// glfw3.h declares its Vulkan entry points only when the Vulkan headers
// precede it. The engine takes no Vulkan header dependency before Phase 5,
// so the two functions used here are declared with ABI-identical types
// (handles are pointer sized; VkResult is an int-sized enum). Phase 5 can
// replace this with GLFW_INCLUDE_VULKAN once the SDK is a build dependency.
typedef struct NphVkInstanceT *NphVkInstanceHandle;
typedef struct NphVkAllocationCallbacksT NphVkAllocationCallbacks;
typedef void *NphVkSurfaceHandle;
void glfwInitVulkanLoader(NphVulkanGetInstanceProcAddrFn loader);
int glfwCreateWindowSurface(NphVkInstanceHandle instance, GLFWwindow *window,
                            const NphVkAllocationCallbacks *allocator, NphVkSurfaceHandle *surface);

#define EVENT_QUEUE_CAPACITY 1024U // power of two
#define DEFAULT_WIDTH        1280U
#define DEFAULT_HEIGHT       720U

struct NphWindow {
    GLFWwindow *handle;
    NphAllocator *alloc;
    u32 head; // events[head & mask] is the oldest; tail - head is the count
    u32 tail;
    u32 dropped;
    b32 fullscreen;
    i32 windowed_x; // placement to restore when leaving fullscreen
    i32 windowed_y;
    u32 windowed_width;
    u32 windowed_height;
    NphCursorMode cursor_mode;
    NphWindowEvent events[EVENT_QUEUE_CAPACITY];
};

NPH_STATIC_ASSERT((EVENT_QUEUE_CAPACITY & (EVENT_QUEUE_CAPACITY - 1U)) == 0U,
                  "the event queue is masked");

// The window system is inherently global (one OS message loop per
// process); this is the diagnostic/global state CLAUDE.md §2 allows for it.
static struct {
    b32 initialized;
    const char **vk_extensions;
    u32 vk_extension_count;
} g_ws;

// ---------------------------------------------------------------------------
// Key and modifier mapping
// ---------------------------------------------------------------------------

static const NphKey g_key_from_glfw[GLFW_KEY_LAST + 1] = {
    [GLFW_KEY_SPACE] = NPH_KEY_SPACE,
    [GLFW_KEY_APOSTROPHE] = NPH_KEY_APOSTROPHE,
    [GLFW_KEY_COMMA] = NPH_KEY_COMMA,
    [GLFW_KEY_MINUS] = NPH_KEY_MINUS,
    [GLFW_KEY_PERIOD] = NPH_KEY_PERIOD,
    [GLFW_KEY_SLASH] = NPH_KEY_SLASH,
    [GLFW_KEY_0] = NPH_KEY_0,
    [GLFW_KEY_1] = NPH_KEY_1,
    [GLFW_KEY_2] = NPH_KEY_2,
    [GLFW_KEY_3] = NPH_KEY_3,
    [GLFW_KEY_4] = NPH_KEY_4,
    [GLFW_KEY_5] = NPH_KEY_5,
    [GLFW_KEY_6] = NPH_KEY_6,
    [GLFW_KEY_7] = NPH_KEY_7,
    [GLFW_KEY_8] = NPH_KEY_8,
    [GLFW_KEY_9] = NPH_KEY_9,
    [GLFW_KEY_SEMICOLON] = NPH_KEY_SEMICOLON,
    [GLFW_KEY_EQUAL] = NPH_KEY_EQUAL,
    [GLFW_KEY_A] = NPH_KEY_A,
    [GLFW_KEY_B] = NPH_KEY_B,
    [GLFW_KEY_C] = NPH_KEY_C,
    [GLFW_KEY_D] = NPH_KEY_D,
    [GLFW_KEY_E] = NPH_KEY_E,
    [GLFW_KEY_F] = NPH_KEY_F,
    [GLFW_KEY_G] = NPH_KEY_G,
    [GLFW_KEY_H] = NPH_KEY_H,
    [GLFW_KEY_I] = NPH_KEY_I,
    [GLFW_KEY_J] = NPH_KEY_J,
    [GLFW_KEY_K] = NPH_KEY_K,
    [GLFW_KEY_L] = NPH_KEY_L,
    [GLFW_KEY_M] = NPH_KEY_M,
    [GLFW_KEY_N] = NPH_KEY_N,
    [GLFW_KEY_O] = NPH_KEY_O,
    [GLFW_KEY_P] = NPH_KEY_P,
    [GLFW_KEY_Q] = NPH_KEY_Q,
    [GLFW_KEY_R] = NPH_KEY_R,
    [GLFW_KEY_S] = NPH_KEY_S,
    [GLFW_KEY_T] = NPH_KEY_T,
    [GLFW_KEY_U] = NPH_KEY_U,
    [GLFW_KEY_V] = NPH_KEY_V,
    [GLFW_KEY_W] = NPH_KEY_W,
    [GLFW_KEY_X] = NPH_KEY_X,
    [GLFW_KEY_Y] = NPH_KEY_Y,
    [GLFW_KEY_Z] = NPH_KEY_Z,
    [GLFW_KEY_LEFT_BRACKET] = NPH_KEY_LEFT_BRACKET,
    [GLFW_KEY_BACKSLASH] = NPH_KEY_BACKSLASH,
    [GLFW_KEY_RIGHT_BRACKET] = NPH_KEY_RIGHT_BRACKET,
    [GLFW_KEY_GRAVE_ACCENT] = NPH_KEY_GRAVE_ACCENT,
    [GLFW_KEY_WORLD_1] = NPH_KEY_WORLD_1,
    [GLFW_KEY_WORLD_2] = NPH_KEY_WORLD_2,
    [GLFW_KEY_ESCAPE] = NPH_KEY_ESCAPE,
    [GLFW_KEY_ENTER] = NPH_KEY_ENTER,
    [GLFW_KEY_TAB] = NPH_KEY_TAB,
    [GLFW_KEY_BACKSPACE] = NPH_KEY_BACKSPACE,
    [GLFW_KEY_INSERT] = NPH_KEY_INSERT,
    [GLFW_KEY_DELETE] = NPH_KEY_DELETE,
    [GLFW_KEY_RIGHT] = NPH_KEY_RIGHT,
    [GLFW_KEY_LEFT] = NPH_KEY_LEFT,
    [GLFW_KEY_DOWN] = NPH_KEY_DOWN,
    [GLFW_KEY_UP] = NPH_KEY_UP,
    [GLFW_KEY_PAGE_UP] = NPH_KEY_PAGE_UP,
    [GLFW_KEY_PAGE_DOWN] = NPH_KEY_PAGE_DOWN,
    [GLFW_KEY_HOME] = NPH_KEY_HOME,
    [GLFW_KEY_END] = NPH_KEY_END,
    [GLFW_KEY_CAPS_LOCK] = NPH_KEY_CAPS_LOCK,
    [GLFW_KEY_SCROLL_LOCK] = NPH_KEY_SCROLL_LOCK,
    [GLFW_KEY_NUM_LOCK] = NPH_KEY_NUM_LOCK,
    [GLFW_KEY_PRINT_SCREEN] = NPH_KEY_PRINT_SCREEN,
    [GLFW_KEY_PAUSE] = NPH_KEY_PAUSE,
    [GLFW_KEY_F1] = NPH_KEY_F1,
    [GLFW_KEY_F2] = NPH_KEY_F2,
    [GLFW_KEY_F3] = NPH_KEY_F3,
    [GLFW_KEY_F4] = NPH_KEY_F4,
    [GLFW_KEY_F5] = NPH_KEY_F5,
    [GLFW_KEY_F6] = NPH_KEY_F6,
    [GLFW_KEY_F7] = NPH_KEY_F7,
    [GLFW_KEY_F8] = NPH_KEY_F8,
    [GLFW_KEY_F9] = NPH_KEY_F9,
    [GLFW_KEY_F10] = NPH_KEY_F10,
    [GLFW_KEY_F11] = NPH_KEY_F11,
    [GLFW_KEY_F12] = NPH_KEY_F12,
    [GLFW_KEY_F13] = NPH_KEY_F13,
    [GLFW_KEY_F14] = NPH_KEY_F14,
    [GLFW_KEY_F15] = NPH_KEY_F15,
    [GLFW_KEY_F16] = NPH_KEY_F16,
    [GLFW_KEY_F17] = NPH_KEY_F17,
    [GLFW_KEY_F18] = NPH_KEY_F18,
    [GLFW_KEY_F19] = NPH_KEY_F19,
    [GLFW_KEY_F20] = NPH_KEY_F20,
    [GLFW_KEY_F21] = NPH_KEY_F21,
    [GLFW_KEY_F22] = NPH_KEY_F22,
    [GLFW_KEY_F23] = NPH_KEY_F23,
    [GLFW_KEY_F24] = NPH_KEY_F24,
    [GLFW_KEY_F25] = NPH_KEY_F25,
    [GLFW_KEY_KP_0] = NPH_KEY_KP_0,
    [GLFW_KEY_KP_1] = NPH_KEY_KP_1,
    [GLFW_KEY_KP_2] = NPH_KEY_KP_2,
    [GLFW_KEY_KP_3] = NPH_KEY_KP_3,
    [GLFW_KEY_KP_4] = NPH_KEY_KP_4,
    [GLFW_KEY_KP_5] = NPH_KEY_KP_5,
    [GLFW_KEY_KP_6] = NPH_KEY_KP_6,
    [GLFW_KEY_KP_7] = NPH_KEY_KP_7,
    [GLFW_KEY_KP_8] = NPH_KEY_KP_8,
    [GLFW_KEY_KP_9] = NPH_KEY_KP_9,
    [GLFW_KEY_KP_DECIMAL] = NPH_KEY_KP_DECIMAL,
    [GLFW_KEY_KP_DIVIDE] = NPH_KEY_KP_DIVIDE,
    [GLFW_KEY_KP_MULTIPLY] = NPH_KEY_KP_MULTIPLY,
    [GLFW_KEY_KP_SUBTRACT] = NPH_KEY_KP_SUBTRACT,
    [GLFW_KEY_KP_ADD] = NPH_KEY_KP_ADD,
    [GLFW_KEY_KP_ENTER] = NPH_KEY_KP_ENTER,
    [GLFW_KEY_KP_EQUAL] = NPH_KEY_KP_EQUAL,
    [GLFW_KEY_LEFT_SHIFT] = NPH_KEY_LEFT_SHIFT,
    [GLFW_KEY_LEFT_CONTROL] = NPH_KEY_LEFT_CONTROL,
    [GLFW_KEY_LEFT_ALT] = NPH_KEY_LEFT_ALT,
    [GLFW_KEY_LEFT_SUPER] = NPH_KEY_LEFT_SUPER,
    [GLFW_KEY_RIGHT_SHIFT] = NPH_KEY_RIGHT_SHIFT,
    [GLFW_KEY_RIGHT_CONTROL] = NPH_KEY_RIGHT_CONTROL,
    [GLFW_KEY_RIGHT_ALT] = NPH_KEY_RIGHT_ALT,
    [GLFW_KEY_RIGHT_SUPER] = NPH_KEY_RIGHT_SUPER,
    [GLFW_KEY_MENU] = NPH_KEY_MENU,
};

static u32 key_from_glfw(int key)
{
    if (key < 0 || key > GLFW_KEY_LAST) {
        return NPH_KEY_UNKNOWN;
    }
    return (u32)g_key_from_glfw[key];
}

static u32 mods_from_glfw(int mods)
{
    u32 out = 0U;
    if ((mods & GLFW_MOD_SHIFT) != 0) {
        out |= NPH_MOD_SHIFT;
    }
    if ((mods & GLFW_MOD_CONTROL) != 0) {
        out |= NPH_MOD_CONTROL;
    }
    if ((mods & GLFW_MOD_ALT) != 0) {
        out |= NPH_MOD_ALT;
    }
    if ((mods & GLFW_MOD_SUPER) != 0) {
        out |= NPH_MOD_SUPER;
    }
    if ((mods & GLFW_MOD_CAPS_LOCK) != 0) {
        out |= NPH_MOD_CAPS_LOCK;
    }
    if ((mods & GLFW_MOD_NUM_LOCK) != 0) {
        out |= NPH_MOD_NUM_LOCK;
    }
    return out;
}

static u32 action_from_glfw(int action)
{
    switch (action) {
    case GLFW_PRESS:
        return NPH_KEY_ACTION_PRESS;
    case GLFW_REPEAT:
        return NPH_KEY_ACTION_REPEAT;
    default:
        return NPH_KEY_ACTION_RELEASE;
    }
}

// ---------------------------------------------------------------------------
// Event queue and callbacks
// ---------------------------------------------------------------------------

static NphWindowEvent *push_event(NphWindow *w, NphWindowEventType type)
{
    if (w->tail - w->head == EVENT_QUEUE_CAPACITY) {
        ++w->head; // drop the oldest
        ++w->dropped;
    }
    NphWindowEvent *e = &w->events[w->tail & (EVENT_QUEUE_CAPACITY - 1U)];
    ++w->tail;
    memset(e, 0, sizeof *e);
    e->type = (u32)type;
    e->time_ns = nph_time_now_ns();
    return e;
}

static NphWindow *window_of(GLFWwindow *handle)
{
    return (NphWindow *)glfwGetWindowUserPointer(handle);
}

static void on_key(GLFWwindow *h, int key, int scancode, int action, int mods)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_KEY);
    e->u.key.key = key_from_glfw(key);
    e->u.key.scancode = scancode < 0 ? 0U : (u32)scancode;
    e->u.key.action = action_from_glfw(action);
    e->u.key.mods = mods_from_glfw(mods);
}

static void on_char(GLFWwindow *h, unsigned int codepoint)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_CHAR);
    e->u.ch.codepoint = codepoint;
}

static void on_mouse_button(GLFWwindow *h, int button, int action, int mods)
{
    if (button < 0 || button >= (int)NPH_MOUSE_BUTTON_COUNT) {
        return;
    }
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_MOUSE_BUTTON);
    e->u.mouse_button.button = (u32)button;
    e->u.mouse_button.action = action_from_glfw(action);
    e->u.mouse_button.mods = mods_from_glfw(mods);
}

static void on_cursor_pos(GLFWwindow *h, double x, double y)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_MOUSE_MOVE);
    e->u.mouse_move.x = x;
    e->u.mouse_move.y = y;
}

static void on_scroll(GLFWwindow *h, double dx, double dy)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_MOUSE_SCROLL);
    e->u.scroll.dx = dx;
    e->u.scroll.dy = dy;
}

static void on_cursor_enter(GLFWwindow *h, int entered)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_MOUSE_ENTER);
    e->u.mouse_enter.entered = entered != 0;
}

static void on_window_size(GLFWwindow *h, int width, int height)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_RESIZE);
    e->u.size.width = width < 0 ? 0U : (u32)width;
    e->u.size.height = height < 0 ? 0U : (u32)height;
}

static void on_framebuffer_size(GLFWwindow *h, int width, int height)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_FRAMEBUFFER_RESIZE);
    e->u.size.width = width < 0 ? 0U : (u32)width;
    e->u.size.height = height < 0 ? 0U : (u32)height;
}

static void on_content_scale(GLFWwindow *h, float x, float y)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_CONTENT_SCALE);
    e->u.scale.x = x;
    e->u.scale.y = y;
}

static void on_window_pos(GLFWwindow *h, int x, int y)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_MOVE);
    e->u.position.x = x;
    e->u.position.y = y;
}

static void on_focus(GLFWwindow *h, int focused)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_FOCUS);
    e->u.focus.focused = focused != 0;
}

static void on_iconify(GLFWwindow *h, int iconified)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_ICONIFY);
    e->u.iconify.iconified = iconified != 0;
}

static void on_maximize(GLFWwindow *h, int maximized)
{
    NphWindowEvent *e = push_event(window_of(h), NPH_WINDOW_EVENT_MAXIMIZE);
    e->u.maximize.maximized = maximized != 0;
}

static void on_close(GLFWwindow *h)
{
    (void)push_event(window_of(h), NPH_WINDOW_EVENT_CLOSE);
}

static void on_error(int code, const char *description)
{
    // A clipboard without text is a normal outcome, not an error.
    if (code == GLFW_FORMAT_UNAVAILABLE) {
        NPH_LOG_DEBUG(&NPH_LOG_PLATFORM, "glfw: %s (0x%x)", description, (unsigned)code);
        return;
    }
    NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "glfw: %s (0x%x)", description, (unsigned)code);
}

// ---------------------------------------------------------------------------
// The window system
// ---------------------------------------------------------------------------

NphResult nph_window_system_init(void)
{
    if (g_ws.initialized) {
        return NPH_RESULT_OK;
    }
    glfwSetErrorCallback(on_error);
    if (!glfwInit()) {
        return NPH_RESULT_UNSUPPORTED; // the error callback logged the reason
    }
    g_ws.initialized = 1;
    NPH_LOG_INFO(&NPH_LOG_PLATFORM, "window system: %s backend, GLFW %s",
                 nph_window_system_backend(), glfwGetVersionString());
    return NPH_RESULT_OK;
}

void nph_window_system_shutdown(void)
{
    if (!g_ws.initialized) {
        return;
    }
    glfwTerminate(); // destroys any windows still open
    memset(&g_ws, 0, sizeof g_ws);
}

bool nph_window_system_is_initialized(void)
{
    return g_ws.initialized != 0;
}

const char *nph_window_system_backend(void)
{
    if (!g_ws.initialized) {
        return "none";
    }
    switch (glfwGetPlatform()) {
    case GLFW_PLATFORM_WIN32:
        return "win32";
    case GLFW_PLATFORM_COCOA:
        return "cocoa";
    case GLFW_PLATFORM_WAYLAND:
        return "wayland";
    case GLFW_PLATFORM_X11:
        return "x11";
    case GLFW_PLATFORM_NULL:
        return "null";
    default:
        return "?";
    }
}

void nph_window_poll_events(void)
{
    if (g_ws.initialized) {
        glfwPollEvents();
    }
}

void nph_window_wait_events(u64 timeout_ns)
{
    if (!g_ws.initialized) {
        return;
    }
    if (timeout_ns == 0U) {
        glfwWaitEvents();
    } else {
        glfwWaitEventsTimeout(nph_time_ns_to_s(timeout_ns));
    }
}

// ---------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------

static GLFWmonitor *monitor_at(u32 index)
{
    if (!g_ws.initialized) {
        return NULL;
    }
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    if (monitors == NULL || count <= 0 || index >= (u32)count) {
        return NULL;
    }
    return monitors[index];
}

u32 nph_monitor_count(void)
{
    if (!g_ws.initialized) {
        return 0U;
    }
    int count = 0;
    (void)glfwGetMonitors(&count);
    return count < 0 ? 0U : (u32)count;
}

NphResult nph_monitor_get_info(u32 index, NphMonitorInfo *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    GLFWmonitor *m = monitor_at(index);
    if (m == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    const char *name = glfwGetMonitorName(m);
    if (name != NULL) {
        const u64 len = strlen(name);
        const u64 n = len < sizeof out->name - 1U ? len : sizeof out->name - 1U;
        memcpy(out->name, name, n);
        out->name[n] = '\0';
    }
    int x = 0;
    int y = 0;
    glfwGetMonitorPos(m, &x, &y);
    out->x = x;
    out->y = y;
    const GLFWvidmode *mode = glfwGetVideoMode(m);
    if (mode != NULL) {
        out->width = mode->width < 0 ? 0U : (u32)mode->width;
        out->height = mode->height < 0 ? 0U : (u32)mode->height;
        out->refresh_rate_hz = mode->refreshRate < 0 ? 0U : (u32)mode->refreshRate;
    }
    int mm_w = 0;
    int mm_h = 0;
    glfwGetMonitorPhysicalSize(m, &mm_w, &mm_h);
    out->physical_width_mm = mm_w < 0 ? 0U : (u32)mm_w;
    out->physical_height_mm = mm_h < 0 ? 0U : (u32)mm_h;
    float sx = 1.0F;
    float sy = 1.0F;
    glfwGetMonitorContentScale(m, &sx, &sy);
    out->scale_x = sx;
    out->scale_y = sy;
    out->primary = m == glfwGetPrimaryMonitor();
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

void nph_window_desc_default(NphWindowDesc *desc)
{
    NPH_ASSERT(desc != NULL);
    memset(desc, 0, sizeof *desc);
    desc->title = "Nepheline";
    desc->width = DEFAULT_WIDTH;
    desc->height = DEFAULT_HEIGHT;
    desc->resizable = 1;
    desc->decorated = 1;
    desc->visible = 1;
}

static void remember_windowed_placement(NphWindow *w)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetWindowPos(w->handle, &x, &y);
    glfwGetWindowSize(w->handle, &width, &height);
    w->windowed_x = x;
    w->windowed_y = y;
    w->windowed_width = width < 0 ? 0U : (u32)width;
    w->windowed_height = height < 0 ? 0U : (u32)height;
}

NphResult nph_window_create(NphAllocator *alloc, const NphWindowDesc *desc, NphWindow **out)
{
    NPH_ASSERT(alloc != NULL && desc != NULL && out != NULL);
    *out = NULL;
    if (!g_ws.initialized) {
        return NPH_RESULT_UNSUPPORTED;
    }
    GLFWmonitor *monitor = NULL;
    if (desc->fullscreen) {
        monitor = monitor_at(desc->monitor);
        if (monitor == NULL) {
            return NPH_RESULT_NOT_FOUND;
        }
    }
    u32 width = desc->width != 0U ? desc->width : DEFAULT_WIDTH;
    u32 height = desc->height != 0U ? desc->height : DEFAULT_HEIGHT;

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan: no GL context
    glfwWindowHint(GLFW_RESIZABLE, desc->resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, desc->decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc->visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, desc->maximized ? GLFW_TRUE : GLFW_FALSE);
    if (monitor != NULL) {
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode != NULL) {
            glfwWindowHint(GLFW_RED_BITS, mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
            width = (u32)mode->width;
            height = (u32)mode->height;
        }
    }
    GLFWwindow *handle = glfwCreateWindow(
        (int)width, (int)height, desc->title != NULL ? desc->title : "Nepheline", monitor, NULL);
    if (handle == NULL) {
        return NPH_RESULT_UNSUPPORTED; // logged by the error callback
    }
    NphWindow *w = NPH_ALLOC_TYPE(alloc, NphWindow);
    if (w == NULL) {
        glfwDestroyWindow(handle);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    w->handle = handle;
    w->alloc = alloc;
    w->fullscreen = monitor != NULL;
    w->cursor_mode = NPH_CURSOR_MODE_NORMAL;
    w->windowed_width = desc->width != 0U ? desc->width : DEFAULT_WIDTH;
    w->windowed_height = desc->height != 0U ? desc->height : DEFAULT_HEIGHT;
    if (!w->fullscreen) {
        remember_windowed_placement(w);
    }
    glfwSetWindowUserPointer(handle, w);
    glfwSetKeyCallback(handle, on_key);
    glfwSetCharCallback(handle, on_char);
    glfwSetMouseButtonCallback(handle, on_mouse_button);
    glfwSetCursorPosCallback(handle, on_cursor_pos);
    glfwSetScrollCallback(handle, on_scroll);
    glfwSetCursorEnterCallback(handle, on_cursor_enter);
    glfwSetWindowSizeCallback(handle, on_window_size);
    glfwSetFramebufferSizeCallback(handle, on_framebuffer_size);
    glfwSetWindowContentScaleCallback(handle, on_content_scale);
    glfwSetWindowPosCallback(handle, on_window_pos);
    glfwSetWindowFocusCallback(handle, on_focus);
    glfwSetWindowIconifyCallback(handle, on_iconify);
    glfwSetWindowMaximizeCallback(handle, on_maximize);
    glfwSetWindowCloseCallback(handle, on_close);
    *out = w;
    return NPH_RESULT_OK;
}

void nph_window_destroy(NphWindow *w)
{
    if (w == NULL) {
        return;
    }
    if (g_ws.initialized && w->handle != NULL) {
        glfwDestroyWindow(w->handle);
    }
    NphAllocator *alloc = w->alloc;
    NPH_FREE_TYPE(alloc, NphWindow, w);
}

bool nph_window_pop_event(NphWindow *w, NphWindowEvent *out)
{
    NPH_ASSERT(w != NULL && out != NULL);
    if (w->head == w->tail) {
        return false;
    }
    *out = w->events[w->head & (EVENT_QUEUE_CAPACITY - 1U)];
    ++w->head;
    return true;
}

u32 nph_window_dropped_events(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    const u32 n = w->dropped;
    w->dropped = 0U;
    return n;
}

bool nph_window_should_close(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return glfwWindowShouldClose(w->handle) != 0;
}

void nph_window_set_should_close(NphWindow *w, bool close)
{
    NPH_ASSERT(w != NULL);
    glfwSetWindowShouldClose(w->handle, close ? GLFW_TRUE : GLFW_FALSE);
}

static void int_pair_to_u32(int a, int b, u32 *out_a, u32 *out_b)
{
    if (out_a != NULL) {
        *out_a = a < 0 ? 0U : (u32)a;
    }
    if (out_b != NULL) {
        *out_b = b < 0 ? 0U : (u32)b;
    }
}

void nph_window_get_size(const NphWindow *w, u32 *out_width, u32 *out_height)
{
    NPH_ASSERT(w != NULL);
    int width = 0;
    int height = 0;
    glfwGetWindowSize(w->handle, &width, &height);
    int_pair_to_u32(width, height, out_width, out_height);
}

void nph_window_get_framebuffer_size(const NphWindow *w, u32 *out_width, u32 *out_height)
{
    NPH_ASSERT(w != NULL);
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(w->handle, &width, &height);
    int_pair_to_u32(width, height, out_width, out_height);
}

void nph_window_get_content_scale(const NphWindow *w, f32 *out_x, f32 *out_y)
{
    NPH_ASSERT(w != NULL);
    float x = 1.0F;
    float y = 1.0F;
    glfwGetWindowContentScale(w->handle, &x, &y);
    if (out_x != NULL) {
        *out_x = x;
    }
    if (out_y != NULL) {
        *out_y = y;
    }
}

void nph_window_get_position(const NphWindow *w, i32 *out_x, i32 *out_y)
{
    NPH_ASSERT(w != NULL);
    int x = 0;
    int y = 0;
    glfwGetWindowPos(w->handle, &x, &y);
    if (out_x != NULL) {
        *out_x = x;
    }
    if (out_y != NULL) {
        *out_y = y;
    }
}

void nph_window_set_size(NphWindow *w, u32 width, u32 height)
{
    NPH_ASSERT(w != NULL && width > 0U && height > 0U);
    glfwSetWindowSize(w->handle, (int)width, (int)height);
}

void nph_window_set_position(NphWindow *w, i32 x, i32 y)
{
    NPH_ASSERT(w != NULL);
    glfwSetWindowPos(w->handle, x, y);
}

void nph_window_set_title(NphWindow *w, const char *title)
{
    NPH_ASSERT(w != NULL && title != NULL);
    glfwSetWindowTitle(w->handle, title);
}

NphResult nph_window_set_fullscreen(NphWindow *w, bool fullscreen, u32 monitor_index)
{
    NPH_ASSERT(w != NULL);
    if (fullscreen) {
        GLFWmonitor *monitor = monitor_at(monitor_index);
        if (monitor == NULL) {
            return NPH_RESULT_NOT_FOUND;
        }
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode == NULL) {
            return NPH_RESULT_UNSUPPORTED;
        }
        if (!w->fullscreen) {
            remember_windowed_placement(w);
        }
        glfwSetWindowMonitor(w->handle, monitor, 0, 0, mode->width, mode->height,
                             mode->refreshRate);
        w->fullscreen = 1;
    } else if (w->fullscreen) {
        glfwSetWindowMonitor(w->handle, NULL, w->windowed_x, w->windowed_y, (int)w->windowed_width,
                             (int)w->windowed_height, 0);
        w->fullscreen = 0;
    }
    return NPH_RESULT_OK;
}

bool nph_window_is_fullscreen(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return w->fullscreen != 0;
}

void nph_window_show(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    glfwShowWindow(w->handle);
}

void nph_window_hide(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    glfwHideWindow(w->handle);
}

void nph_window_focus(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    glfwFocusWindow(w->handle);
}

void nph_window_minimize(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    glfwIconifyWindow(w->handle);
}

void nph_window_maximize(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    glfwMaximizeWindow(w->handle);
}

void nph_window_restore(NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    glfwRestoreWindow(w->handle);
}

bool nph_window_is_visible(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return glfwGetWindowAttrib(w->handle, GLFW_VISIBLE) != 0;
}

bool nph_window_is_focused(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return glfwGetWindowAttrib(w->handle, GLFW_FOCUSED) != 0;
}

bool nph_window_is_minimized(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return glfwGetWindowAttrib(w->handle, GLFW_ICONIFIED) != 0;
}

bool nph_window_is_maximized(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return glfwGetWindowAttrib(w->handle, GLFW_MAXIMIZED) != 0;
}

void nph_window_set_cursor_mode(NphWindow *w, NphCursorMode mode)
{
    NPH_ASSERT(w != NULL);
    int glfw_mode;
    switch (mode) {
    case NPH_CURSOR_MODE_HIDDEN:
        glfw_mode = GLFW_CURSOR_HIDDEN;
        break;
    case NPH_CURSOR_MODE_DISABLED:
        glfw_mode = GLFW_CURSOR_DISABLED;
        break;
    case NPH_CURSOR_MODE_NORMAL:
    default:
        glfw_mode = GLFW_CURSOR_NORMAL;
        mode = NPH_CURSOR_MODE_NORMAL;
        break;
    }
    glfwSetInputMode(w->handle, GLFW_CURSOR, glfw_mode);
    w->cursor_mode = mode;
}

NphCursorMode nph_window_get_cursor_mode(const NphWindow *w)
{
    NPH_ASSERT(w != NULL);
    return w->cursor_mode;
}

bool nph_window_raw_mouse_motion_supported(void)
{
    return g_ws.initialized && glfwRawMouseMotionSupported() != 0;
}

void nph_window_set_raw_mouse_motion(NphWindow *w, bool enabled)
{
    NPH_ASSERT(w != NULL);
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(w->handle, GLFW_RAW_MOUSE_MOTION, enabled ? GLFW_TRUE : GLFW_FALSE);
    }
}

void nph_window_get_cursor_position(const NphWindow *w, f64 *out_x, f64 *out_y)
{
    NPH_ASSERT(w != NULL);
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(w->handle, &x, &y);
    if (out_x != NULL) {
        *out_x = x;
    }
    if (out_y != NULL) {
        *out_y = y;
    }
}

void nph_window_set_cursor_position(NphWindow *w, f64 x, f64 y)
{
    NPH_ASSERT(w != NULL);
    glfwSetCursorPos(w->handle, x, y);
}

NphResult nph_window_get_clipboard(NphWindow *w, char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(w != NULL && buf != NULL && cap > 0U);
    const char *text = glfwGetClipboardString(w->handle);
    if (text == NULL) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return NPH_RESULT_NOT_FOUND;
    }
    const u64 len = strlen(text);
    if (out_len != NULL) {
        *out_len = len;
    }
    if (len + 1U > cap) {
        buf[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(buf, text, len + 1U);
    return NPH_RESULT_OK;
}

void nph_window_set_clipboard(NphWindow *w, const char *text)
{
    NPH_ASSERT(w != NULL && text != NULL);
    glfwSetClipboardString(w->handle, text);
}

// ---------------------------------------------------------------------------
// Gamepads
// ---------------------------------------------------------------------------

NPH_STATIC_ASSERT(NPH_GAMEPAD_MAX <= GLFW_JOYSTICK_LAST + 1, "more gamepads than GLFW tracks");
NPH_STATIC_ASSERT(NPH_GAMEPAD_BUTTON_COUNT == GLFW_GAMEPAD_BUTTON_LAST + 1,
                  "gamepad button layout must match GLFW's standard mapping");
NPH_STATIC_ASSERT(NPH_GAMEPAD_AXIS_COUNT == GLFW_GAMEPAD_AXIS_LAST + 1,
                  "gamepad axis layout must match GLFW's standard mapping");

bool nph_window_get_gamepad_state(u32 index, NphGamepadState *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    if (!g_ws.initialized || index >= NPH_GAMEPAD_MAX) {
        return false;
    }
    const int jid = (int)index;
    if (!glfwJoystickPresent(jid) || !glfwJoystickIsGamepad(jid)) {
        return false;
    }
    GLFWgamepadstate state;
    if (!glfwGetGamepadState(jid, &state)) {
        return false;
    }
    for (u32 i = 0; i < NPH_GAMEPAD_AXIS_COUNT; ++i) {
        out->axes[i] = state.axes[i];
    }
    for (u32 i = 0; i < NPH_GAMEPAD_BUTTON_COUNT; ++i) {
        out->buttons[i] = state.buttons[i] == GLFW_PRESS ? 1U : 0U;
    }
    out->connected = 1;
    return true;
}

const char *nph_window_gamepad_name(u32 index)
{
    if (!g_ws.initialized || index >= NPH_GAMEPAD_MAX) {
        return NULL;
    }
    const int jid = (int)index;
    if (!glfwJoystickPresent(jid) || !glfwJoystickIsGamepad(jid)) {
        return NULL;
    }
    return glfwGetGamepadName(jid);
}

// ---------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------

void nph_window_vulkan_set_loader(NphVulkanGetInstanceProcAddrFn get_instance_proc_addr)
{
    glfwInitVulkanLoader(get_instance_proc_addr);
}

NphResult nph_window_vulkan_extensions(const char *const **out_names, u32 *out_count)
{
    NPH_ASSERT(out_names != NULL && out_count != NULL);
    *out_names = NULL;
    *out_count = 0U;
    if (!g_ws.initialized || !glfwVulkanSupported()) {
        return NPH_RESULT_UNSUPPORTED;
    }
    uint32_t count = 0U;
    const char **names = glfwGetRequiredInstanceExtensions(&count);
    if (names == NULL) {
        return NPH_RESULT_UNSUPPORTED;
    }
    g_ws.vk_extensions = names;
    g_ws.vk_extension_count = count;
    *out_names = (const char *const *)names;
    *out_count = count;
    return NPH_RESULT_OK;
}

NphResult nph_window_vulkan_create_surface(NphWindow *w, void *vk_instance,
                                           const void *vk_allocator, u64 *out_vk_surface)
{
    NPH_ASSERT(w != NULL && vk_instance != NULL && out_vk_surface != NULL);
    *out_vk_surface = 0U;
    if (!g_ws.initialized || !glfwVulkanSupported()) {
        return NPH_RESULT_UNSUPPORTED;
    }
    NphVkSurfaceHandle surface = NULL;
    const int result =
        glfwCreateWindowSurface((NphVkInstanceHandle)vk_instance, w->handle,
                                (const NphVkAllocationCallbacks *)vk_allocator, &surface);
    if (result != 0) { // VK_SUCCESS
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "glfwCreateWindowSurface: VkResult %d", result);
        return NPH_RESULT_UNKNOWN;
    }
    *out_vk_surface = (u64)(uptr)surface;
    return NPH_RESULT_OK;
}

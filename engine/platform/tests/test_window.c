// SPDX-License-Identifier: Apache-2.0
// The window system against a real display: a hidden window on Windows
// and under Xvfb in CI. Skipped cleanly on a headless machine.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>

#include "test_platform_support.h"
#include "window_internal.h"

#include <string.h>

typedef struct WindowFixture {
    NphWindow *window;
} WindowFixture;

static bool window_system_available(void)
{
    return nph_window_system_init() == NPH_RESULT_OK;
}

NTEST_SETUP(WindowFixture)
{
    if (!window_system_available()) {
        f->window = NULL;
        return;
    }
    NphWindowDesc desc;
    nph_window_desc_default(&desc);
    desc.title = "nepheline platform_tests";
    desc.width = 640U;
    desc.height = 360U;
    desc.visible = 0; // never flash a window during the test run
    NTEST_ASSERT_EQ_INT(nph_window_create(nph_allocator_system(), &desc, &f->window),
                        NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(f->window);
}

NTEST_TEARDOWN(WindowFixture)
{
    if (f->window != NULL) {
        nph_window_destroy(f->window);
    }
    nph_window_system_shutdown();
    NTEST_ASSERT_FALSE(nph_window_system_is_initialized());
}

#define REQUIRE_WINDOW(f)                                                                          \
    do {                                                                                           \
        if ((f)->window == NULL) {                                                                 \
            NTEST_SKIP("no display available");                                                    \
        }                                                                                          \
    } while (0)

NTEST(window_system, init_is_idempotent_and_shutdown_is_safe)
{
    nph_window_system_shutdown(); // without init
    NTEST_ASSERT_FALSE(nph_window_system_is_initialized());
    NTEST_ASSERT_EQ_STR(nph_window_system_backend(), "none");
    NTEST_ASSERT_EQ_UINT(nph_monitor_count(), 0U);
    nph_window_poll_events(); // no-ops without init
    nph_window_wait_events(NPH_TIME_NS_PER_MS);
    const NphResult r = nph_window_system_init();
    if (r != NPH_RESULT_OK) {
        NTEST_ASSERT_EQ_INT(r, NPH_RESULT_UNSUPPORTED);
        NTEST_SKIP("no display available");
    }
    NTEST_ASSERT_TRUE(nph_window_system_is_initialized());
    NTEST_ASSERT_EQ_INT(nph_window_system_init(), NPH_RESULT_OK); // again
    NTEST_LOG("backend: %s", nph_window_system_backend());
    NTEST_ASSERT_NE_STR(nph_window_system_backend(), "none");
    nph_window_system_shutdown();
    nph_window_system_shutdown();
    NTEST_ASSERT_FALSE(nph_window_system_is_initialized());
}

NTEST_F(WindowFixture, sizes_and_state)
{
    REQUIRE_WINDOW(f);
    u32 w = 0U;
    u32 h = 0U;
    nph_window_get_size(f->window, &w, &h);
    NTEST_ASSERT_EQ_UINT(w, 640U);
    NTEST_ASSERT_EQ_UINT(h, 360U);
    f32 sx = 0.0F;
    f32 sy = 0.0F;
    nph_window_get_content_scale(f->window, &sx, &sy);
    NTEST_ASSERT_GT_UINT(sx > 0.0F, 0U);
    u32 fw = 0U;
    u32 fh = 0U;
    nph_window_get_framebuffer_size(f->window, &fw, &fh);
    NTEST_LOG("window %ux%u, framebuffer %ux%u, scale %.2f", w, h, fw, fh, (f64)sx);
    NTEST_ASSERT_GE_UINT(fw, w);
    NTEST_ASSERT_GE_UINT(fh, h);
    i32 x = 0;
    i32 y = 0;
    nph_window_get_position(f->window, &x, &y);

    NTEST_ASSERT_FALSE(nph_window_should_close(f->window));
    nph_window_set_should_close(f->window, true);
    NTEST_ASSERT_TRUE(nph_window_should_close(f->window));
    nph_window_set_should_close(f->window, false);

    NTEST_ASSERT_FALSE(nph_window_is_visible(f->window));
    NTEST_ASSERT_FALSE(nph_window_is_fullscreen(f->window));
    NTEST_ASSERT_FALSE(nph_window_is_minimized(f->window));
    nph_window_set_title(f->window, "renamed");
    nph_window_set_size(f->window, 800U, 450U);
    nph_window_set_position(f->window, x, y);
    nph_window_poll_events();
    nph_window_get_size(f->window, &w, &h);
    NTEST_LOG("after resize: %ux%u (window managers may defer this)", w, h);

    NTEST_ASSERT_EQ_INT(nph_window_get_cursor_mode(f->window), NPH_CURSOR_MODE_NORMAL);
    nph_window_set_cursor_mode(f->window, NPH_CURSOR_MODE_HIDDEN);
    NTEST_ASSERT_EQ_INT(nph_window_get_cursor_mode(f->window), NPH_CURSOR_MODE_HIDDEN);
    nph_window_set_cursor_mode(f->window, NPH_CURSOR_MODE_NORMAL);
    nph_window_set_raw_mouse_motion(f->window, true);
    nph_window_set_raw_mouse_motion(f->window, false);
    NTEST_LOG("raw mouse motion supported: %d", (int)nph_window_raw_mouse_motion_supported());
    f64 cx = 0.0;
    f64 cy = 0.0;
    nph_window_get_cursor_position(f->window, &cx, &cy);
    nph_window_set_cursor_position(f->window, 10.0, 10.0);
}

NTEST_F(WindowFixture, event_queue)
{
    REQUIRE_WINDOW(f);
    // Pump a few times; whatever the OS sends (focus, resize) must come
    // through the queue well-formed.
    for (u32 i = 0; i < 5U; ++i) {
        nph_window_poll_events();
    }
    NphWindowEvent e;
    u32 count = 0U;
    while (nph_window_pop_event(f->window, &e)) {
        NTEST_ASSERT_GT_UINT(e.type, NPH_WINDOW_EVENT_NONE);
        NTEST_ASSERT_LT_UINT(e.type, NPH_WINDOW_EVENT_COUNT);
        NTEST_ASSERT_GT_UINT(e.time_ns, 0U);
        NTEST_LOG("event: %s", nph_window_event_name((NphWindowEventType)e.type));
        ++count;
    }
    NTEST_LOG("%u events after creation", count);
    NTEST_ASSERT_FALSE(nph_window_pop_event(f->window, &e));
    NTEST_ASSERT_EQ_UINT(nph_window_dropped_events(f->window), 0U);
    // A programmatic resize produces resize events on the next pump.
    nph_window_set_size(f->window, 320U, 200U);
    nph_window_poll_events();
    nph_window_wait_events(10U * NPH_TIME_NS_PER_MS);
    while (nph_window_pop_event(f->window, &e)) {
        NTEST_LOG("event: %s", nph_window_event_name((NphWindowEventType)e.type));
    }
}

NTEST_F(WindowFixture, monitors)
{
    REQUIRE_WINDOW(f);
    const u32 count = nph_monitor_count();
    NTEST_ASSERT_GE_UINT(count, 1U);
    bool primary_seen = false;
    for (u32 i = 0; i < count; ++i) {
        NphMonitorInfo info;
        NTEST_ASSERT_EQ_INT(nph_monitor_get_info(i, &info), NPH_RESULT_OK);
        NTEST_LOG("monitor %u: '%s' %ux%u @ %u Hz at (%d, %d), %ux%u mm, scale %.2f%s", i,
                  info.name, info.width, info.height, info.refresh_rate_hz, info.x, info.y,
                  info.physical_width_mm, info.physical_height_mm, (f64)info.scale_x,
                  info.primary ? ", primary" : "");
        NTEST_ASSERT_GT_UINT(info.width, 0U);
        NTEST_ASSERT_GT_UINT(info.height, 0U);
        primary_seen = primary_seen || info.primary;
    }
    NTEST_ASSERT_TRUE(primary_seen);
    NphMonitorInfo none;
    NTEST_ASSERT_EQ_INT(nph_monitor_get_info(count, &none), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_window_set_fullscreen(f->window, true, count), NPH_RESULT_NOT_FOUND);
}

NTEST_F(WindowFixture, clipboard_round_trip)
{
    REQUIRE_WINDOW(f);
    nph_window_set_clipboard(f->window, "nepheline clipboard \xC3\xA9");
    char buf[128];
    u64 len = 0U;
    const NphResult r = nph_window_get_clipboard(f->window, buf, sizeof buf, &len);
    if (r == NPH_RESULT_NOT_FOUND) {
        NTEST_SKIP("the display server did not hand the clipboard back");
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "nepheline clipboard \xC3\xA9");
    NTEST_ASSERT_EQ_UINT(len, strlen(buf));
    char tiny[4];
    NTEST_ASSERT_EQ_INT(nph_window_get_clipboard(f->window, tiny, sizeof tiny, &len),
                        NPH_RESULT_OVERFLOW);
}

NTEST_F(WindowFixture, gamepads_and_vulkan_queries_do_not_fail_without_hardware)
{
    REQUIRE_WINDOW(f);
    u32 connected = 0U;
    for (u32 i = 0; i < NPH_GAMEPAD_MAX; ++i) {
        NphGamepadState state;
        if (nph_window_get_gamepad_state(i, &state)) {
            NTEST_ASSERT_TRUE(state.connected);
            NTEST_LOG("gamepad %u: %s", i, nph_window_gamepad_name(i));
            ++connected;
        } else {
            NTEST_ASSERT_FALSE(state.connected);
            NTEST_ASSERT_NULL(nph_window_gamepad_name(i));
        }
    }
    NTEST_LOG("%u gamepads connected", connected);
    NphGamepadState bogus;
    NTEST_ASSERT_FALSE(nph_window_get_gamepad_state(NPH_GAMEPAD_MAX, &bogus));

    const char *const *names = NULL;
    u32 count = 0U;
    const NphResult r = nph_window_vulkan_extensions(&names, &count);
    if (r == NPH_RESULT_OK) {
        NTEST_ASSERT_GE_UINT(count, 1U);
        for (u32 i = 0; i < count; ++i) {
            NTEST_LOG("vulkan extension: %s", names[i]);
        }
    } else {
        NTEST_ASSERT_EQ_INT(r, NPH_RESULT_UNSUPPORTED); // no Vulkan loader on this machine
        NTEST_ASSERT_NULL(names);
    }
}

// Files dropped on a window: one event per drop, the paths by number, the
// oldest forgotten once NPH_WINDOW_DROP_PATHS newer ones arrived, and
// nothing left behind when the window goes. No OS lets a program drop
// files on its own window, so the drops come through the entry point the
// window system's drop callback uses.
NTEST(window_system, dropped_files_arrive_with_their_paths)
{
    if (!window_system_available()) {
        NTEST_SKIP("no display available");
    }
    static NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    NphWindowDesc desc;
    nph_window_desc_default(&desc);
    desc.title = "nepheline drop test";
    desc.visible = 0;
    NphWindow *w = NULL;
    NTEST_ASSERT_EQ_INT(nph_window_create(nph_debug_allocator_allocator(&da), &desc, &w),
                        NPH_RESULT_OK);
    nph_window_poll_events();
    NphWindowEvent e;
    while (nph_window_pop_event(w, &e)) {
        // whatever creation queued
    }
    NTEST_ASSERT_NULL(nph_window_dropped_path(w, 0U)); // nothing dropped yet

    const char *const paths[2] = {"C:/content/scene.gltf", "textures/\xC3\xA9t\xC3\xA9.png"};
    nph_window_push_drop(w, 2U, paths);
    const char *const one = "one.png";
    nph_window_push_drop(w, 1U, &one);
    nph_window_push_drop(w, 0U, NULL); // an empty drop is no event
    NTEST_ASSERT_TRUE(nph_window_pop_event(w, &e));
    NTEST_ASSERT_EQ_UINT(e.type, (u32)NPH_WINDOW_EVENT_DROP);
    NTEST_ASSERT_EQ_STR(nph_window_event_name((NphWindowEventType)e.type), "drop");
    NTEST_ASSERT_EQ_UINT(e.u.drop.count, 2U);
    const u32 first = e.u.drop.first;
    NTEST_ASSERT_EQ_STR(nph_window_dropped_path(w, first), paths[0]);
    NTEST_ASSERT_EQ_STR(nph_window_dropped_path(w, first + 1U), paths[1]);
    NTEST_ASSERT_TRUE(nph_window_pop_event(w, &e));
    NTEST_ASSERT_EQ_UINT(e.type, (u32)NPH_WINDOW_EVENT_DROP);
    NTEST_ASSERT_EQ_UINT(e.u.drop.first, first + 2U);
    NTEST_ASSERT_EQ_UINT(e.u.drop.count, 1U);
    NTEST_ASSERT_EQ_STR(nph_window_dropped_path(w, first + 2U), "one.png");
    NTEST_ASSERT_FALSE(nph_window_pop_event(w, &e));
    NTEST_ASSERT_NULL(nph_window_dropped_path(w, first + 3U)); // not given out

    // The window keeps the newest NPH_WINDOW_DROP_PATHS paths.
    for (u32 i = 0; i < NPH_WINDOW_DROP_PATHS - 3U; ++i) {
        nph_window_push_drop(w, 1U, &one);
    }
    NTEST_ASSERT_EQ_STR(nph_window_dropped_path(w, first), paths[0]);
    nph_window_push_drop(w, 1U, &one);
    NTEST_ASSERT_NULL(nph_window_dropped_path(w, first));
    NTEST_ASSERT_EQ_STR(nph_window_dropped_path(w, first + 1U), paths[1]);
    nph_window_destroy(w);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
    nph_window_system_shutdown();
}

// SPDX-License-Identifier: Apache-2.0
// platform_window: the Phase 3 exit sample (CLAUDE.md §12: "A window opens
// and input is logged on Windows and Linux"). It opens a window, maps a
// few actions, and logs every window event, every action edge, and the
// axis values while they are non-zero. Close the window, press Escape, or
// pass --frames=N to stop. Also prints the CPU and monitors on startup.
//
//     platform_window [--frames=N] [--fullscreen]

#include <nepheline/platform/cpu.h>
#include <nepheline/platform/input.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "window", NPH_LOG_LEVEL_TRACE);

typedef struct Actions {
    NphActionId quit;
    NphActionId fullscreen;
    NphActionId fire;
    NphActionId jump;
    NphActionId move_x;
    NphActionId move_y;
    NphActionId look_x;
    NphActionId look_y;
} Actions;

static void log_event(const NphWindow *w, const NphWindowEvent *e)
{
    switch ((NphWindowEventType)e->type) {
    case NPH_WINDOW_EVENT_KEY:
        NPH_LOG_INFO(&LOG_SAMPLE, "key %s %s (scancode %u, mods 0x%x)",
                     nph_key_name((NphKey)e->u.key.key),
                     e->u.key.action == NPH_KEY_ACTION_PRESS    ? "press"
                     : e->u.key.action == NPH_KEY_ACTION_REPEAT ? "repeat"
                                                                : "release",
                     e->u.key.scancode, e->u.key.mods);
        break;
    case NPH_WINDOW_EVENT_CHAR:
        NPH_LOG_INFO(&LOG_SAMPLE, "text U+%04X", e->u.ch.codepoint);
        break;
    case NPH_WINDOW_EVENT_MOUSE_BUTTON:
        NPH_LOG_INFO(&LOG_SAMPLE, "%s %s (mods 0x%x)",
                     nph_mouse_button_name((NphMouseButton)e->u.mouse_button.button),
                     e->u.mouse_button.action == NPH_KEY_ACTION_PRESS ? "press" : "release",
                     e->u.mouse_button.mods);
        break;
    case NPH_WINDOW_EVENT_MOUSE_MOVE:
        NPH_LOG_TRACE(&LOG_SAMPLE, "mouse move %.1f, %.1f", e->u.mouse_move.x, e->u.mouse_move.y);
        break;
    case NPH_WINDOW_EVENT_MOUSE_SCROLL:
        NPH_LOG_INFO(&LOG_SAMPLE, "scroll %.1f, %.1f", e->u.scroll.dx, e->u.scroll.dy);
        break;
    case NPH_WINDOW_EVENT_MOUSE_ENTER:
        NPH_LOG_INFO(&LOG_SAMPLE, "mouse %s", e->u.mouse_enter.entered ? "entered" : "left");
        break;
    case NPH_WINDOW_EVENT_RESIZE:
        NPH_LOG_INFO(&LOG_SAMPLE, "resize %ux%u", e->u.size.width, e->u.size.height);
        break;
    case NPH_WINDOW_EVENT_FRAMEBUFFER_RESIZE:
        NPH_LOG_INFO(&LOG_SAMPLE, "framebuffer %ux%u", e->u.size.width, e->u.size.height);
        break;
    case NPH_WINDOW_EVENT_CONTENT_SCALE:
        NPH_LOG_INFO(&LOG_SAMPLE, "content scale %.2f, %.2f", (f64)e->u.scale.x, (f64)e->u.scale.y);
        break;
    case NPH_WINDOW_EVENT_MOVE:
        NPH_LOG_INFO(&LOG_SAMPLE, "moved to %d, %d", e->u.position.x, e->u.position.y);
        break;
    case NPH_WINDOW_EVENT_FOCUS:
        NPH_LOG_INFO(&LOG_SAMPLE, "focus %s", e->u.focus.focused ? "gained" : "lost");
        break;
    case NPH_WINDOW_EVENT_ICONIFY:
        NPH_LOG_INFO(&LOG_SAMPLE, "%s", e->u.iconify.iconified ? "minimised" : "restored");
        break;
    case NPH_WINDOW_EVENT_MAXIMIZE:
        NPH_LOG_INFO(&LOG_SAMPLE, "%s", e->u.maximize.maximized ? "maximised" : "unmaximised");
        break;
    case NPH_WINDOW_EVENT_CLOSE:
        NPH_LOG_INFO(&LOG_SAMPLE, "close requested");
        break;
    case NPH_WINDOW_EVENT_DROP:
        for (u32 i = 0; i < e->u.drop.count; ++i) {
            const char *path = nph_window_dropped_path(w, e->u.drop.first + i);
            NPH_LOG_INFO(&LOG_SAMPLE, "dropped %s", path != NULL ? path : "(forgotten)");
        }
        break;
    default:
        NPH_LOG_INFO(&LOG_SAMPLE, "event %s", nph_window_event_name((NphWindowEventType)e->type));
        break;
    }
}

static NphResult bind_actions(NphInput *input, Actions *a)
{
    NPH_TRY(nph_input_action_register(input, "quit", &a->quit));
    NPH_TRY(nph_input_action_register(input, "fullscreen", &a->fullscreen));
    NPH_TRY(nph_input_action_register(input, "fire", &a->fire));
    NPH_TRY(nph_input_action_register(input, "jump", &a->jump));
    NPH_TRY(nph_input_action_register(input, "move_x", &a->move_x));
    NPH_TRY(nph_input_action_register(input, "move_y", &a->move_y));
    NPH_TRY(nph_input_action_register(input, "look_x", &a->look_x));
    NPH_TRY(nph_input_action_register(input, "look_y", &a->look_y));
    NPH_TRY(nph_input_bind_key(input, a->quit, NPH_KEY_ESCAPE, NPH_MOD_ANY));
    NPH_TRY(
        nph_input_bind_gamepad_button(input, a->quit, NPH_GAMEPAD_ANY, NPH_GAMEPAD_BUTTON_START));
    NPH_TRY(nph_input_bind_key(input, a->fullscreen, NPH_KEY_F11, NPH_MOD_ANY));
    NPH_TRY(nph_input_bind_mouse_button(input, a->fire, NPH_MOUSE_BUTTON_LEFT, NPH_MOD_ANY));
    NPH_TRY(nph_input_bind_gamepad_button(input, a->fire, NPH_GAMEPAD_ANY,
                                          NPH_GAMEPAD_BUTTON_RIGHT_BUMPER));
    NPH_TRY(nph_input_bind_key(input, a->jump, NPH_KEY_SPACE, NPH_MOD_ANY));
    NPH_TRY(nph_input_bind_gamepad_button(input, a->jump, NPH_GAMEPAD_ANY, NPH_GAMEPAD_BUTTON_A));
    NPH_TRY(nph_input_bind_key_axis(input, a->move_x, NPH_KEY_A, NPH_KEY_D));
    NPH_TRY(nph_input_bind_key_axis(input, a->move_y, NPH_KEY_S, NPH_KEY_W));
    NPH_TRY(nph_input_bind_gamepad_axis(input, a->move_x, NPH_GAMEPAD_ANY, NPH_GAMEPAD_AXIS_LEFT_X,
                                        1.0F, 0.15F));
    NPH_TRY(nph_input_bind_gamepad_axis(input, a->move_y, NPH_GAMEPAD_ANY, NPH_GAMEPAD_AXIS_LEFT_Y,
                                        -1.0F, 0.15F));
    NPH_TRY(nph_input_bind_mouse_axis(input, a->look_x, NPH_MOUSE_AXIS_X, 0.1F));
    NPH_TRY(nph_input_bind_mouse_axis(input, a->look_y, NPH_MOUSE_AXIS_Y, 0.1F));
    NPH_TRY(nph_input_bind_gamepad_axis(input, a->look_x, NPH_GAMEPAD_ANY, NPH_GAMEPAD_AXIS_RIGHT_X,
                                        1.0F, 0.15F));
    NPH_TRY(nph_input_bind_gamepad_axis(input, a->look_y, NPH_GAMEPAD_ANY, NPH_GAMEPAD_AXIS_RIGHT_Y,
                                        1.0F, 0.15F));
    return NPH_RESULT_OK;
}

static void log_action_edges(const NphInput *input, const Actions *a)
{
    const NphActionId digital[] = {a->quit, a->fullscreen, a->fire, a->jump};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(digital); ++i) {
        if (nph_input_action_pressed(input, digital[i])) {
            NPH_LOG_INFO(&LOG_SAMPLE, "action '%s' pressed",
                         nph_input_action_name(input, digital[i]));
        }
        if (nph_input_action_released(input, digital[i])) {
            NPH_LOG_INFO(&LOG_SAMPLE, "action '%s' released",
                         nph_input_action_name(input, digital[i]));
        }
    }
    const NphActionId axes[] = {a->move_x, a->move_y, a->look_x, a->look_y};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(axes); ++i) {
        if (nph_input_action_down(input, axes[i])) {
            NPH_LOG_DEBUG(&LOG_SAMPLE, "axis '%s' = %.3f", nph_input_action_name(input, axes[i]),
                          (f64)nph_input_action_value(input, axes[i]));
        }
    }
}

int main(int argc, char **argv)
{
    u64 max_frames = 0U; // 0: until closed
    bool fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--frames=", 9U) == 0) {
            max_frames = strtoull(argv[i] + 9, NULL, 10);
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else {
            fprintf(stderr, "usage: platform_window [--frames=N] [--fullscreen]\n");
            return 2;
        }
    }

    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = nph_allocator_system();
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_DEBUG) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        return 1;
    }

    const NphCpuInfo *cpu = nph_cpu_info();
    NPH_LOG_INFO(&LOG_SAMPLE, "%s: %u cores / %u threads, L3 %u MiB, simd %s", cpu->brand,
                 cpu->physical_cores, cpu->logical_cores, cpu->l3_size >> 20U,
                 nph_simd_level_name(cpu->simd_level));

    int status = 0;
    NphWindow *window = NULL;
    NphInput *input = NULL;
    const NphResult ws = nph_window_system_init();
    if (ws != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "no window system available (%s)", nph_result_string(ws));
        status = 1;
        goto done;
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "window backend: %s, %u monitor(s)", nph_window_system_backend(),
                 nph_monitor_count());
    for (u32 i = 0; i < nph_monitor_count(); ++i) {
        NphMonitorInfo m;
        if (nph_monitor_get_info(i, &m) == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_SAMPLE, "  monitor %u: %s %ux%u @ %u Hz, scale %.2f%s", i, m.name,
                         m.width, m.height, m.refresh_rate_hz, (f64)m.scale_x,
                         m.primary ? " (primary)" : "");
        }
    }

    NphWindowDesc desc;
    nph_window_desc_default(&desc);
    desc.title = "Nepheline platform_window";
    desc.fullscreen = fullscreen;
    if (nph_window_create(nph_allocator_system(), &desc, &window) != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "window creation failed");
        status = 1;
        goto done;
    }
    Actions actions;
    if (nph_input_create(nph_allocator_system(), NULL, &input) != NPH_RESULT_OK ||
        bind_actions(input, &actions) != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "input setup failed");
        status = 1;
        goto done;
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "window open; Escape quits, F11 toggles fullscreen, WASD/left "
                              "stick move, mouse/right stick look");

    u64 frame = 0U;
    const NphStopwatch run = nph_stopwatch_start();
    while (!nph_window_should_close(window) && (max_frames == 0U || frame < max_frames)) {
        nph_window_poll_events();
        nph_input_begin_frame(input);
        NphWindowEvent e;
        while (nph_window_pop_event(window, &e)) {
            log_event(window, &e);
            nph_input_process_event(input, &e);
        }
        const u32 dropped = nph_window_dropped_events(window);
        if (dropped != 0U) {
            NPH_LOG_WARN(&LOG_SAMPLE, "%u events dropped", dropped);
        }
        nph_input_poll_gamepads(input);
        nph_input_end_frame(input);
        log_action_edges(input, &actions);

        if (nph_input_action_pressed(input, actions.quit)) {
            nph_window_set_should_close(window, true);
        }
        if (nph_input_action_pressed(input, actions.fullscreen)) {
            const bool now = !nph_window_is_fullscreen(window);
            if (nph_window_set_fullscreen(window, now, 0U) == NPH_RESULT_OK) {
                NPH_LOG_INFO(&LOG_SAMPLE, "fullscreen %s", now ? "on" : "off");
            }
        }
        ++frame;
        nph_thread_sleep_ns(4U * NPH_TIME_NS_PER_MS); // ~240 Hz without a renderer
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%llu frames in %.1f ms", (unsigned long long)frame,
                 nph_stopwatch_elapsed_ms(&run));

done:
    nph_input_destroy(input);
    nph_window_destroy(window);
    nph_window_system_shutdown();
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}

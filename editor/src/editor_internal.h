// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's state, shared by editor.c and the panels (panel_*.c).

#include <nepheline/profile/memory.h>
#include <nepheline/profile/profiler.h>

#include "camera.h"
#include "edit.h"
#include "editor.h"
#include "editor_log.h"
#include "history.h"
#include "imgui_c.h"
#include "selection.h"
#include "ui_platform.h"
#include "ui_renderer.h"

#define EDITOR_PATH_MAX            512U
#define EDITOR_CONSOLE_INPUT_MAX   512U
#define EDITOR_CONSOLE_LINES       4096U
#define EDITOR_CONSOLE_COMPLETIONS 64U
#define EDITOR_PROFILER_SEGMENTS   32768U
#define EDITOR_PROFILER_TIMES      512U
#define EDITOR_PROFILER_ZONES      256U
#define EDITOR_PROFILER_PASSES     64U
#define EDITOR_PROFILER_MEMORY     32U

// ImGui knows windows by their names (docking, the layout file).
#define EDITOR_WINDOW_VIEWPORT  "Viewport"
#define EDITOR_WINDOW_CONSOLE   "Console"
#define EDITOR_WINDOW_PROFILER  "Profiler"
#define EDITOR_WINDOW_STATS     "Stats"
#define EDITOR_WINDOW_OUTLINER  "Outliner"
#define EDITOR_WINDOW_INSPECTOR "Inspector"

// Dear ImGui drag-and-drop payload types.
#define EDITOR_DRAG_ENTITY "nph.entity" // an NphEntity

// The smallest viewport image, in pixels a side.
#define EDITOR_VIEWPORT_MIN 16U

typedef struct EditorViewport {
    NphGpuImage image; // the scene's target
    u32 width;         // pixels
    u32 height;
    // Camera drags, begun on the image and held until the button is up.
    bool fly;   // right button: mouse look, WASD and QE to move
    bool orbit; // Alt and the left button
    bool pan;   // middle button
    // Playing with the input going to the game (a click on the image while
    // playing); Escape gives it back.
    bool game_input;
    bool cursor_captured; // the window's cursor is disabled for a fly drag
    bool resize_failed;   // logged once
    bool shown;           // the image was drawn last frame, at `rect`
    f32 rect[4];          // x, y, width, height in display units
} EditorViewport;

typedef struct EditorConsole {
    EditorLogLines lines; // the log as the panel shows it
    char input[EDITOR_CONSOLE_INPUT_MAX];
    i32 history_pos; // -1: a new line, else an index into the console's history
    bool show[NPH_LOG_LEVEL_COUNT];
    char filter[64];
    bool auto_scroll;
    bool focus_input;
    u64 seen; // lines pushed when the panel last drew (scrolls on news)
} EditorConsole;

typedef struct EditorProfilerPanel {
    NphProfileSegment *segments; // EDITOR_PROFILER_SEGMENTS
    u32 segment_count;
    NphProfilerFrame frame; // the frame the timeline shows
    bool has_frame;
    bool paused; // keep the frame captured when paused
    f32 times[EDITOR_PROFILER_TIMES];
    u32 time_count;
    NphProfilerZoneStats zones[EDITOR_PROFILER_ZONES];
    NphRenderPassStats passes[EDITOR_PROFILER_PASSES];
    NphMemoryStats memory[EDITOR_PROFILER_MEMORY];
    f32 ms_width; // timeline zoom: the milliseconds the panel's width shows
    char trace_path[EDITOR_PATH_MAX];
} EditorProfilerPanel;

typedef struct EditorPanels {
    bool viewport;
    bool console;
    bool profiler;
    bool stats;
    bool outliner;
    bool inspector;
    bool demo;    // Dear ImGui's demo window (development builds)
    bool metrics; // Dear ImGui's metrics window
} EditorPanels;

typedef enum EditorPopup {
    EDITOR_POPUP_NONE = 0,
    EDITOR_POPUP_OPEN_SCENE,
    EDITOR_POPUP_SAVE_SCENE,
    EDITOR_POPUP_UNSAVED, // "discard the unsaved changes?" before `pending`
} EditorPopup;

// What waits for the answer about unsaved changes.
typedef enum EditorPending {
    EDITOR_PENDING_NONE = 0,
    EDITOR_PENDING_NEW,
    EDITOR_PENDING_OPEN,
    EDITOR_PENDING_QUIT,
} EditorPending;

struct Editor {
    NphAllocator *alloc;
    EditorLogSink *log_sink;
    char shader_dir[EDITOR_PATH_MAX];

    // Attached.
    NphEngine *engine;
    NphGpuDevice *dev;
    ImGuiContext *imgui;
    NphTrackedAllocator imgui_memory; // what Dear ImGui allocates
    bool imgui_memory_tracked;        // shown in the profiler's memory table
    ImGuiMemAllocFunc prev_alloc;     // Dear ImGui's allocator before attach
    ImGuiMemFreeFunc prev_free;
    void *prev_alloc_user;
    UiPlatform platform;
    UiRenderer *renderer;
    u64 last_frame_ns;
    bool build_layout;  // no saved layout: build the default one
    u32 focus_defaults; // frames left to bring the default tabs forward after a build
    char layout_path[EDITOR_PATH_MAX];
    NphCvar cv_layout;
    bool cvars_registered;

    // The UI.
    EditorPanels show;
    EditorViewport viewport;
    EditorCamera camera;
    EditorConsole console;
    EditorProfilerPanel profiler;
    EditorPopup popup_request; // opened at the top level, after the menus
    char scene_path[EDITOR_PATH_MAX];
    char popup_error[160];
    char cvar_filter[64];

    // Editing.
    EditorHistory *history; // over the edited world
    EditorSelection selection;
    char doc_path[EDITOR_PATH_MAX]; // the scene's file; "" while untitled
    u64 doc_saved_state;            // the history's state when last saved or opened
    char title[EDITOR_PATH_MAX + 64U];
    EditorPending pending;
    bool quit_confirmed; // the window may close with unsaved changes
    char outliner_filter[64];
    NphEntity outliner_primary; // the primary selection the outliner last showed
    bool outliner_reveal;       // open the tree down to a newly selected entity
    NphEntity renaming;         // renamed in place in the outliner
    char rename_buf[NPH_TEXT_CAPACITY];
    bool rename_focus;
};

// editor.c
// Starts playing a copy of the edited world, or stops.
void editor_play_toggle(Editor *ed);
// Takes the input back from the game (Escape, or play ending).
void editor_release_game_input(Editor *ed);

// Selection actions (editor.c): each is one undoable step.
void editor_delete_selection(Editor *ed);
void editor_duplicate_selection(Editor *ed);

// document.c
void editor_update_title(Editor *ed);

// panel_outliner.c
void editor_panel_outliner(Editor *ed);
// Renames `e` in place in the outliner.
void editor_begin_rename(Editor *ed, NphEntity e);

// panel_inspector.c
void editor_panel_inspector(Editor *ed);

// panel_viewport.c
// Points the editor camera at the selection.
void editor_viewport_frame_selection(Editor *ed);
void editor_viewport_free(Editor *ed);
void editor_panel_viewport(Editor *ed);
// Sets the engine's view for the frame: the editor's camera while editing.
void editor_viewport_apply_view(Editor *ed);

// panel_console.c
NPH_MUST_USE NphResult editor_console_init(Editor *ed);
void editor_console_free(Editor *ed);
void editor_panel_console(Editor *ed);

// panel_profiler.c
NPH_MUST_USE NphResult editor_profiler_panel_init(Editor *ed);
void editor_profiler_panel_free(Editor *ed);
void editor_panel_profiler(Editor *ed);

// panel_stats.c
void editor_panel_stats(Editor *ed);

// SPDX-License-Identifier: Apache-2.0
// The editor: its lifecycle, the engine's hooks, and the UI's frame. See
// editor.h; the panels are in panel_*.c.
#include <nepheline/core/assert.h>
#include <nepheline/core/profile.h>
#include <nepheline/gpu/device.h>
#include <nepheline/gpu/resources.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/input.h>
#include <nepheline/platform/time.h>

#include "editor_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef NPH_SHADER_DIR
#error "the editor is built with NPH_SHADER_DIR (nph_target_use_shaders)"
#endif

#define EDITOR_DEFAULT_LAYOUT "nph_editor_layout.ini"

// ---------------------------------------------------------------------------
// Dear ImGui's memory, through a tracked allocator (CLAUDE.md §1.4: all
// allocation goes through Nepheline allocators). ImGui frees without a
// size, so each block carries its size in front.
// ---------------------------------------------------------------------------

#define IMGUI_BLOCK_HEADER 16U

static void *imgui_alloc(size_t size, void *user)
{
    NphAllocator *a = nph_tracked_allocator((NphTrackedAllocator *)user);
    const u64 total = (u64)size + IMGUI_BLOCK_HEADER;
    u8 *block = (u8 *)NPH_ALLOC(a, total, 16U);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &total, sizeof total);
    return block + IMGUI_BLOCK_HEADER;
}

static void imgui_free(void *ptr, void *user)
{
    if (ptr == NULL) {
        return;
    }
    NphAllocator *a = nph_tracked_allocator((NphTrackedAllocator *)user);
    u8 *block = (u8 *)ptr - IMGUI_BLOCK_HEADER;
    u64 total = 0U;
    memcpy(&total, block, sizeof total);
    NPH_FREE(a, block, total, 16U);
}

// ---------------------------------------------------------------------------
// Play controls
// ---------------------------------------------------------------------------

void editor_release_game_input(Editor *ed)
{
    if (!ed->viewport.game_input) {
        return;
    }
    ed->viewport.game_input = false;
    // The game never sees the releases of keys held now: drop them, as a
    // lost focus does.
    NphWindowEvent lost;
    memset(&lost, 0, sizeof lost);
    lost.type = (u32)NPH_WINDOW_EVENT_FOCUS;
    lost.time_ns = nph_time_now_ns();
    lost.u.focus.focused = 0;
    nph_input_process_event(nph_engine_input(ed->engine), &lost);
    NphWindow *w = nph_engine_window(ed->engine);
    if (w != NULL) {
        nph_window_set_cursor_mode(w, NPH_CURSOR_MODE_NORMAL);
    }
}

void editor_play_toggle(Editor *ed)
{
    NphEngine *e = ed->engine;
    if (nph_engine_is_playing(e)) {
        editor_release_game_input(ed);
        nph_engine_play_end(e);
        NPH_LOG_INFO(&LOG_EDITOR, "stopped playing");
        return;
    }
    const NphResult r = nph_engine_play_begin(e, true);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "play: %s", nph_result_string(r));
        return;
    }
    NPH_LOG_INFO(&LOG_EDITOR, "playing a copy of the scene (click the viewport to give the game "
                              "the input, Escape to take it back)");
}

static void play_menu_items(Editor *ed)
{
    NphEngine *e = ed->engine;
    const bool playing = nph_engine_is_playing(e);
    if (igMenuItem_Bool(playing ? "Stop" : "Play", "F5", false, true)) {
        editor_play_toggle(ed);
    }
    if (igMenuItem_Bool("Pause", "F6", nph_engine_is_paused(e), playing)) {
        nph_engine_set_paused(e, !nph_engine_is_paused(e));
    }
    if (igMenuItem_Bool("Step", "F7", false, playing && nph_engine_is_paused(e))) {
        nph_engine_step_once(e);
    }
}

// ---------------------------------------------------------------------------
// Edits and the scene's file
// ---------------------------------------------------------------------------

static void undo(Editor *ed)
{
    const NphResult r = editor_history_undo(ed->history);
    if (r != NPH_RESULT_OK && r != NPH_RESULT_EMPTY) {
        NPH_LOG_ERROR(&LOG_EDITOR, "undo: %s", nph_result_string(r));
    }
}

static void redo(Editor *ed)
{
    const NphResult r = editor_history_redo(ed->history);
    if (r != NPH_RESULT_OK && r != NPH_RESULT_EMPTY) {
        NPH_LOG_ERROR(&LOG_EDITOR, "redo: %s", nph_result_string(r));
    }
}

void editor_delete_selection(Editor *ed)
{
    if (nph_engine_is_playing(ed->engine) || ed->selection.count == 0U) {
        return;
    }
    const NphResult r = editor_edit_delete(ed->history, ed->selection.items, ed->selection.count);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "delete: %s", nph_result_string(r));
    }
    editor_selection_clear(&ed->selection);
}

void editor_duplicate_selection(Editor *ed)
{
    if (nph_engine_is_playing(ed->engine) || ed->selection.count == 0U) {
        return;
    }
    NphEntity copies[EDITOR_SELECTION_MAX];
    const u32 n = ed->selection.count;
    const NphResult r = editor_edit_duplicate(ed->history, ed->selection.items, n, copies);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "duplicate: %s", nph_result_string(r));
        return;
    }
    editor_selection_clear(&ed->selection);
    for (u32 i = 0; i < n; ++i) {
        editor_selection_add(&ed->selection, copies[i]);
    }
}

static void run_pending(Editor *ed, EditorPending pending)
{
    switch (pending) {
    case EDITOR_PENDING_NEW: {
        const NphResult r = editor_new_scene(ed);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_EDITOR, "new scene: %s", nph_result_string(r));
        }
        break;
    }
    case EDITOR_PENDING_OPEN:
        ed->popup_request = EDITOR_POPUP_OPEN_SCENE;
        break;
    case EDITOR_PENDING_QUIT:
        ed->quit_confirmed = true;
        nph_engine_quit(ed->engine);
        break;
    case EDITOR_PENDING_NONE:
    default:
        break;
    }
}

// Runs `pending` now, or after asking about the unsaved changes.
static void ask_then(Editor *ed, EditorPending pending)
{
    if (editor_scene_dirty(ed)) {
        ed->pending = pending;
        ed->popup_request = EDITOR_POPUP_UNSAVED;
    } else {
        run_pending(ed, pending);
    }
}

// Saves to the scene's file, or asks for one while untitled.
static void save(Editor *ed)
{
    if (ed->doc_path[0] == '\0') {
        ed->popup_request = EDITOR_POPUP_SAVE_SCENE;
        return;
    }
    NPH_IGNORE_RESULT(editor_save_scene(ed, NULL)); // logged
}

static void shortcuts(Editor *ed)
{
    const ImGuiIO *io = igGetIO_Nil();
    if (io->WantTextInput) {
        return;
    }
    NphEngine *e = ed->engine;
    const bool editing = !nph_engine_is_playing(e);
    if (igIsKeyPressed_Bool(ImGuiKey_F5, false)) {
        editor_play_toggle(ed);
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F6, false) && !editing) {
        nph_engine_set_paused(e, !nph_engine_is_paused(e));
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F7, false) && nph_engine_is_paused(e)) {
        nph_engine_step_once(e);
    }
    if (io->KeyCtrl) {
        if (igIsKeyPressed_Bool(ImGuiKey_S, false)) {
            if (io->KeyShift) {
                ed->popup_request = EDITOR_POPUP_SAVE_SCENE;
            } else {
                save(ed);
            }
        }
        if (editing && igIsKeyPressed_Bool(ImGuiKey_N, false)) {
            ask_then(ed, EDITOR_PENDING_NEW);
        }
        if (editing && igIsKeyPressed_Bool(ImGuiKey_O, false)) {
            ask_then(ed, EDITOR_PENDING_OPEN);
        }
        if (editing && igIsKeyPressed_Bool(ImGuiKey_Z, true)) {
            if (io->KeyShift) {
                redo(ed);
            } else {
                undo(ed);
            }
        }
        if (editing && igIsKeyPressed_Bool(ImGuiKey_Y, true)) {
            redo(ed);
        }
        if (editing && igIsKeyPressed_Bool(ImGuiKey_D, false)) {
            editor_duplicate_selection(ed);
        }
        return;
    }
    if (editing && igIsKeyPressed_Bool(ImGuiKey_Delete, false)) {
        editor_delete_selection(ed);
    }
    if (editing && igIsKeyPressed_Bool(ImGuiKey_F2, false)) {
        editor_begin_rename(ed, ed->selection.primary);
    }
    if (!ed->viewport.fly && igIsKeyPressed_Bool(ImGuiKey_F, false)) {
        editor_viewport_frame_selection(ed);
    }
    if (editing && !ed->viewport.fly) {
        if (igIsKeyPressed_Bool(ImGuiKey_W, false)) {
            ed->gizmo_mode = EDITOR_GIZMO_TRANSLATE;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_E, false)) {
            ed->gizmo_mode = EDITOR_GIZMO_ROTATE;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_R, false)) {
            ed->gizmo_mode = EDITOR_GIZMO_SCALE;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_X, false)) {
            ed->gizmo_local = !ed->gizmo_local;
        }
    }
}

// ---------------------------------------------------------------------------
// The layout, the menus, the scene popups
// ---------------------------------------------------------------------------

static void dockspace(Editor *ed)
{
    const ImGuiID id = igGetID_Str("nph.editor.dockspace");
    const ImGuiViewport *vp = igGetMainViewport();
    if (ed->build_layout || igDockBuilderGetNode(id) == NULL) {
        // The outliner on the left, the inspector and the statistics on the
        // right, the console and the profiler below, the viewport in the
        // middle.
        ed->build_layout = false;
        ed->focus_defaults = 4U; // the windows exist from the next frame
        igDockBuilderRemoveNode(id);
        (void)igDockBuilderAddNode(id, (ImGuiDockNodeFlags)ImGuiDockNodeFlags_DockSpace);
        igDockBuilderSetNodeSize(id, vp->WorkSize);
        ImGuiID center = id;
        const ImGuiID left = igDockBuilderSplitNode(center, ImGuiDir_Left, 0.18F, NULL, &center);
        const ImGuiID right = igDockBuilderSplitNode(center, ImGuiDir_Right, 0.28F, NULL, &center);
        const ImGuiID bottom = igDockBuilderSplitNode(center, ImGuiDir_Down, 0.28F, NULL, &center);
        igDockBuilderDockWindow(EDITOR_WINDOW_VIEWPORT, center);
        igDockBuilderDockWindow(EDITOR_WINDOW_OUTLINER, left);
        igDockBuilderDockWindow(EDITOR_WINDOW_INSPECTOR, right);
        igDockBuilderDockWindow(EDITOR_WINDOW_STATS, right);
        igDockBuilderDockWindow(EDITOR_WINDOW_CONSOLE, bottom);
        igDockBuilderDockWindow(EDITOR_WINDOW_PROFILER, bottom);
        igDockBuilderFinish(id);
    }
    (void)igDockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_None, NULL);
}

static void main_menu(Editor *ed)
{
    if (!igBeginMainMenuBar()) {
        return;
    }
    NphEngine *e = ed->engine;
    const bool playing = nph_engine_is_playing(e);
    if (igBeginMenu("File", true)) {
        if (igMenuItem_Bool("New Scene", "Ctrl+N", false, !playing)) {
            ask_then(ed, EDITOR_PENDING_NEW);
        }
        if (igMenuItem_Bool("Open Scene...", "Ctrl+O", false, !playing)) {
            ask_then(ed, EDITOR_PENDING_OPEN);
        }
        if (igMenuItem_Bool("Save Scene", "Ctrl+S", false, true)) {
            save(ed);
        }
        if (igMenuItem_Bool("Save Scene As...", "Ctrl+Shift+S", false, true)) {
            ed->popup_request = EDITOR_POPUP_SAVE_SCENE;
        }
        igSeparator();
        if (igMenuItem_Bool("Quit", NULL, false, true)) {
            ask_then(ed, EDITOR_PENDING_QUIT);
        }
        igEndMenu();
    }
    if (igBeginMenu("Edit", true)) {
        const char *u = editor_history_undo_label(ed->history);
        const char *r = editor_history_redo_label(ed->history);
        char label[EDITOR_HISTORY_LABEL_MAX + 8U];
        (void)snprintf(label, sizeof label, "Undo %s", u != NULL ? u : "");
        if (igMenuItem_Bool(label, "Ctrl+Z", false, !playing && u != NULL)) {
            undo(ed);
        }
        (void)snprintf(label, sizeof label, "Redo %s", r != NULL ? r : "");
        if (igMenuItem_Bool(label, "Ctrl+Y", false, !playing && r != NULL)) {
            redo(ed);
        }
        igSeparator();
        const bool selected = ed->selection.count > 0U;
        if (igMenuItem_Bool("Create Entity", NULL, false, !playing)) {
            NphEntity created = NPH_ENTITY_NULL;
            if (editor_edit_create(ed->history, "Entity", NPH_ENTITY_NULL, NULL, &created) ==
                NPH_RESULT_OK) {
                editor_selection_set(&ed->selection, created);
            }
        }
        if (igMenuItem_Bool("Duplicate", "Ctrl+D", false, !playing && selected)) {
            editor_duplicate_selection(ed);
        }
        if (igMenuItem_Bool("Delete", "Del", false, !playing && selected)) {
            editor_delete_selection(ed);
        }
        if (igMenuItem_Bool("Rename", "F2", false, !playing && selected)) {
            editor_begin_rename(ed, ed->selection.primary);
        }
        igSeparator();
        if (igMenuItem_Bool("Frame Selection", "F", false, selected)) {
            editor_viewport_frame_selection(ed);
        }
        igEndMenu();
    }
    if (igBeginMenu("View", true)) {
        (void)igMenuItem_BoolPtr(EDITOR_WINDOW_VIEWPORT, NULL, &ed->show.viewport, true);
        (void)igMenuItem_BoolPtr(EDITOR_WINDOW_OUTLINER, NULL, &ed->show.outliner, true);
        (void)igMenuItem_BoolPtr(EDITOR_WINDOW_INSPECTOR, NULL, &ed->show.inspector, true);
        (void)igMenuItem_BoolPtr(EDITOR_WINDOW_CONSOLE, NULL, &ed->show.console, true);
        (void)igMenuItem_BoolPtr(EDITOR_WINDOW_PROFILER, NULL, &ed->show.profiler, true);
        (void)igMenuItem_BoolPtr(EDITOR_WINDOW_STATS, NULL, &ed->show.stats, true);
        igSeparator();
        (void)igMenuItem_BoolPtr("Grid", NULL, &ed->show_grid, true);
        igSeparator();
        if (igMenuItem_Bool("Reset Layout", NULL, false, true)) {
            ed->build_layout = true;
            ed->show.viewport = true;
            ed->show.outliner = true;
            ed->show.inspector = true;
            ed->show.console = true;
            ed->show.profiler = true;
            ed->show.stats = true;
        }
        igSeparator();
        (void)igMenuItem_BoolPtr("Dear ImGui Metrics", NULL, &ed->show.metrics, true);
#if !NPH_CONFIG_SHIPPING
        (void)igMenuItem_BoolPtr("Dear ImGui Demo", NULL, &ed->show.demo, true);
#endif
        igEndMenu();
    }
    if (igBeginMenu("Play", true)) {
        play_menu_items(ed);
        igEndMenu();
    }
    const NphEngineFrameStats *st = nph_engine_frame_stats(e);
    char text[96];
    (void)snprintf(text, sizeof text, "%s  %.2f ms", playing ? "PLAYING" : "EDITING", st->frame_ms);
    const ImVec2_c size = igCalcTextSize(text, NULL, false, -1.0F);
    const ImVec2_c avail = igGetContentRegionAvail();
    if (avail.x > size.x) {
        igSetCursorPosX(igGetCursorPosX() + avail.x - size.x);
        igTextUnformatted(text, NULL);
    }
    igEndMainMenuBar();
}

static void scene_popup(Editor *ed, const char *title, bool open)
{
    if (!igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    igTextUnformatted(open ? "The scene file to open, text or binary:"
                           : "The file to save the edited scene to, as text:",
                      NULL);
    if (igIsWindowAppearing()) {
        igSetKeyboardFocusHere(0);
    }
    igSetNextItemWidth(igGetFontSize() * 30.0F);
    const bool enter = igInputText("##path", ed->scene_path, sizeof ed->scene_path,
                                   ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    if (ed->popup_error[0] != '\0') {
        igTextColored(ui_vec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", ed->popup_error);
    }
    const ImVec2_c button = ui_vec2(igGetFontSize() * 6.0F, 0.0F);
    if (igButton(open ? "Open" : "Save", button) || enter) {
        const NphResult r =
            open ? editor_open_scene(ed, ed->scene_path) : editor_save_scene(ed, ed->scene_path);
        if (r == NPH_RESULT_OK) {
            igCloseCurrentPopup();
        } else {
            (void)snprintf(ed->popup_error, sizeof ed->popup_error, "%s", nph_result_string(r));
        }
    }
    igSameLine(0.0F, -1.0F);
    if (igButton("Cancel", button) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void unsaved_popup(Editor *ed)
{
    if (!igBeginPopupModal("Unsaved Changes", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    igTextUnformatted("The scene has unsaved changes.", NULL);
    const ImVec2_c button = ui_vec2(igGetFontSize() * 6.0F, 0.0F);
    const EditorPending pending = ed->pending;
    if (igButton("Save", button)) {
        ed->pending = EDITOR_PENDING_NONE;
        igCloseCurrentPopup();
        if (ed->doc_path[0] == '\0') {
            ed->popup_request = EDITOR_POPUP_SAVE_SCENE; // untitled: save first, then go on
        } else if (editor_save_scene(ed, NULL) == NPH_RESULT_OK) {
            run_pending(ed, pending);
        }
    }
    igSameLine(0.0F, -1.0F);
    if (igButton("Discard", button)) {
        ed->pending = EDITOR_PENDING_NONE;
        igCloseCurrentPopup();
        run_pending(ed, pending);
    }
    igSameLine(0.0F, -1.0F);
    if (igButton("Cancel", button) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        ed->pending = EDITOR_PENDING_NONE;
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void scene_popups(Editor *ed)
{
    if (ed->popup_request != EDITOR_POPUP_NONE) {
        const char *name = "Unsaved Changes";
        if (ed->popup_request == EDITOR_POPUP_OPEN_SCENE) {
            name = "Open Scene";
        } else if (ed->popup_request == EDITOR_POPUP_SAVE_SCENE) {
            name = "Save Scene";
            if (ed->doc_path[0] != '\0') {
                (void)snprintf(ed->scene_path, sizeof ed->scene_path, "%s", ed->doc_path);
            }
        }
        (void)igOpenPopup_Str(name, 0);
        ed->popup_error[0] = '\0';
        ed->popup_request = EDITOR_POPUP_NONE;
    }
    scene_popup(ed, "Open Scene", true);
    scene_popup(ed, "Save Scene", false);
    unsaved_popup(ed);
}

// ---------------------------------------------------------------------------
// The engine's hooks
// ---------------------------------------------------------------------------

static bool hook_event(NphEngine *e, const NphWindowEvent *ev, void *user)
{
    Editor *ed = (Editor *)user;
    // Closing the window with unsaved changes asks first.
    if (ev->type == (u32)NPH_WINDOW_EVENT_CLOSE && ed->imgui != NULL && !ed->quit_confirmed &&
        editor_scene_dirty(ed)) {
        NphWindow *w = nph_engine_window(e);
        if (w != NULL) {
            nph_window_set_should_close(w, false);
        }
        ask_then(ed, EDITOR_PENDING_QUIT);
        return true;
    }
    return editor_feed_event(ed, ev);
}

static void hook_world_replaced(NphEngine *e, NphWorld *old_world, NphWorld *new_world, void *user)
{
    NPH_UNUSED(old_world);
    NPH_UNUSED(new_world);
    Editor *ed = (Editor *)user;
    // Play starting or ending swaps the world shown, not the edited one. A
    // game reload that changed component layouts rebuilds the edited world:
    // the history's component ids and values may no longer fit it.
    if (ed->history != NULL && editor_history_world(ed->history) != nph_engine_edit_world(e)) {
        NPH_LOG_INFO(&LOG_EDITOR, "the edited world was rebuilt by a game reload: the undo "
                                  "history starts over");
        editor_history_rebind(ed->history, nph_engine_edit_world(e));
    }
}

static void hook_sync(NphEngine *e, void *user)
{
    NPH_UNUSED(e);
    Editor *ed = (Editor *)user;
    if (ed->imgui == NULL) {
        return;
    }
    NPH_PROFILE_BEGIN("editor.ui");
    if (ed->log_sink != NULL) {
        (void)editor_log_sink_drain(ed->log_sink, &ed->console.lines);
    }
    if (!nph_engine_is_playing(ed->engine)) {
        editor_release_game_input(ed);
    }
    editor_selection_prune(&ed->selection, nph_engine_world(ed->engine));
    const u64 now = nph_time_now_ns();
    ui_platform_new_frame(&ed->platform, nph_time_ns_to_s(now - ed->last_frame_ns));
    ed->last_frame_ns = now;
    igNewFrame();
    dockspace(ed);
    if (ed->focus_defaults > 0U) {
        // A dock node shows the tab of the window with the keyboard focus,
        // and one window has it per frame: the inspector, then the console,
        // then the viewport (which keeps it), a frame each, before the
        // panels draw their tabs.
        static const char *const order[3] = {EDITOR_WINDOW_VIEWPORT, EDITOR_WINDOW_CONSOLE,
                                             EDITOR_WINDOW_INSPECTOR};
        --ed->focus_defaults;
        if (ed->focus_defaults < 3U) {
            igSetWindowFocus_Str(order[ed->focus_defaults]);
        }
    }
    main_menu(ed);
    shortcuts(ed);
    if (ed->show.viewport) {
        editor_panel_viewport(ed);
    }
    if (ed->show.outliner) {
        editor_panel_outliner(ed);
    }
    if (ed->show.inspector) {
        editor_panel_inspector(ed);
    }
    if (ed->show.console) {
        editor_panel_console(ed);
    }
    if (ed->show.profiler) {
        editor_panel_profiler(ed);
    }
    if (ed->show.stats) {
        editor_panel_stats(ed);
    }
    if (ed->show.metrics) {
        igShowMetricsWindow(&ed->show.metrics);
    }
#if !NPH_CONFIG_SHIPPING
    if (ed->show.demo) {
        igShowDemoWindow(&ed->show.demo);
    }
#endif
    scene_popups(ed);
    editor_viewport_apply_view(ed);
    editor_update_title(ed);
    igRender();
    NPH_PROFILE_END();
}

static bool hook_scene_target(NphEngine *e, NphGpuImage *out, void *user)
{
    NPH_UNUSED(e);
    const Editor *ed = (const Editor *)user;
    if (ed->imgui == NULL || ed->viewport.image.value == 0U) {
        return false;
    }
    *out = ed->viewport.image;
    return true;
}

static void hook_render(NphEngine *e, NphGpuRenderGraph *graph, NphGpuRgImage backbuffer,
                        NphGpuRgImage scene, void *user)
{
    Editor *ed = (Editor *)user;
    ImDrawData *dd = ed->imgui != NULL ? igGetDrawData() : NULL;
    if (dd == NULL) {
        return;
    }
    NphGpuRgImage target = backbuffer;
    if (nph_gpu_rg_image_is_null(target)) {
        target = nph_gpu_rg_import_image(graph, nph_engine_offscreen_target(e));
    }
    NphResult r = ui_renderer_prepare(ed->renderer, dd);
    if (r == NPH_RESULT_OK) {
        // The scene is in the viewport's image unless the viewport has none
        // yet, when it is in the backbuffer the UI pass clears.
        const bool sample_scene = !nph_gpu_rg_image_is_null(scene) && scene.value != target.value;
        r = ui_renderer_draw(ed->renderer, graph, target, sample_scene ? &scene : NULL,
                             sample_scene ? 1U : 0U);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "the UI was not drawn: %s", nph_result_string(r));
    }
}

void editor_engine_hooks(Editor *ed, NphEngineHooks *out)
{
    NPH_ASSERT(ed != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->user = ed;
    out->event = hook_event;
    out->sync = hook_sync;
    out->scene_target = hook_scene_target;
    out->render = hook_render;
    out->world_replaced = hook_world_replaced;
}

bool editor_feed_event(Editor *ed, const NphWindowEvent *ev)
{
    NPH_ASSERT(ed != NULL && ev != NULL);
    if (ed->imgui == NULL) {
        return false;
    }
    if (ed->viewport.game_input) {
        // The game has the input. Escape takes it back; focus and the
        // cursor leaving still reach the UI so its state stays right.
        if (ev->type == (u32)NPH_WINDOW_EVENT_KEY && ev->u.key.key == (u32)NPH_KEY_ESCAPE &&
            ev->u.key.action == (u32)NPH_KEY_ACTION_PRESS) {
            editor_release_game_input(ed);
            return true;
        }
        if (ev->type == (u32)NPH_WINDOW_EVENT_FOCUS ||
            ev->type == (u32)NPH_WINDOW_EVENT_MOUSE_ENTER) {
            (void)ui_platform_event(&ed->platform, ev);
        }
        return false;
    }
    (void)ui_platform_event(&ed->platform, ev);
    // Everything else is the editor's: the game sees input only while it
    // has it.
    return ev->type != (u32)NPH_WINDOW_EVENT_FOCUS;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult editor_create(const EditorDesc *desc, Editor **out)
{
    NPH_ASSERT(desc != NULL && desc->alloc != NULL && out != NULL);
    *out = NULL;
    Editor *ed = NPH_ALLOC_TYPE(desc->alloc, Editor);
    if (ed == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    ed->alloc = desc->alloc;
    ed->log_sink = desc->log;
    (void)snprintf(ed->shader_dir, sizeof ed->shader_dir, "%s",
                   desc->shader_dir != NULL ? desc->shader_dir : NPH_SHADER_DIR);
    editor_camera_default(&ed->camera);
    ed->show.viewport = true;
    ed->show.outliner = true;
    ed->show.inspector = true;
    ed->show.console = true;
    ed->show.profiler = true;
    ed->show.stats = true;
    ed->show_grid = true;
    (void)snprintf(ed->scene_path, sizeof ed->scene_path, "scene.nscene");
    *out = ed;
    return NPH_RESULT_OK;
}

static NphResult register_cvars(Editor *ed)
{
    const NphCvar layout = {
        .name = "editor.layout",
        .description = "the file the editor keeps its window layout in (empty: none)",
        .type = NPH_CVAR_TYPE_STRING,
        .flags = NPH_CVAR_FLAG_SAVED,
        .value_string = EDITOR_DEFAULT_LAYOUT,
        .default_string = EDITOR_DEFAULT_LAYOUT,
    };
    ed->cv_layout = layout;
    NPH_TRY(nph_cvar_register(nph_engine_cvars(ed->engine), &ed->cv_layout));
    ed->cvars_registered = true;
    return NPH_RESULT_OK;
}

static void setup_imgui(Editor *ed)
{
    ImGuiIO *io = igGetIO_Nil();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io->BackendFlags |=
        ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io->BackendRendererName = "nepheline";
    // Drags inside a window are the camera's and the widgets', not moves.
    io->ConfigWindowsMoveFromTitleBarOnly = true;
    const NphStr layout = nph_cvar_get_string(&ed->cv_layout);
    (void)snprintf(ed->layout_path, sizeof ed->layout_path, "%.*s", (int)layout.len, layout.data);
    io->IniFilename = ed->layout_path[0] != '\0' ? ed->layout_path : NULL;
    ed->build_layout = io->IniFilename == NULL || !nph_fs_exists(io->IniFilename);
    igStyleColorsDark(NULL);
    NphWindow *w = nph_engine_window(ed->engine);
    if (w != NULL) {
        f32 sx = 1.0F;
        f32 sy = 1.0F;
        nph_window_get_content_scale(w, &sx, &sy);
        if (sx > 1.0F) {
            ImGuiStyle *style = igGetStyle();
            ImGuiStyle_ScaleAllSizes(style, sx);
            style->FontScaleDpi = sx;
        }
    }
}

static NphResult attach(Editor *ed, NphEngine *e)
{
    ed->engine = e;
    ed->dev = nph_engine_device(e);
    NPH_TRY(register_cvars(ed));

    igGetAllocatorFunctions(&ed->prev_alloc, &ed->prev_free, &ed->prev_alloc_user);
    nph_tracked_allocator_init(&ed->imgui_memory, ed->alloc, "editor.imgui");
    igSetAllocatorFunctions(imgui_alloc, imgui_free, &ed->imgui_memory);
    ed->imgui = igCreateContext(NULL);
    if (ed->imgui == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    igSetCurrentContext(ed->imgui);
    setup_imgui(ed);

    NphWindow *w = nph_engine_window(e);
    NPH_TRY(ui_platform_init(&ed->platform, ed->alloc, w));
    NphGpuFormat format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    NphGpuSwapchain *sc = nph_engine_swapchain(e);
    if (sc != NULL) {
        format = nph_gpu_swapchain_format(sc);
    } else {
        const NphGpuImageDesc *d = nph_gpu_image_desc(ed->dev, nph_engine_offscreen_target(e));
        NPH_ASSERT(d != NULL);
        format = d->format;
        ui_platform_set_display_size(&ed->platform, d->width, d->height);
    }
    NPH_TRY(ui_renderer_create(ed->dev, ed->alloc, ed->shader_dir, format, &ed->renderer));
    NPH_TRY(editor_console_init(ed));
    NPH_TRY(editor_profiler_panel_init(ed));
    EditorHistoryDesc hd;
    memset(&hd, 0, sizeof hd);
    hd.alloc = ed->alloc;
    hd.world = nph_engine_edit_world(e);
    NPH_TRY(editor_history_create(&hd, &ed->history));
    ed->doc_saved_state = editor_history_state(ed->history);
    NphProfiler *prof = nph_engine_profiler(e);
    if (prof != NULL && nph_profiler_track_memory(prof, &ed->imgui_memory) == NPH_RESULT_OK) {
        ed->imgui_memory_tracked = true;
    }
    ed->last_frame_ns = nph_time_now_ns();
    return NPH_RESULT_OK;
}

NphResult editor_attach(Editor *ed, NphEngine *engine)
{
    NPH_ASSERT(ed != NULL && engine != NULL && ed->engine == NULL);
    if (nph_engine_mode(engine) == NPH_ENGINE_MODE_HEADLESS) {
        return NPH_RESULT_UNSUPPORTED;
    }
    const NphResult r = attach(ed, engine);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "the editor could not start: %s", nph_result_string(r));
        editor_detach(ed);
        return r;
    }
    NPH_LOG_INFO(&LOG_EDITOR, "editor ready (Dear ImGui %s)", igGetVersion());
    return NPH_RESULT_OK;
}

void editor_detach(Editor *ed)
{
    if (ed == NULL || ed->engine == NULL) {
        return;
    }
    NphEngine *e = ed->engine;
    if (ed->dev != NULL) {
        NPH_IGNORE_RESULT(nph_engine_wait(e));
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(ed->dev));
    }
    if (ed->imgui != NULL) {
        editor_release_game_input(ed);
        nph_engine_set_view(e, NULL);
    }
    if (ed->imgui_memory_tracked) {
        nph_profiler_untrack_memory(nph_engine_profiler(e), &ed->imgui_memory);
        ed->imgui_memory_tracked = false;
    }
    if (ed->imgui != NULL) {
        ui_renderer_destroy(ed->renderer); // the textures ImGui holds first
        ed->renderer = NULL;
        ui_platform_shutdown(&ed->platform);
        igDestroyContext(ed->imgui); // saves the layout (io.IniFilename)
        ed->imgui = NULL;
        igSetAllocatorFunctions(ed->prev_alloc, ed->prev_free, ed->prev_alloc_user);
        const u64 leaked =
            nph_atomic_load_u64(&ed->imgui_memory.allocations, NPH_MEMORY_ORDER_RELAXED);
        if (leaked != 0U) {
            NPH_LOG_WARN(&LOG_EDITOR, "Dear ImGui left %llu allocations behind",
                         (unsigned long long)leaked);
        }
    }
    editor_viewport_free(ed);
    editor_console_free(ed);
    editor_profiler_panel_free(ed);
    // Before the edited world goes: the history releases what it kept.
    editor_history_destroy(ed->history);
    ed->history = NULL;
    editor_selection_clear(&ed->selection);
    if (ed->cvars_registered) {
        (void)nph_cvar_unregister(nph_engine_cvars(e), &ed->cv_layout);
        ed->cvars_registered = false;
    }
    ed->engine = NULL;
    ed->dev = NULL;
}

void editor_destroy(Editor *ed)
{
    if (ed == NULL) {
        return;
    }
    editor_detach(ed);
    NPH_FREE_TYPE(ed->alloc, Editor, ed);
}

void editor_set_display_size(Editor *ed, u32 width, u32 height)
{
    NPH_ASSERT(ed != NULL);
    ui_platform_set_display_size(&ed->platform, width, height);
}

NphGpuImage editor_viewport_image(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->viewport.image;
}

bool editor_viewport_rect(const Editor *ed, f32 *out_x, f32 *out_y, f32 *out_w, f32 *out_h)
{
    NPH_ASSERT(ed != NULL && out_x != NULL && out_y != NULL && out_w != NULL && out_h != NULL);
    if (!ed->viewport.shown) {
        return false;
    }
    *out_x = ed->viewport.rect[0];
    *out_y = ed->viewport.rect[1];
    *out_w = ed->viewport.rect[2];
    *out_h = ed->viewport.rect[3];
    return true;
}

EditorCamera *editor_camera(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return &ed->camera;
}

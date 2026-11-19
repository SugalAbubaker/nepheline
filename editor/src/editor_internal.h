// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's state, shared by editor.c and the panels (panel_*.c).

#include <nepheline/platform/file.h>
#include <nepheline/profile/memory.h>
#include <nepheline/profile/profiler.h>

#include "camera.h"
#include "edit.h"
#include "editor.h"
#include "editor_log.h"
#include "gizmo.h"
#include "history.h"
#include "imgui_c.h"
#include "picking.h"
#include "project.h"
#include "selection.h"
#include "ui_platform.h"
#include "ui_renderer.h"
#include "view.h"

#include <cook/material.h>
#include <cook/preview.h>

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
#define EDITOR_WINDOW_ASSETS    "Assets"
#define EDITOR_WINDOW_MATERIAL  "Material"

// Dear ImGui drag-and-drop payload types.
#define EDITOR_DRAG_ENTITY   "nph.entity"   // an NphEntity
#define EDITOR_DRAG_ASSET    "nph.asset"    // a scene's or a mesh's source key, NUL-terminated
#define EDITOR_DRAG_MATERIAL "nph.material" // a material's source key, NUL-terminated
#define EDITOR_DRAG_IMAGE    "nph.image"    // an image's source key, NUL-terminated

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

typedef enum EditorPlaceKind {
    EDITOR_PLACE_SCENES = 0, // the scenes the request cooked
    EDITOR_PLACE_MESH,       // an entity drawing `mesh` (a new primitive)
} EditorPlaceKind;

// What goes into the edited world once a request is done, and where: for an
// import, the point under the mouse where files were dropped, worked out at
// the next sync point while the camera is as it was.
typedef struct EditorPlacement {
    u32 id;        // the project's request; 0: a free slot
    bool place;    // false: dropped outside the viewport, nothing is placed
    bool resolved; // `at` holds the point
    f32 mouse[2];  // display units, until resolved
    NphDVec3 at;
    u32 kind;     // EditorPlaceKind
    NphGuid mesh; // EDITOR_PLACE_MESH: the mesh asset
    char name[EDITOR_IMPORT_NAME_MAX];
} EditorPlacement;

typedef struct EditorAssetEntry {
    char name[NPH_DIR_ENTRY_NAME_MAX];
    bool is_directory;
    bool cooked; // the cooker's database has assets of it
    u32 kind;    // CookerSourceKind
} EditorAssetEntry;

// The asset browser (panel_assets.c).
typedef struct EditorAssets {
    char folder[EDITOR_PROJECT_PATH_MAX]; // shown, relative to the sources ("": their top)
    EditorAssetEntry *entries;            // the folder's, folders first
    u32 entry_count;
    u32 entry_cap;
    u64 generation; // the project's when listed
    u64 listed_ns;
    bool stale;  // list again
    bool reveal; // bring the panel forward: an import started
    char selected[NPH_DIR_ENTRY_NAME_MAX];
    EditorPlacement placements[EDITOR_PROJECT_REQUESTS];
} EditorAssets;

// What the gizmo moves (gizmo_ui.c): the selected entity, or the vertices
// of the mesh being edited (mesh_edit.c).
typedef struct EditorGizmoTarget {
    NphWorldTransform transform; // where it stands now: its pivot and axes
    u64 id;                      // a drag ends when the target is another
    // A drag: begin (false refuses it), apply for every move with the
    // transform the gizmo made of the one it started from, end.
    bool (*begin)(Editor *ed, void *user);
    void (*apply)(Editor *ed, const NphWorldTransform *start, const NphWorldTransform *moved,
                  void *user);
    void (*end)(Editor *ed, void *user);
    void *user;
} EditorGizmoTarget;

typedef NPH_ARRAY(CookPolyMesh) EditorMeshEditSnapshots;
typedef NPH_ARRAY(u8) EditorFlagArray;

// Mesh editing (mesh_edit.c).
typedef struct EditorMeshEdit {
    bool active;
    NphEntity entity;
    char key[EDITOR_PROJECT_PATH_MAX]; // the source
    char name[EDITOR_IMPORT_NAME_MAX];
    NphGuid asset; // the cooked mesh
    CookPolyMesh mesh;
    EditorMeshSelect mode;
    EditorFlagArray selected;     // per vertex or face, by the mode
    EditorMeshEditSnapshots undo; // the mesh before each change
    EditorMeshEditSnapshots redo;
    bool changed; // since editing began
    u64 revision; // counts the changes, for autosave (autosave.c)
    // A gizmo drag: the vertices it moves, where they and their centre began.
    NphArrayU32 drag_vertices;
    CookVec3Array drag_start;
    NphVec3 drag_center;
    EditorFlagArray seen; // scratch: a flag per position, kept to spare an allocation a frame
    // The mesh as edited, drawn in place of the cooked one.
    NphRenderMesh preview;
    NphRenderInstance preview_instance;
    bool preview_stale;
    // The preview's materials (ADR-0105): for each slot its faces use (`preview_slots`, a bit a
    // slot), the material it draws (the entity's own, else the one the mesh names) held in the
    // scene renderer (NPH_SCENE_MATERIAL_NONE for none), its GUID, and the render material its
    // instance was made with.
    u32 preview_slots;
    u32 preview_refs[COOK_POLYMESH_MATERIALS];
    NphGuid preview_guids[COOK_POLYMESH_MATERIALS];
    NphRenderMaterial preview_drawn[COOK_POLYMESH_MATERIALS];
    // After editing: the cook that brings the cooked mesh up to date, and
    // the reload that brings it back in view.
    bool committing;
    bool commit_done;
    bool commit_ok;
    bool reloading;
    u32 commit;
    NphRenderInstance stale_instance; // the entity's instance before the reload
} EditorMeshEdit;

typedef NPH_ARRAY(CookMaterialSource) EditorMaterialSnapshots;

// An image of the project, for a texture slot's list.
typedef struct EditorImageKey {
    char key[COOK_MATERIAL_IMAGE_MAX];
} EditorImageKey;
typedef NPH_ARRAY(EditorImageKey) EditorImageKeys;

// The material panel (material_edit.c, panel_material.c; ADR-0105 decision 5):
// one material's source, drawn as it is changed and written as each change
// settles, with a history of its own while it is open.
typedef struct EditorMaterialEdit {
    bool open;
    char key[EDITOR_PROJECT_PATH_MAX]; // its source
    NphGuid guid;                      // the cooked material (cooker_asset_guid)
    CookMaterialSource source;         // as shown and drawn
    CookMaterialSource before;         // the source before the change being made
    bool changing;                     // a change not settled yet: drawn, not written
    bool panel_changing;               // and the panel's widgets made it: they settle it
    EditorMaterialSnapshots undo;      // the source before each settled change
    EditorMaterialSnapshots redo;
    bool live_dirty;   // the source changed: drawn at the next sync point
    bool live_pending; // the renderer could not take the source yet (loading): again next frame
    u32 cook;          // the last cook asked for (0: none)
    bool cook_wanted;  // a cook the project had no room for: asked again next frame
    char error[192];   // why the last write or cook failed; "" when none did
    bool focused;      // the panel's window had the keyboard focus last frame
    bool reveal;       // bring the panel's window forward
    u64 revision;      // counts the settled changes
    // The project's images, for the texture slots' lists: listed when one opens.
    EditorImageKeys images;
} EditorMaterialEdit;

// Material previews (material_preview.c; ADR-0105 decision 7): a lit sphere of each material the
// asset browser or the material panel shows, made on jobs, cached under the cooked directory.
#define EDITOR_PREVIEWS_MAX       256U // pictures kept (64 KB of GPU memory each)
#define EDITOR_PREVIEWS_IN_FLIGHT 4U   // made at once

typedef struct EditorPreviewJob EditorPreviewJob; // material_preview.c

typedef struct EditorPreview {
    char key[EDITOR_PROJECT_PATH_MAX]; // the material's source; "" for an entry not in use
    u64 wanted;                        // the key of the content to show (0: it is not cooked)
    u64 checked;                       // the project's generation `wanted` was looked up at
    u64 made;                          // the key the picture was made for (0: no picture yet)
    u64 failed; // the key a making failed for: not tried again until the content changes
    u64 used;   // the frame it was last asked for
    NphGpuImage image;
    EditorPreviewJob *job; // making it, or NULL
} EditorPreview;

typedef struct EditorPreviews {
    CookPreviewStudio *studio; // made on first use
    EditorPreview *items;      // EDITOR_PREVIEWS_MAX, allocated on first use
    u64 frame;
    u32 in_flight;
    u32 drawn;  // pictures drawn since the editor attached
    u32 loaded; // pictures read from the cache
} EditorPreviews;

typedef struct EditorPanels {
    bool viewport;
    bool console;
    bool profiler;
    bool stats;
    bool outliner;
    bool inspector;
    bool assets;
    bool material;
    bool demo;    // Dear ImGui's demo window (development builds)
    bool metrics; // Dear ImGui's metrics window
} EditorPanels;

typedef enum EditorPopup {
    EDITOR_POPUP_NONE = 0,
    EDITOR_POPUP_OPEN_SCENE,
    EDITOR_POPUP_SAVE_SCENE,
    EDITOR_POPUP_UNSAVED, // "discard the unsaved changes?" before `pending`
    EDITOR_POPUP_RECOVER, // a session that did not end cleanly left work (autosave.c)
} EditorPopup;

// This session's autosave (autosave.c; docs/editor.md "Autosave and recovery").
typedef struct EditorAutosave {
    char dir[EDITOR_PATH_MAX]; // the session's directory; "" when autosave is off
    NphFileLock lock;          // held while the editor runs: its directory is alive
    u64 started_wall_ns;       // when the session began (UTC)
    u64 saved_wall_ns;         // when it last autosaved (UTC); 0 before
    u64 last_ns;               // the monotonic time of the last autosave, or of the start
    u64 saved_state;           // the history's state the scene's autosave holds
    u64 mesh_revision;         // the edited mesh's revision its autosave holds
    bool has_scene;            // scene.nscene holds unsaved work
    bool has_mesh;             // mesh.nmesh holds an edited mesh's changes
    u32 count;                 // autosaves written
    f64 last_ms;               // what the last one took
    u32 last_entities;         // the entities it held
} EditorAutosave;

// The work a session that did not end cleanly left, offered back once at
// start (autosave.c).
typedef struct EditorRecovery {
    bool offered;                // waits for Recover, Discard, or Later
    char dir[EDITOR_PATH_MAX];   // its session's directory
    NphFileLock lock;            // held while offered, so that no other editor offers it too
    char scene[EDITOR_PATH_MAX]; // the file its scene was for; "" untitled
    char mesh_key[EDITOR_PROJECT_PATH_MAX]; // the source of a mesh it was editing
    bool has_scene;
    bool has_mesh;
    bool scene_newer; // the scene's file was written after the autosave
    u64 saved_wall_ns;
} EditorRecovery;

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
    NphCvar cv_assets;
    NphCvar cv_cooked;
    NphCvar cv_autosave_dir;
    NphCvar cv_autosave_interval;
    bool cvars_registered;
    f32 mouse[2]; // the cursor, display units, from the window's events

    // The project (panel_assets.c). The resolver reads `project` from any
    // thread that loads by GUID, under `resolve_lock`.
    EditorProject *project;
    NphMutex resolve_lock;
    EditorAssets assets;
    EditorMeshEdit mesh_edit;
    EditorMaterialEdit material;
    EditorPreviews previews;
    EditorAutosave autosave;
    EditorRecovery recovery;

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

    // The viewport's gizmo and overlays.
    EditorGizmoMode gizmo_mode;
    bool gizmo_local;
    EditorGizmoHandle gizmo_hover;
    bool gizmo_dragging;
    EditorGizmoDrag gizmo_drag;
    EditorGizmoTarget gizmo_target; // the drag's
    NphEntity gizmo_entity;
    NphLocalTransform gizmo_start_local;
    u64 gizmo_drag_id; // a merge key per drag
    bool show_grid;
    bool show_colliders; // every Collider's outline (the selection's always)
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

// autosave.c (docs/editor.md "Autosave and recovery")
// Begins this session's autosave under editor.autosave_dir and looks for
// work an editor that did not end cleanly left, which it offers (attach).
NPH_MUST_USE NphResult editor_autosave_open(Editor *ed);
// The name of a new session's directory that no directory under `root`
// holds: `<seconds>-<pid>`, then `-<n>` after it. A session attached again in
// this process within the second would otherwise take over the last one's
// directory, a crashed one's work with it. OVERFLOW when the name or the path
// does not fit, BUSY when a thousand names are held.
NPH_MUST_USE NphResult editor_autosave_session_name(const char *root, u64 seconds, u64 pid,
                                                    char *out, u64 cap);
// Ends this session: its directory goes. An offer not yet answered stays
// for the next start (detach).
void editor_autosave_close(Editor *ed);
// At the sync point: autosaves when there are changes the last autosave
// does not hold and editor.autosave_interval seconds passed since it.
void editor_autosave_update(Editor *ed, u64 now_ns);
// Autosaves now, whatever the interval, the changes the last autosave does
// not hold (before Play, after a recovery); nothing when there are none.
// `why` is for the log.
NPH_MUST_USE NphResult editor_autosave_now(Editor *ed, const char *why);
// The document has no unsaved changes any more (saved, opened, new): the
// scene's autosave holds nothing to recover and goes.
void editor_autosave_settled(Editor *ed);
// The offered work: Recover opens it in place of the scene, unsaved, under
// the scene's file, and puts the mesh back in its source; Discard removes
// it; Later keeps it for the next start.
NPH_MUST_USE NphResult editor_recovery_recover(Editor *ed);
void editor_recovery_discard(Editor *ed);
void editor_recovery_later(Editor *ed);
// The "Recover Work" popup (within the frame's UI).
void editor_recovery_popup(Editor *ed);

// panel_outliner.c
void editor_panel_outliner(Editor *ed);
// Renames `e` in place in the outliner.
void editor_begin_rename(Editor *ed, NphEntity e);

// panel_inspector.c
void editor_panel_inspector(Editor *ed);

// panel_assets.c
// The project from editor.assets and editor.cooked, its cooked files
// mounted at the root of the VFS (attach).
NPH_MUST_USE NphResult editor_assets_open(Editor *ed);
// Stops the imports and releases the project and the browser (detach).
void editor_assets_close(Editor *ed);
// At the sync point: works out where dropped files go, and places the
// scenes of finished imports.
void editor_assets_update(Editor *ed);
// Places the scene cooked from the source `key` at `at`, cooking it first
// when it has not been.
void editor_assets_place(Editor *ed, const char *key, NphDVec3 at);
void editor_panel_assets(Editor *ed);

// mesh_edit.c
void editor_mesh_edit_init(Editor *ed);
// Ends editing without saving and drops the preview (detach).
void editor_mesh_edit_free(Editor *ed);
// At the sync point: the preview follows the entity and is rebuilt when the
// mesh changed; editing ends when another entity is selected or play
// begins; a finished commit hands back to the cooked mesh.
void editor_mesh_edit_update(Editor *ed);
// A request of the project finished (after its assets were reloaded).
void editor_mesh_edit_finished(Editor *ed, const EditorImportResult *res);
// The viewport while editing: the gizmo on the selection and picking
// elements. True when the gizmo has the mouse.
bool editor_mesh_edit_viewport(Editor *ed, const EditorView *v, bool hovered, ImDrawList *dl);
// The face of the mesh being edited that the ray through (x, y) meets first, or false.
bool editor_mesh_edit_pick_face(Editor *ed, const EditorView *v, f32 x, f32 y, u32 *out_face);
// The mesh's edges, vertices or face centres, and the selection, as debug
// lines.
void editor_mesh_edit_overlays(Editor *ed, const EditorView *v);
void editor_mesh_edit_toolbar(Editor *ed);
// Extrudes the selected faces by a quarter of their mean size.
void editor_mesh_edit_extrude_selection(Editor *ed);
void editor_mesh_edit_select_all(Editor *ed);

// material_edit.c (docs/editor.md "Materials")
void editor_material_init(Editor *ed);
// Closes the material (a change being made settles) and frees the panel's
// arrays (detach, destroy).
void editor_material_free(Editor *ed);
// At the sync point: a source the renderer could not take yet, and a cook
// the project had no room for, are tried again.
void editor_material_update(Editor *ed);
// A request of the project finished (after its assets were reloaded).
void editor_material_finished(Editor *ed, const EditorImportResult *res);
// True when the panel's window has the keyboard focus: undo and redo are its.
bool editor_material_focused(const Editor *ed);
// Lists the project's images into ed->material.images, by key.
void editor_material_list_images(Editor *ed);

// panel_material.c
void editor_panel_material(Editor *ed);

// material_preview.c (docs/editor.md "Assets")
void editor_material_previews_init(Editor *ed);
// Waits for the pictures being made and lets every picture go (detach: before the project
// closes, while the engine's jobs and device are there).
void editor_material_previews_free(Editor *ed);
// At the sync point: pictures made are shown, and the next asked for begun.
void editor_material_previews_update(Editor *ed);
// The preview of the material whose source is `key`, as a texture for the UI (0 while it has
// none): its picture as last made, made again on jobs when the material or its images are cooked
// from other content.
ImTextureID editor_material_preview(Editor *ed, const char *key);

// gizmo_ui.c
// Shows the gizmo on the primary selection and drags it. True when it has
// the mouse (a handle under it, or a drag): the viewport then does not pick.
bool editor_gizmo_update(Editor *ed, const EditorView *v, bool image_hovered, ImDrawList *dl);
// As editor_gizmo_update for any target; NULL: none (a drag ends).
bool editor_gizmo_drive(Editor *ed, const EditorView *v, bool image_hovered, ImDrawList *dl,
                        const EditorGizmoTarget *target);

// panel_viewport.c
// Points the editor camera at the selection.
void editor_viewport_frame_selection(Editor *ed);
void editor_viewport_free(Editor *ed);
void editor_panel_viewport(Editor *ed);
// Sets the engine's view for the frame: the editor's camera while editing.
void editor_viewport_apply_view(Editor *ed);
// The world point under (x, y), display units, over the viewport's image as
// last shown: the nearest surface there, else the ground plane, else a
// point at the pivot's distance. False when (x, y) is not over the image.
bool editor_viewport_point(Editor *ed, f32 x, f32 y, NphDVec3 *out);

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

// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor (CLAUDE.md §8.13; docs/editor.md; docs/adr/0025-editor-ui.md):
// a host of the engine (runtime/engine.h) that draws a docked Dear ImGui
// interface over the engine's frame, through the engine's hooks:
//
//   - event: window events go to ImGui; the game sees them only while it
//     has the input (playing, after a click in the viewport; Escape gives
//     it back);
//   - sync: at the sync point, where the world is quiescent, the editor
//     builds its UI (menus, the viewport, the console, the profiler, the
//     statistics) and sets the view: its own camera while editing, the
//     world's while playing;
//   - scene_target: the scene renders into the viewport's image, sized to
//     the viewport panel;
//   - render: the UI pass draws into the backbuffer, sampling that image.
//
// An editor is made in steps, because the engine needs the hooks and the
// asset resolver before the editor can use the engine:
//
//     editor_create(&desc, &ed);
//     editor_engine_desc(ed, &engine_desc);
//     nph_engine_create(&engine_desc, &engine);
//     editor_attach(ed, engine);
//     ... nph_engine_run(engine) ...
//     editor_detach(ed);           // before the engine goes
//     nph_engine_destroy(engine);
//     editor_destroy(ed);
//
// Until attached and after detaching, the hooks do nothing. An OFFSCREEN
// engine works too (tests): the UI draws into the engine's offscreen
// target, and input arrives through editor_feed_event.
//
// One editor at a time per process: Dear ImGui's allocator hooks are
// process-wide, and the editor installs its own while attached.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY: the engine's thread.

#include <nepheline/runtime/engine.h>

#include "camera.h"
#include "editor_log.h"
#include "project.h"

#include <cook/polymesh.h>

typedef struct Editor Editor;

typedef struct EditorDesc {
    NphAllocator *alloc;    // required
    EditorLogSink *log;     // the sink feeding the console panel (NULL: none)
    const char *shader_dir; // the editor's shaders (NULL: this build's)
} EditorDesc;

// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult editor_create(const EditorDesc *desc, Editor **out);

// What the engine is created with: the hooks (`user` the editor), and the
// asset resolver that names the files of the project's cooked assets by
// GUID (editor_resolve). The other fields are left as they are.
void editor_engine_desc(Editor *ed, NphEngineDesc *desc);

// The engine's asset resolver (asset/loader.h NphAssetResolveFn, `user` the
// editor): the project's (project.h editor_project_resolve) while attached,
// NPH_RESULT_NOT_FOUND otherwise. NPH_THREADSAFE.
NphResult editor_resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len);

// Brings the editor up on `engine` (WINDOWED or OFFSCREEN): its cvars,
// Dear ImGui and its backends, and the viewport. NPH_RESULT_UNSUPPORTED for
// a HEADLESS engine; the shaders' (NOT_FOUND, IO_ERROR) and the device's
// errors. Nothing is left attached on failure.
NPH_MUST_USE NphResult editor_attach(Editor *ed, NphEngine *engine);

// Waits for the GPU, saves the layout (editor.layout), and releases what
// attach made. Safe when not attached.
void editor_detach(Editor *ed);

// Safe on NULL. Detaches first.
void editor_destroy(Editor *ed);

// Feeds a window event as the engine's event hook does; for an offscreen
// editor, which has no window. Returns whether the editor consumed it.
bool editor_feed_event(Editor *ed, const NphWindowEvent *ev);

// The display size of an offscreen editor (the engine's offscreen target
// size by default), in pixels.
void editor_set_display_size(Editor *ed, u32 width, u32 height);

// The viewport's scene image (null before the first frame lays it out).
NphGpuImage editor_viewport_image(const Editor *ed);

// Where the last frame showed the viewport's image, in display units, for
// tests that drive the editor with events. False before it was shown.
bool editor_viewport_rect(const Editor *ed, f32 *out_x, f32 *out_y, f32 *out_w, f32 *out_h);

// The editor's camera, which frames render from while not playing.
EditorCamera *editor_camera(Editor *ed);

// ---------------------------------------------------------------------------
// The scene being edited (docs/editor.md "Scenes")
// ---------------------------------------------------------------------------

// Clears the edited world and the history: an untitled scene.
// NPH_RESULT_INVALID_STATE while playing. The UI asks before discarding
// unsaved changes; these do not.
NPH_MUST_USE NphResult editor_new_scene(Editor *ed);

// Replaces the edited world with a scene file (text or binary). The file is
// loaded into a scratch world first: one that does not load leaves the
// scene as it was. NPH_RESULT_INVALID_STATE while playing; the file's and
// the loader's errors (logged).
NPH_MUST_USE NphResult editor_open_scene(Editor *ed, const char *path);

// Saves the edited world as a text scene to `path` (NULL: the scene's
// file), which becomes the scene's file. NPH_RESULT_INVALID_ARGUMENT for
// NULL while untitled; the saver's errors.
NPH_MUST_USE NphResult editor_save_scene(Editor *ed, const char *path);

// Unsaved changes: the history's state differs from the one saved or opened
// (undoing back to it reads as clean).
bool editor_scene_dirty(const Editor *ed);

// The scene's file, or "" while untitled.
const char *editor_scene_path(const Editor *ed);

// ---------------------------------------------------------------------------
// Assets (docs/editor.md "Assets")
// ---------------------------------------------------------------------------

// Imports files as dropping them on the window does: they are copied into
// the asset browser's folder and cooked on the project's import thread
// (project.h editor_project_import). With `place`, the scenes among them
// then join the edited world at the point under (x, y), display units,
// when that is over the viewport's image, each as one undoable step and
// selected: the point is worked out at the next frame, and the scenes
// placed at the first frame outside play after the import is done.
// NPH_RESULT_INVALID_STATE when not attached; NPH_RESULT_FULL with too
// many imports outstanding; the errors of editor_project_import.
NPH_MUST_USE NphResult editor_import(Editor *ed, const char *const *paths, u32 count, bool place,
                                     f32 x, f32 y);

// The project, NULL when not attached (tests wait for its imports).
EditorProject *editor_project(Editor *ed);

// ---------------------------------------------------------------------------
// Model creation (docs/editor.md "Model creation")
// ---------------------------------------------------------------------------

typedef enum EditorPrimitive {
    EDITOR_PRIMITIVE_CUBE = 0, // 1 m
    EDITOR_PRIMITIVE_SPHERE,   // 1 m across, 32 segments by 16 rings
    EDITOR_PRIMITIVE_PLANE,    // 2 m square
    EDITOR_PRIMITIVE_CYLINDER, // 1 m across and tall, 32 segments
    EDITOR_PRIMITIVE_COUNT
} EditorPrimitive;

// Creates a primitive: an editable mesh source of its own in the project
// (<sources>/meshes/<cube|sphere|plane|cylinder>[_N].nmesh, the first free
// name), cooked on the import thread, then an entity drawing it, resting on
// the point in front of the camera. The entity is one undoable step, is
// selected, and appears once the cook is done (a frame or two).
// NPH_RESULT_INVALID_STATE when not attached or while playing; FULL with too
// many requests outstanding; the file system's and the project's errors.
NPH_MUST_USE NphResult editor_create_primitive(Editor *ed, EditorPrimitive kind);

// ---------------------------------------------------------------------------
// Mesh editing (docs/editor.md "Mesh editing")
// ---------------------------------------------------------------------------

typedef enum EditorMeshSelect {
    EDITOR_MESH_VERTICES = 0,
    EDITOR_MESH_FACES,
} EditorMeshSelect;

// Edits the mesh the primary selection draws, when it is cooked from an
// editable mesh (.nmesh) of the project: the source is read and drawn, as
// it changes, in place of the cooked mesh. NPH_RESULT_INVALID_STATE while
// playing or when not attached; NPH_RESULT_NOT_FOUND when nothing is
// selected or the entity draws no such mesh; the reader's errors.
NPH_MUST_USE NphResult editor_mesh_edit_begin(Editor *ed);

// Ends editing. A changed mesh is saved to its source and cooked again,
// and the edited version stays drawn until the scene renderer draws the
// cooked one again. Safe when not editing.
void editor_mesh_edit_end(Editor *ed);

bool editor_mesh_editing(const Editor *ed);

// The mesh being edited, or NULL.
const CookPolyMesh *editor_mesh_edit_mesh(const Editor *ed);

// Selects vertices or faces, switching to that kind of element (which
// clears the selection), adding to it with `add`.
// NPH_RESULT_INVALID_STATE when not editing; NPH_RESULT_INVALID_ARGUMENT
// for an index out of range (nothing changes).
NPH_MUST_USE NphResult editor_mesh_edit_select(Editor *ed, EditorMeshSelect mode, const u32 *items,
                                               u32 count, bool add);

// Moves the selected vertices (a selected face moves its corners) by
// `offset`, in the entity's space: one change.
NPH_MUST_USE NphResult editor_mesh_edit_move(Editor *ed, NphVec3 offset);

// Extrudes the selected faces as one region, `distance` along their
// normals (cook/polymesh.h): one change. The extruded faces stay selected.
// NPH_RESULT_INVALID_STATE without faces selected.
NPH_MUST_USE NphResult editor_mesh_edit_extrude(Editor *ed, f32 distance);

// Undoes or redoes the last change while editing; false when there is none.
bool editor_mesh_edit_undo(Editor *ed);
bool editor_mesh_edit_redo(Editor *ed);

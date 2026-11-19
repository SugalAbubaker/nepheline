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

#include <cook/material.h>
#include <cook/polymesh.h>
#include <cook/scene_import.h>
#include <cook/sound.h>
#include <cook/texture_import.h>

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

// Gives face `face` of the mesh being edited (and every selected face, when it is a selected
// one) the material whose source is `key` (ADR-0105), as one change: the faces go to the slot
// that names it, else to a slot no other face keeps (one the entity's MeshRenderer leaves to
// the mesh first), which is named for it; the material is cooked when it is not. The mesh
// draws it from then, and every entity drawing the cooked mesh, where its MeshRenderer names
// none for the slot, once editing ends. Its slot in `out_slot` (NULL: not wanted).
// NPH_RESULT_INVALID_STATE when not editing; NPH_RESULT_INVALID_ARGUMENT for a face out of
// range or a key that is not a material's source key; NPH_RESULT_FULL when other faces keep
// every slot.
NPH_MUST_USE NphResult editor_mesh_edit_face_material(Editor *ed, u32 face, const char *key,
                                                      u32 *out_slot);

// Undoes or redoes the last change while editing; false when there is none.
bool editor_mesh_edit_undo(Editor *ed);
bool editor_mesh_edit_redo(Editor *ed);

// ---------------------------------------------------------------------------
// Materials (docs/editor.md "Materials"; ADR-0105)
// ---------------------------------------------------------------------------

// The slot editor_material_assign gives every slot of the entity's mesh.
#define EDITOR_MATERIAL_EVERY_SLOT 0xFFFFFFFFU

// Opens the material whose source is `key` (a project's `.nmat`) in the
// material panel, its history empty, and brings the panel forward. The one
// open closes first: a change being made to it settles. NPH_RESULT_INVALID_STATE
// without a project; NPH_RESULT_INVALID_ARGUMENT for a key that is not a
// material's; the file's and the reader's errors (the panel stays as it was).
NPH_MUST_USE NphResult editor_material_open(Editor *ed, const char *key);

// Makes a material with the defaults (cook/material.h) in the sources' folder
// `folder` ("" their top), named "New Material", with the first number from 2
// that is free when that is taken; writes it, cooks it, and opens it. Its key
// in `out_key` (NULL: not wanted). NPH_RESULT_INVALID_STATE without a project;
// NPH_RESULT_OVERFLOW when the key does not fit; NPH_RESULT_BUSY when a hundred
// names are taken; the file's errors.
NPH_MUST_USE NphResult editor_material_new(Editor *ed, const char *folder, char *out_key, u64 cap);

// The source of the material in the panel, NULL when none is open; its key.
const CookMaterialSource *editor_material_source(const Editor *ed);
const char *editor_material_key(const Editor *ed);

// Changes the open material to `now`. It is drawn so from the next frame,
// with nothing written. When `settled` (a drag settles when it ends; a click
// settles at once), the change since the source was last settled is written
// and cooked, and is a step of the panel's history: the scene's history and
// its unsaved state do not change. NPH_RESULT_INVALID_STATE when no material
// is open; NPH_RESULT_INVALID_ARGUMENT for a source that does not validate
// (nothing changes); the file's errors and the project's.
NPH_MUST_USE NphResult editor_material_change(Editor *ed, const CookMaterialSource *now,
                                              bool settled);

// Undoes or redoes the panel's last settled change (a change being made
// settles first), written and cooked as one; false when there is none.
bool editor_material_undo(Editor *ed);
bool editor_material_redo(Editor *ed);

// Closes the panel's material: a change being made settles. Safe when none
// is open.
void editor_material_close(Editor *ed);

// Gives slot `slot` of entity `e` (EDITOR_MATERIAL_EVERY_SLOT: each slot its
// mesh has) the material whose source is `key`, as one step of the scene's
// history; cooks the material when it is not. NPH_RESULT_INVALID_STATE while
// playing or without a project; NPH_RESULT_NOT_FOUND for an entity that is
// not alive or has no MeshRenderer; NPH_RESULT_INVALID_ARGUMENT for a key
// that is not a material's or a slot past NPH_MESH_RENDERER_MATERIALS +
// NPH_MESH_MATERIALS_EXTRA; the history's errors.
NPH_MUST_USE NphResult editor_material_assign(Editor *ed, NphEntity e, u32 slot, const char *key);

// What dropping the material whose source is `key` on the viewport at (x, y)
// does (display units, over its image as last shown): the slot of the surface
// there gets it (editor_material_assign); while a mesh is being edited, the
// face there does (editor_mesh_edit_face_material). NPH_RESULT_NOT_FOUND when
// no entity (or face of the mesh being edited) is there, or (x, y) is not over
// the image; assign's errors, or the face's.
NPH_MUST_USE NphResult editor_viewport_drop_material(Editor *ed, const char *key, f32 x, f32 y);

// ---------------------------------------------------------------------------
// Import settings (docs/editor.md "Import settings"; ADR-0107)
// ---------------------------------------------------------------------------

// Opens the import settings of the source `key` (a scene, an image, or a sound, relative to the
// sources) in the Import Settings window and brings it forward: its settings file's
// (<source>.import.json), or the cooker's when it has none. A file that does not read shows the
// cooker's settings, with why (editor_import_settings_error), and Save replaces it. Changes not
// saved in the one shown are dropped. NPH_RESULT_INVALID_STATE without a project;
// NPH_RESULT_INVALID_ARGUMENT for a key of another kind of source; NPH_RESULT_NOT_FOUND for a
// source that is not there.
NPH_MUST_USE NphResult editor_import_settings_open(Editor *ed, const char *key);

// The source the window shows, "" when none; why its file did not read or did not save, "" when
// nothing is wrong; whether the settings shown are not what its file holds.
const char *editor_import_settings_key(const Editor *ed);
const char *editor_import_settings_error(const Editor *ed);
bool editor_import_settings_changed(const Editor *ed);

// The settings the window shows, by its source's kind; NULL when it shows another kind.
const CookSceneImport *editor_import_settings_scene(const Editor *ed);
const CookTextureImport *editor_import_settings_texture(const Editor *ed);
const CookSoundSettings *editor_import_settings_sound(const Editor *ed);

// Changes the settings the window shows (nothing is written). NPH_RESULT_INVALID_STATE when it
// shows another kind of source, or none; NPH_RESULT_INVALID_ARGUMENT for settings out of range.
NPH_MUST_USE NphResult editor_import_settings_change_scene(Editor *ed, const CookSceneImport *s);
NPH_MUST_USE NphResult editor_import_settings_change_texture(Editor *ed,
                                                             const CookTextureImport *s);
NPH_MUST_USE NphResult editor_import_settings_change_sound(Editor *ed, const CookSoundSettings *s);

// Writes the settings shown to the source's settings file and cooks the source again by them.
// NPH_RESULT_INVALID_STATE when no source is shown; the file's errors (logged, and shown).
NPH_MUST_USE NphResult editor_import_settings_save(Editor *ed);

// Removes the source's settings file, so it cooks again by the cooker's settings, and shows
// those. NPH_RESULT_INVALID_STATE when no source is shown; the file system's errors.
NPH_MUST_USE NphResult editor_import_settings_use_cooker(Editor *ed);

// Shows what the source's settings file holds again, dropping changes not saved.
void editor_import_settings_revert(Editor *ed);

// Closes the window's source; changes not saved are dropped. Safe when none is shown.
void editor_import_settings_close(Editor *ed);

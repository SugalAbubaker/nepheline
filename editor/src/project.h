// SPDX-License-Identifier: Apache-2.0
#pragma once

// The project (docs/editor.md "Assets"; docs/adr/0026-editor-import.md):
// two directories.
//
//   - The sources (editor.assets): what the asset browser shows and where
//     imports copy files. The cooker keys every asset by its source's path
//     relative to this directory, and the asset's GUID derives from that
//     key, so a project can move and its scenes still name the same assets.
//   - The cooked files (editor.cooked): the cooker's output and its
//     database, cook.db.json. The editor mounts the directory at the root
//     of the engine's VFS, and editor_project_resolve names the file of
//     each GUID from a snapshot of the database.
//
// Imports and cooks run on the project's import thread, one request at a
// time, so the editor keeps drawing while textures encode. The thread
// copies what lies outside the sources, cooks it through the cooker (the
// engine's job system encodes textures in parallel), saves the database,
// and swaps in a new snapshot. The main thread takes finished requests
// with editor_project_poll and places the scenes they cooked.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY, except editor_project_resolve,
// which is NPH_THREADSAFE: the loader calls it from whichever thread loads
// by GUID. The project's lock is held for one lookup, one swap, or one
// queue operation, never across a copy or a cook.

#include <nepheline/core/alloc.h>
#include <nepheline/core/guid.h>
#include <nepheline/core/result.h>
#include <nepheline/jobs/jobs.h>

typedef struct EditorProject EditorProject;

// Requests queued, running, or finished and not yet taken, at most.
#define EDITOR_PROJECT_REQUESTS 16U
// Paths one import takes.
#define EDITOR_IMPORT_PATHS_MAX 64U
// Scenes one result reports.
#define EDITOR_IMPORT_SCENES_MAX 8U
#define EDITOR_IMPORT_NAME_MAX   64U
#define EDITOR_PROJECT_PATH_MAX  1024U

typedef struct EditorProjectDesc {
    NphAllocator *alloc;    // required and thread-safe: the import thread allocates too
    const char *assets_dir; // required: the sources
    const char *cooked_dir; // required: the cooker's output
    NphJobSystem *jobs;     // optional: textures encode in parallel
} EditorProjectDesc;

typedef struct EditorImportResult {
    u32 id;           // the request's
    u64 tag;          // the caller's
    NphResult result; // the first failure (the rest of the request still ran)
    u32 cooked;       // assets written
    u32 skipped;      // assets already current
    u32 failed;       // sources that failed
    // The scenes (.gltf, .glb) the request named, in its order, with their
    // file names without the extension. A directory's scenes are cooked but
    // not named here.
    NphGuid scenes[EDITOR_IMPORT_SCENES_MAX];
    char scene_names[EDITOR_IMPORT_SCENES_MAX][EDITOR_IMPORT_NAME_MAX];
    u32 scene_count;
    // The editable meshes (.nmesh) the request named, likewise: their mesh
    // assets.
    NphGuid meshes[EDITOR_IMPORT_SCENES_MAX];
    char mesh_names[EDITOR_IMPORT_SCENES_MAX][EDITOR_IMPORT_NAME_MAX];
    u32 mesh_count;
    // Every asset the request wrote (not those found current): what the
    // editor reloads. Owned by the result: editor_project_result_free.
    NphGuid *written;
    u32 written_count;
} EditorImportResult;

// Starts the import thread (at low priority) and loads the snapshot from
// the cooked directory's database; a project without one yet is empty.
// Neither directory needs to exist. NPH_RESULT_INVALID_ARGUMENT for a
// missing field or a path too long; NPH_RESULT_OUT_OF_MEMORY; the
// thread's errors.
NPH_MUST_USE NphResult editor_project_create(const EditorProjectDesc *desc, EditorProject **out);

// Lets the request in progress finish, drops the queued ones, stops the
// thread, and releases the project. Safe on NULL.
void editor_project_destroy(EditorProject *p);

// Queues an import of `paths` (files or directories, anywhere):
//   - one outside the sources is copied into `folder` (relative to the
//     sources, "" for their top), replacing a file of the same name (a
//     re-import). A .gltf or .glb brings the files it names along (cook/gltf.h
//     cook_gltf_files), and a directory everything under it;
//   - one inside the sources is used where it is.
// Then each scene, editable mesh, and image is cooked, and the scenes and
// editable meshes under each directory. An image comes along without being cooked when the request
// holds a scene, which cooks the images it uses itself.
// NPH_RESULT_INVALID_ARGUMENT for no paths, more than
// EDITOR_IMPORT_PATHS_MAX, or a folder that is absolute or climbs out of
// the sources; NPH_RESULT_FULL with EDITOR_PROJECT_REQUESTS outstanding;
// NPH_RESULT_OUT_OF_MEMORY. `out_id` (may be NULL) receives the request's
// id, and the result carries `tag`.
NPH_MUST_USE NphResult editor_project_import(EditorProject *p, const char *const *paths, u32 count,
                                             const char *folder, u64 tag, u32 *out_id);

// Queues a cook of the source `key` (a path relative to the sources: a
// scene, an image, or a directory of scenes), even when it is current with
// `force`. The errors of editor_project_import.
NPH_MUST_USE NphResult editor_project_cook(EditorProject *p, const char *key, bool force, u64 tag,
                                           u32 *out_id);

// Takes the oldest finished request's result; false when none is waiting.
// The caller releases it (editor_project_result_free).
bool editor_project_poll(EditorProject *p, EditorImportResult *out);

// Releases what a result owns. Safe on a zero-filled result and twice.
void editor_project_result_free(EditorProject *p, EditorImportResult *res);

// Requests queued or running.
u32 editor_project_pending(EditorProject *p);

// Blocks until no request is queued or running (tests).
void editor_project_wait(EditorProject *p);

// What the import thread is doing, for the interface ("" when idle).
void editor_project_status(EditorProject *p, char *out, u64 cap);

// The scene asset cooked from the source `key`; false when it has none.
bool editor_project_scene_of(EditorProject *p, const char *key, NphGuid *out);

// The mesh asset cooked from the editable mesh `key`; false when it has none.
bool editor_project_mesh_of(EditorProject *p, const char *key, NphGuid *out);

// The source key (into `out`, `cap` bytes) and sub-asset name of the asset
// with `guid`; false when the database does not know it, or the key does
// not fit.
bool editor_project_source_of(EditorProject *p, NphGuid guid, char *out, u64 cap, char *out_sub,
                              u64 sub_cap);

// Whether the source `key` has cooked assets.
bool editor_project_is_cooked(EditorProject *p, const char *key);

// Changes whenever the snapshot does: a panel showing cooked states looks
// again.
u64 editor_project_generation(EditorProject *p);

// The loader's resolver (asset/loader.h NphAssetResolveFn, `user` the
// project): the virtual path of the asset with `guid`, its output path in
// the cooked directory, from the snapshot. NPH_RESULT_NOT_FOUND for a GUID
// the database does not know; NPH_RESULT_OVERFLOW when the path does not
// fit. NPH_THREADSAFE.
NphResult editor_project_resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len);

// The directories, canonical (cook/cooker.h cooker_canonical_path).
const char *editor_project_assets_dir(const EditorProject *p);
const char *editor_project_cooked_dir(const EditorProject *p);

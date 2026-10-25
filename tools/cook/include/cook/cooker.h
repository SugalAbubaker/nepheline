// SPDX-License-Identifier: Apache-2.0
#pragma once

// The cooker (CLAUDE.md §8.8): turns source files into cooked assets in
// an output directory, keeps the asset database there, and packs the
// results.
//
// Sources are named by their path relative to the source root (the
// "source key", '/' separators): that name, not the path a caller typed,
// is what every asset's GUID is derived from (nph_guid_from_str of
// "<key>#<sub>"), so a cook is reproducible from any working directory or
// machine, and references between assets stay stable across runs. A
// source outside the root is refused.
//
// A glTF file becomes one asset per mesh, material, and texture (one per
// image and usage) plus one scene asset; an image file becomes one
// texture asset; an editable mesh (`.nmesh`, polymesh.h) becomes one mesh
// asset; a WAV file becomes one sound asset (sound.h), with the settings
// of the JSON file beside it when there is one. Output paths mirror the
// source tree:
// `<key, its '.' made '_'>/<kind>_<index>_<name>.nph` (for example
// `models/crate_gltf/mesh_0_Crate.nph`), sanitised to [A-Za-z0-9._-];
// if sanitising ever makes two assets collide, the later one gets a
// GUID suffix. The same path, with '/' separators, is the asset's
// virtual path in a pack.
//
// Incremental cooking: an asset whose content hash and settings hash
// match the database and whose output exists is skipped; `force` cooks
// everything. A texture's content hash is its image's bytes alone, so
// editing a glTF's materials or nodes re-encodes no texture. When a
// source is cooked again, assets it no longer produces (a deleted mesh)
// leave the database and their files are removed.
//
// Thread-safety: a cooker belongs to one thread; the texture encoders
// use the job system when one is given.

#include <nepheline/asset/pack.h>
#include <nepheline/jobs/jobs.h>

#include <cook/database.h>
#include <cook/mesh.h>
#include <cook/sound.h>
#include <cook/texture.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOKER_PATH_MAX 1024U

typedef struct Cooker Cooker;

typedef struct CookerDesc {
    NphAllocator *alloc;         // required
    const char *out_dir;         // required; created when missing
    const char *source_root;     // sources are keyed relative to it; NULL: the current directory
    NphJobSystem *jobs;          // optional: parallel texture encoding
    b32 force;                   // recook everything
    CookTextureSettings texture; // defaults for textures; the usage is set per material slot
    CookMeshSettings mesh;
    CookSoundSettings sound; // defaults for sounds; a source's settings file overrides them
    // Optional: told of every asset written (not of those found current), on
    // the cooker's thread, with its GUID and NphAssetType: how the editor
    // knows what to reload.
    void (*on_cooked)(void *user, NphGuid guid, u32 type);
    void *on_cooked_user;
} CookerDesc;

typedef struct CookerStats {
    u32 cooked;  // assets written
    u32 skipped; // assets found current in the database
    u32 removed; // stale assets dropped (their source no longer produces them)
    u32 failed;  // sources that failed
} CookerStats;

// Creates the cooker and loads the database from the output directory.
// NPH_RESULT_INVALID_ARGUMENT for a missing field or a path too long.
NPH_MUST_USE NphResult cooker_create(const CookerDesc *desc, Cooker **out);

// Saves the database and releases the cooker. Safe on NULL.
void cooker_destroy(Cooker *cooker);

// What the cooker makes of a source, by its extension in any case.
typedef enum CookerSourceKind {
    COOKER_SOURCE_NONE = 0, // not a source the cooker reads
    COOKER_SOURCE_SCENE,    // .gltf and .glb
    COOKER_SOURCE_IMAGE,    // .png, .jpg, .jpeg, .tga, .bmp, .hdr, and .dds
    COOKER_SOURCE_MESH,     // .nmesh (polymesh.h)
    COOKER_SOURCE_SOUND,    // .wav (sound.h)
} CookerSourceKind;

CookerSourceKind cooker_source_kind(const char *path);

// Cooks one source by its kind: a scene, an image as a texture with the
// default usage (HDR for .hdr), an editable mesh, or a sound. A directory
// is walked (cooker_cook_directory). NPH_RESULT_UNSUPPORTED for another
// extension.
NPH_MUST_USE NphResult cooker_cook_file(Cooker *cooker, const char *source_path);

// Cooks every scene (.gltf, .glb), editable mesh (.nmesh), and sound (.wav)
// under `dir`, recursively, in name order (images are cooked through the
// scenes that use them, or one by one with cooker_cook_image). The first
// error is returned after every file was tried.
NPH_MUST_USE NphResult cooker_cook_directory(Cooker *cooker, const char *dir);

// Cooks a glTF scene with every asset it contains.
NPH_MUST_USE NphResult cooker_cook_gltf(Cooker *cooker, const char *source_path);

// Cooks an image as a texture of the given usage.
NPH_MUST_USE NphResult cooker_cook_image(Cooker *cooker, const char *source_path,
                                         CookTextureUsage usage);

// Cooks an editable mesh (polymesh.h) as a mesh asset, sub-asset "mesh":
// its faces triangulated, with normals by its smoothing angle, through the
// mesh pipeline. NPH_RESULT_INVALID_ARGUMENT for a mesh without faces; the
// reader's errors.
NPH_MUST_USE NphResult cooker_cook_mesh(Cooker *cooker, const char *source_path);

// Cooks a WAV file as a sound asset, sub-asset "sound", with the cooker's
// sound settings under those of the source's settings file
// (<source>.import.json, sound.h), whose bytes join the content hash.
// NPH_RESULT_PARSE_ERROR or NPH_RESULT_INVALID_ARGUMENT for a bad settings
// file; the WAV reader's errors; cook_sound's.
NPH_MUST_USE NphResult cooker_cook_sound(Cooker *cooker, const char *source_path);

// Writes every asset the database knows into a pack at `pack_path`, with
// the output paths as entry paths. NPH_RESULT_NOT_FOUND when an asset's
// output is missing (recook it): a pack never silently lacks an asset
// another one references.
NPH_MUST_USE NphResult cooker_pack(Cooker *cooker, const char *pack_path);

CookerStats cooker_stats(const Cooker *cooker);
const CookDb *cooker_db(const Cooker *cooker);

// The source key of `path` (see above) in `out`. NPH_RESULT_INVALID_ARGUMENT
// when the path lies outside the source root, NPH_RESULT_OVERFLOW when it
// does not fit.
NPH_MUST_USE NphResult cooker_source_key(const Cooker *cooker, const char *path, char *out,
                                         u64 cap);

// The canonical form the cooker compares paths in: absolute (a relative
// path is taken from the working directory), '/' separators, no empty,
// "." or ".." components, and no trailing separator. A Windows drive
// ("C:") or UNC prefix ("//server/share") is kept as the root, which ".."
// never climbs above. NPH_RESULT_OVERFLOW when it does not fit.
NPH_MUST_USE NphResult cooker_canonical_path(const char *path, char *out, u64 cap);

// The part of `path` below `root`, both canonical, in `out`: "" when they
// are the same directory, NPH_RESULT_INVALID_ARGUMENT when `path` is not
// under `root` (a sibling sharing a prefix included). Names compare
// without case on Windows. NPH_RESULT_OVERFLOW when it does not fit.
NPH_MUST_USE NphResult cooker_relative_path(const char *root, const char *path, char *out, u64 cap);

#ifdef __cplusplus
}
#endif

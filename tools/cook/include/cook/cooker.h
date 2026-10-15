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
// texture asset. Output paths mirror the source tree:
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

// Cooks one source by extension: .gltf and .glb as scenes, .png, .jpg,
// .jpeg, .tga, .bmp, and .hdr as textures with the default usage (HDR for
// .hdr). A directory is walked (cooker_cook_directory).
// NPH_RESULT_UNSUPPORTED for another extension.
NPH_MUST_USE NphResult cooker_cook_file(Cooker *cooker, const char *source_path);

// Cooks every .gltf and .glb file under `dir`, recursively, in name order
// (images are cooked through the scenes that use them, or one by one with
// cooker_cook_image). The first error is returned after every file was
// tried.
NPH_MUST_USE NphResult cooker_cook_directory(Cooker *cooker, const char *dir);

// Cooks a glTF scene with every asset it contains.
NPH_MUST_USE NphResult cooker_cook_gltf(Cooker *cooker, const char *source_path);

// Cooks an image as a texture of the given usage.
NPH_MUST_USE NphResult cooker_cook_image(Cooker *cooker, const char *source_path,
                                         CookTextureUsage usage);

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

#ifdef __cplusplus
}
#endif

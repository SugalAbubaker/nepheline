// SPDX-License-Identifier: Apache-2.0
#pragma once

// Import settings for scenes (docs/cook.md "Scene import settings";
// docs/adr/0107-the-blender-round-trip.md): a JSON file beside a glTF document,
// `<file>.import.json`, that the cooker reads when it cooks the document (cooker_cook_gltf). Its
// bytes join the document's content hash, so a change cooks it again.
//
//     {"scene": 1, "collision": "mesh", "hull_vertices": 32, "lods": true, "voxels": false,
//      "texture_max_extent": 2048, "texture_uncompressed": false, "texture_mips": true}
//
// Every key but "scene" may be left out; the cooker's setting then holds. "collision" is what the
// scene's meshes collide as when nothing on an object says otherwise: "none" (the default),
// "mesh" (their triangles, for the static world) or "hull" (a convex hull of at most
// "hull_vertices" points, 4 to 256, by default 32, for props). "lods" and "voxels" turn the mesh
// pipeline's levels of detail and voxel levels on or off for this scene (cook/mesh.h). The
// "texture_" keys set the scene's images as an image's own settings set an image cooked alone
// (cook/texture_import.h). The writer writes what is set, in this order.
//
// Reading is strict: another version gives NPH_RESULT_VERSION_MISMATCH, an unknown or repeated
// key or a value out of range NPH_RESULT_PARSE_ERROR, never a crash: the reader is a fuzz target
// (tests/fuzz).
//
// Thread-safety: pure.

#include <nepheline/core/string.h>

#include <cook/mesh.h>
#include <cook/texture.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_SCENE_IMPORT_VERSION        1U
#define COOK_SCENE_IMPORT_MAX_EXTENT     16384U
#define COOK_SCENE_HULL_VERTICES_MIN     4U
#define COOK_SCENE_HULL_VERTICES_MAX     256U // the physics module's hull builder's limit
#define COOK_SCENE_HULL_VERTICES_DEFAULT 32U  // ADR-0042's advice for props

// What a mesh collides as (ADR-0107).
typedef enum CookCollisionKind {
    COOK_COLLISION_NONE = 0,
    COOK_COLLISION_MESH, // its triangles: static and kinematic bodies
    COOK_COLLISION_HULL, // a convex hull of its positions
    COOK_COLLISION_KIND_COUNT
} CookCollisionKind;

typedef struct CookSceneImport {
    b32 has_collision;
    CookCollisionKind collision;
    b32 has_hull_vertices;
    u32 hull_vertices; // COOK_SCENE_HULL_VERTICES_MIN..MAX
    b32 has_lods;
    b32 lods;
    b32 has_voxels;
    b32 voxels;
    b32 has_texture_max_extent;
    u32 texture_max_extent; // 0..COOK_SCENE_IMPORT_MAX_EXTENT; 0: no cap
    b32 has_texture_uncompressed;
    b32 texture_uncompressed;
    b32 has_texture_mips;
    b32 texture_mips;
} CookSceneImport;

// Nothing set: the cooker's settings, no collision.
void cook_scene_import_default(CookSceneImport *out);
// True when the two say the same.
bool cook_scene_import_equal(const CookSceneImport *a, const CookSceneImport *b);

// The mesh settings with these over `base`.
CookMeshSettings cook_scene_import_mesh(const CookSceneImport *s, const CookMeshSettings *base);
// The texture settings of the scene's images with these over `base` (its usage stays the slot's).
CookTextureSettings cook_scene_import_texture(const CookSceneImport *s,
                                              const CookTextureSettings *base);
// What the scene's meshes collide as, and the hull's budget: the defaults where not set.
CookCollisionKind cook_scene_import_collision(const CookSceneImport *s);
u32 cook_scene_import_hull_vertices(const CookSceneImport *s);

// The name of a collision kind as the settings write it ("none", "mesh", "hull"), and back:
// false for another name.
const char *cook_collision_kind_name(CookCollisionKind kind);
bool cook_collision_kind_parse(NphStr name, CookCollisionKind *out);

// Reads settings from `text` into `out`. `alloc` holds the parse only.
NPH_MUST_USE NphResult cook_scene_import_read(NphAllocator *alloc, NphStr text,
                                              CookSceneImport *out);
// Appends the settings to `out`. NPH_RESULT_CORRUPT for settings out of range.
NPH_MUST_USE NphResult cook_scene_import_write(const CookSceneImport *s, NphStrBuilder *out);
// Writes the settings to `path`, replacing it whole (`path` + ".part" renamed over it).
NPH_MUST_USE NphResult cook_scene_import_save(const CookSceneImport *s, NphAllocator *alloc,
                                              const char *path);

#ifdef __cplusplus
}
#endif

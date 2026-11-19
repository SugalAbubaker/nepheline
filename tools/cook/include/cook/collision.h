// SPDX-License-Identifier: Apache-2.0
#pragma once

// Collision cooked from glTF (docs/cook.md "Collision"; docs/adr/0107-the-blender-round-trip.md):
// what each node of a scene collides with, and the collision assets (asset/collision.h) made of
// the physics module's blocks (physics/hull.h, physics/mesh.h).
//
// A node collides as, the first that applies:
// - its proxies: nodes named after it with UCX_, UBX_, USP_ or UCP_ before the name (Unreal's
//   convention), one convex hull each, of the proxy's positions moved into the node's frame. A
//   proxy's name finds its node by the whole rest of the name, then without a Blender
//   duplicate's ".NNN", then without a "_NN" count: "UCX_Crate_01.001" looks for
//   "Crate_01.001", "Crate_01", then "Crate". Proxies leave the scene: one with no child, camera
//   or light is taken out of its node list, one with any stays as a node that draws nothing, and
//   a mesh that only proxies drew is no longer drawn. A proxy that finds no node stays as it
//   was, drawn, with a warning.
// - its node's extra `collision`, "mesh", "hull" or "none" (an object's custom property, which
//   Blender's exporter writes into the node's extras);
// - its scene's import settings' `collision` (cook/scene_import.h).
// "mesh" and "hull" collide where the node draws a mesh: as the mesh's triangles (static and
// kinematic bodies), or as the convex hull of its positions, in the mesh's frame, which is the
// node's. A hull asked of points that span no volume (a plane) is made the triangle mesh of the
// same triangles, with a warning.
//
// Thread-safety: per call; the scene, the plan and the shapes are the caller's.

#include <nepheline/asset/collision.h>
#include <nepheline/asset/format.h>

#include <cook/gltf.h>
#include <cook/scene_import.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the cooker makes of the physics module's builders: raised when their output changes, so
// collision cooked before is cooked again (the cooker's own version does not see them).
#define COOK_COLLISION_VERSION 1U

// A proxy's geometry: its points and triangles in its node's frame.
typedef struct CookCollisionProxy {
    NphVec3 *points;
    u32 point_count;
    u32 *indices; // three a triangle, into points
    u32 index_count;
    char name[COOK_GLTF_NAME_MAX];
} CookCollisionProxy;

// What a node collides with.
typedef struct CookNodeCollision {
    u32 node;               // its index in the scene's node list, as cooked
    CookCollisionKind kind; // MESH or HULL: of the mesh it draws; NONE: of its proxies
    u32 mesh;               // the mesh it draws (MESH, HULL)
    u32 first_proxy;        // its proxies, proxies[first_proxy, first_proxy + proxy_count) (NONE)
    u32 proxy_count;
    b32 two_sided; // MESH: a material of the mesh's is double-sided, so its triangles collide
                   // from both sides
} CookNodeCollision;

typedef struct CookCollisionPlan {
    NphAllocator *alloc;
    CookNodeCollision *nodes; // by ascending node
    u32 node_count;
    u32 node_capacity;
    CookCollisionProxy *proxies;
    u32 proxy_count;
    u32 proxy_capacity;
    u32 hull_vertices; // the settings' budget for every hull
    u32 warnings;      // also added to the scene's
} CookCollisionPlan;

// Takes the proxies out of `scene` and decides what each of its nodes collides with (above),
// under `settings`. The plan's memory, and the scene's changes, come from `alloc`.
// NPH_RESULT_OUT_OF_MEMORY; the scene may then be half changed and is only fit to free.
NPH_MUST_USE NphResult cook_collision_plan(CookGltfScene *scene, const CookSceneImport *settings,
                                           NphAllocator *alloc, CookCollisionPlan *out);
// Releases a plan. Safe on a zero-filled one.
void cook_collision_plan_free(CookCollisionPlan *plan);

// Whether `name` is a proxy's (a prefix above); the rest of it, the name of what it stands for,
// in `*out_rest`.
bool cook_collision_proxy_name(const char *name, const char **out_rest);

// One of the shapes an asset will hold, its physics block from the shapes' allocator.
typedef struct CookCollisionBlock {
    NphCollisionShapeKind kind;
    void *block; // NphPhysicsHull or NphPhysicsMesh
    u32 size;
    NphAABB bounds;
} CookCollisionBlock;

typedef NPH_ARRAY(CookCollisionBlock) CookCollisionBlockArray;

typedef struct CookCollisionShapes {
    NphAllocator *alloc;
    CookCollisionBlockArray blocks;
} CookCollisionShapes;

void cook_collision_shapes_init(CookCollisionShapes *s, NphAllocator *alloc);
void cook_collision_shapes_free(CookCollisionShapes *s);

// Adds the convex hull of `points`, of at most `max_vertices` (4 to NPH_PHYSICS_HULL_MAX_VERTICES)
// points. Where they span no volume, adds the triangle mesh of `indices` (three a triangle) in its
// place, collided with from both sides, and sets `*out_flat`. NPH_RESULT_INVALID_ARGUMENT where
// neither can be made (no triangle
// with an area, or a coordinate beyond NPH_PHYSICS_MAX_COORDINATE); NPH_RESULT_OUT_OF_MEMORY;
// NPH_RESULT_FULL past NPH_COLLISION_MAX_SHAPES.
NPH_MUST_USE NphResult cook_collision_add_hull(CookCollisionShapes *s, const NphVec3 *points,
                                               u32 point_count, const u32 *indices, u32 index_count,
                                               u32 max_vertices, bool *out_flat);
// Adds the triangle mesh of `indices` over `points`, collided with from its triangles' fronts, or
// from both sides where `two_sided`. NPH_RESULT_INVALID_ARGUMENT where no triangle has an area,
// or for a coordinate beyond NPH_PHYSICS_MAX_COORDINATE; NPH_RESULT_OUT_OF_MEMORY;
// NPH_RESULT_FULL past NPH_COLLISION_MAX_SHAPES.
NPH_MUST_USE NphResult cook_collision_add_mesh(CookCollisionShapes *s, const NphVec3 *points,
                                               u32 point_count, const u32 *indices, u32 index_count,
                                               bool two_sided);
// Adds the shapes to `w`, a writer of a collision asset (asset/collision.h), its blocks
// compressed at `lz`. NPH_RESULT_INVALID_ARGUMENT for no shape; the writer's errors.
NPH_MUST_USE NphResult cook_collision_write(const CookCollisionShapes *s, NphLzLevel lz,
                                            NphAssetWriter *w);

#ifdef __cplusplus
}
#endif

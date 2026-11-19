// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked collision (CLAUDE.md §8.8, §8.9; docs/asset.md "Collision";
// docs/adr/0107-the-blender-round-trip.md): the shapes something collides
// with, as the cooker makes them from a glTF scene.
//
// A collision asset (NPH_ASSET_TYPE_COLLISION) holds a header, a record a
// shape, and the physics module's geometry blocks, one blob a block:
//
//     HEAD  NphCollisionHeader
//     SHAP  NphCollisionShape[shape]  a shape's kind and its block
//     HULL  a convex hull's block (physics/hull.h), one blob a hull
//     TMSH  a triangle mesh's block (physics/mesh.h), one blob a mesh
//
// A shape's block is the next of its kind's blobs: the first hull shape's
// is HULL 0, the second's HULL 1, and so on. The blocks hold no pointer and
// are used where they lie (every blob is 16-byte aligned, as the blocks
// need). Every shape is in the frame of what collides, the node or the mesh
// drawn there, and carries no pose and no scale.
//
// The view checks the container: the counts, each shape's kind and block,
// no block left over, and the sizes a block may have. The blocks themselves
// are checked by the physics module's validators (nph_physics_hull_validate,
// nph_physics_mesh_validate) where a runtime makes shapes of them, since the
// asset module does not depend on physics (CLAUDE.md §4).
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>
#include <nepheline/math/geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_COLLISION_TYPE_VERSION 1U

#define NPH_COLLISION_TAG_HEADER NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_COLLISION_TAG_SHAPES NPH_FOURCC('S', 'H', 'A', 'P')
#define NPH_COLLISION_TAG_HULL   NPH_FOURCC('H', 'U', 'L', 'L')
#define NPH_COLLISION_TAG_MESH   NPH_FOURCC('T', 'M', 'S', 'H')

// The most shapes an asset may hold.
#define NPH_COLLISION_MAX_SHAPES 1024U
// The smallest block of each kind: the physics module's block headers
// (physics/hull.h, physics/mesh.h), which a block's size must hold.
#define NPH_COLLISION_HULL_MIN_BYTES 128U
#define NPH_COLLISION_MESH_MIN_BYTES 64U

// Values are part of the file format and never renumbered.
typedef enum NphCollisionShapeKind {
    NPH_COLLISION_SHAPE_HULL = 1, // a convex hull (HULL)
    NPH_COLLISION_SHAPE_MESH = 2, // a triangle mesh (TMSH): static and kinematic bodies
} NphCollisionShapeKind;

typedef struct NphCollisionHeader {
    u32 shape_count; // 1 .. NPH_COLLISION_MAX_SHAPES
    u32 flags;       // zero
    NphAABB bounds;  // of every shape, in the frame of what collides
    u32 reserved[8]; // zero
} NphCollisionHeader;

NPH_STATIC_ASSERT(sizeof(NphCollisionHeader) == 64U, "the collision header is 64 bytes");

typedef struct NphCollisionShape {
    u32 kind;        // NphCollisionShapeKind
    u32 block;       // its blob's index among its kind's
    u32 reserved[2]; // zero
} NphCollisionShape;

NPH_STATIC_ASSERT(sizeof(NphCollisionShape) == 16U, "a collision shape is 16 bytes");

typedef struct NphCollisionView {
    const NphCollisionHeader *header;
    const NphCollisionShape *shapes; // header->shape_count
} NphCollisionView;

// A shape's geometry: its block's bytes, for the physics module's validator
// and shape (physics/hull.h, physics/mesh.h).
typedef struct NphCollisionBlock {
    const void *data;
    u32 size;
} NphCollisionBlock;

// Resolves and validates a READY collision asset (above): the header's
// count, flags, and finite, ordered bounds; each shape's kind, reserved
// words, and block, the next of its kind's blobs, at least that kind's
// smallest size and below 4 GiB; no block of either kind left over.
// NPH_RESULT_TYPE_MISMATCH and NPH_RESULT_VERSION_MISMATCH for another type
// or schema; NPH_RESULT_CORRUPT for anything out of range.
NPH_MUST_USE NphResult nph_collision_view(const NphAsset *asset, NphCollisionView *out);

// The block of shape `index` (below the shape count) of `view`, which
// nph_collision_view made of `asset`.
NphCollisionBlock nph_collision_shape_block(const NphAsset *asset, const NphCollisionView *view,
                                            u32 index);

#ifdef __cplusplus
}
#endif

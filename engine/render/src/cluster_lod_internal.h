// SPDX-License-Identifier: Apache-2.0
#pragma once

// The pieces of the cluster level-of-detail builder (render/cluster_lod.h),
// internal to the render module and its white-box tests: the mesh's welded
// view, its quadrics, and the simplification of one group of triangles.
#include <nepheline/core/alloc.h>
#include <nepheline/core/array.h>
#include <nepheline/core/result.h>
#include <nepheline/math/vec.h>

// A symmetric 4 x 4 quadric (Garland & Heckbert 1997) in double precision,
// its upper triangle row by row (a00 a01 a02 a03 a11 a12 a13 a22 a23 a33),
// and the weight that went into it: Q(p) / w is the weighted mean squared
// distance from p to the planes summed into it.
typedef struct LodQuadric {
    f64 a[10];
    f64 w;
} LodQuadric;

// Adds the plane n . p + d = 0 (n a unit vector) with `weight`.
void lod_quadric_add_plane(LodQuadric *q, NphVec3 n, f64 d, f64 weight);
void lod_quadric_add(LodQuadric *q, const LodQuadric *o);
// The weighted mean squared distance of `p` from q's planes (0 for an
// empty quadric).
f64 lod_quadric_error(const LodQuadric *q, NphVec3 p);

// The mesh as the simplifier sees it, for every group of every level:
// vertices welded by position into classes, vertices at one position with
// the same attributes into wedges (named by their first vertex), and one
// quadric per class, accumulated from level 0's triangles and, collapse by
// collapse, from every class merged into it. Quadrics take positions
// relative to `origin` (the mesh's centre), which keeps their constant
// terms small for the double-precision cancellation.
typedef struct LodMesh {
    const NphVec3 *positions;
    u32 vertex_count;
    u32 class_count;
    NphVec3 origin;
    u32 *class_of;        // per vertex: its class (0 .. class_count - 1)
    u32 *wedge_of;        // per vertex: its wedge's first vertex
    LodQuadric *quadrics; // per class
    // Per class: the area-weighted normals of the level-0 triangles around
    // every class merged into it (their cross products), the orientation of
    // the surface it stands for.
    NphVec3 *normals;
    // Per class: its first vertex, and the list of the classes merged into
    // it (itself first; `absorbed_next` links them, NPH_U32_MAX ends a list,
    // `absorbed_tail` is a list's last), so a level's error can be measured
    // from every level-0 point it stands for.
    u32 *class_vertex;
    u32 *absorbed_next;
    u32 *absorbed_tail;
    // Scratch, NPH_U32_MAX between uses: each group's simplification sets
    // and resets only the entries its triangles touch.
    u32 *local_wedge; // per vertex
    u32 *local_class; // per class
} LodMesh;

// Welds the mesh and builds its level-0 quadrics: each triangle's plane,
// weighted by its area, at its corners' classes; each open edge's
// perpendicular plane, so borders keep their shape (Garland & Heckbert
// §6.2); and the classes' normals. `attribs` (NULL: none) are compared as
// `attrib_stride` bytes.
NPH_MUST_USE NphResult lod_mesh_init(LodMesh *m, NphAllocator *alloc, const NphVec3 *positions,
                                     const void *attribs, u32 attrib_stride, u32 vertex_count,
                                     const u32 *indices, u32 index_count);
void lod_mesh_free(LodMesh *m, NphAllocator *alloc);

// One group's simplification: its triangles (wedge indices, rewritten in
// place and compacted to the first `*out_triangles`), each one's submesh,
// and which classes may not move (`locked`, per class: the classes the
// group shares with its neighbours).
typedef struct LodSimplifyInput {
    LodMesh *mesh;
    u32 *indices;
    u32 *submeshes; // per triangle, rewritten alongside
    u32 triangle_count;
    const u8 *locked;
    u32 target_triangles;
    NphArrayU32 *orphans; // NULL, or takes the classes left without a triangle
    NphAllocator *scratch;
} LodSimplifyInput;

// Collapses edges, cheapest first, until the group has at most
// `target_triangles` or no collapse is left that keeps its topology, its
// seams and borders, and its triangles' facing. `*out_error` is the largest
// collapse's quadric error (a distance: the square root of the mean squared
// distance), 0 without collapses: the measured error of a level, from every
// level-0 point to its whole result, is cluster_lod.c's. The classes left
// without a triangle that went into no other (a piece taken away whole ends
// in them) are appended to `orphans`. The quadrics, normals and lists of the
// classes collapsed are added to those they collapsed into.
NPH_MUST_USE NphResult lod_simplify(const LodSimplifyInput *in, u32 *out_triangles, f32 *out_error);

// ---------------------------------------------------------------------------
// Voxels (cluster_voxel.c; render/voxel.h; ADR-0061)
// ---------------------------------------------------------------------------

#include <nepheline/render/voxel.h>

// A triangle a group voxelizes: three mesh vertices and its submesh.
typedef struct LodVoxelSource {
    u32 v[3];
    u32 submesh;
} LodVoxelSource;

typedef NPH_ARRAY(LodVoxelSource) LodVoxelSourceArray;

// What a group voxelizes: its triangles, over the mesh's positions, with the
// caller's materials.
typedef struct LodVoxelInput {
    const NphVec3 *positions;
    const LodVoxelSource *sources;
    u32 source_count;
    const NphRenderVoxelSampler *sampler;
    NphAllocator *scratch;
} LodVoxelInput;

// Cell coordinates are kept within +-2^20 cells of the mesh's origin (21
// bits each in a 64-bit key).
#define LOD_VOXEL_COORD_LIMIT 1048576

// One cell the rays hit: its coordinates (its lowest corner is the cell size
// times them), its voxel, and the submesh most of its hits show.
typedef struct LodVoxelCell {
    i32 c[3];
    u32 submesh;
    NphRenderVoxel voxel;
} LodVoxelCell;

typedef NPH_ARRAY(LodVoxelCell) LodVoxelCellArray;

// The cells of edge `cell` that the sources' triangles overlap (Akenine-
// Möller's triangle-box test), counted up to `limit`: *out_count is more
// than `limit` when there are more. NPH_RESULT_OVERFLOW when a cell lies
// past LOD_VOXEL_COORD_LIMIT.
NPH_MUST_USE NphResult lod_voxel_count(const LodVoxelInput *in, f64 cell, u32 limit,
                                       u32 *out_count);

// Voxelizes the sources at cells of edge `cell` (render/voxel.h): for each
// cell they overlap, NPH_RENDER_VOXEL_RAYS^2 jittered rays along each axis
// against its triangles (a masked submesh's points through the sampler's
// `covers`), and from the rays that hit, the voxel. `out` (cleared first)
// takes the cells some ray hit, in order of their coordinates (z, then y,
// then x); `source_cell` (source_count entries) takes for each source the
// index in `out` of the cell holding its centroid where some ray hit there,
// else of the first cell where a ray hit the source itself, else
// NPH_U32_MAX. Deterministic. NPH_RESULT_OVERFLOW as lod_voxel_count.
NPH_MUST_USE NphResult lod_voxelize(const LodVoxelInput *in, f64 cell, LodVoxelCellArray *out,
                                    u32 *source_cell);

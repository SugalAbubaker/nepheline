// SPDX-License-Identifier: Apache-2.0
#pragma once

// Meshlets: the clusters the GPU-driven renderer culls and rasterises
// (CLAUDE.md §8.6.3-4; docs/render.md "Meshlets"). A meshlet holds at most
// NPH_RENDER_MESHLET_MAX_VERTICES vertices and
// NPH_RENDER_MESHLET_MAX_TRIANGLES triangles, with a bounding sphere and a
// normal cone for backface culling (Kapoulkine, meshoptimizer, 2017), and
// lists its triangles as meshlet-local corner indices.
//
// The records are bit for bit those of a cooked mesh (engine/asset mesh.h:
// NphMeshlet, NphMeshletBounds, MLVX, MLTR), so cooked meshlets reach the
// GPU unchanged. The renderer owns the builder because meshes made at run
// time (procedural content, the editor's meshes, tests) need meshlets as
// much as cooked ones, and the cooker calls the same builder so both are
// identical (tools/cook mesh.h cook_mesh_build_meshlets).
//
// The builder is the greedy clusterizer of meshoptimizer (Kapoulkine,
// clusterizer.cpp, meshopt_buildMeshlets), our implementation: a meshlet
// grows one triangle at a time over adjacency, topology first (a triangle
// adding no vertex, then one that would be left dangling, then fewest new
// vertices), then a spatial score (distance from the meshlet's centre
// relative to the expected meshlet radius, scaled by how far the
// triangle's normal leans from the meshlet's, `cone_weight`); when nothing
// touches the meshlet, the nearest unused triangle comes from a k-d tree
// over centroids. Deterministic: the same input gives the same meshlets.
//
// Thread-safety: the functions are pure (NPH_THREADSAFE) given distinct
// outputs.

#include <nepheline/core/array.h>
#include <nepheline/core/result.h>
#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_RENDER_MESHLET_MAX_VERTICES  64U
#define NPH_RENDER_MESHLET_MAX_TRIANGLES 124U

// One meshlet: its run of meshlet vertices (MLVX) and of triangles (MLTR,
// three local corners each), and the submesh it belongs to.
typedef struct NphRenderMeshlet {
    u32 vertex_offset;   // into the meshlet vertex array
    u32 triangle_offset; // into the meshlet triangle array, in triangles
    u16 vertex_count;    // <= NPH_RENDER_MESHLET_MAX_VERTICES
    u16 triangle_count;  // <= NPH_RENDER_MESHLET_MAX_TRIANGLES
    u32 submesh;
} NphRenderMeshlet;

// The bounding sphere and normal cone of a meshlet. The cone culls when
// dot(normalize(apex - camera), axis) >= cutoff with axis and cutoff read
// as snorm8; the values are derived for the quantised axis, so the test
// never culls a triangle that faces the camera (docs/cook.md "The meshlet
// normal cone"). A zero axis with cutoff 127 never culls.
typedef struct NphRenderMeshletBounds {
    NphVec3 center;
    f32 radius;
    i8 cone_axis[3];
    i8 cone_cutoff;
    NphVec3 cone_apex;
} NphRenderMeshletBounds;

NPH_STATIC_ASSERT(sizeof(NphRenderMeshlet) == 16U, "a meshlet is 16 bytes");
NPH_STATIC_ASSERT(sizeof(NphRenderMeshletBounds) == 32U, "meshlet bounds are 32 bytes");

typedef NPH_ARRAY(NphRenderMeshlet) NphRenderMeshletArray;
typedef NPH_ARRAY(NphRenderMeshletBounds) NphRenderMeshletBoundsArray;

// A mesh's meshlets: the records, their bounds, the meshlet vertex array
// (indices into the mesh's vertices), and the meshlet-local triangle
// corners (three bytes per triangle).
typedef struct NphRenderMeshlets {
    NphRenderMeshletArray meshlets;
    NphRenderMeshletBoundsArray bounds;
    NphArrayU32 vertices;
    NphArrayU8 triangles;
} NphRenderMeshlets;

void nph_render_meshlets_init(NphRenderMeshlets *m, NphAllocator *alloc);
void nph_render_meshlets_free(NphRenderMeshlets *m);

// A triangle-list range: the submeshes meshlets are built per (no meshlet
// mixes two).
typedef struct NphRenderIndexRange {
    u32 index_offset;
    u32 index_count; // a multiple of 3
} NphRenderIndexRange;

typedef struct NphRenderMeshletInput {
    const NphVec3 *positions; // the positions the GPU will see (dequantised)
    u32 vertex_count;
    const u32 *indices; // triangle list
    u32 index_count;
    const NphRenderIndexRange *ranges; // one per submesh, in submesh order
    u32 range_count;
    u32 max_vertices;      // 3..64; 0: 64
    u32 max_triangles;     // 1..124; 0: 124
    f32 cone_weight;       // 0..1: how much normal-cone tightness counts against distance
    NphAllocator *scratch; // working memory, released before returning
} NphRenderMeshletInput;

// Builds the meshlets of every range, in range order, appending to `out`
// (initialised by the caller); meshlet.submesh is the range's index.
// NPH_RESULT_INVALID_ARGUMENT for limits or a cone weight outside their
// ranges, an index outside the vertices, or a range outside the indices
// or not a multiple of 3; NPH_RESULT_OUT_OF_MEMORY otherwise. On failure
// `out` may hold a partial result for the caller to free.
NPH_MUST_USE NphResult nph_render_meshlets_build(const NphRenderMeshletInput *in,
                                                 NphRenderMeshlets *out);

// The bounding sphere (Ritter's) and normal cone of one meshlet:
// `vertices` are its meshlet-vertex entries (indices into `positions`),
// `triangles` its local corners.
void nph_render_meshlet_bounds(const NphVec3 *positions, const u32 *vertices, const u8 *triangles,
                               u32 triangle_count, NphRenderMeshletBounds *out);

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cluster levels of detail: the hierarchy of virtualized geometry (CLAUDE.md
// §8.6 stage 9; docs/render.md "Levels of detail"; ADR-0060).
//
// A mesh's clusters (its meshlets, render/meshlet.h) are level 0. Each
// coarser level is made from the one before: its clusters are partitioned
// into groups of neighbours, each group's triangles are simplified to about
// half with the group's outer boundary kept exactly, and the result is split
// into clusters again (Cignoni et al., "Batched Multi Triangulation", IEEE
// Visualization 2005; Karis, Stubbe & Thomas, "Nanite: A Deep Dive",
// SIGGRAPH 2021). Because a group's boundary does not move, the clusters a
// group made can stand in for the group's own clusters wherever the group's
// error is small enough, and a cut through the levels is watertight wherever
// it switches.
//
// The errors are monotonic: a group's error is the largest of its own
// simplification's (its quadric error, and the distances measured both ways
// between its result and the mesh) and of the errors of the groups that
// made its clusters, and its sphere contains theirs, so the error a camera
// sees projected from a group never falls going up. The renderer draws a cluster exactly when
// the error projected from the group that made it is within the threshold
// and the error projected from the group that consumes it is not.
//
// The simplification is ours (render/src/cluster_simplify.c): edge
// collapses ranked by quadric error (Garland & Heckbert, "Surface
// Simplification Using Quadric Error Metrics", SIGGRAPH 1997), each moving a
// vertex onto one of its neighbours, so every level's clusters index the
// mesh's own vertices and no attribute is ever interpolated; vertices on
// attribute seams and material borders move only along them, after the
// vertex classes of meshoptimizer's simplifier (Kapoulkine, 2017). The
// quadrics accumulate from level 0, so a level's error is measured against
// the mesh itself rather than the level before.
//
// Thread-safety: the functions are pure (NPH_THREADSAFE) given distinct
// outputs. Deterministic: the same input gives the same output, byte for
// byte.

#include <nepheline/core/array.h>
#include <nepheline/core/result.h>
#include <nepheline/math/vec.h>
#include <nepheline/render/meshlet.h>

#ifdef __cplusplus
extern "C" {
#endif

// No group: a level-0 cluster's maker, or a root cluster's consumer.
#define NPH_RENDER_LOD_NONE 0xFFFFFFFFU
// The children of a hierarchy node.
#define NPH_RENDER_LOD_NODE_CHILDREN 8U
// A node's child that is a group (its index in the low bits), not a node.
#define NPH_RENDER_LOD_LEAF 0x80000000U

// A group: clusters of one level simplified together into clusters of the
// next. Its clusters (the ones it consumes) are listed contiguously in
// NphRenderClusterLods.group_clusters.
typedef struct NphRenderLodGroup {
    NphVec3 center;     // the sphere the error is projected from, in mesh space: it
    f32 radius;         // holds its clusters' own spheres (those of the groups that
                        // made them; at level 0 the clusters' bounds), exactly
    f32 error;          // mesh units: how far its simplified surface may lie from
                        // the mesh's own: the largest of its simplification's
                        // quadric error, the distances measured both ways between
                        // that surface and level 0's (docs/render.md "Levels of
                        // detail"), and its makers' errors; the root group's is
                        // NPH_F32_MAX
    u32 level;          // its clusters' level (what it makes is at level + 1)
    u32 cluster_offset; // into group_clusters
    u32 cluster_count;
} NphRenderLodGroup;

// A cluster's place in the levels, beside its meshlet record.
typedef struct NphRenderClusterLod {
    u32 group;  // the group whose simplification made it; NPH_RENDER_LOD_NONE at level 0
    u32 parent; // the group that consumes it (the root group for the coarsest clusters)
    u32 level;
    u32 reserved;
} NphRenderClusterLod;

// A node of the hierarchy the renderer walks to find the clusters to draw:
// up to NPH_RENDER_LOD_NODE_CHILDREN children, each a node or a group
// (NPH_RENDER_LOD_LEAF), with a sphere containing everything below it, the
// largest group error there, and the smallest error of the groups that made
// the clusters below (0 where a level-0 cluster is). The walk takes a child
// whose largest error is too coarse for the view (below it are clusters to
// replace) unless its smallest maker error is too (every cluster below would
// be replaced in turn). Each level has a tree of its own under the root, so a
// node holds groups of one level, whose errors are alike; node 0 is the root.
// A leaf's numbers are its group's own: its sphere and error the record's,
// its maker error the smallest of its clusters' makers'.
typedef struct NphRenderLodNode {
    NphVec4 sphere[NPH_RENDER_LOD_NODE_CHILDREN];  // xyz centre, w radius (mesh space)
    f32 error[NPH_RENDER_LOD_NODE_CHILDREN];       // the largest group error below
    f32 maker_error[NPH_RENDER_LOD_NODE_CHILDREN]; // the smallest maker error below
    u32 child[NPH_RENDER_LOD_NODE_CHILDREN];       // node index, group | LEAF, or NONE
} NphRenderLodNode;

NPH_STATIC_ASSERT(sizeof(NphRenderLodGroup) == 32U, "a group is 32 bytes");
NPH_STATIC_ASSERT(sizeof(NphRenderClusterLod) == 16U, "a cluster's level record is 16 bytes");
NPH_STATIC_ASSERT(sizeof(NphRenderLodNode) == 224U, "a node is 224 bytes");

typedef NPH_ARRAY(NphRenderLodGroup) NphRenderLodGroupArray;
typedef NPH_ARRAY(NphRenderClusterLod) NphRenderClusterLodArray;
typedef NPH_ARRAY(NphRenderLodNode) NphRenderLodNodeArray;

// A mesh's levels: every level's clusters, level 0 first (the input's
// meshlets, unchanged and in their order), then each coarser level's, with
// their places, the groups, and the hierarchy.
typedef struct NphRenderClusterLods {
    NphRenderMeshlets clusters; // records, bounds, vertices (the mesh's), corners
    NphRenderClusterLodArray lods;
    NphRenderLodGroupArray groups; // level by level; the root group last
    NphArrayU32 group_clusters;    // each group's clusters in turn
    NphRenderLodNodeArray nodes;   // node 0 the root
    u32 levels;                    // levels of clusters (1: level 0 alone)
    u32 level0_clusters;           // the input's meshlets: clusters [0, level0_clusters)
} NphRenderClusterLods;

void nph_render_cluster_lods_init(NphRenderClusterLods *l, NphAllocator *alloc);
void nph_render_cluster_lods_free(NphRenderClusterLods *l);

typedef struct NphRenderClusterLodInput {
    const NphVec3 *positions; // as the GPU sees them (dequantised)
    // Each vertex's attributes, `attrib_stride` bytes apart, compared as
    // bytes: vertices at one position with other attributes make a seam.
    // NULL: every vertex at a shared position is a seam vertex of its own.
    const void *attribs;
    u32 attrib_stride;
    u32 vertex_count;
    const u32 *indices; // triangle list
    u32 index_count;
    const NphRenderIndexRange *ranges; // the submeshes, as the meshlets were built over
    u32 range_count;
    const NphRenderMeshlets *meshlets; // level 0, built over `ranges`
    u32 max_vertices; // the coarser levels' cluster limits (0: as nph_render_meshlets_build's)
    u32 max_triangles;
    f32 cone_weight;
    u32 group_size;        // clusters per group (0: 8)
    u32 max_levels;        // 0: until one cluster is left or the levels stop shrinking
    NphAllocator *scratch; // working memory, released before returning
} NphRenderClusterLodInput;

// Builds the levels of `in`'s mesh into `out` (initialised by the caller).
// NPH_RESULT_INVALID_ARGUMENT for limits outside the meshlet builder's, an
// index outside the vertices, ranges that do not match the meshlets, or a
// non-finite position; NPH_RESULT_OUT_OF_MEMORY otherwise. A mesh that
// cannot be simplified (a single cluster, or none) gets level 0 alone under
// its root group. On failure `out` may hold a partial result to free.
NPH_MUST_USE NphResult nph_render_cluster_lods_build(const NphRenderClusterLodInput *in,
                                                     NphRenderClusterLods *out);

// The error, in pixels, that a group's sphere and error project to for a
// camera at `camera` (mesh space, the error and radius scaled by
// `scale`): error * pixels_per_unit / max(distance to the sphere's nearest
// point, znear). The GPU's test, vis_lod_group_too_coarse, is its twin.
f32 nph_render_lod_projected_error(NphVec3 center, f32 radius, f32 error, f32 scale, NphVec3 camera,
                                   f32 pixels_per_unit, f32 znear);

#ifdef __cplusplus
}
#endif

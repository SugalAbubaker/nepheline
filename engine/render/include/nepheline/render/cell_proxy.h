// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cell proxies: the far field of a world partition's cells (docs/render.md
// "Cell proxies"; ADR-0062).
//
// A cell's proxy stands for its instances from `distance` out: a mesh of
// voxels alone (render/voxel.h; ADR-0061), in levels from the cell `cell` up,
// each twice as coarse, to a level that fits a cluster or a few. Its levels
// are an octree: each level's clusters hold the bricks of a block of 2 x 2 x 2
// bricks, and each group consumes the clusters of a block of 4 x 4 x 4 of its
// level's bricks and makes the one cluster of the next level there, its error
// that cluster's cell (render/cluster_lod.h's records, so the renderer walks,
// culls, draws and shadows a proxy as any mesh with voxel levels; its level 0
// is voxel clusters, made by no group).
//
// Each level is voxelized as the levels of a mesh are (render/cluster_lod.h's
// voxelizer: a jittered grid of rays along each axis of each cell for its
// coverage, footprint and look), from the instances' own levels through their
// transforms: each instance's cut at half the level's cell, its triangles
// sampled through its mesh's materials and its voxels' footprints tested as
// the renderer tests them.
//
// What no camera `distance` or more away can see is left out ("environmental
// occlusion": Gobbetti & Marton, "Far Voxels", SIGGRAPH 2005): from each of
// directions spread over the sphere, a grid of parallel rays half the
// visibility level's cell apart is cast through the cell's voxels at that
// level and the occluders' around it (the neighbouring cells' instances, the
// terrain), each ray stopping at the first footprint it meets, as the
// renderer's rays do (a sheet's footprints close up, so a wall hides what is
// behind it from every side); a voxel some ray stops at is visible, and so
// are its neighbours, against a gap the directions missed. The finer levels
// keep the cells inside visible ones, the coarser the cells holding one, so
// the proxy's cost follows the surface seen from outside, not the volume.
//
// Thread-safety: the function is pure (NPH_THREADSAFE) given distinct
// outputs; the samplers' functions are called from the calling thread.
// Deterministic: the same input gives the same output, byte for byte.

#include <nepheline/core/result.h>
#include <nepheline/math/geometry.h>
#include <nepheline/render/cluster_lod.h>

#ifdef __cplusplus
extern "C" {
#endif

// The visibility sweep's directions by default.
#define NPH_RENDER_PROXY_DIRECTIONS 96U
// The most levels a proxy has.
#define NPH_RENDER_PROXY_MAX_LEVELS 16U

// Flags (NphRenderCellProxyInput.flags).
// Keep every voxel: no visibility sweep (to measure what it leaves out).
#define NPH_RENDER_PROXY_NO_VISIBILITY 1U
// Keep the visible voxels alone: without it, their neighbours at the
// visibility level are kept too, against a gap the sweep's directions missed.
#define NPH_RENDER_PROXY_NO_DILATION 2U

// A mesh the instances draw: its levels (as nph_render_cluster_lods_build
// made them, or the part of them a proxy samples, the other clusters
// emptied), the positions they were built over (NULL where no cluster has
// vertices), and the materials its voxels were made with (its
// sampler's submesh_surface has `submesh_count` entries). An occluder's mesh
// may have no sampler: its triangles hide what is behind them, opaque.
typedef struct NphRenderProxyMesh {
    const NphRenderClusterLods *lods;
    const NphVec3 *positions;
    const NphRenderVoxelSampler *sampler;
    u32 submesh_count;
} NphRenderProxyMesh;

// An instance of a mesh: the transform from its mesh's space into the
// proxy's (rows of a 3 x 4 matrix, the translation last), invertible.
typedef struct NphRenderProxyInstance {
    f64 transform[3][4];
    u32 mesh;
    u32 reserved;
} NphRenderProxyInstance;

typedef struct NphRenderCellProxyInput {
    const NphRenderProxyMesh *meshes;
    u32 mesh_count;
    const NphRenderProxyInstance *instances; // the cell's: what the proxy stands for
    u32 instance_count;
    // What lies around the cell and hides it from outside (the neighbouring
    // cells' instances, the terrain), within `occluder_bounds` (proxy space).
    const NphRenderProxyInstance *occluders;
    u32 occluder_count;
    NphAABB occluder_bounds;
    f32 cell;                  // the finest level's cell (proxy units), a power of two
    f32 distance;              // the proxy is seen from at least this far (documentation: the
                               // sweep takes every occluder within occluder_bounds)
    u32 visibility_level;      // the level swept (0: 1, twice the finest cell)
    u32 visibility_directions; // 0: NPH_RENDER_PROXY_DIRECTIONS
    u32 flags;                 // NPH_RENDER_PROXY_*
    NphAllocator *scratch;     // working memory, released before returning
} NphRenderCellProxyInput;

// What a build made and left out.
typedef struct NphRenderCellProxyStats {
    u32 levels;
    u32 visibility_level;
    u32 voxels[NPH_RENDER_PROXY_MAX_LEVELS];   // kept, per level
    u32 clusters[NPH_RENDER_PROXY_MAX_LEVELS]; // per level
    u32 swept;                                 // the visibility level's voxels before the sweep
    u32 visible;                               // of them, the ones some direction sees
    u32 occluders;                             // the occluders' voxels swept with them
    u64 triangles;                             // the sources sampled over every level: triangles,
    u64 footprints;                            // and voxels of the instances' cuts
} NphRenderCellProxyStats;

// Builds the proxy of `in`'s instances into `out` (initialised by the
// caller, empty): its clusters (voxel clusters alone: meshlet records with
// no vertices, their bricks from `vertex_offset`, submesh 0), their level
// records, groups, hierarchy, bricks and voxels. `stats` may be NULL.
// NPH_RESULT_INVALID_ARGUMENT for a cell that is not a positive power of
// two, no instances, a mesh index out of range, a singular or non-finite
// transform, a mesh without levels, or an instance's mesh without a sampler;
// NPH_RESULT_OVERFLOW where a cell lies past the voxelizer's coordinate
// limit; NPH_RESULT_INVALID_ARGUMENT too where the sweep's grid would pass
// 16M rays (occluder bounds far larger than a cell's neighbourhood);
// NPH_RESULT_OUT_OF_MEMORY otherwise. The proxy is empty (no clusters, no
// levels) where nothing of the instances voxelizes. On failure `out` may
// hold a partial result to free.
NPH_MUST_USE NphResult nph_render_cell_proxy_build(const NphRenderCellProxyInput *in,
                                                   NphRenderClusterLods *out,
                                                   NphRenderCellProxyStats *stats);

#ifdef __cplusplus
}
#endif

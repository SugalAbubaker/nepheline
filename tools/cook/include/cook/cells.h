// SPDX-License-Identifier: Apache-2.0
#pragma once

// The world partition's cells and their proxies, as the cooker builds them
// (ADR-0062; docs/cook.md "Cells"): a scene's static instances partitioned
// into squares of the ground plane, and each square's proxy, a mesh of
// voxels alone that stands for its instances from afar (render/cell_proxy.h),
// written as a mesh asset of voxels alone (asset/mesh.h version 4).
//
// Thread-safety: the functions are pure (NPH_THREADSAFE) given distinct
// outputs; cook_cell_proxy calls the meshes' samplers, which must be
// NPH_THREADSAFE where cells are built at once.

#include <nepheline/asset/format.h>
#include <nepheline/core/array.h>
#include <nepheline/math/geometry.h>
#include <nepheline/render/cell_proxy.h>

#ifdef __cplusplus
extern "C" {
#endif

// The partition's settings by default: squares of 64 m, proxies from 560 m,
// their finest cell sized to a pixel there in the reference views (3840 x
// 2160, 0.9 rad vertical: 4.47e-4 m a metre a pixel).
#define COOK_HLOD_CELL     64.0F
#define COOK_HLOD_DISTANCE 560.0F
#define COOK_HLOD_PIXEL    4.47e-4F

typedef struct CookHlodSettings {
    b32 enabled;
    f32 cell;     // the squares' edge (0: COOK_HLOD_CELL)
    f32 distance; // the least distance a proxy is seen from (0: COOK_HLOD_DISTANCE)
    f32 pixel;    // a pixel's size at a distance of 1 (0: COOK_HLOD_PIXEL)
} CookHlodSettings;

// The settings with their defaults filled in.
CookHlodSettings cook_hlod_resolved(const CookHlodSettings *s);

// The proxies' finest cell: the largest power of two at most a pixel's size
// at their distance.
f32 cook_hlod_finest_cell(const CookHlodSettings *s);

// An instance as the partition sees it: its mesh (an index into the
// caller's meshes), its transform into the scene's space (rows of a 3 x 4
// matrix, the translation last), its bounding sphere there, and whether it
// may join a cell (its mesh voxelizes: every submesh opaque or masked, its
// materials given).
typedef struct CookCellInstance {
    f64 transform[3][4];
    NphSphere sphere;
    u32 mesh;
    b32 cellable;
} CookCellInstance;

// A cell: its square (x, z), its instances (order[first, first + count)),
// and their spheres' bounds.
typedef struct CookCell {
    i32 x;
    i32 z;
    u32 first;
    u32 count;
    NphAABB bounds;
} CookCell;

typedef NPH_ARRAY(CookCell) CookCellArray;

// Partitions `instances` into squares of edge `edge` on the x-z plane by
// their spheres' centres: the cellable ones whose sphere's radius is at most
// half the edge. `order` (cleared first) takes every instance's index, each
// cell's together, the cells in order of (z, x), and after them the
// instances of no cell in their own order; `cells` (cleared first) the
// cells. NPH_RESULT_INVALID_ARGUMENT for an edge that is not positive and
// finite or a centre past +-2^30 edges.
NPH_MUST_USE NphResult cook_cells_partition(const CookCellInstance *instances, u32 count, f32 edge,
                                            NphArrayU32 *order, CookCellArray *cells);

typedef struct CookCellProxyInput {
    const CookCellInstance *instances;
    u32 instance_count;
    const u32 *order; // cook_cells_partition's
    const CookCell *cells;
    u32 cell_count;
    u32 cell; // the one to build
    const NphRenderProxyMesh *meshes;
    u32 mesh_count;
    CookHlodSettings settings;
    NphAllocator *scratch; // NPH_THREADSAFE where cells are built at once
} CookCellProxyInput;

// Cell `cell`'s proxy (render/cell_proxy.h) in `out` (initialised by the
// caller, empty), its space's origin at the square's lowest corner on the
// ground plane (`*out_origin`): from its instances, with the instances of
// the eight squares round it and of no cell that reach its block of 3 x 3
// squares (its height and an edge above and below) as occluders. `stats`
// may be NULL. nph_render_cell_proxy_build's results.
NPH_MUST_USE NphResult cook_cell_proxy(const CookCellProxyInput *in, NphRenderClusterLods *out,
                                       NphVec3 *out_origin, NphRenderCellProxyStats *stats);

// Writes a proxy as a mesh asset of voxels alone (asset/mesh.h version 4)
// into `writer` (a mesh asset's): no vertices or indices, one submesh of
// material slot 0 over level 0's clusters, the clusters, their levels and
// their voxels. NPH_RESULT_INVALID_ARGUMENT for a proxy without clusters.
NPH_MUST_USE NphResult cook_proxy_write(const NphRenderClusterLods *proxy, NphAssetWriter *writer);

#ifdef __cplusplus
}
#endif

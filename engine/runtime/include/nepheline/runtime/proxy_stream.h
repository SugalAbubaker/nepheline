// SPDX-License-Identifier: Apache-2.0
#pragma once

// Streaming a world partition's cells' proxies level by level (ADR-0063
// stage 3; docs/runtime.md "Proxy streaming").
//
// A cell's proxy cooked as its levels apart (asset/mesh.h version 6) is a
// head naming its levels' mesh assets, finest first. The stream holds, for
// each cell it is given, a run of those levels from some level to the
// coarsest, which the cell is made with and always holds, and keeps the
// cell's render proxy those meshes (nph_render_cell_set_proxy); the
// instance of the finest held level takes its makers as fine, so a cell
// holding fewer levels draws coarser, without holes.
//
// Each update it works out the level each cell is wanted at:
//   - asked for by the views (nph_render_cell_request, a few frames late,
//     a request standing a few updates once they stop asking, so that a
//     cell flickering in and out of view keeps its level): that level;
//   - else, past its distance less a quarter (where the requests begin): one
//     level coarser than its cut would use from the camera, by the requests'
//     own test on the CPU, so that what a turn brings into view is at most a
//     level coarser until its own arrives;
//   - else (nearer: its instances are drawn): its coarsest alone.
// A level is worth holding the error its cell shows without it, seen from
// the camera over the view's threshold; a level at or coarser than its
// cell's wanted one (needed) is worth more than any finer (spare). Each
// update the stream gives the cells the levels that have loaded, those worth
// most first, a mesh made of each (at most `max_install_bytes` begun a
// frame): with `jobs` and the renderer's voxel pool, on a loading job, which
// checks the asset, uploads the mesh and releases the asset, the update
// reserving the mesh and later finishing it (ADR-0063 stage 3c); else on the
// main thread. Past `budget_bytes` held, it drops the levels worth
// least (a cell's finest held, never a coarsest); and loads the levels worth
// most (a cell's next finer, where held coarser than wanted), at most
// `max_loads` at once (the asset loader's I/O thread and jobs), each within
// the budget with the loads under way (a level's bytes as it loaded last,
// else by its clusters), making room by dropping levels worth enough less:
// spare ones, or needed ones worth a quarter less, so that cells of nearly
// the same worth do not trade a level back and forth; a level that does not
// fit so is passed over (a few an update) for smaller ones worth less that
// do. A cell whose level fails to load is left as it is, the failure logged
// and counted.
//
// Costs: create allocates the cells' table (`max_cells`) and the builds'
// (`max_loads`); update allocates nothing of its own, but what the loader
// and the renderer do for a level.
// Thread-safety: NPH_MAIN_THREAD_ONLY (it drives the renderer); update
// between nph_gpu_frame_begin and nph_render_frame, so that a mesh's uploads
// go with the frame's.

#include <nepheline/asset/loader.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/render/renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphProxyStream NphProxyStream;

typedef struct NphProxyStreamDesc {
    NphAllocator *alloc;    // required; thread-safe with `jobs` (the jobs' temporaries)
    NphRenderer *renderer;  // required
    NphAssetLoader *loader; // required, with a job system (asynchronous loads)
    // Optional: the levels' meshes made in the renderer's voxel pool
    // (NphRendererDesc.voxel_pool_bytes), their checks and uploads on these
    // jobs, the main thread only reserving and finishing them (ADR-0063 stage
    // 3c). Without (or without a pool, or with no room in it), they are made
    // on the main thread.
    NphJobSystem *jobs;
    u32 max_cells; // cells it streams (at least 1)
    // The bytes the held levels past each cell's coarsest may take on the
    // GPU (their meshes' streams; 0: unlimited).
    u64 budget_bytes;
    u32 max_loads;         // levels loading (or being made) at once (0: 8)
    u64 max_install_bytes; // levels begun a frame, in their bytes (0: 8 MiB; one at least)
} NphProxyStreamDesc;

// A cell to stream, made by the caller with its coarsest level's mesh alone
// and its levels' errors (NphRenderCellDesc.proxy_level_errors).
typedef struct NphProxyStreamCell {
    NphRenderCell cell;
    NphRenderMesh coarsest;     // its coarsest level's mesh (the caller's)
    const NphMeshLevel *levels; // its head's levels, finest first (copied)
    u32 level_count;            // 1 to NPH_RENDER_CELL_MAX_LEVELS
    NphDVec3 center;            // its box, world space, as its NphRenderCellDesc
    NphVec3 extent;
    f32 distance; // its proxy drawn from this far
} NphProxyStreamCell;

// NPH_RESULT_INVALID_ARGUMENT for a missing renderer, loader or allocator,
// or no cells; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_proxy_stream_create(const NphProxyStreamDesc *desc,
                                               NphProxyStream **out);

// Waits for its loads, gives every cell back its coarsest level alone,
// destroys the meshes it made, and frees the stream. The cells, and their
// coarsest meshes, are the caller's. Safe on NULL.
void nph_proxy_stream_destroy(NphProxyStream *s);

// Adds a cell. NPH_RESULT_INVALID_ARGUMENT for a stale cell or mesh, no
// levels or more than NPH_RENDER_CELL_MAX_LEVELS, a level naming no asset, or
// a box or distance that is not finite; NPH_RESULT_FULL past max_cells.
NPH_MUST_USE NphResult nph_proxy_stream_add(NphProxyStream *s, const NphProxyStreamCell *cell);

// What the stream sees of the frame's view: where the camera is, and the
// cut's scale (as the renderer's: pixels a unit spans at distance one,
// |projection P11| * height / 2, over the view's threshold), so that its
// test on the CPU is the requests' on the GPU.
typedef struct NphProxyStreamView {
    NphDVec3 camera;
    f32 pixels_per_unit; // at distance one
    f32 lod_error;       // the view's threshold, pixels (0: no levels of detail)
    f32 znear;
} NphProxyStreamView;

// The view of `view` drawn `height` pixels tall.
NphProxyStreamView nph_proxy_stream_view(const NphRenderView *view, u32 height);

// The render mesh of a level of a proxy cooked apart (asset/mesh.h
// NPH_MESH_FLAG_LEVEL; `level` a READY asset), made as the stream makes the
// finer ones: how the caller makes a cell's coarsest. The mesh is the
// caller's. NPH_RESULT_INVALID_ARGUMENT for a mesh that is not a level
// apart; otherwise the errors of nph_mesh_view and nph_render_mesh_create.
// NPH_MAIN_THREAD_ONLY.
NPH_MUST_USE NphResult nph_proxy_level_mesh_create(NphRenderer *r, const NphAsset *level,
                                                   NphRenderMesh *out);

// One frame's streaming (see the file comment). Fails only where the
// renderer refuses a cell's proxy; a level that fails to load or to become a
// mesh is counted (NphProxyStreamStats.failed) and its cell left as it is.
NPH_MUST_USE NphResult nph_proxy_stream_update(NphProxyStream *s, const NphProxyStreamView *view);

typedef struct NphProxyStreamStats {
    u32 cells;
    u32 held_levels; // past the cells' coarsest
    u64 held_bytes;  // theirs, on the GPU
    u32 asked;       // cells the views asked levels of (the last update)
    u32 short_cells; // cells held coarser than wanted (the last update)
    u32 loading;     // levels loading, or being made
    u32 building;    // of those, being made on the jobs
    // The last update's work: levels begun, and their bytes; levels given,
    // and their bytes; levels dropped.
    u32 started;
    u64 started_bytes;
    u32 installed;
    u64 installed_bytes;
    u32 dropped;
    u32 failed; // loads or meshes that failed, in all
} NphProxyStreamStats;

void nph_proxy_stream_stats(const NphProxyStream *s, NphProxyStreamStats *out);

// What the stream holds and wants of a cell, as of the last update: for
// tools (an editor's overlay) and tests.
typedef struct NphProxyStreamCellState {
    u32 held;    // its finest held level (its coarsest: none past it)
    u32 wanted;  // the level it is wanted at
    b32 loading; // its next finer level is loading
    b32 failed;  // a level of it failed: it is left as it is
} NphProxyStreamCellState;

// The cell added `index`-th (from 0; asserted below the cells added).
NphProxyStreamCellState nph_proxy_stream_cell_state(const NphProxyStream *s, u32 index);

#ifdef __cplusplus
}
#endif

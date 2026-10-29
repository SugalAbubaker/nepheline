// SPDX-License-Identifier: Apache-2.0
// Streaming the cells' proxies level by level (runtime/proxy_stream.h;
// ADR-0063 stage 3).
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/runtime/proxy_stream.h>

#include "runtime_internal.h"

#include <string.h>

// The requests' margin (shaders/shared/nph_vis.h NPH_VIS_CELL_PREFETCH): a
// cell is wanted at the level its cut would use a quarter nearer.
#define STREAM_KEEP 0.75F

// How much more a level must be worth than a needed one to push it out past
// the budget: so that two cells of nearly the same worth do not trade a
// level back and forth as the camera moves.
#define STREAM_SWAP 1.25F

// Updates a cell's last request stands once the views stop asking (out of
// view a moment, behind something): so that its finest asked level is not
// given up and fetched again as it flickers in and out of view.
#define STREAM_ASK_HOLD 30U

// Levels an update passes over for not fitting the budget, so that smaller
// ones worth less may fill what is left.
#define STREAM_SKIPS 8U

// The most levels one load may push out past the budget.
#define STREAM_VICTIMS 16U

typedef struct StreamCell {
    NphRenderCell cell;
    // Each level's mesh where held (the coarsest's the caller's), its GPU
    // bytes once it has loaded (kept when it goes: its next load's
    // estimate; the coarsest's not counted), its asset, its largest consumer
    // error, and its clusters.
    NphRenderMesh meshes[NPH_RENDER_CELL_MAX_LEVELS];
    u64 bytes[NPH_RENDER_CELL_MAX_LEVELS];
    NphGuid guids[NPH_RENDER_CELL_MAX_LEVELS];
    f32 errors[NPH_RENDER_CELL_MAX_LEVELS];
    u32 clusters[NPH_RENDER_CELL_MAX_LEVELS];
    u32 count; // its levels
    u32 first; // held from this level on
    u32 wanted;
    u32 asked;           // its last request (NPH_RENDER_CELL_NO_REQUEST: none standing)
    u32 asked_age;       // updates since
    f32 scale;           // its errors over the view's threshold from the camera, this update
    NphAssetHandle load; // the level loading (first - 1), or none
    u64 load_bytes;      // its bytes as estimated when it began
    u32 build;           // its mesh's build in the making (an index of builds, plus one; 0: none)
    u32 skipped;         // the update that last passed its next level over
    b32 failed;          // a level failed: the cell is left as it is
    NphDVec3 center;
    NphVec3 extent;
    f32 distance;
} StreamCell;

// A level's mesh in the making on a loading job, in the renderer's voxel
// pool (ADR-0063 stage 3c): begun by an update, its asset checked, described,
// uploaded and released by the job, finished by a later update. A build
// counts as its cell's load (whose handle it takes over), so max_loads of
// them are always enough.
typedef struct StreamBuild {
    NphProxyStream *stream;
    StreamCell *cell; // NULL: free
    NphAssetHandle asset;
    b32 kept; // the job kept the asset (the pool had no room)
    NphRenderVoxelMesh mesh;
    NphRenderMeshDesc desc;
    NphRenderSubmesh subs[NPH_RENDER_MAX_SUBMESHES];
    u64 bytes;
    u32 clusters;
    NphResult result; // the job's
    NphJobCounter done;
} StreamBuild;

struct NphProxyStream {
    NphProxyStreamDesc desc;
    StreamCell *cells;
    u32 cell_count;
    StreamCell **arrived; // max_loads: the loads an update finds done
    StreamBuild *builds;  // max_loads
    u32 building;
    b32 pool_full_logged;
    u64 held_bytes;
    u64 loading_bytes; // the loads', as estimated
    u32 held_levels;
    u32 loading;
    // The bytes and clusters of the levels loaded so far, by level: the
    // estimate of a level not loaded yet.
    u64 seen_bytes[NPH_RENDER_CELL_MAX_LEVELS];
    u64 seen_clusters[NPH_RENDER_CELL_MAX_LEVELS];
    u32 update; // updates so far, from 1
    NphProxyStreamStats last;
    u32 failed;
};

// ---------------------------------------------------------------------------
// The decisions
// ---------------------------------------------------------------------------

f32 proxy_stream_scale(f32 near, f32 pixels_per_unit, f32 lod_error, f32 znear)
{
    if (!(lod_error > 0.0F)) {
        return 0.0F;
    }
    return pixels_per_unit / (nph_f32_max(near * STREAM_KEEP, znear) * lod_error);
}

u32 proxy_stream_wanted(u32 asked, const f32 *errors, u32 level_count, f32 near, f32 distance,
                        f32 scale)
{
    NPH_ASSERT(level_count != 0U);
    const u32 coarsest = level_count - 1U;
    if (!(scale > 0.0F)) {
        return coarsest;
    }
    if (asked != NPH_RENDER_CELL_NO_REQUEST) {
        return nph_min_u32(asked, coarsest);
    }
    if (near < distance * STREAM_KEEP) {
        return coarsest;
    }
    u32 level = coarsest;
    for (u32 i = 0; i < level_count && level == coarsest; ++i) {
        level = errors[i] * scale > 1.0F ? i : level;
    }
    return nph_min_u32(level + 1U, coarsest);
}

ProxyStreamWorth proxy_stream_worth(const f32 *errors, u32 level, u32 wanted, f32 scale)
{
    ProxyStreamWorth w;
    w.needed = level >= wanted ? 1 : 0;
    w.error = errors[level] * scale;
    return w;
}

bool proxy_stream_worth_above(ProxyStreamWorth a, ProxyStreamWorth b)
{
    return a.needed != b.needed ? a.needed != 0 : a.error > b.error;
}

bool proxy_stream_displaces(ProxyStreamWorth w, ProxyStreamWorth held)
{
    return w.needed != held.needed ? w.needed != 0 : w.error > held.error * STREAM_SWAP;
}

u64 proxy_stream_level_bytes(const NphMeshView *mv)
{
    const NphMeshHeader *h = mv->header;
    u64 bytes = (u64)h->meshlet_count * (sizeof(NphMeshlet) + sizeof(NphMeshletBounds) + 20U) +
                (u64)h->submesh_count * 8U;
    if (mv->lod != NULL) {
        bytes += (u64)mv->lod->group_count * sizeof(NphMeshLodGroup) +
                 (u64)mv->lod->group_cluster_count * sizeof(u32) +
                 (u64)mv->lod->node_count * sizeof(NphMeshLodNode) +
                 (u64)mv->lod->voxel_cluster_count * sizeof(NphMeshVoxelCluster) +
                 (u64)mv->lod->brick_count * sizeof(NphMeshVoxelBrick) +
                 (u64)mv->lod->voxel_word_count * sizeof(u32);
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

NphProxyStreamView nph_proxy_stream_view(const NphRenderView *view, u32 height)
{
    NPH_ASSERT(view != NULL);
    NphProxyStreamView v;
    memset(&v, 0, sizeof v);
    v.camera = view->camera.position;
    // The renderer's scale (vis.c): the projection's P11 over half the height.
    const NphMat4 p = nph_mat4_perspective_infinite(view->camera.yfov, 1.0F, view->camera.znear);
    v.pixels_per_unit = nph_f32_abs(p.c[1].y) * 0.5F * (f32)height;
    v.lod_error = view->lod_error;
    v.znear = view->camera.znear;
    return v;
}

NphResult nph_proxy_stream_create(const NphProxyStreamDesc *desc, NphProxyStream **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->renderer == NULL || desc->loader == NULL ||
        desc->max_cells == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphProxyStream *s = NPH_ALLOC_TYPE(desc->alloc, NphProxyStream);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(s, 0, sizeof *s);
    s->desc = *desc;
    s->desc.max_loads = desc->max_loads != 0U ? desc->max_loads : 8U;
    s->desc.max_install_bytes =
        desc->max_install_bytes != 0U ? desc->max_install_bytes : NPH_MIB(8);
    s->cells = NPH_ALLOC_ARRAY_ZEROED(desc->alloc, StreamCell, desc->max_cells);
    s->arrived = NPH_ALLOC_ARRAY_ZEROED(desc->alloc, StreamCell *, s->desc.max_loads);
    s->builds = NPH_ALLOC_ARRAY_ZEROED(desc->alloc, StreamBuild, s->desc.max_loads);
    if (s->cells == NULL || s->arrived == NULL || s->builds == NULL) {
        nph_proxy_stream_destroy(s);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    *out = s;
    return NPH_RESULT_OK;
}

// A level's render mesh from its view (a mesh of voxels alone of one level,
// its makers as records).
static NphResult level_mesh_create(NphRenderer *r, const NphMeshView *mv, NphRenderMesh *out)
{
    if ((mv->header->flags & (u32)NPH_MESH_FLAG_LEVEL) == 0U || mv->lod_makers == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT; // not a level apart
    }
    NphRenderSubmesh subs[NPH_RENDER_MAX_SUBMESHES];
    NphRenderMeshDesc d;
    NPH_TRY(render_assets_mesh_desc(mv, subs, &d));
    d.name = "proxy level";
    return nph_render_mesh_create(r, &d, out);
}

NphResult nph_proxy_level_mesh_create(NphRenderer *r, const NphAsset *level, NphRenderMesh *out)
{
    NPH_ASSERT(r != NULL && out != NULL);
    *out = NPH_RENDER_MESH_NULL;
    NphMeshView mv;
    NPH_TRY(nph_mesh_view(level, &mv));
    return level_mesh_create(r, &mv, out);
}

// Gives cell `c` its levels from `first` on.
static NphResult set_first(NphProxyStream *s, StreamCell *c, u32 first)
{
    NPH_TRY(
        nph_render_cell_set_proxy(s->desc.renderer, c->cell, c->meshes + first, c->count - first));
    c->first = first;
    return NPH_RESULT_OK;
}

void nph_proxy_stream_destroy(NphProxyStream *s)
{
    if (s == NULL) {
        return;
    }
    // The builds' jobs done, their meshes given back unmade, their cells'
    // loads forgotten where the jobs released the assets.
    for (u32 i = 0; s->builds != NULL && i < s->desc.max_loads; ++i) {
        StreamBuild *b = &s->builds[i];
        if (b->cell != NULL) {
            nph_job_wait(s->desc.jobs, &b->done);
            nph_render_voxel_mesh_cancel(s->desc.renderer, b->mesh);
            if (!b->kept) {
                b->cell->load = NPH_ASSET_HANDLE_NULL;
            }
            b->cell->build = 0U;
            b->cell = NULL;
        }
    }
    for (u32 i = 0; i < s->cell_count; ++i) {
        StreamCell *c = &s->cells[i];
        if (c->load.value != 0U) {
            NPH_IGNORE_RESULT(nph_asset_wait(s->desc.loader, c->load));
            nph_asset_release(s->desc.loader, c->load);
        }
        if (c->first + 1U < c->count && set_first(s, c, c->count - 1U) != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "proxy stream: a cell kept levels it is losing");
        }
        for (u32 k = 0; k + 1U < c->count; ++k) {
            nph_render_mesh_destroy(s->desc.renderer, c->meshes[k]);
        }
    }
    if (s->arrived != NULL) {
        NPH_FREE_ARRAY(s->desc.alloc, StreamCell *, s->arrived, s->desc.max_loads);
    }
    if (s->builds != NULL) {
        NPH_FREE_ARRAY(s->desc.alloc, StreamBuild, s->builds, s->desc.max_loads);
    }
    if (s->cells != NULL) {
        NPH_FREE_ARRAY(s->desc.alloc, StreamCell, s->cells, s->desc.max_cells);
    }
    NPH_FREE_TYPE(s->desc.alloc, NphProxyStream, s);
}

NphResult nph_proxy_stream_add(NphProxyStream *s, const NphProxyStreamCell *cell)
{
    NPH_ASSERT(s != NULL && cell != NULL);
    if (cell->levels == NULL || cell->level_count == 0U ||
        cell->level_count > NPH_RENDER_CELL_MAX_LEVELS ||
        !nph_render_cell_is_valid(s->desc.renderer, cell->cell) ||
        !nph_render_mesh_is_valid(s->desc.renderer, cell->coarsest) ||
        !nph_f64_is_finite(cell->center.x) || !nph_f64_is_finite(cell->center.y) ||
        !nph_f64_is_finite(cell->center.z) || !nph_vec3_is_finite(cell->extent) ||
        !nph_f32_is_finite(cell->distance)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 k = 0; k < cell->level_count; ++k) {
        if (nph_guid_is_null(cell->levels[k].mesh)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    if (s->cell_count == s->desc.max_cells) {
        return NPH_RESULT_FULL;
    }
    StreamCell *c = &s->cells[s->cell_count++];
    memset(c, 0, sizeof *c);
    c->cell = cell->cell;
    c->count = cell->level_count;
    for (u32 k = 0; k < c->count; ++k) {
        c->guids[k] = cell->levels[k].mesh;
        c->errors[k] = cell->levels[k].error;
        c->clusters[k] = cell->levels[k].cluster_count;
    }
    c->meshes[c->count - 1U] = cell->coarsest;
    c->first = c->count - 1U;
    c->wanted = c->first;
    c->asked = NPH_RENDER_CELL_NO_REQUEST;
    c->asked_age = STREAM_ASK_HOLD;
    c->center = cell->center;
    c->extent = cell->extent;
    c->distance = cell->distance;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// A frame
// ---------------------------------------------------------------------------

// The camera's distance from cell `c`'s box.
static f32 box_distance(const StreamCell *c, NphDVec3 camera)
{
    const f64 dx = nph_f64_max(nph_f64_abs(camera.x - c->center.x) - (f64)c->extent.x, 0.0);
    const f64 dy = nph_f64_max(nph_f64_abs(camera.y - c->center.y) - (f64)c->extent.y, 0.0);
    const f64 dz = nph_f64_max(nph_f64_abs(camera.z - c->center.z) - (f64)c->extent.z, 0.0);
    return (f32)nph_f64_sqrt(dx * dx + dy * dy + dz * dz);
}

static ProxyStreamWorth worth(const StreamCell *c, u32 level)
{
    return proxy_stream_worth(c->errors, level, c->wanted, c->scale);
}

// Cell `c`'s level `level`'s bytes: as it loaded last, else by its clusters
// at the bytes a cluster of the levels of its depth loaded so far took (its
// voxels' size: the finest cell is the world's), else 0, so that it loads
// and teaches (the budget then drops what it must).
static u64 estimate(const NphProxyStream *s, const StreamCell *c, u32 level)
{
    if (c->bytes[level] != 0U) {
        return c->bytes[level];
    }
    return s->seen_clusters[level] != 0U
               ? c->clusters[level] * s->seen_bytes[level] / s->seen_clusters[level]
               : 0U;
}

static void log_failure(const StreamCell *c, u32 level, const char *what, NphResult r)
{
    char guid[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(c->guids[level], guid);
    NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "proxy stream: level %u (%s) %s: %s", level, guid, what,
                  nph_result_string(r));
}

// Cell `c` given its level `level`'s mesh, made: held and counted, its bytes
// and clusters teaching the estimates. The mesh destroyed where the cell's
// proxy refuses it.
static NphResult adopt(NphProxyStream *s, StreamCell *c, u32 level, u64 bytes, u32 clusters)
{
    const NphResult r = set_first(s, c, level);
    if (r != NPH_RESULT_OK) {
        nph_render_mesh_destroy(s->desc.renderer, c->meshes[level]);
        c->meshes[level] = NPH_RENDER_MESH_NULL;
        return r;
    }
    if (c->bytes[level] == 0U) {
        s->seen_bytes[level] += bytes;
        s->seen_clusters[level] += clusters;
    }
    c->bytes[level] = bytes;
    s->held_bytes += bytes;
    s->held_levels += 1U;
    return NPH_RESULT_OK;
}

// Cell `c`'s loaded level made a mesh on the main thread from its view, and
// given to the cell.
static NphResult install(NphProxyStream *s, StreamCell *c, const NphMeshView *mv, u64 bytes)
{
    const u32 level = c->first - 1U;
    NPH_TRY(level_mesh_create(s->desc.renderer, mv, &c->meshes[level]));
    return adopt(s, c, level, bytes, mv->header->meshlet_count);
}

// A cell's load let go: its bookkeeping (its asset released elsewhere).
static void forget(NphProxyStream *s, StreamCell *c)
{
    c->load = NPH_ASSET_HANDLE_NULL;
    s->loading -= 1U;
    s->loading_bytes -= c->load_bytes;
    c->load_bytes = 0U;
}

// A cell's load let go, its asset released.
static void unload(NphProxyStream *s, StreamCell *c)
{
    nph_asset_release(s->desc.loader, c->load);
    forget(s, c);
}

// A level's work on a loading job (StreamBuild): its asset checked (the view
// validates it) and described, the mesh's checks and uploads into the voxel
// pool, and the asset let go (its decompressed bytes freed off the main
// thread), unless the pool had no room: the update then makes the level on
// the main thread, from the asset kept.
static void build_job(void *user)
{
    StreamBuild *b = (StreamBuild *)user;
    const NphProxyStream *s = b->stream;
    NphMeshView mv;
    NphResult r = nph_mesh_view(nph_asset_get(s->desc.loader, b->asset), &mv);
    if (r == NPH_RESULT_OK &&
        ((mv.header->flags & (u32)NPH_MESH_FLAG_LEVEL) == 0U || mv.lod_makers == NULL)) {
        r = NPH_RESULT_INVALID_ARGUMENT; // not a level apart
    }
    if (r == NPH_RESULT_OK) {
        r = render_assets_mesh_desc(&mv, b->subs, &b->desc);
    }
    if (r == NPH_RESULT_OK) {
        b->desc.name = "proxy level";
        b->bytes = proxy_stream_level_bytes(&mv);
        b->clusters = mv.header->meshlet_count;
        r = nph_render_voxel_mesh_upload(s->desc.renderer, b->mesh, &b->desc, s->desc.alloc);
    }
    b->result = r;
    b->kept = r == NPH_RESULT_FULL ? 1 : 0;
    if (!b->kept) {
        nph_asset_release(s->desc.loader, b->asset);
    }
}

// Cell `c`'s loaded level begun as a mesh in the renderer's voxel pool, its
// work on a loading job (finished by a later update, finish_builds). False
// without a job system, a pool, a mesh slot or a job's: the caller makes it
// at once.
static bool start(NphProxyStream *s, StreamCell *c)
{
    if (s->desc.jobs == NULL) {
        return false;
    }
    StreamBuild *b = NULL;
    for (u32 i = 0; i < s->desc.max_loads && b == NULL; ++i) {
        b = s->builds[i].cell == NULL ? &s->builds[i] : NULL;
    }
    NPH_ASSERT_MSG(b != NULL, "a build for every load");
    if (b == NULL || nph_render_voxel_mesh_begin(s->desc.renderer, &b->mesh) != NPH_RESULT_OK) {
        return false;
    }
    b->stream = s;
    b->asset = c->load;
    b->bytes = 0U;
    b->clusters = 0U;
    b->result = NPH_RESULT_OK;
    b->kept = 0;
    nph_job_counter_init(&b->done);
    const NphJobDecl decl = {build_job, b, "proxy level"};
    if (nph_job_run(s->desc.jobs, &decl, 1U, NPH_JOB_PRIORITY_LOW, &b->done) != NPH_RESULT_OK) {
        nph_render_voxel_mesh_cancel(s->desc.renderer, b->mesh);
        return false;
    }
    b->cell = c;
    c->build = (u32)(b - s->builds) + 1U;
    s->building += 1U;
    return true;
}

// Cell `c`'s loaded level made a mesh on the main thread (no job, or no
// room in the voxel pool) while still needed: given to the cell.
static NphResult make_now(NphProxyStream *s, StreamCell *c, NphProxyStreamStats *st)
{
    if (c->first - 1U < c->wanted) {
        return NPH_RESULT_OK; // no longer needed
    }
    NphMeshView mv;
    NPH_TRY(nph_mesh_view(nph_asset_get(s->desc.loader, c->load), &mv));
    const u64 bytes = proxy_stream_level_bytes(&mv);
    NPH_TRY(install(s, c, &mv, bytes));
    st->installed += 1U;
    st->installed_bytes += bytes;
    return NPH_RESULT_OK;
}

// Cell `c`'s load, done: its level begun on a job, or made at once, while
// it is still needed and the frame's bytes allow (false: left for a later
// frame; its bytes are its estimate's, the job's to learn), else let go.
static bool arrive(NphProxyStream *s, StreamCell *c, NphProxyStreamStats *st)
{
    const u32 level = c->first - 1U;
    NphResult r = nph_asset_state(s->desc.loader, c->load) == NPH_ASSET_STATE_READY
                      ? NPH_RESULT_OK
                      : nph_asset_error(s->desc.loader, c->load);
    if (r == NPH_RESULT_OK && level >= c->wanted) {
        if (st->started != 0U && st->started_bytes + c->load_bytes > s->desc.max_install_bytes) {
            return false; // the frame's share is spent: a later frame's
        }
        st->started += 1U;
        st->started_bytes += c->load_bytes;
        if (start(s, c)) {
            return true; // its build holds the asset
        }
        r = make_now(s, c, st);
    }
    if (r != NPH_RESULT_OK) {
        log_failure(c, level, "failed", r);
        c->failed = 1;
        s->failed += 1U;
    }
    unload(s, c);
    return true;
}

// The builds whose jobs are done: each level made and given to its cell
// while still needed, else let go (a failure logged and counted). Where the
// pool had no room, the level is made on the main thread from the asset
// kept.
static void finish_builds(NphProxyStream *s, NphProxyStreamStats *st)
{
    for (u32 i = 0; i < s->desc.max_loads && s->building != 0U; ++i) {
        StreamBuild *b = &s->builds[i];
        if (b->cell == NULL || nph_job_counter_value(&b->done) != 0U) {
            continue;
        }
        nph_job_wait(s->desc.jobs, &b->done); // returns at once: the job's writes seen
        StreamCell *c = b->cell;
        const u32 level = c->first - 1U;
        NphResult r = NPH_RESULT_OK;
        if (b->kept) {
            nph_render_voxel_mesh_cancel(s->desc.renderer, b->mesh);
            if (!s->pool_full_logged) {
                NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                             "proxy stream: the renderer's voxel pool is full: levels get "
                             "buffers of their own, made on the main thread");
                s->pool_full_logged = 1;
            }
            r = make_now(s, c, st);
        } else if (b->result != NPH_RESULT_OK) {
            // Failed before its upload (its asset) or in it: given back.
            nph_render_voxel_mesh_cancel(s->desc.renderer, b->mesh);
            r = b->result;
        } else if (level >= c->wanted) {
            r = nph_render_voxel_mesh_finish(s->desc.renderer, b->mesh, &c->meshes[level]);
            if (r == NPH_RESULT_OK) {
                r = adopt(s, c, level, b->bytes, b->clusters);
            }
            if (r == NPH_RESULT_OK) {
                st->installed += 1U;
                st->installed_bytes += b->bytes;
            }
        } else {
            nph_render_voxel_mesh_cancel(s->desc.renderer, b->mesh);
        }
        if (r != NPH_RESULT_OK) {
            log_failure(c, level, "failed", r);
            c->failed = 1;
            s->failed += 1U;
        }
        c->build = 0U;
        b->cell = NULL;
        s->building -= 1U;
        if (b->kept) {
            unload(s, c);
        } else {
            forget(s, c); // the job released its asset
        }
    }
}

// The held level worth keeping least: the finest held level of a cell not
// loading, never a coarsest. NULL: none.
static StreamCell *least_held(NphProxyStream *s)
{
    StreamCell *best = NULL;
    ProxyStreamWorth best_worth = {0, 0.0F};
    for (u32 i = 0; i < s->cell_count; ++i) {
        StreamCell *c = &s->cells[i];
        if (c->first + 1U >= c->count || c->load.value != 0U) {
            continue;
        }
        const ProxyStreamWorth w = worth(c, c->first);
        if (best == NULL || proxy_stream_worth_above(best_worth, w)) {
            best = c;
            best_worth = w;
        }
    }
    return best;
}

// The level worth loading most: the next finer level of a cell held coarser
// than wanted, neither loading nor failed nor passed over this update. NULL:
// none.
static StreamCell *most_short(NphProxyStream *s)
{
    StreamCell *best = NULL;
    ProxyStreamWorth best_worth = {0, 0.0F};
    for (u32 i = 0; i < s->cell_count; ++i) {
        StreamCell *c = &s->cells[i];
        if (c->first <= c->wanted || c->load.value != 0U || c->failed || c->skipped == s->update) {
            continue;
        }
        const ProxyStreamWorth w = worth(c, c->first - 1U);
        if (best == NULL || proxy_stream_worth_above(w, best_worth)) {
            best = c;
            best_worth = w;
        }
    }
    return best;
}

// The held levels a level worth `w` would push out to free `need` bytes,
// into `victims` (*count of them): the finest held levels of cells not
// loading, those worth least first, each worth enough less than it
// (proxy_stream_displaces), at most STREAM_VICTIMS. False, when they do not
// free enough: then none goes, rather than some given up for nothing (and
// fetched again).
static bool plan_room(NphProxyStream *s, ProxyStreamWorth w, u64 need, StreamCell **victims,
                      u32 *count)
{
    ProxyStreamWorth worths[STREAM_VICTIMS];
    u32 n = 0U;
    for (u32 i = 0; i < s->cell_count; ++i) {
        StreamCell *c = &s->cells[i];
        if (c->first + 1U >= c->count || c->load.value != 0U) {
            continue;
        }
        const ProxyStreamWorth vw = worth(c, c->first);
        if (!proxy_stream_displaces(w, vw) ||
            (n == STREAM_VICTIMS && !proxy_stream_worth_above(worths[n - 1U], vw))) {
            continue;
        }
        u32 j = n < STREAM_VICTIMS ? n++ : n - 1U;
        for (; j > 0U && proxy_stream_worth_above(worths[j - 1U], vw); --j) {
            worths[j] = worths[j - 1U];
            victims[j] = victims[j - 1U];
        }
        worths[j] = vw;
        victims[j] = c;
    }
    u64 freed = 0U;
    for (u32 i = 0; i < n; ++i) {
        freed += victims[i]->bytes[victims[i]->first];
        if (freed >= need) {
            *count = i + 1U;
            return true;
        }
    }
    *count = 0U;
    return false;
}

// Cell `c`'s finest held level given up.
static NphResult drop(NphProxyStream *s, StreamCell *c, NphProxyStreamStats *st)
{
    const u32 level = c->first;
    NPH_TRY(set_first(s, c, level + 1U));
    nph_render_mesh_destroy(s->desc.renderer, c->meshes[level]);
    c->meshes[level] = NPH_RENDER_MESH_NULL;
    s->held_bytes -= c->bytes[level];
    s->held_levels -= 1U;
    st->dropped += 1U;
    return NPH_RESULT_OK;
}

NphResult nph_proxy_stream_update(NphProxyStream *s, const NphProxyStreamView *view)
{
    NPH_ASSERT(s != NULL && view != NULL);
    NphProxyStreamStats st;
    memset(&st, 0, sizeof st);
    const u64 budget = s->desc.budget_bytes != 0U ? s->desc.budget_bytes : NPH_U64_MAX;
    s->update += 1U;
    // The levels wanted, and the loads done.
    u32 arrived = 0U;
    for (u32 i = 0; i < s->cell_count; ++i) {
        StreamCell *c = &s->cells[i];
        const u32 request = nph_render_cell_request(s->desc.renderer, c->cell);
        if (request != NPH_RENDER_CELL_NO_REQUEST) {
            c->asked = request;
            c->asked_age = 0U;
            st.asked += 1U;
        } else if (c->asked_age < STREAM_ASK_HOLD) {
            c->asked_age += 1U;
        } else {
            c->asked = NPH_RENDER_CELL_NO_REQUEST;
        }
        const f32 near = box_distance(c, view->camera);
        c->scale = proxy_stream_scale(near, view->pixels_per_unit, view->lod_error, view->znear);
        c->wanted = proxy_stream_wanted(c->asked, c->errors, c->count, near, c->distance, c->scale);
        if (c->load.value != 0U && c->build == 0U &&
            nph_asset_state(s->desc.loader, c->load) != NPH_ASSET_STATE_LOADING) {
            NPH_ASSERT(arrived < s->desc.max_loads);
            s->arrived[arrived++] = c;
        }
    }
    // The builds done, then the loads done, those worth most first (they
    // began so, but the camera moves), within the frame's bytes.
    finish_builds(s, &st);
    for (u32 i = 1; i < arrived; ++i) {
        StreamCell *c = s->arrived[i];
        const ProxyStreamWorth w = worth(c, c->first - 1U);
        u32 j = i;
        for (; j > 0U && proxy_stream_worth_above(
                             w, worth(s->arrived[j - 1U], s->arrived[j - 1U]->first - 1U));
             --j) {
            s->arrived[j] = s->arrived[j - 1U];
        }
        s->arrived[j] = c;
    }
    for (u32 i = 0; i < arrived; ++i) {
        if (!arrive(s, s->arrived[i], &st)) {
            break; // the frame's install bytes are spent
        }
    }
    // Past the budget (a level larger than its estimate), the levels worth
    // keeping least.
    while (s->held_bytes > budget) {
        StreamCell *c = least_held(s);
        if (c == NULL) {
            break;
        }
        NPH_TRY(drop(s, c, &st));
    }
    // The levels worth loading most, while loads are free: each within the
    // budget, making room by dropping levels worth enough less than it, or
    // passed over (a few an update) for smaller ones that fit.
    u32 skips = 0U;
    while (s->loading < s->desc.max_loads) {
        StreamCell *c = most_short(s);
        if (c == NULL) {
            break;
        }
        const u32 level = c->first - 1U;
        const u64 bytes = estimate(s, c, level);
        const u64 total = s->held_bytes + s->loading_bytes + bytes;
        if (total > budget) {
            StreamCell *victims[STREAM_VICTIMS];
            u32 n = 0U;
            if (!plan_room(s, worth(c, level), total - budget, victims, &n)) {
                c->skipped = s->update;
                if (++skips == STREAM_SKIPS) {
                    break;
                }
                continue;
            }
            for (u32 i = 0; i < n; ++i) {
                NPH_TRY(drop(s, victims[i], &st));
            }
        }
        const NphResult r =
            nph_asset_load_guid_async(s->desc.loader, c->guids[level], NULL, &c->load);
        if (r != NPH_RESULT_OK) {
            log_failure(c, level, "not loaded", r);
            c->load = NPH_ASSET_HANDLE_NULL;
            c->failed = 1;
            s->failed += 1U;
            continue;
        }
        c->load_bytes = bytes;
        s->loading += 1U;
        s->loading_bytes += bytes;
    }
    for (u32 i = 0; i < s->cell_count; ++i) {
        st.short_cells += s->cells[i].first > s->cells[i].wanted ? 1U : 0U;
    }
    s->last = st;
    return NPH_RESULT_OK;
}

void nph_proxy_stream_stats(const NphProxyStream *s, NphProxyStreamStats *out)
{
    NPH_ASSERT(s != NULL && out != NULL);
    *out = s->last;
    out->cells = s->cell_count;
    out->held_levels = s->held_levels;
    out->held_bytes = s->held_bytes;
    out->loading = s->loading;
    out->building = s->building;
    out->failed = s->failed;
}

NphProxyStreamCellState nph_proxy_stream_cell_state(const NphProxyStream *s, u32 index)
{
    NPH_ASSERT(s != NULL);
    NphProxyStreamCellState out;
    memset(&out, 0, sizeof out);
    NPH_ASSERT_MSG(index < s->cell_count, "no such cell");
    if (index < s->cell_count) {
        const StreamCell *c = &s->cells[index];
        out.held = c->first;
        out.wanted = c->wanted;
        out.loading = c->load.value != 0U ? 1 : 0;
        out.failed = c->failed;
    }
    return out;
}

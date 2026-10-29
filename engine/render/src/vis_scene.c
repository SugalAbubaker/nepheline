// SPDX-License-Identifier: Apache-2.0
// The production path's GPU scene (docs/render.md "The GPU scene";
// docs/adr/0029-gpu-driven-renderer.md): persistent tables of meshes and
// instances indexed by pool slot, the material map, each mesh's meshlet
// streams, and the index pool of the indirect raster.
//
// Only what changes is sent, so a frame's CPU cost does not grow with the
// object count (§12 Phase 10 "constant CPU cost vs object count"):
//   - a mesh's record and streams go up once, at creation, through the
//     staging ring;
//   - an instance's material entries go up once, at creation (they hold
//     material handle values, which the GPU checks against the material
//     table, so a destroyed material never needs the entries rewritten);
//   - an instance's record goes up in the frame after it was created,
//     moved, hidden, shown, or destroyed, as one entry of the frame's
//     update list that vis_scatter.comp applies; the frame after that, a
//     second entry brings its previous transform up to date;
//   - a light's record goes up in the frame after it was created, changed,
//     or destroyed, as an entry of the frame's light updates
//     (vis_lights.comp).
//
// Translations are 64-bit on the CPU and split into a high and a low float
// for the GPU, which subtracts the camera's split the same way
// (render_vis_split; Cozzi & Ring 2011).
#include <nepheline/core/log.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphVisTransform) == 64U, "NphVisTransform matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVisInstance) == 80U, "NphVisInstance matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVisInstanceUpdate) == 96U,
                  "NphVisInstanceUpdate matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVisMesh) == 192U, "NphVisMesh matches its GLSL layout");
NPH_STATIC_ASSERT(offsetof(NphVisMesh, voxel_bricks) % 8U == 0U &&
                      offsetof(NphVisMesh, voxels) % 8U == 0U &&
                      offsetof(NphVisMesh, voxel_clusters) % 8U == 0U,
                  "addresses are aligned");
NPH_STATIC_ASSERT(sizeof(NphRenderPackedBrick) == 16U && sizeof(NphRenderVoxelCluster) == 8U,
                  "the compact voxels match vis_voxel.glsl's uvec4 a brick and uvec2 a cluster");
NPH_STATIC_ASSERT(sizeof(NphVisMesh) % 16U == 0U,
                  "a mesh record's stride keeps each record at the 16 bytes buffer references "
                  "declare (the resolve indexes them as VisMeshRef(...)[i])");
NPH_STATIC_ASSERT(offsetof(NphVisMesh, lod_nodes) % 8U == 0U &&
                      offsetof(NphVisMesh, lod_makers) % 8U == 0U,
                  "addresses are aligned");
NPH_STATIC_ASSERT(sizeof(NphRenderLodNode) == 224U && sizeof(NphRenderLodGroup) == 32U,
                  "the levels of detail match vis_common.glsl's NphVisLodNode and NphVisLodGroup");
NPH_STATIC_ASSERT(offsetof(NphVisMesh, blas) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(sizeof(NphVisCluster) == 16U, "NphVisCluster matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVisCell) == 48U, "NphVisCell matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVisDraw) == 20U, "NphVisDraw is VkDrawIndexedIndirectCommand");
NPH_STATIC_ASSERT(sizeof(NphVisLight) == 96U, "NphVisLight matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVisLightUpdate) == 112U, "NphVisLightUpdate matches its GLSL layout");

// ---------------------------------------------------------------------------
// Pending frees
// ---------------------------------------------------------------------------

// The frame from which nothing the GPU runs can read a range freed now:
// frames up to the one being recorded (or last recorded) may still, and
// frame_begin of that index + NPH_GPU_FRAMES_IN_FLIGHT waits for them.
static u64 safe_frame(const NphRenderer *r)
{
    const u64 current =
        nph_gpu_frame_is_active(r->dev) ? nph_gpu_frame_index(r->dev) : r->last_frame_index;
    return current == NPH_U64_MAX ? 0U : current + NPH_GPU_FRAMES_IN_FLIGHT;
}

static NphRangeAllocator *pool_of(RenderVis *v, u32 pool)
{
    return pool == 0U ? &v->material_ranges : &v->index_ranges;
}

static void free_later(NphRenderer *r, u32 pool, NphRange range)
{
    RenderVis *v = &r->vis;
    if (nph_range_is_null(range)) {
        return;
    }
    const VisPendingFree p = {safe_frame(r), range, pool};
    if (p.frame == 0U || NPH_ARRAY_PUSH(&v->pending, p) != NPH_RESULT_OK) {
        // No frame has been recorded (nothing can read it), or no memory to
        // wait with: free now. The second only risks a frame in flight
        // reading entries a new owner rewrote, which uploads, ordered after
        // the previous frame's graphics work, already rule out.
        nph_range_free(pool_of(v, pool), range);
    }
}

void render_vis_release_pending(NphRenderer *r, u64 current_frame)
{
    RenderVis *v = &r->vis;
    for (u32 i = 0; i < v->pending.len;) {
        if (v->pending.data[i].frame <= current_frame) {
            nph_range_free(pool_of(v, v->pending.data[i].pool), v->pending.data[i].range);
            NPH_ARRAY_REMOVE_SWAP(&v->pending, i);
        } else {
            ++i;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

static NphResult make_buffer(NphRenderer *r, u64 size, u32 usage, NphGpuMemory memory,
                             const char *name, NphGpuBuffer *out)
{
    NphGpuBufferDesc d;
    memset(&d, 0, sizeof d);
    d.size = size;
    d.usage = usage;
    d.memory = memory;
    d.name = name;
    return nph_gpu_buffer_create(r->dev, &d, out);
}

void render_vis_light_layout(u32 lights, VisLightLayout *out)
{
    // Every region holds 16-byte records or is a multiple of 16 bytes, so
    // each starts 16-byte aligned for the shaders' buffer references.
    u64 at = 0U;
    out->histogram = at;
    at += (u64)NPH_RENDER_ZBINS * sizeof(u32);
    out->zbins = at;
    at += (u64)NPH_RENDER_ZBINS * sizeof(u32);
    out->visible = at;
    at += (u64)lights * 4U * sizeof(u32);
    out->staged = at;
    at += (u64)lights * sizeof(NphRenderLightData);
    out->locals = at;
    at += (u64)lights * sizeof(NphRenderLightData);
    out->globals = at;
    at += (u64)lights * sizeof(NphRenderLightData);
    out->rects = at;
    at += (u64)lights * 2U * sizeof(u32);
    out->size = at;
}

NphResult render_vis_scene_create(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    const NphRendererDesc *d = &r->desc;
    NphAllocator *a = r->alloc;
    const u32 storage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    NPH_TRY(make_buffer(r, (u64)d->max_instances * sizeof(NphVisInstance), storage,
                        NPH_GPU_MEMORY_DEVICE, "vis instances", &v->instances));
    NPH_TRY(make_buffer(r, (u64)d->max_instances * sizeof(NphVisTransform), storage,
                        NPH_GPU_MEMORY_DEVICE, "vis previous transforms", &v->prev_transforms));
    NPH_TRY(make_buffer(r, (u64)d->max_meshes * sizeof(NphVisMesh), storage, NPH_GPU_MEMORY_DEVICE,
                        "vis meshes", &v->meshes));
    NPH_TRY(make_buffer(r, (u64)d->max_cells * sizeof(NphVisCell), storage, NPH_GPU_MEMORY_DEVICE,
                        "vis cells", &v->cells));
    NPH_TRY(make_buffer(r, (u64)d->max_material_slots * sizeof(u32), storage, NPH_GPU_MEMORY_DEVICE,
                        "vis material map", &v->material_map));
    // Ranges for the live instances and as many again for destroyed ones
    // whose ranges wait out the frames that may still read them (free_later):
    // a whole population can be replaced between two frames.
    NPH_TRY(
        nph_range_alloc_init(&v->material_ranges, a, d->max_material_slots, 2U * d->max_instances));
    if (!v->mesh_shaders) {
        NPH_TRY(make_buffer(r, (u64)d->meshlet_index_pool * sizeof(u16),
                            storage | (u32)NPH_GPU_BUFFER_USAGE_INDEX, NPH_GPU_MEMORY_DEVICE,
                            "vis meshlet index pool", &v->index_pool));
        NPH_TRY(
            nph_range_alloc_init(&v->index_ranges, a, d->meshlet_index_pool, 2U * d->max_meshes));
    }
    const u32 indirect = storage | (u32)NPH_GPU_BUFFER_USAGE_INDIRECT;
    NPH_TRY(make_buffer(r, sizeof(NphVisCounters),
                        indirect | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC, NPH_GPU_MEMORY_DEVICE,
                        "vis counters", &v->counters));
    NPH_TRY(make_buffer(r, sizeof(NphVisArgs), indirect, NPH_GPU_MEMORY_DEVICE, "vis arguments",
                        &v->args));
    NPH_TRY(make_buffer(r, (u64)d->max_cull_meshlets * 2U * sizeof(u32), storage,
                        NPH_GPU_MEMORY_DEVICE, "vis meshlet candidates", &v->cull_meshlets));
    NPH_TRY(make_buffer(r, (u64)d->max_deferred * (sizeof(u32) + 2U * sizeof(u32)), storage,
                        NPH_GPU_MEMORY_DEVICE, "vis deferred", &v->deferred));
    // Read back by the tests (the clusters a frame drew).
    NPH_TRY(make_buffer(r, (u64)d->max_visible_clusters * sizeof(NphVisCluster),
                        storage | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC, NPH_GPU_MEMORY_DEVICE,
                        "vis clusters", &v->clusters));
    NPH_TRY(make_buffer(r, 2U * (u64)NPH_VIS_RASTER_KINDS * d->max_visible_clusters * sizeof(u32),
                        storage, NPH_GPU_MEMORY_DEVICE, "vis raster lists", &v->lists));
    if (!v->mesh_shaders) {
        NPH_TRY(make_buffer(r, 4U * (u64)d->max_visible_clusters * sizeof(NphVisDraw), indirect,
                            NPH_GPU_MEMORY_DEVICE, "vis draws", &v->draws));
    }
    NPH_TRY(make_buffer(r, (u64)NPH_VIS_LOD_QUEUES * d->max_lod_nodes * 2U * sizeof(u32), storage,
                        NPH_GPU_MEMORY_DEVICE, "vis hierarchy queues", &v->lod_queue));
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NPH_TRY(make_buffer(r, sizeof(NphVisCounters), (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                            NPH_GPU_MEMORY_READBACK, "vis statistics", &v->stats[i]));
    }
    NPH_TRY(make_buffer(r, (u64)d->max_lights * sizeof(NphVisLight), storage, NPH_GPU_MEMORY_DEVICE,
                        "vis lights", &v->lights));
    render_vis_light_layout(d->max_lights, &v->light_layout);
    NPH_TRY(make_buffer(r, v->light_layout.size, storage | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                        NPH_GPU_MEMORY_DEVICE, "vis light grid", &v->light_grid));
    v->light_words = (d->max_lights + 31U) / 32U;
    v->light_flags = NPH_ALLOC_ARRAY_ZEROED(a, u8, d->max_lights);
    v->light_dirty = NPH_ALLOC_ARRAY(a, u32, d->max_lights);
    if (v->light_flags == NULL || v->light_dirty == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    v->slot_state = NPH_ALLOC_ARRAY_ZEROED(a, u8, d->max_instances);
    v->dirty = NPH_ALLOC_ARRAY(a, u32, d->max_instances);
    v->sent = NPH_ALLOC_ARRAY(a, u32, d->max_instances);
    v->sent_next = NPH_ALLOC_ARRAY(a, u32, d->max_instances);
    v->blend = NPH_ALLOC_ARRAY(a, u32, d->max_instances);
    NPH_ARRAY_INIT(&v->pending, a);
    if (v->slot_state == NULL || v->dirty == NULL || v->sent == NULL || v->sent_next == NULL ||
        v->blend == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return NPH_RESULT_OK;
}

void render_vis_scene_destroy(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    NphGpuDevice *dev = r->dev;
    NphAllocator *a = r->alloc;
    const NphRendererDesc *d = &r->desc;
    nph_gpu_buffer_destroy(dev, v->instances);
    nph_gpu_buffer_destroy(dev, v->prev_transforms);
    nph_gpu_buffer_destroy(dev, v->meshes);
    nph_gpu_buffer_destroy(dev, v->cells);
    nph_gpu_buffer_destroy(dev, v->material_map);
    nph_gpu_buffer_destroy(dev, v->index_pool);
    nph_gpu_buffer_destroy(dev, v->counters);
    nph_gpu_buffer_destroy(dev, v->args);
    nph_gpu_buffer_destroy(dev, v->cull_meshlets);
    nph_gpu_buffer_destroy(dev, v->deferred);
    nph_gpu_buffer_destroy(dev, v->clusters);
    nph_gpu_buffer_destroy(dev, v->lists);
    nph_gpu_buffer_destroy(dev, v->draws);
    nph_gpu_buffer_destroy(dev, v->lod_queue);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        nph_gpu_buffer_destroy(dev, v->stats[i]);
    }
    nph_gpu_buffer_destroy(dev, v->lights);
    nph_gpu_buffer_destroy(dev, v->light_grid);
    if (v->light_flags != NULL) {
        NPH_FREE_ARRAY(a, u8, v->light_flags, d->max_lights);
    }
    if (v->light_dirty != NULL) {
        NPH_FREE_ARRAY(a, u32, v->light_dirty, d->max_lights);
    }
    nph_range_alloc_destroy(&v->material_ranges);
    nph_range_alloc_destroy(&v->index_ranges);
    NPH_ARRAY_FREE(&v->pending);
    if (v->slot_state != NULL) {
        NPH_FREE_ARRAY(a, u8, v->slot_state, d->max_instances);
    }
    if (v->dirty != NULL) {
        NPH_FREE_ARRAY(a, u32, v->dirty, d->max_instances);
    }
    if (v->sent != NULL) {
        NPH_FREE_ARRAY(a, u32, v->sent, d->max_instances);
    }
    if (v->sent_next != NULL) {
        NPH_FREE_ARRAY(a, u32, v->sent_next, d->max_instances);
    }
    if (v->blend != NULL) {
        NPH_FREE_ARRAY(a, u32, v->blend, d->max_instances);
    }
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

static NphResult upload_stream(NphRenderer *r, const void *data, u64 size, const char *name,
                               NphGpuBuffer *out)
{
    // Streams read as u32 words (the meshlet triangles) are padded to four
    // bytes; an empty stream still gets a buffer, so every address is valid.
    const u64 padded = (size + 3U) & ~(u64)3U;
    NPH_TRY(make_buffer(r, nph_max_u64(padded, 16U),
                        (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                        NPH_GPU_MEMORY_DEVICE, name, out));
    if (size == 0U) {
        return NPH_RESULT_OK;
    }
    if (padded == size) {
        return nph_gpu_upload_buffer(r->dev, *out, 0U, data, size);
    }
    NPH_TRY(nph_gpu_upload_buffer(r->dev, *out, 0U, data, size - size % 4U));
    u8 tail[4] = {0U, 0U, 0U, 0U};
    memcpy(tail, (const u8 *)data + (size - size % 4U), (size_t)(size % 4U));
    return nph_gpu_upload_buffer(r->dev, *out, size - size % 4U, tail, 4U);
}

// The positions the GPU sees: dequantised exactly as vis_position does.
static void dequantise(const NphRenderMeshDesc *d, NphVec3 *out)
{
    const f32 k = 1.0F / 65535.0F;
    for (u32 i = 0; i < d->vertex_count; ++i) {
        const NphRenderVertexPosition p = d->positions[i];
        out[i] = nph_vec3(d->position_offset.x + (f32)p.x * k * d->position_scale.x,
                          d->position_offset.y + (f32)p.y * k * d->position_scale.y,
                          d->position_offset.z + (f32)p.z * k * d->position_scale.z);
    }
}

// The meshlets of a mesh made without them.
static NphResult build_meshlets(NphRenderer *r, const NphRenderMeshDesc *d, NphRenderMeshlets *out)
{
    NphAllocator *a = r->alloc;
    NphVec3 *positions = NPH_ALLOC_ARRAY(a, NphVec3, d->vertex_count);
    NphRenderIndexRange *ranges = NPH_ALLOC_ARRAY(a, NphRenderIndexRange, d->submesh_count);
    NphResult res = positions != NULL && ranges != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (res == NPH_RESULT_OK) {
        dequantise(d, positions);
        for (u32 s = 0; s < d->submesh_count; ++s) {
            ranges[s].index_offset = d->submeshes[s].index_offset;
            ranges[s].index_count = d->submeshes[s].index_count;
        }
        NphRenderMeshletInput in;
        memset(&in, 0, sizeof in);
        in.positions = positions;
        in.vertex_count = d->vertex_count;
        in.indices = d->indices;
        in.index_count = d->index_count;
        in.ranges = ranges;
        in.range_count = d->submesh_count;
        in.cone_weight = 0.25F; // the cooker's default (docs/cook.md)
        in.scratch = a;
        res = nph_render_meshlets_build(&in, out);
    }
    if (positions != NULL) {
        NPH_FREE_ARRAY(a, NphVec3, positions, d->vertex_count);
    }
    if (ranges != NULL) {
        NPH_FREE_ARRAY(a, NphRenderIndexRange, ranges, d->submesh_count);
    }
    return res;
}

// Each cluster's maker as the culls test it (NphVisMesh.lod_makers): the
// sphere of the group that made it, then, after every cluster's, its error,
// its record's bits (or the records given, a level apart's); at level 0, a
// sphere of zeros and an error of 0.
static NphResult upload_makers(NphRenderer *r, RenderMesh *m, const NphRenderMeshDesc *d)
{
    const u32 n = d->meshlet_count;
    NphVec4 *spheres = NPH_ALLOC_ARRAY(r->alloc, NphVec4, n);
    f32 *errors = NPH_ALLOC_ARRAY(r->alloc, f32, n);
    NphResult res = spheres != NULL && errors != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    const u64 at = (u64)n * sizeof(NphVec4); // the errors' place
    if (res == NPH_RESULT_OK) {
        for (u32 i = 0; i < n; ++i) {
            if (d->lod_makers != NULL) {
                const NphRenderLodMaker *k = &d->lod_makers[i];
                spheres[i] = nph_vec4_from_vec3(k->center, k->radius);
                errors[i] = k->error;
                continue;
            }
            const u32 maker = d->cluster_lods[i].group;
            const NphRenderLodGroup *g =
                maker != NPH_RENDER_LOD_NONE ? &d->lod_groups[maker] : NULL;
            spheres[i] = g != NULL ? nph_vec4_from_vec3(g->center, g->radius) : nph_vec4_zero();
            errors[i] = g != NULL ? g->error : 0.0F;
        }
        res =
            make_buffer(r, at + (u64)n * sizeof(f32),
                        (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                        NPH_GPU_MEMORY_DEVICE, "mesh lod makers", &m->lod_makers);
    }
    if (res == NPH_RESULT_OK) {
        res = nph_gpu_upload_buffer(r->dev, m->lod_makers, 0U, spheres, at);
    }
    if (res == NPH_RESULT_OK) {
        res = nph_gpu_upload_buffer(r->dev, m->lod_makers, at, errors, (u64)n * sizeof(f32));
    }
    if (spheres != NULL) {
        NPH_FREE_ARRAY(r->alloc, NphVec4, spheres, n);
    }
    if (errors != NULL) {
        NPH_FREE_ARRAY(r->alloc, f32, errors, n);
    }
    return res;
}

// A mesh's levels of detail (NphVisMesh.lod_*): the hierarchy, the groups,
// their clusters, and each cluster's maker.
static NphResult upload_lods(NphRenderer *r, RenderMesh *m, const NphRenderMeshDesc *d,
                             NphVisMesh *rec)
{
    NPH_TRY(upload_stream(r, d->lod_nodes, (u64)d->lod_node_count * sizeof(NphRenderLodNode),
                          "mesh lod nodes", &m->lod_nodes));
    NPH_TRY(upload_stream(r, d->lod_groups, (u64)d->lod_group_count * sizeof(NphRenderLodGroup),
                          "mesh lod groups", &m->lod_groups));
    NPH_TRY(upload_stream(r, d->lod_group_clusters, (u64)d->lod_group_cluster_count * sizeof(u32),
                          "mesh lod group clusters", &m->lod_group_clusters));
    NPH_TRY(upload_makers(r, m, d));
    rec->lod_nodes = nph_gpu_buffer_address(r->dev, m->lod_nodes);
    rec->lod_groups = nph_gpu_buffer_address(r->dev, m->lod_groups);
    rec->lod_group_clusters = nph_gpu_buffer_address(r->dev, m->lod_group_clusters);
    rec->lod_makers = nph_gpu_buffer_address(r->dev, m->lod_makers);
    rec->lod_clusters = d->meshlet_count;
    // The finest consumer of level 0 (vis_cull_instances.comp's shortcut): a
    // cluster no group consumes (the levels allow it) is fine at any distance.
    rec->lod_level0_error = NPH_F32_MAX;
    for (u32 i = 0; i < d->lod_level0_meshlets; ++i) {
        const u32 parent = d->cluster_lods[i].parent;
        if (parent != NPH_RENDER_LOD_NONE) {
            rec->lod_level0_error = nph_f32_min(rec->lod_level0_error, d->lod_groups[parent].error);
        }
    }
    // The voxel clusters' records, bricks and voxels, compact (vis_voxel.glsl).
    if (d->voxel_bricks != NULL && d->voxel_brick_count != 0U) {
        NPH_TRY(upload_stream(r, d->voxel_clusters,
                              (u64)d->voxel_cluster_count * sizeof(NphRenderVoxelCluster),
                              "mesh voxel clusters", &m->voxel_clusters));
        NPH_TRY(upload_stream(r, d->voxel_bricks,
                              (u64)d->voxel_brick_count * sizeof(NphRenderPackedBrick),
                              "mesh voxel bricks", &m->voxel_bricks));
        NPH_TRY(upload_stream(r, d->voxel_words, (u64)d->voxel_word_count * sizeof(u32),
                              "mesh voxels", &m->voxels));
        rec->voxel_clusters = nph_gpu_buffer_address(r->dev, m->voxel_clusters);
        rec->voxel_bricks = nph_gpu_buffer_address(r->dev, m->voxel_bricks);
        rec->voxels = nph_gpu_buffer_address(r->dev, m->voxels);
    }
    // Without the levels (a view at threshold 0, the shadow map's pages),
    // the mesh draws level 0's meshlets.
    rec->meshlet_count = d->lod_level0_meshlets;
    return NPH_RESULT_OK;
}

static NphResult upload_mesh(NphRenderer *r, RenderMesh *m, u32 slot, const NphRenderMeshDesc *d,
                             const NphRenderMeshlet *meshlets, const NphRenderMeshletBounds *bounds,
                             u32 meshlet_count, const u32 *vertices, u32 vertex_count,
                             const u8 *triangles, u32 triangle_count)
{
    RenderVis *v = &r->vis;
    NPH_TRY(upload_stream(r, meshlets, (u64)meshlet_count * sizeof(NphRenderMeshlet),
                          "mesh meshlets", &m->meshlets));
    NPH_TRY(upload_stream(r, bounds, (u64)meshlet_count * sizeof(NphRenderMeshletBounds),
                          "mesh meshlet bounds", &m->meshlet_bounds));
    NPH_TRY(upload_stream(r, vertices, (u64)vertex_count * sizeof(u32), "mesh meshlet vertices",
                          &m->meshlet_vertices));
    NPH_TRY(upload_stream(r, triangles, (u64)triangle_count * 3U, "mesh meshlet triangles",
                          &m->meshlet_triangles));
    // Each submesh's material slot and first index (NphVisMesh.submeshes).
    u32 table[NPH_RENDER_MAX_SUBMESHES][2];
    for (u32 s = 0; s < d->submesh_count; ++s) {
        table[s][0] = d->submeshes[s].material_slot;
        table[s][1] = d->submeshes[s].index_offset;
    }
    NPH_TRY(upload_stream(r, table, (u64)d->submesh_count * 2U * sizeof(u32), "mesh submeshes",
                          &m->submesh_table));
    u32 index_base = 0U;
    if (!v->mesh_shaders && triangle_count != 0U) {
        // The indirect raster's indices: the meshlet-local corners widened
        // to 16 bits, at the mesh's place in the pool (a draw's first
        // index is the meshlet's first corner from there).
        const u32 corners = triangle_count * 3U;
        NPH_TRY(nph_range_alloc(&v->index_ranges, corners, &m->index_range));
        index_base = m->index_range.offset;
        u16 *wide = NPH_ALLOC_ARRAY(r->alloc, u16, corners);
        if (wide == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        for (u32 i = 0; i < corners; ++i) {
            wide[i] = triangles[i];
        }
        const NphResult res = nph_gpu_upload_buffer(r->dev, v->index_pool, (u64)index_base * 2U,
                                                    wide, (u64)corners * 2U);
        NPH_FREE_ARRAY(r->alloc, u16, wide, corners);
        NPH_TRY(res);
    }
    m->meshlet_count = meshlet_count;
    m->meshlet_triangle_count = triangle_count;
    m->sphere = nph_aabb_bounding_sphere(d->bounds);

    NphVisMesh rec;
    memset(&rec, 0, sizeof rec);
    rec.position_offset = nph_vec4_from_vec3(d->position_offset, 0.0F);
    rec.position_scale = nph_vec4_from_vec3(d->position_scale, 0.0F);
    rec.sphere = nph_vec4_from_vec3(m->sphere.center, m->sphere.radius);
    rec.positions = m->positions_address;
    rec.attribs = m->attribs_address;
    rec.meshlets = nph_gpu_buffer_address(r->dev, m->meshlets);
    rec.meshlet_bounds = nph_gpu_buffer_address(r->dev, m->meshlet_bounds);
    rec.meshlet_vertices = nph_gpu_buffer_address(r->dev, m->meshlet_vertices);
    rec.meshlet_triangles = nph_gpu_buffer_address(r->dev, m->meshlet_triangles);
    rec.submeshes = nph_gpu_buffer_address(r->dev, m->submesh_table);
    rec.indices = nph_gpu_buffer_address(r->dev, m->indices);
    rec.meshlet_count = meshlet_count;
    rec.index_base = index_base;
    rec.blas =
        nph_gpu_accel_is_valid(r->dev, m->blas) ? nph_gpu_accel_address(r->dev, m->blas) : 0U;
    if (d->lod_nodes != NULL) {
        NPH_TRY(upload_lods(r, m, d, &rec));
    }
    return nph_gpu_upload_buffer(r->dev, v->meshes, (u64)slot * sizeof rec, &rec, sizeof rec);
}

NphResult render_vis_mesh_set_blas(NphRenderer *r, u32 slot, u64 address)
{
    if (!r->vis.supported) {
        return NPH_RESULT_OK;
    }
    return nph_gpu_upload_buffer(r->dev, r->vis.meshes,
                                 (u64)slot * sizeof(NphVisMesh) + offsetof(NphVisMesh, blas),
                                 &address, sizeof address);
}

NphResult render_vis_mesh_create(NphRenderer *r, u32 slot, const NphRenderMeshDesc *d,
                                 u32 lod_depth)
{
    RenderMesh *m = &r->meshes[slot];
    m->index_range = NPH_RANGE_NULL;
    if (!r->vis.supported) {
        return NPH_RESULT_OK;
    }
    if (d->meshlets != NULL) {
        NPH_TRY(upload_mesh(r, m, slot, d, d->meshlets, d->meshlet_bounds, d->meshlet_count,
                            d->meshlet_vertices, d->meshlet_vertex_count, d->meshlet_triangles,
                            d->meshlet_triangle_count));
        if (lod_depth != 0U) {
            // Counted once created, so that a failed creation's clean-up
            // has nothing to take back.
            m->lod_depth = lod_depth;
            r->vis.lod_depths[lod_depth - 1U] += 1U;
        }
        if (d->voxel_bricks != NULL && d->voxel_brick_count != 0U) {
            m->voxels_counted = 1;
            r->vis.voxel_meshes += 1U;
        }
        return NPH_RESULT_OK;
    }
    NphRenderMeshlets built;
    nph_render_meshlets_init(&built, r->alloc);
    NphResult res = build_meshlets(r, d, &built);
    if (res == NPH_RESULT_OK) {
        res = upload_mesh(r, m, slot, d, built.meshlets.data, built.bounds.data, built.meshlets.len,
                          built.vertices.data, built.vertices.len, built.triangles.data,
                          built.triangles.len / 3U);
    }
    nph_render_meshlets_free(&built);
    return res;
}

void render_vis_mesh_destroy(NphRenderer *r, u32 slot)
{
    RenderMesh *m = &r->meshes[slot];
    nph_gpu_buffer_destroy(r->dev, m->meshlets);
    nph_gpu_buffer_destroy(r->dev, m->meshlet_bounds);
    nph_gpu_buffer_destroy(r->dev, m->meshlet_vertices);
    nph_gpu_buffer_destroy(r->dev, m->meshlet_triangles);
    nph_gpu_buffer_destroy(r->dev, m->submesh_table);
    nph_gpu_buffer_destroy(r->dev, m->lod_nodes);
    nph_gpu_buffer_destroy(r->dev, m->lod_groups);
    nph_gpu_buffer_destroy(r->dev, m->lod_group_clusters);
    nph_gpu_buffer_destroy(r->dev, m->lod_makers);
    nph_gpu_buffer_destroy(r->dev, m->voxel_clusters);
    nph_gpu_buffer_destroy(r->dev, m->voxel_bricks);
    nph_gpu_buffer_destroy(r->dev, m->voxels);
    m->meshlets = NPH_GPU_BUFFER_NULL;
    m->meshlet_bounds = NPH_GPU_BUFFER_NULL;
    m->meshlet_vertices = NPH_GPU_BUFFER_NULL;
    m->meshlet_triangles = NPH_GPU_BUFFER_NULL;
    m->submesh_table = NPH_GPU_BUFFER_NULL;
    m->lod_nodes = NPH_GPU_BUFFER_NULL;
    m->lod_groups = NPH_GPU_BUFFER_NULL;
    m->lod_group_clusters = NPH_GPU_BUFFER_NULL;
    m->lod_makers = NPH_GPU_BUFFER_NULL;
    m->voxel_clusters = NPH_GPU_BUFFER_NULL;
    m->voxel_bricks = NPH_GPU_BUFFER_NULL;
    m->voxels = NPH_GPU_BUFFER_NULL;
    if (m->lod_depth != 0U) {
        r->vis.lod_depths[m->lod_depth - 1U] -= 1U;
        m->lod_depth = 0U;
    }
    if (m->voxels_counted) {
        r->vis.voxel_meshes -= 1U;
        m->voxels_counted = 0;
    }
    if (r->vis.supported) {
        free_later(r, 1U, m->index_range);
    }
    m->index_range = NPH_RANGE_NULL;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

static void mark_dirty(RenderVis *v, u32 slot)
{
    if ((v->slot_state[slot] & (u8)RENDER_SLOT_DIRTY) == 0U) {
        v->slot_state[slot] |= (u8)RENDER_SLOT_DIRTY;
        v->dirty[v->dirty_count++] = slot;
    }
}

static bool instance_blends(const NphRenderer *r, const RenderInstance *inst)
{
    for (u32 i = 0; i < inst->material_count; ++i) {
        const u32 m = render_slot_of(&r->material_pool, inst->materials[i].value);
        if (m != NPH_U32_MAX && r->materials[m].desc.alpha_mode == NPH_RENDER_ALPHA_BLEND) {
            return true;
        }
    }
    return false;
}

static void blend_add(RenderVis *v, RenderInstance *inst, u32 slot)
{
    inst->blend_index = v->blend_count;
    v->blend[v->blend_count++] = slot;
}

static void blend_remove(NphRenderer *r, RenderInstance *inst)
{
    RenderVis *v = &r->vis;
    if (inst->blend_index == NPH_U32_MAX) {
        return;
    }
    const u32 last = v->blend[--v->blend_count];
    v->blend[inst->blend_index] = last;
    r->instances[last].blend_index = inst->blend_index;
    inst->blend_index = NPH_U32_MAX;
}

NphResult render_vis_instance_created(NphRenderer *r, u32 slot)
{
    RenderVis *v = &r->vis;
    RenderInstance *inst = &r->instances[slot];
    inst->material_range = NPH_RANGE_NULL;
    inst->blend_index = NPH_U32_MAX;
    if (!v->supported) {
        return NPH_RESULT_OK;
    }
    if (inst->material_count != 0U) {
        NPH_TRY(nph_range_alloc(&v->material_ranges, inst->material_count, &inst->material_range));
        u32 handles[64];
        for (u32 first = 0; first < inst->material_count; first += 64U) {
            const u32 n = nph_min_u32(inst->material_count - first, 64U);
            for (u32 i = 0; i < n; ++i) {
                handles[i] = inst->materials[first + i].value;
            }
            const NphResult res = nph_gpu_upload_buffer(
                r->dev, v->material_map, ((u64)inst->material_range.offset + first) * sizeof(u32),
                handles, (u64)n * sizeof(u32));
            if (res != NPH_RESULT_OK) {
                nph_range_free(&v->material_ranges, inst->material_range);
                inst->material_range = NPH_RANGE_NULL;
                return res;
            }
        }
        r->pending_uploads = 1;
    }
    v->slot_state[slot] |= (u8)RENDER_SLOT_FRESH;
    mark_dirty(v, slot);
    if (instance_blends(r, inst)) {
        blend_add(v, inst, slot);
    }
    return NPH_RESULT_OK;
}

void render_vis_instance_changed(NphRenderer *r, u32 slot)
{
    if (r->vis.supported) {
        mark_dirty(&r->vis, slot);
    }
}

void render_vis_instance_destroyed(NphRenderer *r, u32 slot)
{
    RenderVis *v = &r->vis;
    if (!v->supported) {
        return;
    }
    RenderInstance *inst = &r->instances[slot];
    blend_remove(r, inst);
    free_later(r, 0U, inst->material_range);
    inst->material_range = NPH_RANGE_NULL;
    // Sent as not visible (the slot is dead when the update is written).
    v->slot_state[slot] = (u8)(v->slot_state[slot] & ~(u32)RENDER_SLOT_FRESH);
    mark_dirty(v, slot);
}

void render_vis_materials_changed(NphRenderer *r)
{
    r->vis.blend_stale = 1;
}

const u32 *render_vis_blend_list(NphRenderer *r, u32 *out_count)
{
    RenderVis *v = &r->vis;
    if (v->blend_stale) {
        v->blend_count = 0U;
        for (u32 i = 0; i < r->desc.max_instances; ++i) {
            RenderInstance *inst = &r->instances[i];
            inst->blend_index = NPH_U32_MAX;
            if (inst->handle != 0U && instance_blends(r, inst)) {
                blend_add(v, inst, i);
            }
        }
        v->blend_stale = 0;
    }
    *out_count = v->blend_count;
    return v->blend;
}

// The high and low floats of a 64-bit coordinate: their sum is the value
// to about 2^-48 of its magnitude (render_vis_split's GPU counterpart,
// vis_relative_translation, subtracts the camera's split part by part).
static void split(f64 value, f32 *high, f32 *low)
{
    *high = (f32)value;
    *low = (f32)(value - (f64)*high);
}

// Whether the linear part keeps angles: equal column lengths, orthogonal
// columns (rotation, uniform scale, and mirroring). The normal-cone test
// is only valid then.
static bool is_similarity(NphMat3 l)
{
    const f32 l0 = nph_vec3_length(l.c[0]);
    const f32 l1 = nph_vec3_length(l.c[1]);
    const f32 l2 = nph_vec3_length(l.c[2]);
    const f32 tol = 1e-5F;
    return nph_f32_abs(l1 - l0) <= tol * l0 && nph_f32_abs(l2 - l0) <= tol * l0 &&
           nph_f32_abs(nph_vec3_dot(l.c[0], l.c[1])) <= tol * l0 * l1 &&
           nph_f32_abs(nph_vec3_dot(l.c[0], l.c[2])) <= tol * l0 * l2 &&
           nph_f32_abs(nph_vec3_dot(l.c[1], l.c[2])) <= tol * l1 * l2;
}

static void instance_record(const NphRenderer *r, u32 slot, bool fresh, NphVisInstance *out)
{
    memset(out, 0, sizeof *out);
    const RenderInstance *inst = &r->instances[slot];
    if (inst->handle == 0U) {
        return; // destroyed: not visible
    }
    const NphMat3 l = inst->transform.linear;
    f32 hx = 0.0F;
    f32 hy = 0.0F;
    f32 hz = 0.0F;
    f32 lx = 0.0F;
    f32 ly = 0.0F;
    f32 lz = 0.0F;
    split(inst->transform.translation.x, &hx, &lx);
    split(inst->transform.translation.y, &hy, &ly);
    split(inst->transform.translation.z, &hz, &lz);
    out->transform.row0 = nph_vec4_from_vec3(nph_mat3_row(l, 0U), hx);
    out->transform.row1 = nph_vec4_from_vec3(nph_mat3_row(l, 1U), hy);
    out->transform.row2 = nph_vec4_from_vec3(nph_mat3_row(l, 2U), hz);
    out->transform.translation_low = nph_vec4(lx, ly, lz, 0.0F);
    out->mesh = render_slot_of(&r->mesh_pool, inst->mesh.value);
    out->material_base = nph_range_is_null(inst->material_range) ? 0U : inst->material_range.offset;
    out->material_count = nph_range_is_null(inst->material_range) ? 0U : inst->material_count;
    out->flags = (inst->hidden ? 0U : (u32)NPH_VIS_INSTANCE_VISIBLE) |
                 (nph_mat3_determinant(l) < 0.0F ? (u32)NPH_VIS_INSTANCE_MIRRORED : 0U) |
                 (is_similarity(l) ? (u32)NPH_VIS_INSTANCE_SIMILAR : 0U) |
                 (fresh ? (u32)NPH_VIS_INSTANCE_FRESH : 0U) |
                 (inst->proxy ? (u32)NPH_VIS_INSTANCE_PROXY : 0U) |
                 (inst->proxy_counted ? (u32)NPH_VIS_INSTANCE_PROXY_COUNTED : 0U) |
                 (inst->proxy_finest ? (u32)NPH_VIS_INSTANCE_FINEST : 0U) |
                 (inst->cell << (u32)NPH_VIS_INSTANCE_CELL_SHIFT);
}

// ---------------------------------------------------------------------------
// Cells
// ---------------------------------------------------------------------------

NphResult render_vis_cell_created(NphRenderer *r, u32 slot, const NphRenderCellDesc *desc)
{
    if (!r->vis.supported) {
        return NPH_RESULT_OK;
    }
    NphVisCell rec;
    memset(&rec, 0, sizeof rec);
    f32 hx = 0.0F;
    f32 hy = 0.0F;
    f32 hz = 0.0F;
    f32 lx = 0.0F;
    f32 ly = 0.0F;
    f32 lz = 0.0F;
    split(desc->center.x, &hx, &lx);
    split(desc->center.y, &hy, &ly);
    split(desc->center.z, &hz, &lz);
    rec.center_high = nph_vec4(hx, hy, hz, desc->distance);
    rec.center_low = nph_vec4(lx, ly, lz, desc->proxy_error);
    rec.extent = nph_vec4_from_vec3(desc->extent, 0.0F);
    r->pending_uploads = 1;
    return nph_gpu_upload_buffer(r->dev, r->vis.cells, (u64)slot * sizeof rec, &rec, sizeof rec);
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

void render_vis_light_changed(NphRenderer *r, u32 slot)
{
    RenderVis *v = &r->vis;
    if (!v->supported || v->light_flags[slot] != 0U) {
        return;
    }
    v->light_flags[slot] = 1U;
    v->light_dirty[v->light_dirty_count++] = slot;
}

static void light_record(const NphRenderer *r, u32 slot, NphVisLight *out)
{
    memset(out, 0, sizeof *out);
    const RenderLight *l = &r->lights[slot];
    if (l->handle == 0U) {
        return; // destroyed: a free slot
    }
    const NphDVec3 p = l->desc.position;
    f32 hx = 0.0F;
    f32 hy = 0.0F;
    f32 hz = 0.0F;
    f32 lx = 0.0F;
    f32 ly = 0.0F;
    f32 lz = 0.0F;
    split(p.x, &hx, &lx);
    split(p.y, &hy, &ly);
    split(p.z, &hz, &lz);
    render_light_data(&l->desc, nph_vec3(hx, hy, hz), &out->data);
    if (slot == r->vsm.sun_slot) {
        out->data.color.w = 2.0F; // the sun: its virtual shadow map where the view has one
    }
    out->position_low = nph_vec4(lx, ly, lz, l->desc.range);
    out->live = 1U;
}

u32 render_vis_write_light_updates(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    NphVisLightUpdate *out = render_frame_at(r, r->layout.vis_light_updates_offset);
    for (u32 i = 0; i < v->light_dirty_count; ++i) {
        const u32 slot = v->light_dirty[i];
        NphVisLightUpdate *u = &out[i];
        memset(u, 0, sizeof *u);
        u->slot = slot;
        light_record(r, slot, &u->light);
        v->light_flags[slot] = 0U;
    }
    const u32 n = v->light_dirty_count;
    v->light_dirty_count = 0U;
    return n;
}

u32 render_vis_write_updates(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    NphVisInstanceUpdate *out = render_frame_at(r, r->layout.vis_updates_offset);
    const u32 cap = r->desc.max_instance_updates;
    u32 n = 0U;
    // Instances sent last frame catch their previous transform up, unless
    // they change again (their update below moves it anyway). These never
    // wait: a lagging previous transform would give wrong motion.
    for (u32 i = 0; i < v->sent_count; ++i) {
        const u32 slot = v->sent[i];
        v->slot_state[slot] = (u8)(v->slot_state[slot] & ~(u32)RENDER_SLOT_SENT);
        if ((v->slot_state[slot] & (u8)RENDER_SLOT_DIRTY) != 0U) {
            continue;
        }
        NphVisInstanceUpdate *u = &out[n++];
        memset(u, 0, sizeof *u);
        u->slot = slot;
        u->copy_previous = 1U;
    }
    // Then the changed instances, as many as fit; the rest wait a frame.
    const u32 catch_ups = n;
    v->sent_next_count = 0U;
    u32 kept = 0U;
    for (u32 i = 0; i < v->dirty_count; ++i) {
        const u32 slot = v->dirty[i];
        if (n == cap) {
            v->dirty[kept++] = slot;
            continue;
        }
        const bool fresh = (v->slot_state[slot] & (u8)RENDER_SLOT_FRESH) != 0U;
        NphVisInstanceUpdate *u = &out[n++];
        memset(u, 0, sizeof *u);
        u->slot = slot;
        instance_record(r, slot, fresh, &u->instance);
        v->slot_state[slot] =
            (u8)(v->slot_state[slot] & ~((u32)RENDER_SLOT_DIRTY | (u32)RENDER_SLOT_FRESH));
        v->slot_state[slot] |= (u8)RENDER_SLOT_SENT;
        v->sent_next[v->sent_next_count++] = slot;
    }
    if (kept != 0U && !v->updates_logged) {
        NPH_LOG_WARN(&NPH_LOG_RENDER,
                     "%s: %u instance updates wait for the next frame (max_instance_updates %u; "
                     "said once)",
                     r->name, kept, cap);
        v->updates_logged = 1;
    }
    v->dirty_count = kept;
    v->frame_changed = n - catch_ups;
    u32 *swap = v->sent;
    v->sent = v->sent_next;
    v->sent_next = swap;
    v->sent_count = v->sent_next_count;
    return n;
}

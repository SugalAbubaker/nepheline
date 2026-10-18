// SPDX-License-Identifier: Apache-2.0
// Ray-traced shadows' acceleration structures (render/renderer.h;
// docs/render.md "Shadows"), where the device has ray queries:
//
//   - a bottom level per mesh over its quantised positions (16-bit unorm,
//     read as they are where the device builds from them, else a float copy
//     of the same values), created with the mesh and built in the next frame
//     that traces, before that frame's top level;
//   - a top level over the instances: their records fold each mesh's
//     dequantisation into their transform, and come first in the record
//     array, hidden and free slots after them as inactive records (a null
//     structure and a zero mask). Vulkan allows inactive records anywhere,
//     but lavapipe (Mesa 25.2) encodes only as many leaves as are active,
//     from the start of the array, so an active record behind an inactive
//     one would vanish from the structure. Its space is the world's, less an
//     anchor near the camera, so 32-bit coordinates hold anywhere; a shader
//     moves a view (camera-relative) point into it by adding tlas_offset,
//     the camera's position less the anchor. The reference writes the
//     records on the CPU, as it does its draws, and rebuilds every frame
//     with the anchor at the camera. The production path writes them on the
//     GPU from its GPU scene (rt_instances.comp), so its frame costs the CPU
//     nothing per instance, and rebuilds only when its instances changed,
//     after a frame that did not trace, or when the camera has moved more
//     than RT_ANCHOR_RADIUS from the anchor (which then moves to it): a
//     camera moving through a still scene costs no build.
//
// The builds share one scratch buffer, sized at creation for the top level
// and grown with each mesh whose bottom level needs more, so a frame never
// allocates. A frame's bottom-level builds fill the scratch in batches,
// with a barrier between batches; the top level's build follows them in
// the same pass, after a barrier that also makes the bottom levels visible
// to the shaders that trace.
#include <nepheline/core/log.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphGpuAccelInstance) == 64U, "an instance record is 64 bytes");

// How far the camera may move from the top level's anchor before the
// production path rebuilds it around the camera. Ray origins are offset by
// units in the last place of their coordinates in that space (rt_common.glsl),
// so near the camera the offset is at most 256 ulp(16 m), half a millimetre.
#define RT_ANCHOR_RADIUS 16.0

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1U) / a * a;
}

static u64 scratch_alignment(const NphRenderer *r)
{
    return nph_max_u32(nph_gpu_device_info(r->dev)->limits.accel_scratch_alignment, 1U);
}

// The mesh's triangles as one geometry (every submesh shares the index
// buffer; the shadows treat every triangle as opaque).
static void mesh_geometry(const NphRenderer *r, const RenderMesh *m, NphGpuAccelTriangles *out)
{
    memset(out, 0, sizeof *out);
    if (r->rt.unorm_vertices) {
        out->vertices = m->positions;
        out->vertex_stride = sizeof(NphRenderVertexPosition);
        out->vertex_format = NPH_GPU_FORMAT_R16G16B16A16_UNORM;
    } else {
        out->vertices = m->blas_vertices;
        out->vertex_stride = 3U * sizeof(f32);
        out->vertex_format = NPH_GPU_FORMAT_R32G32B32_SFLOAT;
    }
    out->vertex_count = m->vertex_count;
    out->indices = m->indices;
    out->triangle_count = m->index_count / 3U;
    out->opaque = 1;
}

// Room in the scratch for one build of `size` bytes at the device's
// alignment. The old buffer goes only once the new one exists (deferred:
// frames in flight may still build with it).
static NphResult reserve_scratch(NphRenderer *r, u64 size)
{
    RenderRt *rt = &r->rt;
    const u64 align = scratch_alignment(r);
    const u64 need = align_up(size, align) + align; // the buffer's start aligned up
    if (need <= rt->scratch_size) {
        return NPH_RESULT_OK;
    }
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = need;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "rt scratch";
    NphGpuBuffer scratch = NPH_GPU_BUFFER_NULL;
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &scratch));
    nph_gpu_buffer_destroy(r->dev, rt->scratch);
    rt->scratch = scratch;
    rt->scratch_size = need;
    return NPH_RESULT_OK;
}

NphResult render_rt_create(NphRenderer *r)
{
    RenderRt *rt = &r->rt;
    NphGpuDevice *dev = r->dev;
    if (!rt->supported) {
        return NPH_RESULT_OK;
    }
    rt->unorm_vertices = nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R16G16B16A16_UNORM,
                                                 (u32)NPH_GPU_FORMAT_FEATURE_ACCEL_VERTEX);
    const u32 n = r->desc.max_instances;
    NphGpuAccelSizes sizes;
    NPH_TRY(nph_gpu_accel_tlas_sizes(dev, n, (u32)NPH_GPU_ACCEL_PREFER_BUILD, &sizes));
    NphGpuAccelDesc ad;
    memset(&ad, 0, sizeof ad);
    ad.type = NPH_GPU_ACCEL_TOP;
    ad.size = sizes.accel;
    ad.name = "rt instances (top level)";
    NPH_TRY(nph_gpu_accel_create(dev, &ad, &rt->tlas));
    NPH_TRY(reserve_scratch(r, sizes.scratch));
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = (u64)n * sizeof(NphGpuAccelInstance);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_ACCEL_INPUT;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "rt instance records";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &rt->instances));
    rt->pending = NPH_ALLOC_ARRAY(r->alloc, u32, r->desc.max_meshes);
    if (rt->pending == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = r->shaders[RENDER_SHADER_RT_INSTANCES];
    cd.name = "rt instances";
    NPH_TRY(nph_gpu_pipeline_create_compute(dev, &cd, &rt->instance_writer));
    NPH_LOG_INFO(&NPH_LOG_RENDER,
                 "%s: ray-traced shadows: a top level of %u instances (%.2f MB, records %.2f MB, "
                 "scratch %.2f MB); bottom levels from %s positions",
                 r->name, n, (f64)sizes.accel / 1.0e6, (f64)bd.size / 1.0e6,
                 (f64)rt->scratch_size / 1.0e6, rt->unorm_vertices ? "unorm16" : "float copies of");
    return NPH_RESULT_OK;
}

void render_rt_destroy(NphRenderer *r)
{
    RenderRt *rt = &r->rt;
    NphGpuDevice *dev = r->dev;
    if (!rt->supported) {
        return;
    }
    nph_gpu_pipeline_destroy(dev, rt->instance_writer);
    nph_gpu_accel_destroy(dev, rt->tlas);
    nph_gpu_buffer_destroy(dev, rt->instances);
    nph_gpu_buffer_destroy(dev, rt->scratch);
    if (rt->pending != NULL) {
        NPH_FREE_ARRAY(r->alloc, u32, rt->pending, r->desc.max_meshes);
    }
    memset(rt, 0, sizeof *rt);
}

NphResult render_rt_mesh_create(NphRenderer *r, u32 slot, const NphRenderMeshDesc *desc)
{
    RenderRt *rt = &r->rt;
    RenderMesh *m = &r->meshes[slot];
    if (!rt->supported) {
        return NPH_RESULT_OK;
    }
    NphGpuDevice *dev = r->dev;
    if (!rt->unorm_vertices) {
        // The float values the unorm decode gives (q / 65535), copied once.
        const u64 count = desc->vertex_count;
        f32 *v = NPH_ALLOC_ARRAY(r->alloc, f32, count * 3U);
        if (v == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        for (u64 i = 0; i < count; ++i) {
            const NphRenderVertexPosition p = desc->positions[i];
            v[i * 3U] = (f32)p.x / 65535.0F;
            v[i * 3U + 1U] = (f32)p.y / 65535.0F;
            v[i * 3U + 2U] = (f32)p.z / 65535.0F;
        }
        NphGpuBufferDesc bd;
        memset(&bd, 0, sizeof bd);
        bd.size = count * 3U * sizeof(f32);
        bd.usage = (u32)NPH_GPU_BUFFER_USAGE_ACCEL_INPUT | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
        bd.memory = NPH_GPU_MEMORY_DEVICE;
        bd.name = "rt vertices";
        NphResult res = nph_gpu_buffer_create(dev, &bd, &m->blas_vertices);
        if (res == NPH_RESULT_OK) {
            res = nph_gpu_upload_buffer(dev, m->blas_vertices, 0U, v, bd.size);
        }
        NPH_FREE_ARRAY(r->alloc, f32, v, count * 3U);
        NPH_TRY(res);
    }
    NphGpuAccelTriangles g;
    mesh_geometry(r, m, &g);
    NphGpuAccelSizes sizes;
    NPH_TRY(nph_gpu_accel_blas_sizes(dev, &g, 1U, (u32)NPH_GPU_ACCEL_PREFER_TRACE, &sizes));
    NPH_TRY(reserve_scratch(r, sizes.scratch));
    NphGpuAccelDesc ad;
    memset(&ad, 0, sizeof ad);
    ad.type = NPH_GPU_ACCEL_BOTTOM;
    ad.size = sizes.accel;
    ad.name = desc->name != NULL ? desc->name : "rt mesh";
    NPH_TRY(nph_gpu_accel_create(dev, &ad, &m->blas));
    NPH_LOG_DEBUG(&NPH_LOG_RENDER, "%s: %s: a bottom level of %u triangles, %.1f KB", r->name,
                  ad.name, g.triangle_count, (f64)sizes.accel / 1.0e3);
    m->blas_scratch = sizes.scratch;
    m->blas_pending = 1;
    rt->pending[rt->pending_count++] = slot;
    return NPH_RESULT_OK;
}

void render_rt_mesh_destroy(NphRenderer *r, u32 slot)
{
    RenderRt *rt = &r->rt;
    RenderMesh *m = &r->meshes[slot];
    if (!rt->supported) {
        return;
    }
    if (m->blas_pending) {
        for (u32 i = 0; i < rt->pending_count; ++i) {
            if (rt->pending[i] == slot) {
                rt->pending[i] = rt->pending[--rt->pending_count];
                break;
            }
        }
    }
    nph_gpu_accel_destroy(r->dev, m->blas);
    nph_gpu_buffer_destroy(r->dev, m->blas_vertices);
    m->blas = NPH_GPU_ACCEL_NULL;
    m->blas_vertices = NPH_GPU_BUFFER_NULL;
    m->blas_pending = 0;
}

void render_rt_shadow_lights(NphRenderer *r, i32 change)
{
    RenderRt *rt = &r->rt;
    rt->shadow_lights = (u32)((i32)rt->shadow_lights + change);
}

bool render_rt_frame_begin(NphRenderer *r, bool production, NphDVec3 camera, u32 handled)
{
    RenderRt *rt = &r->rt;
    rt->frame_active = 0;
    rt->frame_production = production ? 1 : 0;
    rt->frame_rebuild = 0;
    rt->frame_instances = r->instance_pool.bump;
    rt->frame_builds = 0U;
    rt->tlas_offset = nph_vec3_zero();
    // The lights whose shadows are this frame's to trace.
    const bool traced = rt->shadow_lights > handled;
    if (!traced || !rt->supported) {
        // Instances may change unseen: the next frame that traces rebuilds.
        rt->tlas_valid = 0;
    }
    if (!traced) {
        return false;
    }
    if (!rt->supported) {
        if (!rt->unsupported_logged) {
            NPH_LOG_WARN(&NPH_LOG_RENDER,
                         "%s: lights cast shadows, but this renderer has no ray tracing: drawing "
                         "them unshadowed (said once)",
                         r->name);
            rt->unsupported_logged = 1;
        }
        return false;
    }
    rt->frame_active = 1;
    const NphVec3 moved = nph_dvec3_relative(camera, rt->anchor);
    if (!production || !rt->tlas_valid || !rt->tlas_production ||
        nph_vec3_length_sq(moved) > (f32)(RT_ANCHOR_RADIUS * RT_ANCHOR_RADIUS)) {
        rt->anchor = camera;
        rt->frame_rebuild = 1;
    }
    rt->tlas_offset = nph_dvec3_relative(camera, rt->anchor);
    return true;
}

void render_rt_write_instances(NphRenderer *r)
{
    const NphDVec3 anchor = r->rt.anchor;
    NphGpuAccelInstance *out = render_frame_at(r, r->layout.rt_instances_offset);
    // The live, shown instances only, one after another: the top level is
    // built over as many as there are (see the file comment).
    const u32 slots = r->instance_pool.bump;
    u32 n = 0U;
    for (u32 i = 0; i < slots; ++i) {
        const RenderInstance *inst = &r->instances[i];
        const u32 ms =
            inst->handle != 0U ? render_slot_of(&r->mesh_pool, inst->mesh.value) : NPH_U32_MAX;
        if (ms == NPH_U32_MAX || inst->hidden) {
            continue;
        }
        NphGpuAccelInstance *o = &out[n++];
        memset(o, 0, sizeof *o);
        const RenderMesh *m = &r->meshes[ms];
        // world = L (offset + scale q) + t = (L diag(scale)) q + (L offset + t)
        const NphMat3 l = inst->transform.linear;
        const NphVec3 t = nph_dvec3_relative(inst->transform.translation, anchor);
        const NphVec3 lo = nph_mat3_mul_vec3(l, m->position_offset);
        const f32 tr[3] = {t.x + lo.x, t.y + lo.y, t.z + lo.z};
        const f32 sc[3] = {m->position_scale.x, m->position_scale.y, m->position_scale.z};
        for (u32 row = 0; row < 3U; ++row) {
            const NphVec3 rv = nph_mat3_row(l, row);
            const f32 rr[3] = {rv.x, rv.y, rv.z};
            for (u32 col = 0; col < 3U; ++col) {
                o->transform[row][col] = rr[col] * sc[col];
            }
            o->transform[row][3] = tr[row];
        }
        o->index_and_mask = i | (0xFFU << 24U);
        o->offset_and_flags = NPH_GPU_ACCEL_INSTANCE_NO_FACE_CULL << 24U;
        o->blas = nph_gpu_accel_address(r->dev, m->blas);
    }
    r->rt.frame_instances = n;
}

static void accel_barrier(NphGpuCmd *cmd)
{
    // Builds written, then read by the next builds and by the shaders that
    // trace (and scratch reused).
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_ACCEL_BUILD,
                                   (u32)NPH_GPU_ACCESS_ACCEL_WRITE | (u32)NPH_GPU_ACCESS_ACCEL_READ,
                                   (u32)NPH_GPU_STAGE_ACCEL_BUILD | (u32)NPH_GPU_STAGE_COMPUTE |
                                       (u32)NPH_GPU_STAGE_FRAGMENT_SHADER,
                                   (u32)NPH_GPU_ACCESS_ACCEL_READ |
                                       (u32)NPH_GPU_ACCESS_ACCEL_WRITE};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
}

// The production path's records: every slot's record inactive first, then
// the active instances appended from the start (rt_instances.comp), so the
// inactive ones that remain are all at the end (see the file comment).
static void instances_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    RenderRt *rt = &r->rt;
    NphVisPush p;
    memset(&p, 0, sizeof p);
    p.view = r->vis.frame.view_address;
    p.arg0 = rt->frame_instances;
    const u64 dst = nph_gpu_buffer_address(r->dev, rt->instances);
    p.arg2 = (u32)(dst & 0xFFFFFFFFU);
    p.arg3 = (u32)(dst >> 32U);
    nph_gpu_cmd_bind_pipeline(cmd, rt->instance_writer);
    const u32 groups = (rt->frame_instances + 63U) / 64U;
    for (u32 mode = 0; mode < 2U; ++mode) {
        p.mode = mode;
        nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
        nph_gpu_cmd_dispatch(cmd, groups, 1U, 1U);
        if (mode == 0U) {
            const NphGpuMemoryBarrier m = {
                (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE};
            NphGpuBarrierDesc bd;
            memset(&bd, 0, sizeof bd);
            bd.memory = &m;
            bd.memory_count = 1U;
            nph_gpu_cmd_barrier(cmd, &bd);
        }
    }
}

static void accel_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    RenderRt *rt = &r->rt;
    NphGpuDevice *dev = r->dev;
    const u64 align = scratch_alignment(r);
    const u64 base = nph_gpu_buffer_address(dev, rt->scratch);
    const u64 lead = align_up(base, align) - base;
    // Every build fits (reserve_scratch); a batch holds as many as fit.
    const u64 capacity = rt->scratch_size - align;
    // Earlier frames' builds (and this frame's instance records) first.
    accel_barrier(cmd);
    u64 at = 0U;
    for (u32 i = 0; i < rt->frame_builds; ++i) {
        RenderMesh *m = &r->meshes[rt->pending[i]];
        const u64 s = align_up(m->blas_scratch, align);
        if (at != 0U && at + s > capacity) {
            accel_barrier(cmd); // the batch is done with the scratch
            at = 0U;
        }
        NphGpuAccelTriangles g;
        mesh_geometry(r, m, &g);
        nph_gpu_cmd_build_blas(cmd, m->blas, &g, 1U, (u32)NPH_GPU_ACCEL_PREFER_TRACE, rt->scratch,
                               lead + at);
        m->blas_pending = 0;
        at += s;
    }
    if (rt->frame_builds != 0U) {
        accel_barrier(cmd);
        // Built: off the list, which only this frame's declaration grew.
        rt->pending_count -= rt->frame_builds;
        memmove(rt->pending, rt->pending + rt->frame_builds, sizeof(u32) * rt->pending_count);
    }
    if (!rt->frame_rebuild) {
        return;
    }
    const NphGpuBuffer instances =
        rt->frame_production ? rt->instances : r->frame_buffers[r->frame_slot];
    const u64 offset = rt->frame_production ? 0U : r->layout.rt_instances_offset;
    nph_gpu_cmd_build_tlas(cmd, rt->tlas, instances, offset, rt->frame_instances,
                           (u32)NPH_GPU_ACCEL_PREFER_BUILD, rt->scratch, lead);
}

NphResult render_rt_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgBuffer instance_table,
                            NphGpuRgBuffer mesh_table, NphGpuRgBuffer counters,
                            NphGpuRgBuffer *out_tlas)
{
    RenderRt *rt = &r->rt;
    NPH_ASSERT(rt->frame_active);
    // The production path's changed instances reach its table this frame:
    // the top level follows them.
    if (rt->frame_production && r->vis.frame_changed != 0U) {
        rt->frame_rebuild = 1;
    }
    // The bottom levels waiting now are this frame's.
    rt->frame_builds = rt->pending_count;
    const NphGpuRgBuffer tlas =
        nph_gpu_rg_import_buffer(graph, nph_gpu_accel_buffer(r->dev, rt->tlas));
    const NphGpuRgBuffer scratch = nph_gpu_rg_import_buffer(graph, rt->scratch);
    NphGpuRgBuffer instances = NPH_GPU_RG_BUFFER_NULL;
    if (rt->frame_production) {
        instances = nph_gpu_rg_import_buffer(graph, rt->instances);
    }
    if (nph_gpu_rg_buffer_is_null(tlas) || nph_gpu_rg_buffer_is_null(scratch) ||
        (rt->frame_production && nph_gpu_rg_buffer_is_null(instances))) {
        return NPH_RESULT_FULL;
    }
    *out_tlas = tlas;
    if (!rt->frame_rebuild && rt->frame_builds == 0U) {
        return NPH_RESULT_OK; // the top level as an earlier frame built it
    }
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.type = NPH_GPU_RG_PASS_COMPUTE;
    pd.user = r;
    if (rt->frame_production && rt->frame_rebuild) {
        pd.name = "rt instances";
        pd.execute = instances_execute;
        NphGpuRgPass *p = nph_gpu_rg_add_pass(graph, &pd);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(p, instances, NPH_GPU_RG_USE_SHADER_WRITE);
        // The instance and mesh tables, after the frame's scatter; the
        // append counter, cleared with the frame's others.
        nph_gpu_rg_read_buffer(p, instance_table, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, mesh_table, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_write_buffer(p, counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    pd.name = "rt accel";
    pd.execute = accel_execute;
    NphGpuRgPass *p = nph_gpu_rg_add_pass(graph, &pd);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_write_buffer(p, scratch, NPH_GPU_RG_USE_ACCEL_SCRATCH);
    if (rt->frame_rebuild) {
        if (rt->frame_production) {
            nph_gpu_rg_read_buffer(p, instances, NPH_GPU_RG_USE_ACCEL_INPUT);
        }
        nph_gpu_rg_write_buffer(p, tlas, NPH_GPU_RG_USE_ACCEL_BUILD);
        rt->tlas_valid = 1;
        rt->tlas_production = rt->frame_production;
    }
    return NPH_RESULT_OK;
}

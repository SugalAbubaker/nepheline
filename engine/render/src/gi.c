// SPDX-License-Identifier: Apache-2.0
// The production path's radiance cache (render/renderer.h NphRenderView.gi;
// docs/render.md "Global illumination"; docs/adr/0039-radiance-cache.md).
//
// A hash grid of irradiance cells in the world (gi_common.glsl), kept across
// frames in three buffers of NphRendererDesc.gi_cells slots: the keys'
// fingerprints, this frame's sums, and the cells' irradiance with their
// sample counts and ages. Each frame that has it:
//
//   gi update   within a fixed budget of rays (NphRendererDesc.gi_rays),
//               a sample from a random pixel of each block of the frame:
//               one ray from its surface, what arrives along it added to the
//               surface's cell (gi_update.comp);
//   gi resolve  every slot: a cell blends its sums into its irradiance, or
//               ages and is freed (gi_resolve.comp);
//
// after which the resolve's shading takes the cells' irradiance for the
// diffuse indirect light (vis_resolve.glsl). The cells are keyed in the
// world: each level's camera cell is worked out here in double precision,
// and the shaders add the camera-relative point to it (CLAUDE.md §8.2).
//
// The sun whose light the update's hits reflect is the live directional
// light in the lowest slot, kept as lights change (constant time, as the
// virtual shadow map keeps its own).
#include <nepheline/core/log.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <math.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderGiLevel) == 32U, "NphRenderGiLevel matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderGiView) == 96U + 32U * NPH_RENDER_GI_LEVELS,
                  "NphRenderGiView matches its GLSL layout");

// Fixed-point units of one pre-exposed sample (gi_update.comp): a sample is
// clamped to NPH_RENDER_GI_SAMPLE_MAX, 2^23 units, so a cell's sums hold 512
// samples a frame.
#define GI_SUM_SCALE 2048.0F
// A cell is 1 / (2 GI_SCALE) to 1 / GI_SCALE of its distance from the
// camera across (23 to 47 pixels at 3840 x 2160 and a 0.9 rad field of
// view).
#define GI_SCALE 48.0
// Update rays that hit nearer than this share of the ambient occlusion's
// radius count nothing (its range: the middle of its fade).
#define GI_NEAR 0.95F
// The update's and the resolve's workgroups (gi_update.comp, gi_resolve.comp).
#define GI_UPDATE_GROUP  8U
#define GI_RESOLVE_GROUP 64U

static NphResult make_buffer(NphRenderer *r, u64 size, const char *name, NphGpuBuffer *out)
{
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = size;
    // Cleared by fills, and read back by the tests.
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = name;
    return nph_gpu_buffer_create(r->dev, &bd, out);
}

bool render_gi_device_supported(const NphRenderer *r)
{
    return !r->desc.disable_gi && r->rt.supported;
}

NphResult render_gi_create(NphRenderer *r)
{
    RenderGi *gi = &r->gi;
    gi->sun_slot = NPH_U32_MAX;
    if (!gi->supported) {
        return NPH_RESULT_OK; // render_vis_create decided
    }
    const u32 cells = r->desc.gi_cells;
    NPH_TRY(make_buffer(r, (u64)cells * sizeof(u32), "gi keys", &gi->keys));
    NPH_TRY(make_buffer(r, (u64)cells * 16U, "gi sums", &gi->sums));
    NPH_TRY(make_buffer(r, (u64)cells * 16U, "gi cells", &gi->cells));
    NPH_TRY(render_load_shader_file(r, "gi_update.comp", &gi->update_shader));
    NPH_TRY(render_load_shader_file(r, "gi_resolve.comp", &gi->resolve_shader));
    NphGpuComputePipelineDesc d;
    memset(&d, 0, sizeof d);
    d.shader = gi->update_shader;
    d.name = "gi update";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &gi->update));
    render_log_statistics(r, gi->update, "gi update");
    memset(&d, 0, sizeof d);
    d.shader = gi->resolve_shader;
    d.name = "gi resolve";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &gi->resolve));
    gi->capacity = cells;
    NPH_LOG_INFO(&NPH_LOG_RENDER, "%s: radiance cache: %u cells (%.1f MB), %u rays a frame",
                 r->name, cells, (f64)cells * 36.0 / (1024.0 * 1024.0), r->desc.gi_rays);
    return NPH_RESULT_OK;
}

void render_gi_destroy(NphRenderer *r)
{
    RenderGi *gi = &r->gi;
    nph_gpu_pipeline_destroy(r->dev, gi->resolve);
    nph_gpu_pipeline_destroy(r->dev, gi->update);
    nph_gpu_shader_destroy(r->dev, gi->resolve_shader);
    nph_gpu_shader_destroy(r->dev, gi->update_shader);
    nph_gpu_buffer_destroy(r->dev, gi->cells);
    nph_gpu_buffer_destroy(r->dev, gi->sums);
    nph_gpu_buffer_destroy(r->dev, gi->keys);
    memset(gi, 0, sizeof *gi);
    gi->sun_slot = NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// The sun
// ---------------------------------------------------------------------------

static bool sun_candidate(const RenderLight *l)
{
    return l->handle != 0U && l->desc.type == NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
}

// The sun is the candidate in the lowest slot, so a change elsewhere can
// only make a new candidate ahead of it the sun; only the sun ceasing to be
// one (or going) looks further, from its own slot on.
void render_gi_light_changed(NphRenderer *r, u32 slot)
{
    RenderGi *gi = &r->gi;
    if (slot == gi->sun_slot) {
        if (!sun_candidate(&r->lights[slot])) {
            gi->sun_slot = NPH_U32_MAX;
            for (u32 i = slot + 1U; i < r->light_pool.bump; ++i) {
                if (sun_candidate(&r->lights[i])) {
                    gi->sun_slot = i;
                    break;
                }
            }
        }
    } else if (sun_candidate(&r->lights[slot]) &&
               (gi->sun_slot == NPH_U32_MAX || slot < gi->sun_slot)) {
        gi->sun_slot = slot;
    }
}

// ---------------------------------------------------------------------------
// A frame
// ---------------------------------------------------------------------------

bool render_gi_frame_wanted(const NphRenderer *r, const NphRenderView *view, bool production)
{
    // What shows the diffuse indirect light.
    const bool shown = view->debug_view == NPH_RENDER_VIEW_LIT ||
                       view->debug_view == NPH_RENDER_VIEW_INDIRECT ||
                       view->debug_view == NPH_RENDER_VIEW_IRRADIANCE;
    return r->gi.supported && production && view->gi && shown;
}

u32 render_gi_stride(u32 width, u32 height, u32 rays)
{
    // The smallest square block that keeps the samples within the budget.
    const f64 area = (f64)width * (f64)height / (f64)nph_max_u32(rays, 1U);
    u32 stride = nph_max_u32((u32)ceil(sqrt(area)), 1U);
    while ((u64)((width + stride - 1U) / stride) * (u64)((height + stride - 1U) / stride) >
           (u64)rays) {
        ++stride;
    }
    return stride;
}

void render_gi_levels(NphDVec3 camera, f64 scale, NphRenderGiLevel *levels)
{
    const f64 c[3] = {camera.x, camera.y, camera.z};
    for (u32 l = 0; l < (u32)NPH_RENDER_GI_LEVELS; ++l) {
        const f64 size = ldexp(1.0, (i32)l - NPH_RENDER_GI_LEVEL_BIAS) / scale;
        f32 origin[3];
        for (u32 a = 0; a < 3U; ++a) {
            const f64 cell = floor(c[a] / size);
            // The low 32 bits of the cell's two's complement (the keys keep
            // 17 of them), and where in it the camera is.
            levels[l].cell[a] = (u32)(u64)(i64)cell;
            origin[a] = (f32)(c[a] - cell * size);
        }
        levels[l].cell[3] = 0U;
        levels[l].origin = nph_vec4(origin[0], origin[1], origin[2], (f32)size);
    }
}

bool render_gi_frame_begin(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                           bool production)
{
    RenderGi *gi = &r->gi;
    gi->frame_active = 0;
    if (!render_gi_frame_wanted(r, view, production) || !r->rt.frame_active) {
        return false;
    }
    NphRenderGiView *g = render_frame_at(r, r->layout.gi_view_offset);
    memset(g, 0, sizeof *g);
    g->keys = nph_gpu_buffer_address(r->dev, gi->keys);
    g->sums = nph_gpu_buffer_address(r->dev, gi->sums);
    g->cells = nph_gpu_buffer_address(r->dev, gi->cells);
    g->bucket_mask = gi->capacity / (u32)NPH_RENDER_GI_BUCKET - 1U;
    g->scale = (f32)GI_SCALE;
    g->sum_scale = GI_SUM_SCALE;
    // Leave to the ambient occlusion what it darkens (ao.c).
    g->near = r->ao.frame_active ? GI_NEAR * r->ao.frame_push.radius : 0.0F;
    g->frame = (u32)nph_gpu_frame_index(r->dev);
    g->jitter = r->taa.frame_active ? 1U : 0U;
    g->stride = render_gi_stride(width, height, r->desc.gi_rays);
    g->rays_x = (width + g->stride - 1U) / g->stride;
    gi->frame_rays_y = (height + g->stride - 1U) / g->stride;
    gi->frame_rays_x = g->rays_x;
    if (gi->sun_slot != NPH_U32_MAX) {
        const NphRenderLightDesc *sun = &r->lights[gi->sun_slot].desc;
        const NphVec3 d = nph_vec3_normalize(sun->direction);
        g->sun_direction = nph_vec4(d.x, d.y, d.z, sun->shadows ? 1.0F : 0.0F);
        g->sun_color = nph_vec4_from_vec3(nph_vec3_scale(sun->color, sun->intensity), 0.0F);
    } else {
        g->sun_direction = nph_vec4(0.0F, -1.0F, 0.0F, -1.0F);
    }
    render_gi_levels(view->camera.position, GI_SCALE, g->levels);
    gi->frame_active = 1;
    return true;
}

u64 render_gi_view_address(const NphRenderer *r)
{
    return r->frame_addresses[r->frame_slot] + r->layout.gi_view_offset;
}

// ---------------------------------------------------------------------------
// The passes
// ---------------------------------------------------------------------------

static void clear_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    const RenderGi *gi = &((NphRenderer *)user)->gi;
    nph_gpu_cmd_fill_buffer(cmd, gi->keys, 0U, NPH_GPU_WHOLE_SIZE, 0U);
    nph_gpu_cmd_fill_buffer(cmd, gi->sums, 0U, NPH_GPU_WHOLE_SIZE, 0U);
    nph_gpu_cmd_fill_buffer(cmd, gi->cells, 0U, NPH_GPU_WHOLE_SIZE, 0U);
}

static void update_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderGi *gi = &r->gi;
    NphVisPush p;
    memset(&p, 0, sizeof p);
    p.view = r->vis.frame.view_address;
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, gi->frame_visbuffer), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, gi->update);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, (gi->frame_rays_x + GI_UPDATE_GROUP - 1U) / GI_UPDATE_GROUP,
                         (gi->frame_rays_y + GI_UPDATE_GROUP - 1U) / GI_UPDATE_GROUP, 1U);
}

static void resolve_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    const RenderGi *gi = &r->gi;
    NphVisPush p;
    memset(&p, 0, sizeof p);
    p.view = r->vis.frame.view_address;
    p.arg0 = gi->capacity;
    nph_gpu_cmd_bind_pipeline(cmd, gi->resolve);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, (gi->capacity + GI_RESOLVE_GROUP - 1U) / GI_RESOLVE_GROUP, 1U, 1U);
}

static NphGpuRgPass *add_pass(NphGpuRenderGraph *graph, NphRenderer *r, const char *name,
                              NphGpuRgPassType type, NphGpuRgExecuteFn execute)
{
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = name;
    pd.type = type;
    pd.execute = execute;
    pd.user = r;
    return nph_gpu_rg_add_pass(graph, &pd);
}

NphResult render_gi_declare(NphRenderer *r, NphGpuRenderGraph *graph, const RenderGiInputs *in)
{
    RenderGi *gi = &r->gi;
    gi->frame_visbuffer = in->visbuffer;
    gi->frame_keys = nph_gpu_rg_import_buffer(graph, gi->keys);
    gi->frame_sums = nph_gpu_rg_import_buffer(graph, gi->sums);
    gi->frame_cells = nph_gpu_rg_import_buffer(graph, gi->cells);
    if (nph_gpu_rg_buffer_is_null(gi->frame_keys) || nph_gpu_rg_buffer_is_null(gi->frame_sums) ||
        nph_gpu_rg_buffer_is_null(gi->frame_cells)) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgPass *p = NULL;
    if (!gi->cleared) {
        // The first frame: every slot free.
        p = add_pass(graph, r, "gi clear", NPH_GPU_RG_PASS_TRANSFER, clear_execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(p, gi->frame_keys, NPH_GPU_RG_USE_TRANSFER_DST);
        nph_gpu_rg_write_buffer(p, gi->frame_sums, NPH_GPU_RG_USE_TRANSFER_DST);
        nph_gpu_rg_write_buffer(p, gi->frame_cells, NPH_GPU_RG_USE_TRANSFER_DST);
        gi->cleared = 1;
    }
    p = add_pass(graph, r, "gi update", NPH_GPU_RG_PASS_COMPUTE, update_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, in->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    nph_gpu_rg_read_buffer(p, in->clusters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, in->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, in->meshes, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, in->material_map, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, in->tlas, NPH_GPU_RG_USE_ACCEL_READ);
    nph_gpu_rg_read_image(p, in->env, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_buffer(p, in->sh, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_buffer(p, gi->frame_keys, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, gi->frame_sums, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_read_buffer(p, gi->frame_cells, NPH_GPU_RG_USE_SHADER_READ);
    p = add_pass(graph, r, "gi resolve", NPH_GPU_RG_PASS_COMPUTE, resolve_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_write_buffer(p, gi->frame_keys, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, gi->frame_sums, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, gi->frame_cells, NPH_GPU_RG_USE_STORAGE_WRITE);
    return NPH_RESULT_OK;
}

void render_gi_read(const NphRenderer *r, NphGpuRgPass *pass)
{
    const RenderGi *gi = &r->gi;
    if (!gi->frame_active) {
        return;
    }
    nph_gpu_rg_read_buffer(pass, gi->frame_keys, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(pass, gi->frame_cells, NPH_GPU_RG_USE_SHADER_READ);
}

// ---------------------------------------------------------------------------
// The keys on the CPU (gi_common.glsl's twins, for the tests)
// ---------------------------------------------------------------------------

i32 render_gi_level(f32 dist)
{
    int e = 0;
    (void)frexpf(nph_f32_max(dist, 1e-30F), &e);
    // frexp gives dist = m 2^e with m in [0.5, 1): floor(log2) is e - 1.
    const i32 level = (i32)e - 1 + NPH_RENDER_GI_LEVEL_BIAS;
    return level < 0 ? 0 : (level > NPH_RENDER_GI_LEVELS - 1 ? NPH_RENDER_GI_LEVELS - 1 : level);
}

u64 render_gi_key(const NphRenderGiLevel *levels, NphVec3 p, NphVec3 n)
{
    const i32 level = render_gi_level(nph_vec3_length(p));
    const NphRenderGiLevel *l = &levels[level];
    // Half a cell out along the normal, off the cells' faces.
    const f32 out = 0.5F * l->origin.w;
    const f32 pc[3] = {p.x + out * n.x, p.y + out * n.y, p.z + out * n.z};
    const f32 oc[3] = {l->origin.x, l->origin.y, l->origin.z};
    u64 key = 0U;
    for (u32 a = 0; a < 3U; ++a) {
        // Unsigned arithmetic wraps as the shader's two's complement does.
        const u32 c = l->cell[a] + (u32)(i32)floorf((pc[a] + oc[a]) / l->origin.w);
        key |= (u64)(c & 0x1FFFFU) << (17U * a);
    }
    const u32 octant =
        (n.x > -1e-3F ? 1U : 0U) | (n.y > -1e-3F ? 2U : 0U) | (n.z > -1e-3F ? 4U : 0U);
    return key | ((u64)(u32)level << 51U) | ((u64)octant << 56U);
}

u64 render_gi_mix(u64 k)
{
    k ^= k >> 33U;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33U;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33U;
    return k;
}

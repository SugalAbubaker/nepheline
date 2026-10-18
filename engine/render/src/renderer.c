// SPDX-License-Identifier: Apache-2.0
// The renderer's lifetime and its resources (render/renderer.h;
// docs/render.md). Frames are in frame.c, image-based lighting in ibl.c,
// debug lines in debug_draw.c.
#include <nepheline/core/log.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <stdio.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_RENDER, "render", NPH_LOG_LEVEL_INFO);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool is_power_of_two(u32 v)
{
    return v != 0U && (v & (v - 1U)) == 0U;
}

static u32 round_up_64(u32 v)
{
    return v > NPH_U32_MAX - 63U ? v : (v + 63U) & ~63U;
}

static u32 log2_floor(u32 v)
{
    u32 n = 0U;
    while (v > 1U) {
        v >>= 1U;
        ++n;
    }
    return n;
}

// Index bits for a pool of `capacity` slots: enough to address them all.
static u32 index_bits_for(u32 capacity)
{
    u32 bits = 1U;
    while (bits < NPH_HANDLE_POOL_MAX_INDEX_BITS && (1U << bits) < capacity) {
        ++bits;
    }
    return bits;
}

static NphResult pool_init(NphHandlePool *pool, NphAllocator *alloc, u32 capacity)
{
    // Released slots wait for a few others before reuse, so a stale handle
    // keeps failing for a while rather than aliasing a new resource.
    const u32 delay = capacity >= 64U ? 16U : 0U;
    return nph_handle_pool_init(pool, alloc, capacity, index_bits_for(capacity), delay);
}

u32 render_slot_of(const NphHandlePool *pool, u32 value)
{
    const NphHandle h = {value};
    if (value == 0U || !nph_handle_pool_is_valid(pool, h)) {
        return NPH_U32_MAX;
    }
    return nph_handle_pool_index(pool, h);
}

static NphResult alloc_slot(NphHandlePool *pool, u32 *out_value, u32 *out_slot)
{
    NphHandle h = NPH_HANDLE_NULL;
    if (nph_handle_pool_alloc(pool, &h) != NPH_RESULT_OK) {
        return NPH_RESULT_FULL;
    }
    *out_value = h.value;
    *out_slot = nph_handle_pool_index(pool, h);
    return NPH_RESULT_OK;
}

static void release_slot(NphHandlePool *pool, u32 value)
{
    const NphHandle h = {value};
    NPH_IGNORE_RESULT(nph_handle_pool_release(pool, h));
}

NphResult render_load_shader_file(NphRenderer *r, const char *file, NphGpuShader *out)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s.spv", r->shader_dir, file);
    const NphResult res = nph_gpu_shader_load(r->dev, path, file, out);
    if (res != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RENDER, "%s: cannot load shader %s: %s", r->name, path,
                      nph_result_string(res));
    }
    return res;
}

NphResult render_load_shader(NphRenderer *r, RenderShader shader, const char *file)
{
    return render_load_shader_file(r, file, &r->shaders[shader]);
}

// ---------------------------------------------------------------------------
// Pipelines and samplers
// ---------------------------------------------------------------------------

NphResult render_forward_pipeline(NphRenderer *r, u32 alpha, u32 cull, NphGpuFormat format,
                                  bool ray_query, NphGpuPipeline *out)
{
    static const RenderShader frags[2][3] = {
        {RENDER_SHADER_FORWARD_OPAQUE, RENDER_SHADER_FORWARD_MASK, RENDER_SHADER_FORWARD_BLEND},
        {RENDER_SHADER_FORWARD_OPAQUE_RT, RENDER_SHADER_FORWARD_MASK_RT,
         RENDER_SHADER_FORWARD_BLEND_RT}};
    static const char *const names[3][RENDER_CULL_COUNT] = {
        {"forward opaque", "forward opaque (mirrored)", "forward opaque (double-sided)",
         "forward opaque (double-sided, mirrored)"},
        {"forward masked", "forward masked (mirrored)", "forward masked (double-sided)",
         "forward masked (double-sided, mirrored)"},
        {"forward blended", "forward blended (mirrored)", "forward blended (double-sided)",
         "forward blended (double-sided, mirrored)"},
    };
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_FORWARD_VERT];
    d.fragment = r->shaders[frags[ray_query ? 1 : 0][alpha]];
    const bool none = cull == (u32)RENDER_CULL_NONE || cull == (u32)RENDER_CULL_NONE_CW;
    const bool cw = cull == (u32)RENDER_CULL_BACK_CW || cull == (u32)RENDER_CULL_NONE_CW;
    d.cull_mode = none ? NPH_GPU_CULL_NONE : NPH_GPU_CULL_BACK;
    d.front_face = cw ? NPH_GPU_FRONT_FACE_CW : NPH_GPU_FRONT_FACE_CCW;
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.depth_format = RENDER_DEPTH_FORMAT;
    d.depth.test = 1;
    d.depth.compare = NPH_GPU_COMPARE_GREATER; // reverse-Z
    if (alpha == (u32)NPH_RENDER_ALPHA_BLEND) {
        // "Over" compositing; blended surfaces do not occlude.
        d.blend[0].enable = 1;
        d.blend[0].src_color = NPH_GPU_BLEND_SRC_ALPHA;
        d.blend[0].dst_color = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
        d.blend[0].color_op = NPH_GPU_BLEND_OP_ADD;
        d.blend[0].src_alpha = NPH_GPU_BLEND_ONE;
        d.blend[0].dst_alpha = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
        d.blend[0].alpha_op = NPH_GPU_BLEND_OP_ADD;
    } else {
        d.depth.write = 1;
    }
    d.name = names[alpha][cull];
    return nph_gpu_pipeline_create_graphics(r->dev, &d, out);
}

static NphResult create_forward_pipelines(NphRenderer *r)
{
    const bool rt = r->rt.supported != 0;
    for (u32 alpha = 0; alpha < 3U; ++alpha) {
        for (u32 cull = 0; cull < (u32)RENDER_CULL_COUNT; ++cull) {
            NPH_TRY(render_forward_pipeline(r, alpha, cull, RENDER_HDR_FORMAT, false,
                                            &r->forward[alpha][cull]));
            if (rt) {
                NPH_TRY(render_forward_pipeline(r, alpha, cull, RENDER_HDR_FORMAT, true,
                                                &r->forward_rt[alpha][cull]));
            }
        }
    }
    NphGpuGraphicsPipelineDesc s;
    memset(&s, 0, sizeof s);
    s.vertex = r->shaders[RENDER_SHADER_FULLSCREEN_VERT];
    s.fragment = r->shaders[RENDER_SHADER_SKY_FRAG];
    s.color_formats[0] = RENDER_HDR_FORMAT;
    s.color_count = 1U;
    s.depth_format = RENDER_DEPTH_FORMAT;
    s.depth.test = 1;
    s.depth.compare = NPH_GPU_COMPARE_GREATER_OR_EQUAL; // only where depth is still the far plane
    s.name = "sky";
    return nph_gpu_pipeline_create_graphics(r->dev, &s, &r->sky);
}

static NphResult create_compute_pipelines(NphRenderer *r)
{
    static const RenderShader shaders[RENDER_COMPUTE_COUNT] = {
        RENDER_SHADER_IBL_SKY,        RENDER_SHADER_IBL_EQUIRECT,  RENDER_SHADER_IBL_CUBE,
        RENDER_SHADER_IBL_DOWNSAMPLE, RENDER_SHADER_IBL_PREFILTER, RENDER_SHADER_IBL_SH,
        RENDER_SHADER_BRDF_LUT,
    };
    static const char *const names[RENDER_COMPUTE_COUNT] = {
        "ibl sky",       "ibl equirect", "ibl cube", "ibl downsample",
        "ibl prefilter", "ibl sh",       "brdf lut",
    };
    for (u32 i = 0; i < (u32)RENDER_COMPUTE_COUNT; ++i) {
        NphGpuComputePipelineDesc d;
        memset(&d, 0, sizeof d);
        d.shader = r->shaders[shaders[i]];
        d.name = names[i];
        NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &r->compute[i]));
    }
    return NPH_RESULT_OK;
}

static NphGpuAddressMode address_mode(u32 wrap)
{
    switch (wrap) {
    case NPH_RENDER_WRAP_CLAMP:
        return NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    case NPH_RENDER_WRAP_MIRROR:
        return NPH_GPU_ADDRESS_MIRRORED_REPEAT;
    default:
        return NPH_GPU_ADDRESS_REPEAT;
    }
}

static NphResult create_samplers(NphRenderer *r)
{
    for (u32 u = 0; u < 3U; ++u) {
        for (u32 v = 0; v < 3U; ++v) {
            for (u32 nearest = 0; nearest < 2U; ++nearest) {
                NphGpuSamplerDesc d;
                nph_gpu_sampler_desc_default(&d);
                d.address_u = address_mode(u);
                d.address_v = address_mode(v);
                if (nearest != 0U) {
                    d.min_filter = NPH_GPU_FILTER_NEAREST;
                    d.mag_filter = NPH_GPU_FILTER_NEAREST;
                    d.mip_filter = NPH_GPU_FILTER_NEAREST;
                } else {
                    d.max_anisotropy = r->desc.max_anisotropy;
                }
                d.name = "material sampler";
                NPH_TRY(nph_gpu_sampler_create(r->dev, &d, &r->material_samplers[u][v][nearest]));
            }
        }
    }
    NphGpuSamplerDesc c;
    nph_gpu_sampler_desc_default(&c);
    c.address_u = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    c.address_v = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    c.address_w = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    c.name = "lighting tables";
    return nph_gpu_sampler_create(r->dev, &c, &r->clamp_sampler);
}

u32 render_material_sampler(const NphRenderer *r, const NphRenderTextureBinding *binding)
{
    const u32 u = (u32)binding->wrap_u < 3U ? (u32)binding->wrap_u : 0U;
    const u32 v = (u32)binding->wrap_v < 3U ? (u32)binding->wrap_v : 0U;
    return nph_gpu_sampler_index(r->dev, r->material_samplers[u][v][binding->nearest ? 1 : 0]);
}

static NphResult create_frame_buffers(NphRenderer *r)
{
    render_frame_layout(&r->desc, r->rt.supported != 0, &r->layout);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NphGpuBufferDesc d;
        memset(&d, 0, sizeof d);
        d.size = r->layout.size;
        // The reference's top-level instance records live here too (rt.c).
        d.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE |
                  (r->rt.supported ? (u32)NPH_GPU_BUFFER_USAGE_ACCEL_INPUT : 0U);
        d.memory = NPH_GPU_MEMORY_UPLOAD;
        d.name = "render frame records";
        NPH_TRY(nph_gpu_buffer_create(r->dev, &d, &r->frame_buffers[i]));
        r->frame_maps[i] = (u8 *)nph_gpu_buffer_map(r->dev, r->frame_buffers[i]);
        r->frame_addresses[i] = nph_gpu_buffer_address(r->dev, r->frame_buffers[i]);
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static NphResult apply_defaults(NphRendererDesc *d)
{
    d->max_textures = d->max_textures != 0U ? d->max_textures : 1024U;
    d->max_meshes = d->max_meshes != 0U ? d->max_meshes : 1024U;
    d->max_materials = d->max_materials != 0U ? d->max_materials : 1024U;
    d->max_instances = d->max_instances != 0U ? d->max_instances : 16384U;
    d->max_lights = d->max_lights != 0U ? d->max_lights : 256U;
    d->max_draws = d->max_draws != 0U ? d->max_draws : 16384U;
    d->debug_lines = d->debug_lines != 0U ? d->debug_lines : 32768U;
    d->environment_size = d->environment_size != 0U ? d->environment_size : 256U;
    d->specular_size = d->specular_size != 0U ? d->specular_size : 128U;
    d->specular_mips = d->specular_mips != 0U ? d->specular_mips : 6U;
    d->specular_samples = d->specular_samples != 0U ? d->specular_samples : 256U;
    d->brdf_lut_size = d->brdf_lut_size != 0U ? d->brdf_lut_size : 128U;
    d->max_anisotropy = d->max_anisotropy == 0.0F ? 8.0F : d->max_anisotropy;
    // The production path's lists; their GPU regions sit back to back, so
    // the capacities are rounded up to 64 entries to keep every region
    // 16-byte aligned for the shaders' buffer references.
    d->max_visible_clusters =
        round_up_64(d->max_visible_clusters != 0U ? d->max_visible_clusters : 524288U);
    d->max_cull_meshlets =
        round_up_64(d->max_cull_meshlets != 0U ? d->max_cull_meshlets : 1U << 20U);
    d->max_deferred = round_up_64(d->max_deferred != 0U ? d->max_deferred : 262144U);
    d->sw_raster_pixels = d->sw_raster_pixels > 0.0F ? d->sw_raster_pixels : 3.0F;
    d->max_instance_updates = d->max_instance_updates != 0U ? d->max_instance_updates : 16384U;
    d->max_material_slots =
        d->max_material_slots != 0U ? d->max_material_slots : 4U * d->max_instances;
    d->meshlet_index_pool = d->meshlet_index_pool != 0U ? d->meshlet_index_pool : 1U << 24U;
    if (d->max_visible_clusters > (u32)NPH_VIS_MAX_CLUSTERS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!is_power_of_two(d->environment_size) || !is_power_of_two(d->specular_size) ||
        d->specular_mips > log2_floor(d->specular_size) + 1U ||
        d->specular_size > d->environment_size || d->brdf_lut_size < 8U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Pool slots are addressed with at most 28 bits, and the GPU material
    // table holds one more entry than max_materials (the default).
    const u32 limit = 1U << 24U;
    if (d->max_textures > limit || d->max_meshes > limit || d->max_materials > limit ||
        d->max_instances > limit || d->max_draws > limit || d->debug_lines > limit) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // The production path's light grid indexes a view's lights in 16 bits
    // and sorts them in one workgroup.
    if (d->max_lights > (u32)NPH_RENDER_MAX_LIGHTS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return NPH_RESULT_OK;
}

static NphResult create_contents(NphRenderer *r)
{
    NphAllocator *a = r->alloc;
    const NphRendererDesc *d = &r->desc;
    NPH_TRY(pool_init(&r->texture_pool, a, d->max_textures));
    NPH_TRY(pool_init(&r->mesh_pool, a, d->max_meshes));
    NPH_TRY(pool_init(&r->material_pool, a, d->max_materials));
    NPH_TRY(pool_init(&r->instance_pool, a, d->max_instances));
    NPH_TRY(pool_init(&r->light_pool, a, d->max_lights));
    r->textures = NPH_ALLOC_ARRAY_ZEROED(a, RenderTexture, d->max_textures);
    r->meshes = NPH_ALLOC_ARRAY_ZEROED(a, RenderMesh, d->max_meshes);
    r->materials = NPH_ALLOC_ARRAY_ZEROED(a, RenderMaterial, d->max_materials);
    r->instances = NPH_ALLOC_ARRAY_ZEROED(a, RenderInstance, d->max_instances);
    r->lights = NPH_ALLOC_ARRAY_ZEROED(a, RenderLight, d->max_lights);
    r->draw_cmds = NPH_ALLOC_ARRAY_ZEROED(a, RenderDrawCmd, d->max_draws);
    if (r->textures == NULL || r->meshes == NULL || r->materials == NULL || r->instances == NULL ||
        r->lights == NULL || r->draw_cmds == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }

    static const char *const files[RENDER_SHADER_COUNT] = {
        "forward.vert",          "forward_opaque.frag",    "forward_mask.frag",
        "forward_blend.frag",    "fullscreen.vert",        "sky.frag",
        "tonemap.frag",          "debug_line.vert",        "debug_line.frag",
        "ibl_sky.comp",          "ibl_equirect.comp",      "ibl_cube.comp",
        "ibl_downsample.comp",   "ibl_prefilter.comp",     "ibl_sh.comp",
        "brdf_lut.comp",         "forward_opaque_rt.frag", "forward_mask_rt.frag",
        "forward_blend_rt.frag", "rt_instances.comp",
    };
    // Ray-traced shadows where the device has ray queries, unless the
    // description turned them off: the ray-query shaders cannot even be
    // loaded on a device without them.
    r->rt.supported = nph_gpu_device_info(r->dev)->features.ray_query && !d->disable_ray_tracing;
    const bool rt = r->rt.supported != 0;
    for (u32 i = 0; i < (u32)RENDER_SHADER_COUNT; ++i) {
        if (!rt && i >= (u32)RENDER_SHADER_FORWARD_OPAQUE_RT) {
            continue;
        }
        NPH_TRY(render_load_shader(r, (RenderShader)i, files[i]));
    }
    NPH_TRY(create_forward_pipelines(r));
    NPH_TRY(create_compute_pipelines(r));
    NPH_TRY(create_samplers(r));
    NPH_TRY(create_frame_buffers(r));
    NPH_TRY(render_ibl_create(r));
    NPH_TRY(render_debug_create(r));
    NPH_TRY(render_rt_create(r));
    if (!d->disable_production && render_vis_device_supported(r)) {
        NPH_TRY(render_vis_create(r));
    }
    return NPH_RESULT_OK;
}

NphResult nph_renderer_create(const NphRendererDesc *desc, NphRenderer **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->dev == NULL || desc->shader_dir == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphRendererDesc d = *desc;
    NPH_TRY(apply_defaults(&d));
    if (strlen(desc->shader_dir) >= 512U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphRenderer *r = NPH_ALLOC_TYPE(desc->alloc, NphRenderer);
    if (r == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(r, 0, sizeof *r);
    r->alloc = desc->alloc;
    r->dev = desc->dev;
    r->desc = d;
    snprintf(r->name, sizeof r->name, "%s", desc->name != NULL ? desc->name : "renderer");
    snprintf(r->shader_dir, sizeof r->shader_dir, "%s", desc->shader_dir);
    r->desc.name = r->name;
    r->desc.shader_dir = r->shader_dir;
    r->last_frame_index = NPH_U64_MAX;
    r->material_version = 1U;
    r->env.source = NPH_RENDER_ENV_NONE;
    r->env.intensity = 1.0F;
    r->env_dirty = 1;
    const NphResult res = create_contents(r);
    if (res != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RENDER, "%s: creation failed: %s", r->name, nph_result_string(res));
        nph_renderer_destroy(r);
        return res;
    }
    const NphGpuDeviceInfo *info = nph_gpu_device_info(r->dev);
    NPH_LOG_INFO(&NPH_LOG_RENDER,
                 "%s: on %s: reference renderer (environment %u, prefiltered %u x %u levels, "
                 "%u lights, %u draws per frame); production path %s; ray-traced shadows %s",
                 r->name, info->name, d.environment_size, d.specular_size, d.specular_mips,
                 d.max_lights, d.max_draws,
                 !r->vis.supported     ? "absent"
                 : r->vis.mesh_shaders ? "with mesh shaders"
                                       : "with indirect draws",
                 r->rt.supported ? "on" : "absent");
    *out = r;
    return NPH_RESULT_OK;
}

static void free_instance_tables(NphRenderer *r)
{
    if (r->instances == NULL) {
        return;
    }
    for (u32 i = 0; i < r->desc.max_instances; ++i) {
        RenderInstance *inst = &r->instances[i];
        if (inst->materials != NULL) {
            NPH_FREE_ARRAY(r->alloc, NphRenderMaterial, inst->materials, inst->material_count);
            inst->materials = NULL;
        }
    }
}

void nph_renderer_destroy(NphRenderer *r)
{
    if (r == NULL) {
        return;
    }
    NphGpuDevice *dev = r->dev;
    NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(dev));
    u32 leaked = 0U;
    if (r->meshes != NULL) {
        for (u32 i = 0; i < r->desc.max_meshes; ++i) {
            RenderMesh *m = &r->meshes[i];
            if (m->handle == 0U) {
                continue;
            }
            ++leaked;
            nph_gpu_buffer_destroy(dev, m->positions);
            nph_gpu_buffer_destroy(dev, m->attribs);
            nph_gpu_buffer_destroy(dev, m->indices);
            render_vis_mesh_destroy(r, i);
            render_rt_mesh_destroy(r, i);
            NPH_FREE_ARRAY(r->alloc, NphRenderSubmesh, m->submeshes, m->submesh_count);
        }
    }
    if (r->textures != NULL) {
        for (u32 i = 0; i < r->desc.max_textures; ++i) {
            if (r->textures[i].handle != 0U) {
                ++leaked;
                nph_gpu_image_destroy(dev, r->textures[i].image);
            }
        }
    }
    free_instance_tables(r);
    const u32 live = nph_handle_pool_live(&r->instance_pool) +
                     nph_handle_pool_live(&r->material_pool) + nph_handle_pool_live(&r->light_pool);
    leaked += live;
    if (leaked != 0U) {
        NPH_LOG_WARN(&NPH_LOG_RENDER, "%s: %u resources were still alive at destruction", r->name,
                     leaked);
    }
    render_debug_destroy(r);
    render_ibl_destroy(r);
    render_vis_destroy(r);
    render_rt_destroy(r);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        nph_gpu_buffer_destroy(dev, r->frame_buffers[i]);
    }
    for (u32 i = 0; i < RENDER_TARGET_FORMATS; ++i) {
        const RenderTargetPipelines *t = &r->target_pipelines[i];
        nph_gpu_pipeline_destroy(dev, t->tonemap);
        nph_gpu_pipeline_destroy(dev, t->debug_depth);
        nph_gpu_pipeline_destroy(dev, t->debug_overlay);
        nph_gpu_pipeline_destroy(dev, t->vis_tonemap);
        nph_gpu_pipeline_destroy(dev, t->vis_debug_depth);
        nph_gpu_pipeline_destroy(dev, t->vis_debug_overlay);
    }
    for (u32 a = 0; a < 3U; ++a) {
        for (u32 c = 0; c < (u32)RENDER_CULL_COUNT; ++c) {
            nph_gpu_pipeline_destroy(dev, r->forward[a][c]);
            nph_gpu_pipeline_destroy(dev, r->forward_rt[a][c]);
        }
    }
    nph_gpu_pipeline_destroy(dev, r->sky);
    for (u32 i = 0; i < (u32)RENDER_COMPUTE_COUNT; ++i) {
        nph_gpu_pipeline_destroy(dev, r->compute[i]);
    }
    for (u32 i = 0; i < (u32)RENDER_SHADER_COUNT; ++i) {
        nph_gpu_shader_destroy(dev, r->shaders[i]);
    }
    for (u32 u = 0; u < 3U; ++u) {
        for (u32 v = 0; v < 3U; ++v) {
            for (u32 n = 0; n < 2U; ++n) {
                nph_gpu_sampler_destroy(dev, r->material_samplers[u][v][n]);
            }
        }
    }
    nph_gpu_sampler_destroy(dev, r->clamp_sampler);
    NphAllocator *a = r->alloc;
    NPH_FREE_ARRAY(a, RenderTexture, r->textures, r->desc.max_textures);
    NPH_FREE_ARRAY(a, RenderMesh, r->meshes, r->desc.max_meshes);
    NPH_FREE_ARRAY(a, RenderMaterial, r->materials, r->desc.max_materials);
    NPH_FREE_ARRAY(a, RenderInstance, r->instances, r->desc.max_instances);
    NPH_FREE_ARRAY(a, RenderLight, r->lights, r->desc.max_lights);
    NPH_FREE_ARRAY(a, RenderDrawCmd, r->draw_cmds, r->desc.max_draws);
    nph_handle_pool_free(&r->texture_pool);
    nph_handle_pool_free(&r->mesh_pool);
    nph_handle_pool_free(&r->material_pool);
    nph_handle_pool_free(&r->instance_pool);
    nph_handle_pool_free(&r->light_pool);
    NPH_FREE_TYPE(a, NphRenderer, r);
}

NphGpuDevice *nph_renderer_device(NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    return r->dev;
}

bool nph_renderer_production_supported(const NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    return r->vis.supported != 0;
}

bool nph_renderer_ray_tracing_supported(const NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    return r->rt.supported != 0;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

const RenderTexture *render_texture_get(const NphRenderer *r, NphRenderTexture texture)
{
    const u32 slot = render_slot_of(&r->texture_pool, texture.value);
    return slot == NPH_U32_MAX ? NULL : &r->textures[slot];
}

NphResult nph_render_texture_create(NphRenderer *r, const NphRenderTextureDesc *desc,
                                    NphRenderTexture *out)
{
    NPH_ASSERT(r != NULL && desc != NULL && out != NULL);
    *out = NPH_RENDER_TEXTURE_NULL;
    const bool cube = desc->kind == NPH_RENDER_TEXTURE_CUBE;
    const u32 layers = desc->layer_count != 0U ? desc->layer_count : (cube ? 6U : 1U);
    const NphGpuFormatInfo *fi = nph_gpu_format_info(desc->format);
    if ((desc->kind != NPH_RENDER_TEXTURE_2D && !cube) ||
        desc->format == NPH_GPU_FORMAT_UNDEFINED || nph_gpu_format_is_depth(desc->format) ||
        desc->width == 0U || desc->height == 0U || desc->mip_count == 0U ||
        desc->mip_count > NPH_GPU_MAX_MIPS ||
        desc->mip_count > nph_gpu_mip_count(desc->width, desc->height, 1U) ||
        (cube && (layers != 6U || desc->width != desc->height)) || (!cube && layers != 1U) ||
        desc->mips == NULL || desc->mip_sizes == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 m = 0; m < desc->mip_count; ++m) {
        const u64 expected =
            nph_gpu_format_level_bytes(desc->format, nph_gpu_mip_extent(desc->width, m),
                                       nph_gpu_mip_extent(desc->height, m), 1U) *
            layers;
        if (desc->mips[m] == NULL || desc->mip_sizes[m] != expected) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    if (fi->compressed && !nph_gpu_device_info(r->dev)->features.texture_compression_bc) {
        return NPH_RESULT_UNSUPPORTED;
    }
    u32 value = 0U;
    u32 slot = 0U;
    NPH_TRY(alloc_slot(&r->texture_pool, &value, &slot));
    RenderTexture *t = &r->textures[slot];
    memset(t, 0, sizeof *t);
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.type = cube ? NPH_GPU_IMAGE_CUBE : NPH_GPU_IMAGE_2D;
    id.format = desc->format;
    id.width = desc->width;
    id.height = desc->height;
    id.mip_count = desc->mip_count;
    id.layer_count = layers;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    id.name = desc->name != NULL ? desc->name : "render texture";
    NphResult res = nph_gpu_image_create(r->dev, &id, &t->image);
    for (u32 m = 0; m < desc->mip_count && res == NPH_RESULT_OK; ++m) {
        res = nph_gpu_upload_image(r->dev, t->image, m, 0U, layers, desc->mips[m],
                                   desc->mip_sizes[m]);
    }
    if (res != NPH_RESULT_OK) {
        nph_gpu_image_destroy(r->dev, t->image);
        t->image = NPH_GPU_IMAGE_NULL;
        release_slot(&r->texture_pool, value);
        return res;
    }
    t->handle = value;
    t->kind = desc->kind;
    t->flags = desc->flags;
    t->width = desc->width;
    t->mip_count = desc->mip_count;
    r->pending_uploads = 1;
    r->material_version += 1U; // materials resolve texture slots per frame
    out->value = value;
    return NPH_RESULT_OK;
}

void nph_render_texture_destroy(NphRenderer *r, NphRenderTexture texture)
{
    NPH_ASSERT(r != NULL);
    if (texture.value == 0U) {
        return;
    }
    const u32 slot = render_slot_of(&r->texture_pool, texture.value);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale texture handle");
    if (slot == NPH_U32_MAX) {
        return;
    }
    RenderTexture *t = &r->textures[slot];
    nph_gpu_image_destroy(r->dev, t->image);
    memset(t, 0, sizeof *t);
    release_slot(&r->texture_pool, texture.value);
    r->material_version += 1U;
}

bool nph_render_texture_is_valid(const NphRenderer *r, NphRenderTexture texture)
{
    return render_slot_of(&r->texture_pool, texture.value) != NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

static NphResult make_buffer(NphRenderer *r, const void *data, u64 size, u32 usage,
                             const char *name, NphGpuBuffer *out)
{
    NphGpuBufferDesc d;
    memset(&d, 0, sizeof d);
    d.size = size;
    d.usage = usage | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    d.name = name;
    NPH_TRY(nph_gpu_buffer_create(r->dev, &d, out));
    return nph_gpu_upload_buffer(r->dev, *out, 0U, data, size);
}

// Meshlets given with a mesh are checked so far as the GPU's reads stay
// inside the streams: every range, local corner, and vertex index. That
// they cover their submeshes' triangles exactly is the caller's contract.
static NphResult validate_meshlets(const NphRenderMeshDesc *d)
{
    if (d->meshlets == NULL) {
        return NPH_RESULT_OK;
    }
    if (d->meshlet_count == 0U || d->meshlet_bounds == NULL || d->meshlet_vertices == NULL ||
        d->meshlet_triangles == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < d->meshlet_vertex_count; ++i) {
        if (d->meshlet_vertices[i] >= d->vertex_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    for (u32 i = 0; i < d->meshlet_count; ++i) {
        const NphRenderMeshlet *m = &d->meshlets[i];
        const NphRenderMeshletBounds *b = &d->meshlet_bounds[i];
        if (m->vertex_count == 0U || m->vertex_count > NPH_RENDER_MESHLET_MAX_VERTICES ||
            m->triangle_count == 0U || m->triangle_count > NPH_RENDER_MESHLET_MAX_TRIANGLES ||
            m->submesh >= d->submesh_count || m->vertex_offset > d->meshlet_vertex_count ||
            m->vertex_count > d->meshlet_vertex_count - m->vertex_offset ||
            m->triangle_offset > d->meshlet_triangle_count ||
            m->triangle_count > d->meshlet_triangle_count - m->triangle_offset ||
            !nph_vec3_is_finite(b->center) || !(b->radius >= 0.0F) ||
            !nph_f32_is_finite(b->radius) || !nph_vec3_is_finite(b->cone_apex)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        const u8 *corners = d->meshlet_triangles + (u64)m->triangle_offset * 3U;
        for (u32 c = 0; c < (u32)m->triangle_count * 3U; ++c) {
            if (corners[c] >= m->vertex_count) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
    }
    return NPH_RESULT_OK;
}

static NphResult validate_mesh(const NphRenderMeshDesc *d)
{
    if (d->vertex_count == 0U || d->index_count == 0U || d->index_count % 3U != 0U ||
        d->positions == NULL || d->attribs == NULL || d->indices == NULL || d->submeshes == NULL ||
        d->submesh_count == 0U || d->submesh_count > NPH_RENDER_MAX_SUBMESHES ||
        !nph_vec3_is_finite(d->position_offset) || !nph_vec3_is_finite(d->position_scale)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < d->index_count; ++i) {
        if (d->indices[i] >= d->vertex_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    for (u32 s = 0; s < d->submesh_count; ++s) {
        const NphRenderSubmesh *sm = &d->submeshes[s];
        if (sm->index_count == 0U || sm->index_count % 3U != 0U ||
            sm->index_offset > d->index_count ||
            sm->index_count > d->index_count - sm->index_offset) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    return validate_meshlets(d);
}

NphResult nph_render_mesh_create(NphRenderer *r, const NphRenderMeshDesc *desc, NphRenderMesh *out)
{
    NPH_ASSERT(r != NULL && desc != NULL && out != NULL);
    *out = NPH_RENDER_MESH_NULL;
    NPH_TRY(validate_mesh(desc));
    u32 value = 0U;
    u32 slot = 0U;
    NPH_TRY(alloc_slot(&r->mesh_pool, &value, &slot));
    RenderMesh *m = &r->meshes[slot];
    memset(m, 0, sizeof *m);
    m->index_range = NPH_RANGE_NULL; // a zero-filled range would name node 0
    m->submeshes = NPH_ALLOC_ARRAY(r->alloc, NphRenderSubmesh, desc->submesh_count);
    NphResult res = m->submeshes != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    // Bottom-level builds read the positions and indices (rt.c).
    const u32 accel_input = r->rt.supported ? (u32)NPH_GPU_BUFFER_USAGE_ACCEL_INPUT : 0U;
    if (res == NPH_RESULT_OK) {
        m->submesh_count = desc->submesh_count;
        memcpy(m->submeshes, desc->submeshes, sizeof(NphRenderSubmesh) * desc->submesh_count);
        res = make_buffer(
            r, desc->positions, (u64)desc->vertex_count * sizeof(NphRenderVertexPosition),
            (u32)NPH_GPU_BUFFER_USAGE_STORAGE | accel_input, "mesh positions", &m->positions);
    }
    if (res == NPH_RESULT_OK) {
        res = make_buffer(r, desc->attribs, (u64)desc->vertex_count * sizeof(NphRenderVertexAttrib),
                          (u32)NPH_GPU_BUFFER_USAGE_STORAGE, "mesh attributes", &m->attribs);
    }
    if (res == NPH_RESULT_OK) {
        res =
            make_buffer(r, desc->indices, (u64)desc->index_count * sizeof(u32),
                        (u32)NPH_GPU_BUFFER_USAGE_INDEX | accel_input, "mesh indices", &m->indices);
    }
    if (res == NPH_RESULT_OK) {
        m->positions_address = nph_gpu_buffer_address(r->dev, m->positions);
        m->attribs_address = nph_gpu_buffer_address(r->dev, m->attribs);
        m->vertex_count = desc->vertex_count;
        m->index_count = desc->index_count;
        // The bottom level first: the GPU scene's mesh record names it.
        res = render_rt_mesh_create(r, slot, desc);
    }
    if (res == NPH_RESULT_OK) {
        res = render_vis_mesh_create(r, slot, desc);
    }
    if (res != NPH_RESULT_OK) {
        nph_gpu_buffer_destroy(r->dev, m->positions);
        nph_gpu_buffer_destroy(r->dev, m->attribs);
        nph_gpu_buffer_destroy(r->dev, m->indices);
        render_vis_mesh_destroy(r, slot);
        render_rt_mesh_destroy(r, slot);
        if (m->submeshes != NULL) {
            NPH_FREE_ARRAY(r->alloc, NphRenderSubmesh, m->submeshes, m->submesh_count);
        }
        memset(m, 0, sizeof *m);
        release_slot(&r->mesh_pool, value);
        return res;
    }
    m->handle = value;
    m->index_count = desc->index_count;
    m->position_offset = desc->position_offset;
    m->position_scale = desc->position_scale;
    m->bounds = desc->bounds;
    r->pending_uploads = 1;
    out->value = value;
    return NPH_RESULT_OK;
}

void nph_render_mesh_destroy(NphRenderer *r, NphRenderMesh mesh)
{
    NPH_ASSERT(r != NULL);
    if (mesh.value == 0U) {
        return;
    }
    const u32 slot = render_slot_of(&r->mesh_pool, mesh.value);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale mesh handle");
    if (slot == NPH_U32_MAX) {
        return;
    }
    RenderMesh *m = &r->meshes[slot];
    NPH_ASSERT_MSG(m->instances == 0U, "destroying a mesh that instances still use");
    if (m->instances != 0U) {
        NPH_LOG_ERROR(&NPH_LOG_RENDER, "%s: a mesh with %u live instances was not destroyed",
                      r->name, m->instances);
        return;
    }
    nph_gpu_buffer_destroy(r->dev, m->positions);
    nph_gpu_buffer_destroy(r->dev, m->attribs);
    nph_gpu_buffer_destroy(r->dev, m->indices);
    render_vis_mesh_destroy(r, slot);
    render_rt_mesh_destroy(r, slot);
    NPH_FREE_ARRAY(r->alloc, NphRenderSubmesh, m->submeshes, m->submesh_count);
    memset(m, 0, sizeof *m);
    release_slot(&r->mesh_pool, mesh.value);
}

bool nph_render_mesh_is_valid(const NphRenderer *r, NphRenderMesh mesh)
{
    return render_slot_of(&r->mesh_pool, mesh.value) != NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

void nph_render_material_desc_default(NphRenderMaterialDesc *desc)
{
    NPH_ASSERT(desc != NULL);
    memset(desc, 0, sizeof *desc);
    desc->base_color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    desc->emissive = nph_vec3_zero();
    desc->metallic = 0.0F;
    desc->roughness = 1.0F;
    desc->normal_scale = 1.0F;
    desc->occlusion_strength = 1.0F;
    desc->alpha_cutoff = 0.5F;
    desc->alpha_mode = NPH_RENDER_ALPHA_OPAQUE;
}

static NphResult validate_material(const NphRenderer *r, const NphRenderMaterialDesc *d)
{
    if ((u32)d->alpha_mode > (u32)NPH_RENDER_ALPHA_BLEND || !nph_vec4_is_finite(d->base_color) ||
        !nph_vec3_is_finite(d->emissive) || !nph_f32_is_finite(d->metallic) ||
        !nph_f32_is_finite(d->roughness) || !nph_f32_is_finite(d->normal_scale) ||
        !nph_f32_is_finite(d->occlusion_strength) || !nph_f32_is_finite(d->alpha_cutoff)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 s = 0; s < (u32)NPH_RENDER_SLOT_COUNT; ++s) {
        const NphRenderTextureBinding *b = &d->textures[s];
        if ((u32)b->wrap_u > (u32)NPH_RENDER_WRAP_MIRROR ||
            (u32)b->wrap_v > (u32)NPH_RENDER_WRAP_MIRROR) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (b->texture.value != 0U) {
            const RenderTexture *t = render_texture_get(r, b->texture);
            if (t == NULL || t->kind != NPH_RENDER_TEXTURE_2D) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_render_material_create(NphRenderer *r, const NphRenderMaterialDesc *desc,
                                     NphRenderMaterial *out)
{
    NPH_ASSERT(r != NULL && desc != NULL && out != NULL);
    *out = NPH_RENDER_MATERIAL_NULL;
    NPH_TRY(validate_material(r, desc));
    u32 value = 0U;
    u32 slot = 0U;
    NPH_TRY(alloc_slot(&r->material_pool, &value, &slot));
    RenderMaterial *m = &r->materials[slot];
    m->handle = value;
    m->desc = *desc;
    m->desc.name = NULL;
    r->material_version += 1U;
    out->value = value;
    return NPH_RESULT_OK;
}

NphResult nph_render_material_update(NphRenderer *r, NphRenderMaterial material,
                                     const NphRenderMaterialDesc *desc)
{
    NPH_ASSERT(r != NULL && desc != NULL);
    const u32 slot = render_slot_of(&r->material_pool, material.value);
    if (slot == NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(validate_material(r, desc));
    r->materials[slot].desc = *desc;
    r->materials[slot].desc.name = NULL;
    r->material_version += 1U;
    render_vis_materials_changed(r);
    return NPH_RESULT_OK;
}

void nph_render_material_destroy(NphRenderer *r, NphRenderMaterial material)
{
    NPH_ASSERT(r != NULL);
    if (material.value == 0U) {
        return;
    }
    const u32 slot = render_slot_of(&r->material_pool, material.value);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale material handle");
    if (slot == NPH_U32_MAX) {
        return;
    }
    memset(&r->materials[slot], 0, sizeof r->materials[slot]);
    release_slot(&r->material_pool, material.value);
    r->material_version += 1U;
    render_vis_materials_changed(r);
}

bool nph_render_material_is_valid(const NphRenderer *r, NphRenderMaterial material)
{
    return render_slot_of(&r->material_pool, material.value) != NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

NphRenderTransform nph_render_transform_identity(void)
{
    NphRenderTransform t;
    t.linear = nph_mat3_identity();
    t.translation = nph_dvec3_zero();
    return t;
}

NphRenderTransform nph_render_transform_from_mat4(const NphMat4 *m)
{
    NPH_ASSERT(m != NULL);
    NphRenderTransform t;
    t.linear =
        nph_mat3_from_cols(nph_vec4_xyz(m->c[0]), nph_vec4_xyz(m->c[1]), nph_vec4_xyz(m->c[2]));
    t.translation = nph_dvec3_from_vec3(nph_vec4_xyz(m->c[3]));
    return t;
}

NphRenderTransform nph_render_transform_from_trs(NphDVec3 translation, NphQuat rotation,
                                                 NphVec3 scale)
{
    NphRenderTransform t;
    t.linear = nph_mat3_mul(nph_mat3_from_quat(rotation), nph_mat3_from_scale(scale));
    t.translation = translation;
    return t;
}

static bool transform_ok(const NphRenderTransform *t)
{
    const f32 det = nph_mat3_determinant(t->linear);
    return nph_f32_is_finite(det) && det != 0.0F && nph_f64_is_finite(t->translation.x) &&
           nph_f64_is_finite(t->translation.y) && nph_f64_is_finite(t->translation.z);
}

NphResult nph_render_instance_create(NphRenderer *r, const NphRenderInstanceDesc *desc,
                                     NphRenderInstance *out)
{
    NPH_ASSERT(r != NULL && desc != NULL && out != NULL);
    *out = NPH_RENDER_INSTANCE_NULL;
    const u32 mesh_slot = render_slot_of(&r->mesh_pool, desc->mesh.value);
    if (mesh_slot == NPH_U32_MAX || !transform_ok(&desc->transform) ||
        (desc->material_count != 0U && desc->materials == NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < desc->material_count; ++i) {
        if (desc->materials[i].value != 0U &&
            render_slot_of(&r->material_pool, desc->materials[i].value) == NPH_U32_MAX) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    u32 value = 0U;
    u32 slot = 0U;
    NPH_TRY(alloc_slot(&r->instance_pool, &value, &slot));
    RenderInstance *inst = &r->instances[slot];
    memset(inst, 0, sizeof *inst);
    if (desc->material_count != 0U) {
        inst->materials = NPH_ALLOC_ARRAY(r->alloc, NphRenderMaterial, desc->material_count);
        if (inst->materials == NULL) {
            release_slot(&r->instance_pool, value);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        memcpy(inst->materials, desc->materials, sizeof(NphRenderMaterial) * desc->material_count);
    }
    inst->handle = value;
    inst->mesh = desc->mesh;
    inst->material_count = desc->material_count;
    inst->transform = desc->transform;
    inst->hidden = desc->hidden;
    const NphResult res = render_vis_instance_created(r, slot);
    if (res != NPH_RESULT_OK) {
        if (inst->materials != NULL) {
            NPH_FREE_ARRAY(r->alloc, NphRenderMaterial, inst->materials, inst->material_count);
        }
        memset(inst, 0, sizeof *inst);
        release_slot(&r->instance_pool, value);
        return res;
    }
    r->meshes[mesh_slot].instances += 1U;
    r->hidden_count += inst->hidden ? 1U : 0U;
    out->value = value;
    return NPH_RESULT_OK;
}

NphResult nph_render_instance_set_transform(NphRenderer *r, NphRenderInstance instance,
                                            const NphRenderTransform *transform)
{
    NPH_ASSERT(r != NULL && transform != NULL);
    const u32 slot = render_slot_of(&r->instance_pool, instance.value);
    if (slot == NPH_U32_MAX || !transform_ok(transform)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    r->instances[slot].transform = *transform;
    render_vis_instance_changed(r, slot);
    return NPH_RESULT_OK;
}

void nph_render_instance_set_hidden(NphRenderer *r, NphRenderInstance instance, bool hidden)
{
    NPH_ASSERT(r != NULL);
    const u32 slot = render_slot_of(&r->instance_pool, instance.value);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale instance handle");
    if (slot != NPH_U32_MAX && r->instances[slot].hidden != (b32)hidden) {
        r->instances[slot].hidden = hidden;
        r->hidden_count = hidden ? r->hidden_count + 1U : r->hidden_count - 1U;
        render_vis_instance_changed(r, slot);
    }
}

void nph_render_instance_destroy(NphRenderer *r, NphRenderInstance instance)
{
    NPH_ASSERT(r != NULL);
    if (instance.value == 0U) {
        return;
    }
    const u32 slot = render_slot_of(&r->instance_pool, instance.value);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale instance handle");
    if (slot == NPH_U32_MAX) {
        return;
    }
    RenderInstance *inst = &r->instances[slot];
    const u32 mesh_slot = render_slot_of(&r->mesh_pool, inst->mesh.value);
    if (mesh_slot != NPH_U32_MAX) {
        r->meshes[mesh_slot].instances -= 1U;
    }
    r->hidden_count -= inst->hidden ? 1U : 0U;
    render_vis_instance_destroyed(r, slot);
    if (inst->materials != NULL) {
        NPH_FREE_ARRAY(r->alloc, NphRenderMaterial, inst->materials, inst->material_count);
    }
    memset(inst, 0, sizeof *inst);
    release_slot(&r->instance_pool, instance.value);
}

bool nph_render_instance_is_valid(const NphRenderer *r, NphRenderInstance instance)
{
    return render_slot_of(&r->instance_pool, instance.value) != NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

static NphResult validate_light(const NphRenderLightDesc *d)
{
    if ((u32)d->type > (u32)NPH_RENDER_LIGHT_TYPE_SPOT || !nph_vec3_is_finite(d->color) ||
        !nph_f32_is_finite(d->intensity) || d->intensity < 0.0F || !nph_f32_is_finite(d->range) ||
        d->range < 0.0F || !nph_f64_is_finite(d->position.x) || !nph_f64_is_finite(d->position.y) ||
        !nph_f64_is_finite(d->position.z)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (d->type != NPH_RENDER_LIGHT_TYPE_POINT &&
        (!nph_vec3_is_finite(d->direction) || nph_vec3_length_sq(d->direction) < 1e-12F)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (d->type == NPH_RENDER_LIGHT_TYPE_SPOT &&
        !(d->inner_cone >= 0.0F && d->inner_cone < d->outer_cone &&
          d->outer_cone <= 0.5F * NPH_PI)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return NPH_RESULT_OK;
}

NphResult nph_render_light_create(NphRenderer *r, const NphRenderLightDesc *desc,
                                  NphRenderLight *out)
{
    NPH_ASSERT(r != NULL && desc != NULL && out != NULL);
    *out = NPH_RENDER_LIGHT_NULL;
    NPH_TRY(validate_light(desc));
    u32 value = 0U;
    u32 slot = 0U;
    NPH_TRY(alloc_slot(&r->light_pool, &value, &slot));
    r->lights[slot].handle = value;
    r->lights[slot].desc = *desc;
    render_rt_shadow_lights(r, desc->shadows ? 1 : 0);
    render_vis_light_changed(r, slot);
    out->value = value;
    return NPH_RESULT_OK;
}

NphResult nph_render_light_update(NphRenderer *r, NphRenderLight light,
                                  const NphRenderLightDesc *desc)
{
    NPH_ASSERT(r != NULL && desc != NULL);
    const u32 slot = render_slot_of(&r->light_pool, light.value);
    if (slot == NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(validate_light(desc));
    render_rt_shadow_lights(r, (desc->shadows ? 1 : 0) - (r->lights[slot].desc.shadows ? 1 : 0));
    r->lights[slot].desc = *desc;
    render_vis_light_changed(r, slot);
    return NPH_RESULT_OK;
}

void nph_render_light_destroy(NphRenderer *r, NphRenderLight light)
{
    NPH_ASSERT(r != NULL);
    if (light.value == 0U) {
        return;
    }
    const u32 slot = render_slot_of(&r->light_pool, light.value);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale light handle");
    if (slot == NPH_U32_MAX) {
        return;
    }
    render_rt_shadow_lights(r, r->lights[slot].desc.shadows ? -1 : 0);
    memset(&r->lights[slot], 0, sizeof r->lights[slot]);
    render_vis_light_changed(r, slot);
    release_slot(&r->light_pool, light.value);
}

bool nph_render_light_is_valid(const NphRenderer *r, NphRenderLight light)
{
    return render_slot_of(&r->light_pool, light.value) != NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

void nph_render_sky_desc_default(NphRenderSkyDesc *desc)
{
    NPH_ASSERT(desc != NULL);
    // A clear sky: a saturated blue zenith brightening to a pale horizon,
    // and a dark, warm ground (radiance in cd/m^2).
    desc->zenith = nph_vec3(1600.0F, 3200.0F, 6400.0F);
    desc->horizon = nph_vec3(6000.0F, 7000.0F, 8000.0F);
    desc->ground = nph_vec3(1500.0F, 1300.0F, 1100.0F);
    desc->zenith_exponent = 0.5F;
    desc->ground_sharpness = 8.0F;
}

NphResult nph_render_set_environment(NphRenderer *r, const NphRenderEnvironmentDesc *desc)
{
    NPH_ASSERT(r != NULL && desc != NULL);
    if ((u32)desc->source > (u32)NPH_RENDER_ENV_CUBE || !nph_f32_is_finite(desc->intensity) ||
        desc->intensity < 0.0F) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (desc->source == NPH_RENDER_ENV_SKY &&
        (!nph_vec3_is_finite(desc->sky.zenith) || !nph_vec3_is_finite(desc->sky.horizon) ||
         !nph_vec3_is_finite(desc->sky.ground) || desc->sky.zenith_exponent < 0.0F ||
         desc->sky.ground_sharpness < 0.0F)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (desc->source == NPH_RENDER_ENV_EQUIRECT || desc->source == NPH_RENDER_ENV_CUBE) {
        const RenderTexture *t = render_texture_get(r, desc->texture);
        const NphRenderTextureKind kind =
            desc->source == NPH_RENDER_ENV_CUBE ? NPH_RENDER_TEXTURE_CUBE : NPH_RENDER_TEXTURE_2D;
        if (t == NULL || t->kind != kind) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    r->env = *desc;
    if (r->env.intensity == 0.0F) {
        r->env.intensity = 1.0F;
    }
    if (r->env.source == NPH_RENDER_ENV_SKY) {
        if (r->env.sky.zenith_exponent == 0.0F) {
            r->env.sky.zenith_exponent = 0.5F;
        }
        if (r->env.sky.ground_sharpness == 0.0F) {
            r->env.sky.ground_sharpness = 8.0F;
        }
    }
    r->env_dirty = 1;
    return NPH_RESULT_OK;
}

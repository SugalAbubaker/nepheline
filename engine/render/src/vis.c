// SPDX-License-Identifier: Apache-2.0
// The production path's pipelines and frames (render/renderer.h; docs/render.md
// "The production path"; docs/adr/0029-gpu-driven-renderer.md): the
// GPU-driven visibility-buffer renderer of CLAUDE.md §8.6 stages 3 and 4.
//
// A frame, as render-graph passes:
//
//   vis clear             counters to zero, the visibility buffer to "nothing"
//   vis scatter           the frame's instance updates (vis_scene.c)
//   vis light updates, vis lights, vis light bins, vis light tiles
//                         the light grid the shading reads (docs/render.md
//                         "Lights")
//   vis cull instances 0  frustum, then last frame's HZB from last frame's view
//   vis args, vis cull meshlets 0, vis args, vis raster 0 (hardware), vis sw
//                         raster 0 (small clusters, in compute)
//   vis hzb 0             this frame's depth so far
//   vis args, vis cull instances 1, vis args, vis cull meshlets 1 (the
//                         meshlets phase 0 deferred, then those of instances
//                         now visible), vis args, vis raster 1, vis sw raster 1
//   vis hzb 1             the frame's depth: next frame's phase 0 tests it
//   vsm ...               the sun's virtual shadow map (vsm.c), its page
//                         requests made by the vis hzb passes (and, with
//                         blended surfaces, by `vis depth` and `vsm requests
//                         (blended)` first)
//   rt ...                shadow rays' top level (rt.c)
//   vis resolve           shading into the HDR image
//   vis depth, transparent (only with blended surfaces)
//   vis statistics        the counters, read back two frames later
//
// Everything after `vis clear` is sized by counters the GPU itself wrote,
// through indirect dispatches and draws: the CPU records the same passes
// whatever the scene holds.
#include <nepheline/core/log.h>
#include <nepheline/core/profile.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphVisViewData) == 784U, "NphVisViewData matches its GLSL layout");
NPH_STATIC_ASSERT(NPH_RENDER_ZBINS % 256 == 0, "vis_light_bins.comp gives each thread whole bins");
NPH_STATIC_ASSERT(NPH_RENDER_MAX_LIGHTS <= 65535, "light indices fit 16 bits");
NPH_STATIC_ASSERT(sizeof(NphVisPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(offsetof(NphVisViewData, render_view) % 8U == 0U, "addresses are aligned");

static const char *const k_shader_files[VIS_SHADER_COUNT] = {
    "vis_scatter.comp",
    "vis_cull_instances.comp",
    "vis_cull_meshlets.comp",
    "vis_args.comp",
    "vis_hzb.comp",
    "vis_resolve.comp",
    "vis_sw_raster.comp",
    "vis_lights.comp",
    "vis_light_bins.comp",
    "vis_light_tiles.comp",
    "vis_resolve_rt.comp",
    "vis_raster_opaque.mesh",
    "vis_raster_masked.mesh",
    "vis_raster_mesh_opaque.frag",
    "vis_raster_mesh_masked.frag",
    "vis_raster_opaque.vert",
    "vis_raster_masked.vert",
    "vis_raster_vert_opaque.frag",
    "vis_raster_vert_masked.frag",
    "vis_depth.frag",
    "vis_debug_line.frag",
};

static const VisShader k_compute_shaders[VIS_COMPUTE_COUNT] = {
    VIS_SHADER_SCATTER,    VIS_SHADER_CULL_INSTANCES, VIS_SHADER_CULL_MESHLETS, VIS_SHADER_ARGS,
    VIS_SHADER_HZB,        VIS_SHADER_RESOLVE,        VIS_SHADER_SW_RASTER,     VIS_SHADER_LIGHTS,
    VIS_SHADER_LIGHT_BINS, VIS_SHADER_LIGHT_TILES,    VIS_SHADER_RESOLVE_RT,    VIS_SHADER_RESOLVE,
    VIS_SHADER_RESOLVE_RT,
};

static const char *const k_compute_names[VIS_COMPUTE_COUNT] = {
    "vis scatter",
    "vis cull instances",
    "vis cull meshlets",
    "vis args",
    "vis hzb",
    "vis resolve",
    "vis sw raster",
    "vis lights",
    "vis light bins",
    "vis light tiles",
    "vis resolve (ray traced)",
    "vis resolve (virtual shadows)",
    "vis resolve (ray traced, virtual shadows)",
};

// ---------------------------------------------------------------------------
// Support and creation
// ---------------------------------------------------------------------------

bool render_vis_device_supported(const NphRenderer *r)
{
    const NphGpuDeviceInfo *info = nph_gpu_device_info(r->dev);
    const NphGpuFeatures *f = &info->features;
    const char *missing = !f->image_atomic_int64 ? "64-bit image atomics"
                          : !f->shader_int64     ? "64-bit integers in shaders"
                          : !(f->subgroup_ballot && f->subgroup_arithmetic && f->subgroup_shuffle)
                              ? "subgroup ballot, arithmetic, and shuffle in compute shaders"
                          : !f->geometry_shader ? "gl_PrimitiveID in fragment shaders"
                                                : NULL;
    if (missing != NULL) {
        NPH_LOG_INFO(&NPH_LOG_RENDER, "%s: no production path on %s: it needs %s", r->name,
                     info->name, missing);
        return false;
    }
    return true;
}

// The mesh-shader raster path: its fragment shaders read the visibility
// buffer's index from push constants, which drivers with
// NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS lose; those take the indirect
// path.
static bool mesh_path_supported(const NphRenderer *r)
{
    const NphGpuDeviceInfo *info = nph_gpu_device_info(r->dev);
    const NphGpuLimits *l = &info->limits;
    return info->features.mesh_shader &&
           (info->quirks & (u32)NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS) == 0U &&
           l->max_mesh_output_vertices >= NPH_RENDER_MESHLET_MAX_VERTICES &&
           l->max_mesh_output_primitives >= NPH_RENDER_MESHLET_MAX_TRIANGLES &&
           l->max_mesh_workgroup_invocations >= 32U;
}

// What the driver reports of a compiled pipeline (registers, spills), when
// the device keeps it (NphGpuDeviceDesc.shader_statistics): one line each,
// for tuning the shaders' occupancy.
void render_log_statistics(NphRenderer *r, NphGpuPipeline pipeline, const char *name)
{
    NphGpuShaderStatistic stats[24];
    const u32 n = nph_gpu_pipeline_statistics(r->dev, pipeline, stats, 24U);
    if (n == 0U) {
        return;
    }
    char line[768];
    u32 at = 0U;
    line[0] = '\0';
    for (u32 i = 0; i < n && at + 1U < (u32)sizeof line; ++i) {
        const int w = snprintf(line + at, sizeof line - at, "%s%s %llu", i == 0U ? "" : ", ",
                               stats[i].name, (unsigned long long)stats[i].value);
        if (w < 0) {
            break;
        }
        at += (u32)w;
    }
    NPH_LOG_INFO(&NPH_LOG_RENDER, "%s: %s: %s", r->name, name, line);
}

static NphResult create_raster_pipeline(NphRenderer *r, VisShader stage, VisShader fragment,
                                        const char *name, NphGpuPipeline *out)
{
    RenderVis *v = &r->vis;
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    if (v->mesh_shaders) {
        d.mesh = v->shaders[stage];
    } else {
        d.vertex = v->shaders[stage];
    }
    d.fragment = v->shaders[fragment];
    // No attachments: the fragment stage writes the visibility buffer
    // through image atomics, and faces are culled per triangle.
    d.cull_mode = NPH_GPU_CULL_NONE;
    d.name = name;
    return nph_gpu_pipeline_create_graphics(r->dev, &d, out);
}

NphResult render_vis_create(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    v->mesh_shaders = mesh_path_supported(r);
    // The HDR target in B10G11R11 where the resolve can store to it, the
    // tonemap sample it, and blended surfaces blend into it (the resolve's
    // pipelines are specialised for it).
    const u32 hdr_use = (u32)NPH_GPU_FORMAT_FEATURE_STORAGE | (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED |
                        (u32)NPH_GPU_FORMAT_FEATURE_COLOR_ATTACHMENT |
                        (u32)NPH_GPU_FORMAT_FEATURE_BLEND;
    v->hdr_format = nph_gpu_format_supports(r->dev, RENDER_HDR_COMPACT, hdr_use)
                        ? RENDER_HDR_COMPACT
                        : RENDER_HDR_FORMAT;
    // The sun's virtual shadow map (vsm.c), decided here so the resolve and
    // the blended surfaces get the pipelines that read it.
    r->vsm.supported = render_vsm_device_supported(r) ? 1 : 0;
    const bool vsm = r->vsm.supported != 0;
    for (u32 i = 0; i < (u32)VIS_SHADER_COUNT; ++i) {
        const bool mesh_stage =
            i >= (u32)VIS_SHADER_MESH_OPAQUE && i <= (u32)VIS_SHADER_MESH_FRAG_MASKED;
        const bool vertex_stage =
            i >= (u32)VIS_SHADER_VERT_OPAQUE && i <= (u32)VIS_SHADER_VERT_FRAG_MASKED;
        // Only the raster path in use: a mesh shader cannot even be loaded
        // on a device without the extension, nor the ray-traced resolve on
        // one without ray queries.
        if ((mesh_stage && !v->mesh_shaders) || (vertex_stage && v->mesh_shaders) ||
            (i == (u32)VIS_SHADER_RESOLVE_RT && !r->rt.supported)) {
            continue;
        }
        NPH_TRY(render_load_shader_file(r, k_shader_files[i], &v->shaders[i]));
    }
    for (u32 i = 0; i < (u32)VIS_COMPUTE_COUNT; ++i) {
        const bool resolve_rt =
            i == (u32)VIS_COMPUTE_RESOLVE_RT || i == (u32)VIS_COMPUTE_RESOLVE_RT_VSM;
        const bool resolve_vsm =
            i == (u32)VIS_COMPUTE_RESOLVE_VSM || i == (u32)VIS_COMPUTE_RESOLVE_RT_VSM;
        if ((resolve_rt && !r->rt.supported) || (resolve_vsm && !vsm)) {
            continue;
        }
        NphGpuComputePipelineDesc d;
        memset(&d, 0, sizeof d);
        d.shader = v->shaders[k_compute_shaders[i]];
        d.name = k_compute_names[i];
        if (i == (u32)VIS_COMPUTE_RESOLVE || resolve_rt || resolve_vsm) {
            // Whether the resolve dithers to the HDR target's precision, and
            // whether it reads the sun's virtual shadow map
            // (vis_resolve.glsl k_dither, k_vsm).
            d.specialization[0] = v->hdr_format == RENDER_HDR_COMPACT ? 1U : 0U;
            d.specialization[1] = resolve_vsm ? 1U : 0U;
            d.specialization_count = 2U;
        }
        NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &v->compute[i]));
        render_log_statistics(r, v->compute[i], k_compute_names[i]);
    }
    if (v->mesh_shaders) {
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_MESH_OPAQUE, VIS_SHADER_MESH_FRAG_OPAQUE,
                                       "vis raster (mesh, opaque)", &v->raster[0]));
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_MESH_MASKED, VIS_SHADER_MESH_FRAG_MASKED,
                                       "vis raster (mesh, masked)", &v->raster[1]));
    } else {
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_VERT_OPAQUE, VIS_SHADER_VERT_FRAG_OPAQUE,
                                       "vis raster (indirect, opaque)", &v->raster[0]));
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_VERT_MASKED, VIS_SHADER_VERT_FRAG_MASKED,
                                       "vis raster (indirect, masked)", &v->raster[1]));
    }
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_FULLSCREEN_VERT];
    d.fragment = v->shaders[VIS_SHADER_DEPTH];
    d.depth_format = RENDER_DEPTH_FORMAT;
    d.depth.test = 1;
    d.depth.write = 1;
    d.depth.compare = NPH_GPU_COMPARE_ALWAYS;
    d.name = "vis depth";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &v->depth));
    // The blended surfaces, into the HDR format: with and without shadow
    // rays, and with and without the virtual shadow map.
    for (u32 rt = 0; rt < 2U; ++rt) {
        for (u32 map = 0; map < 2U; ++map) {
            if ((rt != 0U && !r->rt.supported) || (map != 0U && !vsm)) {
                continue;
            }
            for (u32 c = 0; c < (u32)RENDER_CULL_COUNT; ++c) {
                NPH_TRY(render_forward_pipeline(r, (u32)NPH_RENDER_ALPHA_BLEND, c, v->hdr_format,
                                                rt != 0U, map != 0U,
                                                &v->blend_pipelines[rt][map][c]));
            }
        }
    }
    NPH_TRY(render_vis_scene_create(r));
    v->supported = 1;
    const NphGpuDeviceInfo *info = nph_gpu_device_info(r->dev);
    NPH_LOG_INFO(&NPH_LOG_RENDER, "%s: production path rasterises with %s; HDR in %s", r->name,
                 v->mesh_shaders               ? "mesh shaders"
                 : !info->features.mesh_shader ? "indirect draws (no mesh shaders)"
                                               : "indirect draws (the driver's mesh shaders are "
                                                 "unusable here)",
                 nph_gpu_format_info(v->hdr_format)->name);
    return NPH_RESULT_OK;
}

void render_vis_destroy(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    NphGpuDevice *dev = r->dev;
    render_vis_scene_destroy(r);
    nph_gpu_image_destroy(dev, v->hzb);
    nph_gpu_buffer_destroy(dev, v->hzb_dirty);
    nph_gpu_buffer_destroy(dev, v->light_tiles);
    for (u32 i = 0; i < (u32)VIS_COMPUTE_COUNT; ++i) {
        nph_gpu_pipeline_destroy(dev, v->compute[i]);
    }
    nph_gpu_pipeline_destroy(dev, v->raster[0]);
    nph_gpu_pipeline_destroy(dev, v->raster[1]);
    nph_gpu_pipeline_destroy(dev, v->depth);
    for (u32 rt = 0; rt < 2U; ++rt) {
        for (u32 map = 0; map < 2U; ++map) {
            for (u32 c = 0; c < (u32)RENDER_CULL_COUNT; ++c) {
                nph_gpu_pipeline_destroy(dev, v->blend_pipelines[rt][map][c]);
            }
        }
    }
    for (u32 i = 0; i < (u32)VIS_SHADER_COUNT; ++i) {
        nph_gpu_shader_destroy(dev, v->shaders[i]);
    }
    memset(v, 0, sizeof *v);
}

NphResult render_vis_target_pipelines(NphRenderer *r, RenderTargetPipelines *t, NphGpuFormat format)
{
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_FULLSCREEN_VERT];
    d.fragment = r->shaders[RENDER_SHADER_TONEMAP_FRAG];
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.name = "tonemap (production)";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->vis_tonemap));

    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_DEBUG_VERT];
    d.fragment = r->vis.shaders[VIS_SHADER_DEBUG_LINE];
    d.topology = NPH_GPU_TOPOLOGY_LINE_LIST;
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.blend[0].enable = 1;
    d.blend[0].src_color = NPH_GPU_BLEND_SRC_ALPHA;
    d.blend[0].dst_color = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
    d.blend[0].src_alpha = NPH_GPU_BLEND_ONE;
    d.blend[0].dst_alpha = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
    d.name = "debug lines (production)";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->vis_debug_depth));
    d.fragment = r->shaders[RENDER_SHADER_DEBUG_FRAG];
    d.name = "debug lines (production, overlay)";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->vis_debug_overlay));
    return render_taa_target_pipeline(r, t, format);
}

// ---------------------------------------------------------------------------
// The hierarchical depth buffer
// ---------------------------------------------------------------------------

// Keeps what is sized by the target the size it needs: the light tiles'
// masks, and the HZB, whose mip 0 is half the target, rounded up, with a
// full chain down to 1 x 1 (vis_hzb_occluded relies on the last mip
// covering everything); Vulkan halves each later mip rounding down, which
// vis_hzb.comp's footprints follow.
static NphResult ensure_hzb(NphRenderer *r, u32 width, u32 height)
{
    RenderVis *v = &r->vis;
    if (!nph_gpu_image_is_null(v->hzb) && v->target_width == width && v->target_height == height) {
        return NPH_RESULT_OK;
    }
    nph_gpu_image_destroy(r->dev, v->hzb); // deferred past the frames in flight
    nph_gpu_buffer_destroy(r->dev, v->hzb_dirty);
    nph_gpu_buffer_destroy(r->dev, v->light_tiles);
    v->hzb = NPH_GPU_IMAGE_NULL;
    v->hzb_dirty = NPH_GPU_BUFFER_NULL;
    v->light_tiles = NPH_GPU_BUFFER_NULL;
    v->hzb_valid = 0;
    v->light_tiles_x = (width + (u32)NPH_RENDER_LIGHT_TILE - 1U) / (u32)NPH_RENDER_LIGHT_TILE;
    v->light_tiles_y = (height + (u32)NPH_RENDER_LIGHT_TILE - 1U) / (u32)NPH_RENDER_LIGHT_TILE;
    NphGpuBufferDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.size = (u64)v->light_tiles_x * v->light_tiles_y * v->light_words * sizeof(u32);
    // A transfer source too: the render tests read the grid back.
    ld.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    ld.memory = NPH_GPU_MEMORY_DEVICE;
    ld.name = "vis light tiles";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &ld, &v->light_tiles));
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = RENDER_HZB_FORMAT;
    d.width = (width + 1U) / 2U;
    d.height = (height + 1U) / 2U;
    d.mip_count = nph_gpu_mip_count(d.width, d.height, 1U);
    // A transfer source too: the render tests read the pyramid back (a
    // storage image is not framebuffer-compressed, so this costs nothing).
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED |
              (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    d.name = "vis hzb";
    NPH_TRY(nph_gpu_image_create(r->dev, &d, &v->hzb));
    v->hzb_width = d.width;
    v->hzb_height = d.height;
    v->hzb_mips = d.mip_count;
    v->target_width = width;
    v->target_height = height;
    v->hzb_tiles_x = (width + (u32)NPH_VIS_HZB_TILE - 1U) / (u32)NPH_VIS_HZB_TILE;
    v->hzb_tiles_y = (height + (u32)NPH_VIS_HZB_TILE - 1U) / (u32)NPH_VIS_HZB_TILE;
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = ((u64)v->hzb_tiles_x * v->hzb_tiles_y + 31U) / 32U * sizeof(u32);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "vis hzb tiles";
    return nph_gpu_buffer_create(r->dev, &bd, &v->hzb_dirty);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

// The counters of the frame that last completed in this slot.
static void read_stats(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    const u32 slot = r->frame_slot;
    if (v->stats_frame[slot] == 0U) {
        return;
    }
    const NphVisCounters *c = (const NphVisCounters *)nph_gpu_buffer_map(r->dev, v->stats[slot]);
    NphRenderFrameStats *s = &v->stats_last;
    s->vis_frame = v->stats_frame[slot] - 1U;
    s->vis_clusters = nph_min_u32(c->clusters, r->desc.max_visible_clusters);
    s->vis_clusters_sw =
        c->raster_count[0][NPH_VIS_RASTER_SOFTWARE] + c->raster_count[1][NPH_VIS_RASTER_SOFTWARE];
    s->vis_triangles = c->triangles;
    s->vis_instances_culled_frustum = c->instances_culled_frustum;
    s->vis_instances_culled_occlusion = c->instances_culled_occlusion;
    s->vis_meshlets_culled_frustum = c->meshlets_culled_frustum;
    s->vis_meshlets_culled_cone = c->meshlets_culled_cone;
    s->vis_meshlets_culled_occlusion = c->meshlets_culled_occlusion;
    s->vis_lights = c->lights_global + c->lights_local;
    s->vis_overflow = c->overflow;
    const u32 fresh = c->overflow & ~v->overflow_logged;
    if (fresh != 0U) {
        NPH_LOG_WARN(&NPH_LOG_RENDER,
                     "%s: GPU culling dropped work (frame %llu):%s%s%s; raise the capacities in "
                     "NphRendererDesc (said once per list)",
                     r->name, (unsigned long long)s->vis_frame,
                     (fresh & (u32)NPH_VIS_OVERFLOW_MESHLETS) != 0U ? " max_cull_meshlets" : "",
                     (fresh & (u32)NPH_VIS_OVERFLOW_CLUSTERS) != 0U ? " max_visible_clusters" : "",
                     (fresh & (u32)NPH_VIS_OVERFLOW_DEFERRED) != 0U ? " max_deferred" : "");
        v->overflow_logged |= fresh;
    }
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

static void split_vec(NphDVec3 p, NphVec4 *high, NphVec4 *low)
{
    const f32 hx = (f32)p.x;
    const f32 hy = (f32)p.y;
    const f32 hz = (f32)p.z;
    *high = nph_vec4(hx, hy, hz, 0.0F);
    *low = nph_vec4((f32)(p.x - (f64)hx), (f32)(p.y - (f64)hy), (f32)(p.z - (f64)hz), 0.0F);
}

static u64 region(NphGpuDevice *dev, NphGpuBuffer b, u64 offset)
{
    return nph_gpu_buffer_is_null(b) ? 0U : nph_gpu_buffer_address(dev, b) + offset;
}

// The frame's projection without its jitter (render_taa_frame_begin): the
// motion vectors' frame of reference.
static NphMat4 unjittered(const NphRenderer *r, const NphMat4 *proj, u32 width, u32 height)
{
    NphMat4 p = *proj;
    p.c[2].x += 2.0F * r->taa.frame_jitter.x / (f32)width;
    p.c[2].y += 2.0F * r->taa.frame_jitter.y / (f32)height;
    return p;
}

static void write_view(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                       const NphMat4 *view_m, const NphMat4 *proj, u32 flags)
{
    RenderVis *v = &r->vis;
    NphGpuDevice *dev = r->dev;
    const NphRendererDesc *d = &r->desc;
    const u64 frame = r->frame_addresses[r->frame_slot];
    NphVisViewData *vd = render_frame_at(r, r->layout.vis_view_offset);
    memset(vd, 0, sizeof *vd);
    vd->view = *view_m;
    vd->view_proj = nph_mat4_mul(*proj, *view_m);
    vd->prev_view = v->prev_view;
    vd->prev_view_proj = v->prev_view_proj;
    const NphFrustum f = nph_frustum_from_matrix(&vd->view_proj);
    for (u32 i = 0; i < 5U; ++i) {
        vd->frustum[i] = nph_vec4_from_vec3(f.planes[i].normal, f.planes[i].d);
    }
    split_vec(view->camera.position, &vd->camera_high, &vd->camera_low);
    split_vec(v->prev_camera, &vd->prev_camera_high, &vd->prev_camera_low);
    vd->viewport = nph_vec4((f32)width, (f32)height, 1.0F / (f32)width, 1.0F / (f32)height);
    vd->projection = nph_vec4(proj->c[0].x, proj->c[1].y, view->camera.znear, 0.0F);
    vd->prev_projection = v->prev_projection;
    vd->hzb = nph_vec4((f32)v->hzb_width, (f32)v->hzb_height, (f32)v->hzb_mips, 0.0F);
    vd->jitter = nph_vec4(r->taa.frame_jitter.x / (f32)width, r->taa.frame_jitter.y / (f32)height,
                          v->prev_jitter.x, v->prev_jitter.y);
    vd->render_view = frame + r->layout.view_offset;
    vd->instance_updates = frame + r->layout.vis_updates_offset;
    vd->instances = nph_gpu_buffer_address(dev, v->instances);
    vd->prev_transforms = nph_gpu_buffer_address(dev, v->prev_transforms);
    vd->meshes = nph_gpu_buffer_address(dev, v->meshes);
    vd->material_map = nph_gpu_buffer_address(dev, v->material_map);
    vd->materials = frame + r->layout.materials_offset;
    vd->counters = nph_gpu_buffer_address(dev, v->counters);
    vd->args = nph_gpu_buffer_address(dev, v->args);
    vd->cull_meshlets = nph_gpu_buffer_address(dev, v->cull_meshlets);
    // Deferred meshlets (uvec2) first, so both lists stay aligned.
    vd->deferred_meshlets = region(dev, v->deferred, 0U);
    vd->deferred_instances = region(dev, v->deferred, (u64)d->max_deferred * 8U);
    vd->clusters = nph_gpu_buffer_address(dev, v->clusters);
    const u64 list = (u64)d->max_visible_clusters * sizeof(u32);
    const u64 draws = (u64)d->max_visible_clusters * sizeof(NphVisDraw);
    for (u32 p = 0; p < 2U; ++p) {
        for (u32 k = 0; k < (u32)NPH_VIS_RASTER_KINDS; ++k) {
            vd->raster_lists[p][k] =
                region(dev, v->lists, list * (p * (u32)NPH_VIS_RASTER_KINDS + k));
        }
        for (u32 k = 0; k < 2U; ++k) {
            vd->draws[p][k] = region(dev, v->draws, draws * (p * 2U + k));
        }
    }
    vd->instance_count = r->instance_pool.bump;
    vd->max_cull_meshlets = d->max_cull_meshlets;
    vd->max_deferred = d->max_deferred;
    vd->max_clusters = d->max_visible_clusters;
    vd->hzb_index = nph_gpu_image_sampled_index(dev, v->hzb);
    vd->material_index_mask = r->material_pool.index_mask;
    vd->default_material = d->max_materials;
    vd->flags = flags;
    const NphGpuLimits *lim = &nph_gpu_device_info(dev)->limits;
    vd->max_mesh_groups_x =
        nph_max_u32(nph_min_u32(lim->max_mesh_workgroup_count[0], 1U << 22U), 1U);
    vd->sw_raster_pixels = d->sw_raster_pixels;
    vd->hzb_dirty = nph_gpu_buffer_address(dev, v->hzb_dirty);
    vd->hzb_tiles_x = v->hzb_tiles_x;

    // The lights: the table, this frame's updates, and the grid, whose
    // record the shading finds through the render view.
    const VisLightLayout *ll = &v->light_layout;
    const u64 work = nph_gpu_buffer_address(dev, v->light_grid);
    vd->lights = nph_gpu_buffer_address(dev, v->lights);
    vd->light_updates = frame + r->layout.vis_light_updates_offset;
    vd->light_grid = frame + r->layout.light_grid_offset;
    vd->light_histogram = work + ll->histogram;
    vd->light_visible = work + ll->visible;
    vd->light_staged = work + ll->staged;
    vd->light_rects = work + ll->rects;
    vd->light_slots = r->light_pool.bump;
    vd->light_tiles_y = v->light_tiles_y;
    NphRenderLightGrid *g = render_frame_at(r, r->layout.light_grid_offset);
    memset(g, 0, sizeof *g);
    g->globals = work + ll->globals;
    g->locals = work + ll->locals;
    g->zbins = work + ll->zbins;
    g->tiles = nph_gpu_buffer_address(dev, v->light_tiles);
    g->counts = vd->counters + offsetof(NphVisCounters, lights_global);
    g->znear = view->camera.znear;
    g->zbin_scale = (f32)NPH_RENDER_ZBINS / (f32)NPH_RENDER_ZBIN_OCTAVES;
    g->tiles_x = v->light_tiles_x;
    g->tile_count = v->light_tiles_x * v->light_tiles_y;
    NphRenderViewData *rv = render_frame_at(r, r->layout.view_offset);
    rv->light_grid = vd->light_grid;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

static NphVisPush push_of(const NphRenderer *r, u32 phase, u32 mode)
{
    NphVisPush p;
    memset(&p, 0, sizeof p);
    p.view = r->vis.frame.view_address;
    p.phase = phase;
    p.mode = mode;
    return p;
}

// One invocation per item, in groups of 64, the count spread over x and y.
static void dispatch_items(NphGpuCmd *cmd, u32 items)
{
    const u32 groups = (items + 63U) / 64U;
    const u32 x = nph_max_u32(nph_min_u32(groups, 65535U), 1U);
    nph_gpu_cmd_dispatch(cmd, x, (groups + x - 1U) / x, 1U);
}

static void clear_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const VisFrame *f = &r->vis.frame;
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->counters), 0U, NPH_GPU_WHOLE_SIZE, 0U);
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->light_grid),
                            r->vis.light_layout.histogram, (u64)NPH_RENDER_ZBINS * sizeof(u32), 0U);
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->hzb_dirty), 0U, NPH_GPU_WHOLE_SIZE,
                            r->vis.hzb_full_rebuild ? NPH_U32_MAX : 0U);
    const NphGpuClearValue zero = nph_gpu_clear_color(0.0F, 0.0F, 0.0F, 0.0F);
    nph_gpu_cmd_clear_color_image(cmd, nph_gpu_rg_image(graph, f->visbuffer),
                                  NPH_GPU_LAYOUT_TRANSFER_DST, &zero, 0U, 1U, 0U, 1U);
}

static void scatter_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    NphVisPush p = push_of(r, 0U, 0U);
    p.arg0 = r->vis.frame.update_count;
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_SCATTER]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    dispatch_items(cmd, r->vis.frame.update_count);
}

static void light_updates_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    NphVisPush p = push_of(r, 0U, NPH_VIS_LIGHTS_UPDATE);
    p.arg0 = r->vis.frame.light_update_count;
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_LIGHTS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    dispatch_items(cmd, r->vis.frame.light_update_count);
}

static void lights_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, NPH_VIS_LIGHTS_CULL);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_LIGHTS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    dispatch_items(cmd, nph_max_u32(r->light_pool.bump, 1U));
}

static void light_bins_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_LIGHT_BINS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
}

static void light_tiles_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_LIGHT_TILES]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  (u64)offsetof(NphVisArgs, light_tile_args));
}

static void cull_instances0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_INSTANCES]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    dispatch_items(cmd, nph_max_u32(r->instance_pool.bump, 1U));
}

static void args_record(NphRenderer *r, NphGpuCmd *cmd, u32 mode)
{
    const NphVisPush p = push_of(r, 0U, mode);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_ARGS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
}

#define ARGS_EXECUTE(name, mode)                                                                   \
    static void name(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)                         \
    {                                                                                              \
        NPH_UNUSED(graph);                                                                         \
        args_record((NphRenderer *)user, cmd, (mode));                                             \
    }

ARGS_EXECUTE(args_phase1_execute, NPH_VIS_ARGS_PHASE1)
ARGS_EXECUTE(args_raster1_execute, NPH_VIS_ARGS_RASTER1)
ARGS_EXECUTE(args_phase2_execute, NPH_VIS_ARGS_PHASE2)
ARGS_EXECUTE(args_phase2_mesh_execute, NPH_VIS_ARGS_PHASE2_MESH)
ARGS_EXECUTE(args_raster2_execute, NPH_VIS_ARGS_RASTER2)

#define ARGS_OFFSET(field) ((u64)offsetof(NphVisArgs, field))

static void cull_meshlets0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_MESHLETS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(cull_args));
}

static void cull_instances1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 1U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_INSTANCES]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(instance2_args));
}

static void cull_meshlets1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 1U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_MESHLETS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(cull_args) + 16U);
}

static void raster_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    RenderVis *v = &r->vis;
    const VisFrame *f = &v->frame;
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.width = f->width;
    rd.height = f->height;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    NphVisPush p = push_of(r, phase, 0U);
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    if (!v->mesh_shaders) {
        nph_gpu_cmd_bind_index_buffer(cmd, nph_gpu_rg_buffer(graph, f->index_pool), 0U,
                                      NPH_GPU_INDEX_U16);
    }
    // The hardware kinds: opaque, then masked.
    for (u32 k = 0; k < 2U; ++k) {
        nph_gpu_cmd_bind_pipeline(cmd, v->raster[k]);
        p.mode = k;
        nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
        const u64 kind = (u64)phase * (u64)NPH_VIS_RASTER_KINDS + k;
        if (v->mesh_shaders) {
            nph_gpu_cmd_draw_mesh_tasks_indirect(cmd, nph_gpu_rg_buffer(graph, f->args),
                                                 ARGS_OFFSET(raster_args) + kind * 16U, 1U, 16U);
        } else {
            const u64 list = (u64)r->desc.max_visible_clusters * sizeof(NphVisDraw);
            nph_gpu_cmd_draw_indexed_indirect_count(
                cmd, nph_gpu_rg_buffer(graph, f->draws), list * ((u64)phase * 2U + k),
                nph_gpu_rg_buffer(graph, f->counters),
                (u64)offsetof(NphVisCounters, raster_count) + kind * 4U,
                r->desc.max_visible_clusters, (u32)sizeof(NphVisDraw));
        }
    }
    nph_gpu_cmd_end_rendering(cmd);
}

// The software raster of a phase: a workgroup per small cluster.
static void sw_raster_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    const VisFrame *f = &r->vis.frame;
    NphVisPush p = push_of(r, phase, 0U);
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_SW_RASTER]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(
        cmd, nph_gpu_rg_buffer(graph, f->args),
        ARGS_OFFSET(raster_args) +
            ((u64)phase * (u64)NPH_VIS_RASTER_KINDS + (u64)NPH_VIS_RASTER_SOFTWARE) * 16U);
}

static void sw_raster0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    sw_raster_record((NphRenderer *)user, graph, cmd, 0U);
}

static void sw_raster1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    sw_raster_record((NphRenderer *)user, graph, cmd, 1U);
}

static void raster0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    raster_record((NphRenderer *)user, graph, cmd, 0U);
}

static void raster1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    raster_record((NphRenderer *)user, graph, cmd, 1U);
}

// The pyramid, one dispatch per mip, each reading the one before (the
// first, the visibility buffer's depth). After phase 1, mip 0 is rebuilt
// only in the tiles phase 1 drew into (vis_hzb.comp mode 2): elsewhere the
// pyramid after phase 0 already holds the frame's depth.
static void hzb_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    RenderVis *v = &r->vis;
    NphGpuDevice *dev = r->dev;
    const NphGpuImage hzb = nph_gpu_rg_image(graph, v->frame.hzb);
    nph_gpu_cmd_bind_pipeline(cmd, v->compute[VIS_COMPUTE_HZB]);
    u32 src_w = v->frame.width;
    u32 src_h = v->frame.height;
    for (u32 mip = 0; mip < v->hzb_mips; ++mip) {
        const u32 mode = mip != 0U ? 1U : (phase == 0U ? 0U : 2U);
        NphVisPush p = push_of(r, phase, mode);
        p.arg0 =
            mip == 0U
                ? nph_gpu_image_storage_index(dev, nph_gpu_rg_image(graph, v->frame.visbuffer), 0U)
                : nph_gpu_image_storage_index(dev, hzb, mip - 1U);
        p.arg1 = nph_gpu_image_storage_index(dev, hzb, mip);
        p.arg2 = src_w;
        p.arg3 = src_h;
        nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
        // Mip 0 is half the target rounded up; later mips are halved and
        // rounded down, as Vulkan sizes them (vis_hzb.comp).
        const u32 w = mip == 0U ? (src_w + 1U) / 2U : nph_max_u32(src_w / 2U, 1U);
        const u32 h = mip == 0U ? (src_h + 1U) / 2U : nph_max_u32(src_h / 2U, 1U);
        if (mode == 2U) {
            // A workgroup per tile: 16 x 16 mip-0 texels.
            nph_gpu_cmd_dispatch(cmd, v->hzb_tiles_x, v->hzb_tiles_y, 1U);
        } else {
            nph_gpu_cmd_dispatch(cmd, (w + 15U) / 16U, (h + 15U) / 16U, 1U);
        }
        if (mip + 1U < v->hzb_mips) {
            // The next mip reads this one (the image stays in GENERAL).
            const NphGpuMemoryBarrier m = {
                (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_READ};
            NphGpuBarrierDesc bd;
            memset(&bd, 0, sizeof bd);
            bd.memory = &m;
            bd.memory_count = 1U;
            nph_gpu_cmd_barrier(cmd, &bd);
        }
        src_w = w;
        src_h = h;
    }
}

static void hzb0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    hzb_record((NphRenderer *)user, graph, cmd, 0U);
}

static void hzb1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    hzb_record((NphRenderer *)user, graph, cmd, 1U);
}

static void resolve_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const VisFrame *f = &r->vis.frame;
    NphVisPush p = push_of(r, 0U, 0U);
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->hdr), 0U);
    p.arg1 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    static const VisCompute k_resolve[2][2] = {
        {VIS_COMPUTE_RESOLVE, VIS_COMPUTE_RESOLVE_VSM},
        {VIS_COMPUTE_RESOLVE_RT, VIS_COMPUTE_RESOLVE_RT_VSM}};
    nph_gpu_cmd_bind_pipeline(
        cmd, r->vis.compute[k_resolve[r->rt.frame_active ? 1 : 0][r->vsm.frame_active ? 1 : 0]]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, (f->width + 7U) / 8U, (f->height + 7U) / 8U, 1U);
}

static void depth_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    NphVisPush p = push_of(r, 0U, 0U);
    p.arg0 =
        nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, r->vis.frame.visbuffer), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.depth);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
}

static void stats_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphGpuBufferCopy region = {0U, 0U, sizeof(NphVisCounters)};
    nph_gpu_cmd_copy_buffer(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.counters),
                            nph_gpu_rg_buffer(graph, r->vis.frame.stats), &region, 1U);
}

u32 render_vis_visbuffer_index(const NphRenderer *r, NphGpuRenderGraph *graph)
{
    return nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, r->vis.frame.visbuffer), 0U);
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// The render-graph handles of the frame's persistent and transient
// resources.
typedef struct VisGraph {
    NphGpuRgBuffer instances;
    NphGpuRgBuffer prev_transforms;
    NphGpuRgBuffer meshes;
    NphGpuRgBuffer material_map;
    NphGpuRgBuffer cull_meshlets;
    NphGpuRgBuffer deferred;
    NphGpuRgBuffer clusters;
    NphGpuRgBuffer lists;
} VisGraph;

static NphGpuRgPass *add_pass(NphGpuRenderGraph *graph, NphRenderer *r, const char *name,
                              NphGpuRgPassType type, NphGpuRgExecuteFn execute)
{
    NphGpuRgPassDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.execute = execute;
    d.user = r;
    return nph_gpu_rg_add_pass(graph, &d);
}

static NphGpuRgPass *args_pass(NphGpuRenderGraph *graph, NphRenderer *r, const char *name,
                               NphGpuRgExecuteFn execute)
{
    NphGpuRgPass *p = add_pass(graph, r, name, NPH_GPU_RG_PASS_COMPUTE, execute);
    if (p != NULL) {
        nph_gpu_rg_read_buffer(p, r->vis.frame.counters, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_write_buffer(p, r->vis.frame.args, NPH_GPU_RG_USE_SHADER_WRITE);
    }
    return p;
}

// The scene and the hierarchical depth, as the culling passes read them.
static void read_scene(NphGpuRgPass *p, const VisGraph *g, NphGpuRgImage hzb)
{
    nph_gpu_rg_read_buffer(p, g->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->prev_transforms, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->meshes, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->material_map, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_image(p, hzb, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
}

static NphResult declare_raster(NphGpuRenderGraph *graph, NphRenderer *r, const VisGraph *g,
                                u32 phase)
{
    const VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p =
        add_pass(graph, r, phase == 0U ? "vis raster 0" : "vis raster 1", NPH_GPU_RG_PASS_GRAPHICS,
                 phase == 0U ? raster0_execute : raster1_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    if (r->vis.mesh_shaders) {
        nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
        nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    } else {
        nph_gpu_rg_read_buffer(p, f->draws, NPH_GPU_RG_USE_INDIRECT);
        nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_INDIRECT);
        nph_gpu_rg_read_buffer(p, f->index_pool, NPH_GPU_RG_USE_INDEX);
    }
    nph_gpu_rg_read_buffer(p, g->lists, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->clusters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->meshes, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    // The small clusters, in compute, into the same visibility buffer.
    p = add_pass(graph, r, phase == 0U ? "vis sw raster 0" : "vis sw raster 1",
                 NPH_GPU_RG_PASS_COMPUTE, phase == 0U ? sw_raster0_execute : sw_raster1_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->lists, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->clusters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->meshes, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    return NPH_RESULT_OK;
}

static NphResult declare_hzb(NphGpuRenderGraph *graph, NphRenderer *r, u32 phase)
{
    const VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p = add_pass(graph, r, phase == 0U ? "vis hzb 0" : "vis hzb 1",
                               NPH_GPU_RG_PASS_COMPUTE, phase == 0U ? hzb0_execute : hzb1_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    if (phase == 1U) {
        nph_gpu_rg_read_buffer(p, f->hzb_dirty, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    }
    // Mip 0 requests the virtual shadow map's pages (vis_hzb.comp).
    render_vsm_request_pages(r, p);
    nph_gpu_rg_write_image(p, f->hzb, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, NPH_GPU_ALL_MIPS);
    return NPH_RESULT_OK;
}

// The light grid: the frame's light updates, then the lights in view,
// their bins, and their tiles. Nothing here reads the visibility buffer.
static NphResult declare_lights(NphGpuRenderGraph *graph, NphRenderer *r)
{
    const VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p = NULL;
    if (f->light_update_count != 0U) {
        p = add_pass(graph, r, "vis light updates", NPH_GPU_RG_PASS_COMPUTE, light_updates_execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(p, f->lights, NPH_GPU_RG_USE_SHADER_WRITE);
    }
    p = add_pass(graph, r, "vis lights", NPH_GPU_RG_PASS_COMPUTE, lights_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, f->lights, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, f->light_grid, NPH_GPU_RG_USE_STORAGE_WRITE);
    p = add_pass(graph, r, "vis light bins", NPH_GPU_RG_PASS_COMPUTE, light_bins_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_buffer(p, f->light_grid, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_SHADER_WRITE);
    p = add_pass(graph, r, "vis light tiles", NPH_GPU_RG_PASS_COMPUTE, light_tiles_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->light_grid, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_buffer(p, f->light_tiles, NPH_GPU_RG_USE_SHADER_WRITE);
    return NPH_RESULT_OK;
}

// What the shading reads of the light grid (with the counters).
static void read_lights(NphGpuRgPass *p, const VisFrame *f)
{
    nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->light_grid, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->light_tiles, NPH_GPU_RG_USE_SHADER_READ);
}

// The shading: every pixel in one pass (docs/adr/0031-light-grid.md:
// binning the tiles of background alone into a pipeline of their own
// measured slower).
static NphResult declare_resolve(NphGpuRenderGraph *graph, NphRenderer *r, const VisGraph *g,
                                 NphGpuRgImage env, NphGpuRgImage specular, NphGpuRgImage lut,
                                 NphGpuRgBuffer sh)
{
    const VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p = add_pass(graph, r, "vis resolve", NPH_GPU_RG_PASS_COMPUTE, resolve_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    nph_gpu_rg_read_buffer(p, g->clusters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, g->meshes, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_image(p, env, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(p, specular, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(p, lut, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_buffer(p, sh, NPH_GPU_RG_USE_SHADER_READ);
    read_lights(p, f);
    if (r->rt.frame_active) {
        nph_gpu_rg_read_buffer(p, f->tlas, NPH_GPU_RG_USE_ACCEL_READ);
    }
    render_vsm_read(r, p);
    nph_gpu_rg_write_image(p, f->hdr, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    return NPH_RESULT_OK;
}

static NphResult declare_culling(NphGpuRenderGraph *graph, NphRenderer *r, const VisGraph *g)
{
    RenderVis *v = &r->vis;
    VisFrame *f = &v->frame;

    NphGpuRgPass *p = add_pass(graph, r, "vis clear", NPH_GPU_RG_PASS_TRANSFER, clear_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_TRANSFER_DST);
    nph_gpu_rg_write_buffer(p, f->hzb_dirty, NPH_GPU_RG_USE_TRANSFER_DST);
    nph_gpu_rg_write_buffer(p, f->light_grid, NPH_GPU_RG_USE_TRANSFER_DST);
    nph_gpu_rg_write_image(p, f->visbuffer, NPH_GPU_RG_USE_TRANSFER_DST, 0U, 1U);

    if (r->vsm.frame_active) {
        // The virtual shadow map's frame, and the pages this frame's instance
        // updates touch, read before the scatter replaces the records.
        NPH_TRY(render_vsm_declare_begin(r, graph, g->instances, g->meshes));
    }
    if (f->update_count != 0U) {
        p = add_pass(graph, r, "vis scatter", NPH_GPU_RG_PASS_COMPUTE, scatter_execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(p, g->instances, NPH_GPU_RG_USE_SHADER_WRITE);
        nph_gpu_rg_write_buffer(p, g->prev_transforms, NPH_GPU_RG_USE_SHADER_WRITE);
    }
    NPH_TRY(declare_lights(graph, r));

    // Phase 0.
    p = add_pass(graph, r, "vis cull instances 0", NPH_GPU_RG_PASS_COMPUTE,
                 cull_instances0_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    read_scene(p, g, f->hzb);
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, g->cull_meshlets, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_buffer(p, g->deferred, NPH_GPU_RG_USE_SHADER_WRITE);
    if (args_pass(graph, r, "vis args 0", args_phase1_execute) == NULL) {
        return NPH_RESULT_FULL;
    }
    p = add_pass(graph, r, "vis cull meshlets 0", NPH_GPU_RG_PASS_COMPUTE, cull_meshlets0_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    read_scene(p, g, f->hzb);
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, g->cull_meshlets, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, g->deferred, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_buffer(p, g->clusters, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_buffer(p, g->lists, NPH_GPU_RG_USE_SHADER_WRITE);
    if (!v->mesh_shaders) {
        nph_gpu_rg_write_buffer(p, f->draws, NPH_GPU_RG_USE_SHADER_WRITE);
    }
    if (args_pass(graph, r, "vis args raster 0", args_raster1_execute) == NULL) {
        return NPH_RESULT_FULL;
    }
    NPH_TRY(declare_raster(graph, r, g, 0U));
    NPH_TRY(declare_hzb(graph, r, 0U));

    // Phase 1: what phase 0 deferred, against this frame's depth.
    if (args_pass(graph, r, "vis args 1", args_phase2_execute) == NULL) {
        return NPH_RESULT_FULL;
    }
    p = add_pass(graph, r, "vis cull instances 1", NPH_GPU_RG_PASS_COMPUTE,
                 cull_instances1_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    read_scene(p, g, f->hzb);
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    // Reads the deferred instances, appends meshlets after the deferred ones.
    nph_gpu_rg_write_buffer(p, g->deferred, NPH_GPU_RG_USE_STORAGE_WRITE);
    if (args_pass(graph, r, "vis args 1 meshlets", args_phase2_mesh_execute) == NULL) {
        return NPH_RESULT_FULL;
    }
    p = add_pass(graph, r, "vis cull meshlets 1", NPH_GPU_RG_PASS_COMPUTE, cull_meshlets1_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    read_scene(p, g, f->hzb);
    nph_gpu_rg_write_buffer(p, f->hzb_dirty, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, g->deferred, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, g->clusters, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_buffer(p, g->lists, NPH_GPU_RG_USE_SHADER_WRITE);
    if (!v->mesh_shaders) {
        nph_gpu_rg_write_buffer(p, f->draws, NPH_GPU_RG_USE_SHADER_WRITE);
    }
    if (args_pass(graph, r, "vis args raster 1", args_raster2_execute) == NULL) {
        return NPH_RESULT_FULL;
    }
    NPH_TRY(declare_raster(graph, r, g, 1U));
    return declare_hzb(graph, r, 1U);
}

// The visibility buffer's depth into a depth attachment, for the blended
// surfaces' depth test.
static NphResult declare_depth(NphGpuRenderGraph *graph, NphRenderer *r)
{
    VisFrame *f = &r->vis.frame;
    NphGpuImageDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.format = RENDER_DEPTH_FORMAT;
    dd.width = f->width;
    dd.height = f->height;
    dd.mip_count = 1U;
    dd.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
    dd.name = "vis depth";
    f->depth = nph_gpu_rg_create_image(graph, &dd);
    if (nph_gpu_rg_image_is_null(f->depth)) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgPass *p = add_pass(graph, r, "vis depth", NPH_GPU_RG_PASS_GRAPHICS, depth_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_DONT_CARE; // every pixel is written
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_depth_attachment(p, f->depth, &a, 0);
    nph_gpu_rg_read_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    return NPH_RESULT_OK;
}

NphResult render_vis_frame(NphRenderer *r, const NphRenderView *view, NphGpuRenderGraph *graph,
                           u32 width, u32 height, const NphMat4 *view_m, const NphMat4 *proj,
                           NphGpuRgImage env, NphGpuRgImage specular, NphGpuRgImage lut,
                           NphGpuRgBuffer sh, NphGpuRgExecuteFn blend_requests,
                           NphGpuRgImage *out_hdr, NphRenderFrameStats *stats)
{
    RenderVis *v = &r->vis;
    VisFrame *f = &v->frame;
    NphGpuDevice *dev = r->dev;
    NPH_PROFILE_BEGIN("render.vis");
    render_vis_release_pending(r, nph_gpu_frame_index(dev));
    read_stats(r);
    render_vsm_read_stats(r);
    NphResult res = ensure_hzb(r, width, height);
    if (res != NPH_RESULT_OK) {
        NPH_PROFILE_END();
        return res;
    }
    const bool hzb_valid = v->hzb_valid != 0;
    // This frame's jitter in UV, and its projection without it.
    const NphVec2 jitter =
        nph_vec2(r->taa.frame_jitter.x / (f32)width, r->taa.frame_jitter.y / (f32)height);
    const NphMat4 plain = unjittered(r, proj, width, height);
    if (!hzb_valid) {
        // No previous frame: phase 0's view is this one (its HZB tests are
        // off anyway).
        v->prev_view = *view_m;
        v->prev_view_proj = nph_mat4_mul(plain, *view_m);
        v->prev_projection = nph_vec4(proj->c[0].x, proj->c[1].y, view->camera.znear, 0.0F);
        v->prev_camera = view->camera.position;
        v->prev_jitter = jitter;
    }
    u32 flags = 0U;
    if ((view->vis_flags & (u32)NPH_RENDER_VIS_NO_OCCLUSION) == 0U) {
        flags |= (u32)NPH_VIS_VIEW_OCCLUSION;
    }
    if (hzb_valid) {
        flags |= (u32)NPH_VIS_VIEW_HZB_VALID;
    }
    if (v->mesh_shaders) {
        flags |= (u32)NPH_VIS_VIEW_MESH_SHADERS;
    }
    if ((view->vis_flags & (u32)NPH_RENDER_VIS_NO_CONE) == 0U) {
        flags |= (u32)NPH_VIS_VIEW_CONE_CULLING;
    }
    if ((view->vis_flags & (u32)NPH_RENDER_VIS_NO_SW_RASTER) == 0U) {
        flags |= (u32)NPH_VIS_VIEW_SW_RASTER;
    }
    if ((view->vis_flags & (u32)NPH_RENDER_VIS_NO_LIGHT_GRID) == 0U) {
        flags |= (u32)NPH_VIS_VIEW_LIGHT_GRID;
    }
    write_view(r, view, width, height, view_m, proj, flags);
    memset(f, 0, sizeof *f);
    f->view_address = r->frame_addresses[r->frame_slot] + r->layout.vis_view_offset;
    f->update_count = render_vis_write_updates(r);
    f->light_update_count = render_vis_write_light_updates(r);
    f->width = width;
    f->height = height;

    // Resources.
    NphGpuImageDesc vd;
    memset(&vd, 0, sizeof vd);
    vd.format = RENDER_VIS_FORMAT;
    vd.width = width;
    vd.height = height;
    vd.mip_count = 1U;
    vd.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    // Sampled as two 32-bit words (the temporal resolve gathers the depths).
    vd.sampled_format = NPH_GPU_FORMAT_R32G32_UINT;
    vd.name = "vis visibility buffer";
    NphGpuImageDesc hd = vd;
    hd.format = v->hdr_format;
    hd.sampled_format = NPH_GPU_FORMAT_UNDEFINED;
    hd.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED |
               (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT;
    hd.name = "hdr colour";
    f->visbuffer = nph_gpu_rg_create_image(graph, &vd);
    f->hdr = nph_gpu_rg_create_image(graph, &hd);
    f->hzb = nph_gpu_rg_import_image(graph, v->hzb);
    f->hzb_dirty = nph_gpu_rg_import_buffer(graph, v->hzb_dirty);
    f->counters = nph_gpu_rg_import_buffer(graph, v->counters);
    f->args = nph_gpu_rg_import_buffer(graph, v->args);
    f->stats = nph_gpu_rg_import_buffer(graph, v->stats[r->frame_slot]);
    f->lights = nph_gpu_rg_import_buffer(graph, v->lights);
    f->light_grid = nph_gpu_rg_import_buffer(graph, v->light_grid);
    f->light_tiles = nph_gpu_rg_import_buffer(graph, v->light_tiles);
    if (!v->mesh_shaders) {
        f->draws = nph_gpu_rg_import_buffer(graph, v->draws);
        f->index_pool = nph_gpu_rg_import_buffer(graph, v->index_pool);
    }
    VisGraph g;
    g.instances = nph_gpu_rg_import_buffer(graph, v->instances);
    g.prev_transforms = nph_gpu_rg_import_buffer(graph, v->prev_transforms);
    g.meshes = nph_gpu_rg_import_buffer(graph, v->meshes);
    g.material_map = nph_gpu_rg_import_buffer(graph, v->material_map);
    g.cull_meshlets = nph_gpu_rg_import_buffer(graph, v->cull_meshlets);
    g.deferred = nph_gpu_rg_import_buffer(graph, v->deferred);
    g.clusters = nph_gpu_rg_import_buffer(graph, v->clusters);
    g.lists = nph_gpu_rg_import_buffer(graph, v->lists);
    f->instances = g.instances;
    f->prev_transforms = g.prev_transforms;
    f->clusters = g.clusters;
    if (nph_gpu_rg_image_is_null(f->visbuffer) || nph_gpu_rg_image_is_null(f->hdr) ||
        nph_gpu_rg_image_is_null(f->hzb) || nph_gpu_rg_buffer_is_null(g.lists) ||
        nph_gpu_rg_buffer_is_null(f->stats) || nph_gpu_rg_buffer_is_null(f->light_tiles)) {
        NPH_PROFILE_END();
        return NPH_RESULT_FULL;
    }

    res = declare_culling(graph, r, &g);
    if (res == NPH_RESULT_OK && r->vsm.frame_active && blend_requests != NULL) {
        // The blended surfaces' pages, against the opaque depth (which the
        // transparent pass then tests against too).
        res = declare_depth(graph, r);
        if (res == NPH_RESULT_OK) {
            res = render_vsm_declare_blend_requests(r, graph, f->depth, blend_requests);
        }
    }
    if (res == NPH_RESULT_OK && r->vsm.frame_active) {
        // The sun's virtual shadow map: its pages drawn where the depth
        // pyramid's passes asked for them.
        res = render_vsm_declare_pages(r, graph, g.instances, g.meshes);
    }
    if (res == NPH_RESULT_OK && r->rt.frame_active) {
        // Shadow rays: the instance records from the GPU scene, then the
        // builds, before the resolve traces.
        res = render_rt_declare(r, graph, g.instances, g.meshes, f->counters, &f->tlas);
    }
    if (res == NPH_RESULT_OK) {
        res = declare_resolve(graph, r, &g, env, specular, lut, sh);
    }
    NphGpuRgPass *p = NULL;
    if (res == NPH_RESULT_OK) {
        p = add_pass(graph, r, "vis statistics", NPH_GPU_RG_PASS_TRANSFER, stats_execute);
        res = p != NULL ? NPH_RESULT_OK : NPH_RESULT_FULL;
    }
    if (res == NPH_RESULT_OK) {
        nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_TRANSFER_SRC);
        nph_gpu_rg_write_buffer(p, f->stats, NPH_GPU_RG_USE_TRANSFER_DST);
        v->stats_frame[r->frame_slot] = nph_gpu_frame_index(dev) + 1U;
        // Next frame's phase 0 tests against this frame's HZB, from this
        // frame's view.
        v->prev_view = *view_m;
        v->prev_view_proj = nph_mat4_mul(plain, *view_m);
        v->prev_projection = nph_vec4(proj->c[0].x, proj->c[1].y, view->camera.znear, 0.0F);
        v->prev_camera = view->camera.position;
        v->prev_jitter = jitter;
        v->hzb_valid = 1;
        *out_hdr = f->hdr;
    }
    if (stats != NULL) {
        const NphRenderFrameStats *last = &v->stats_last;
        stats->vis_frame = last->vis_frame;
        stats->vis_clusters = last->vis_clusters;
        stats->vis_clusters_sw = last->vis_clusters_sw;
        stats->vis_triangles = last->vis_triangles;
        stats->vis_instances_culled_frustum = last->vis_instances_culled_frustum;
        stats->vis_instances_culled_occlusion = last->vis_instances_culled_occlusion;
        stats->vis_meshlets_culled_frustum = last->vis_meshlets_culled_frustum;
        stats->vis_meshlets_culled_cone = last->vis_meshlets_culled_cone;
        stats->vis_meshlets_culled_occlusion = last->vis_meshlets_culled_occlusion;
        stats->vis_lights = last->vis_lights;
        stats->vis_overflow = last->vis_overflow;
        stats->vsm_requested = last->vsm_requested;
        stats->vsm_drawn = last->vsm_drawn;
        stats->vsm_resident = last->vsm_resident;
        stats->vsm_evicted = last->vsm_evicted;
        stats->vsm_draws = last->vsm_draws;
        stats->vsm_overflow = last->vsm_overflow;
    }
    NPH_PROFILE_END();
    return res;
}

// The transparent surfaces of a production frame: the visibility buffer's
// depth into a depth attachment, then the reference's forward pipelines
// over the resolved HDR image. Only called when blended draws exist.
NphResult render_vis_declare_transparent(NphRenderer *r, NphGpuRenderGraph *graph,
                                         NphGpuRgExecuteFn draw, NphGpuRgImage env,
                                         NphGpuRgImage specular, NphGpuRgImage lut,
                                         NphGpuRgBuffer sh)
{
    VisFrame *f = &r->vis.frame;
    if (nph_gpu_rg_image_is_null(f->depth)) {
        NPH_TRY(declare_depth(graph, r));
    }
    NphGpuRgPass *p = add_pass(graph, r, "transparent", NPH_GPU_RG_PASS_GRAPHICS, draw);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_LOAD;
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_color_attachment(p, f->hdr, &a);
    a.store = NPH_GPU_STORE_OP_NONE;
    nph_gpu_rg_depth_attachment(p, f->depth, &a, 1);
    nph_gpu_rg_read_image(p, env, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(p, specular, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(p, lut, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_buffer(p, sh, NPH_GPU_RG_USE_SHADER_READ);
    read_lights(p, f);
    if (r->rt.frame_active) {
        nph_gpu_rg_read_buffer(p, f->tlas, NPH_GPU_RG_USE_ACCEL_READ);
    }
    render_vsm_read(r, p);
    return NPH_RESULT_OK;
}

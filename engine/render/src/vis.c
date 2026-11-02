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
//   vis cull groups 0     the groups of the levels each instance's distance may
//                         take (ADR-0070)
//   vis cull nodes 0      the hierarchies of those with too many (as many steps
//                         as the deepest of those needs; none without any)
//   vis args, vis cull meshlets 0, vis args, vis raster 0 (hardware), vis sw
//                         raster 0 (small clusters, in compute), vis voxel
//                         raster 0 (voxel clusters, ADR-0061)
//   vis hzb 0             this frame's depth so far
//   vis args, vis cull instances 1, vis cull groups 1 (the groups phase 0
//                         deferred, then those of instances now visible), vis
//                         cull nodes 1 (the hierarchies of instances now
//                         visible), vis args, vis cull meshlets 1 (the meshlets
//                         phase 0 deferred, then those of instances now
//                         visible), vis args, vis raster 1, vis sw raster 1, vis
//                         voxel raster 1
//   vis hzb 1             the frame's depth: next frame's phase 0 tests it
//   vsm ...               the sun's virtual shadow map (vsm.c), its page
//                         requests made by the vis hzb passes (and, with
//                         blended surfaces, by `vsm requests (blended)`
//                         first)
//   ao depth, ao, ao denoise
//                         the ambient occlusion (ao.c), from the depths the
//                         vis hzb passes took for it
//   rt ...                shadow rays' top level (rt.c)
//   vis resolve           shading into the HDR image, listing the tiles that
//                         hold voxel pixels
//   vis voxel resolve     those tiles' voxel pixels (an indirect dispatch)
//   transparent           blended surfaces, forward, testing the visibility
//                         buffer's depth themselves (only with blended
//                         surfaces)
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

NPH_STATIC_ASSERT(sizeof(NphVisViewData) == 1072U, "NphVisViewData matches its GLSL layout");
NPH_STATIC_ASSERT(offsetof(NphVisViewData, cell_runs) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(offsetof(NphVisViewData, cells) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(offsetof(NphVisViewData, voxel_tiles) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(offsetof(NphVisViewData, lod_queue) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(NPH_RENDER_ZBINS % 256 == 0, "vis_light_bins.comp gives each thread whole bins");
NPH_STATIC_ASSERT(NPH_RENDER_MAX_LIGHTS <= 65535, "light indices fit 16 bits");
NPH_STATIC_ASSERT(sizeof(NphVisPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(sizeof(NphVisResolvePush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(sizeof(NphVisHzbMipsPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
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
    "vis_raster_mesh_opaque_rop.frag",
    "vis_raster_mesh_masked_skin.frag",
    "vis_raster_opaque.vert",
    "vis_raster_masked.vert",
    "vis_raster_vert_opaque.frag",
    "vis_raster_vert_masked.frag",
    "vis_debug_line.frag",
    "vis_hzb_mips.comp",
    "vis_cull_nodes.comp",
    "vis_cull_groups.comp",
    "vis_voxel_raster.comp",
    "vis_voxel_resolve.comp",
    "vis_voxel_resolve_rt.comp",
    "vis_cull_cells.comp",
    "vis_page_requests.comp",
    "vis_sw_raster_masked.comp",
};

static const VisShader k_compute_shaders[VIS_COMPUTE_COUNT] = {
    VIS_SHADER_SCATTER,       VIS_SHADER_CULL_INSTANCES,
    VIS_SHADER_CULL_MESHLETS, VIS_SHADER_ARGS,
    VIS_SHADER_HZB,           VIS_SHADER_HZB_MIPS,
    VIS_SHADER_RESOLVE,       VIS_SHADER_SW_RASTER,
    VIS_SHADER_LIGHTS,        VIS_SHADER_LIGHT_BINS,
    VIS_SHADER_LIGHT_TILES,   VIS_SHADER_CULL_NODES,
    VIS_SHADER_CULL_GROUPS,   VIS_SHADER_VOXEL_RASTER,
    VIS_SHADER_CULL_CELLS,    VIS_SHADER_SW_RASTER_MASKED,
    VIS_SHADER_RESOLVE_RT,    VIS_SHADER_RESOLVE,
    VIS_SHADER_RESOLVE_RT,    VIS_SHADER_RESOLVE,
    VIS_SHADER_RESOLVE_RT,    VIS_SHADER_RESOLVE,
    VIS_SHADER_RESOLVE_RT,
};

static const char *const k_compute_names[VIS_COMPUTE_COUNT] = {
    "vis scatter",
    "vis cull instances",
    "vis cull meshlets",
    "vis args",
    "vis hzb",
    "vis hzb mips",
    "vis resolve",
    "vis sw raster",
    "vis lights",
    "vis light bins",
    "vis light tiles",
    "vis cull nodes",
    "vis cull groups",
    "vis voxel raster",
    "vis cull cells",
    "vis sw raster (masked)",
    "vis resolve (ray traced)",
    "vis resolve (virtual shadows)",
    "vis resolve (ray traced, virtual shadows)",
    "vis resolve (radiance cache)",
    "vis resolve (ray traced, radiance cache)",
    "vis resolve (virtual shadows, radiance cache)",
    "vis resolve (ray traced, virtual shadows, radiance cache)",
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
    // The fragment stage writes the visibility buffer through image atomics,
    // and faces are culled per triangle. The mesh path adds a depth
    // attachment that both fragment stages test early, so that fragments
    // behind what the pass drew skip the shader (equal depths pass, and the
    // atomic decides); the opaque one writes it, the masked one, which
    // discards, does not.
    d.cull_mode = NPH_GPU_CULL_NONE;
    if (v->mesh_shaders) {
        d.depth_format = RENDER_DEPTH_FORMAT;
        d.depth.test = 1;
        d.depth.write = fragment != VIS_SHADER_MESH_FRAG_MASKED ? 1 : 0;
        d.depth.compare = NPH_GPU_COMPARE_GREATER_OR_EQUAL;
    }
    if (fragment == VIS_SHADER_MESH_FRAG_OPAQUE_ROP) {
        // The visibility buffer's colour-attachment view (vis_raster_frag.glsl
        // VIS_ROP): the last fragment the depth test passes, the nearest.
        d.color_formats[0] = NPH_GPU_FORMAT_R32G32_UINT;
        d.color_count = 1U;
    }
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
    // And the radiance cache (gi.c), for the same reason.
    r->gi.supported = render_gi_device_supported(r) ? 1 : 0;
    const bool gi = r->gi.supported != 0;
    for (u32 i = 0; i < (u32)VIS_SHADER_COUNT; ++i) {
        const bool mesh_stage =
            i >= (u32)VIS_SHADER_MESH_OPAQUE && i <= (u32)VIS_SHADER_MESH_FRAG_MASKED_SKIN;
        const bool vertex_stage =
            i >= (u32)VIS_SHADER_VERT_OPAQUE && i <= (u32)VIS_SHADER_VERT_FRAG_MASKED;
        // Only the raster path in use: a mesh shader cannot even be loaded
        // on a device without the extension, nor the ray-traced resolve on
        // one without ray queries.
        const bool ray_queries =
            i == (u32)VIS_SHADER_RESOLVE_RT || i == (u32)VIS_SHADER_VOXEL_RESOLVE_RT;
        // The page requests only where textures are paged.
        const bool paging_only = i == (u32)VIS_SHADER_PAGE_REQUESTS;
        if ((mesh_stage && !v->mesh_shaders) || (vertex_stage && v->mesh_shaders) ||
            (ray_queries && !r->rt.supported) || (paging_only && !r->paging.supported)) {
            continue;
        }
        NPH_TRY(render_load_shader_file(r, k_shader_files[i], &v->shaders[i]));
    }
    for (u32 i = 0; i < (u32)VIS_COMPUTE_COUNT; ++i) {
        const bool resolve_rt =
            i == (u32)VIS_COMPUTE_RESOLVE_RT || i == (u32)VIS_COMPUTE_RESOLVE_RT_VSM ||
            i == (u32)VIS_COMPUTE_RESOLVE_RT_GI || i == (u32)VIS_COMPUTE_RESOLVE_RT_VSM_GI;
        const bool resolve_vsm =
            i == (u32)VIS_COMPUTE_RESOLVE_VSM || i == (u32)VIS_COMPUTE_RESOLVE_RT_VSM ||
            i == (u32)VIS_COMPUTE_RESOLVE_VSM_GI || i == (u32)VIS_COMPUTE_RESOLVE_RT_VSM_GI;
        const bool resolve_gi = i >= (u32)VIS_COMPUTE_RESOLVE_GI;
        if ((resolve_rt && !r->rt.supported) || (resolve_vsm && !vsm) || (resolve_gi && !gi)) {
            continue;
        }
        NphGpuComputePipelineDesc d;
        memset(&d, 0, sizeof d);
        d.shader = v->shaders[k_compute_shaders[i]];
        d.name = k_compute_names[i];
        if (i == (u32)VIS_COMPUTE_RESOLVE || resolve_rt || resolve_vsm || resolve_gi) {
            // Whether the resolve dithers to the HDR target's precision,
            // whether it reads the sun's virtual shadow map, and whether
            // the radiance cache (vis_resolve.glsl k_dither, k_vsm, k_gi).
            d.specialization[0] = v->hdr_format == RENDER_HDR_COMPACT ? 1U : 0U;
            d.specialization[1] = resolve_vsm ? 1U : 0U;
            d.specialization[2] = resolve_gi ? 1U : 0U;
            d.specialization_count = 5U;
        }
        NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &v->compute[i]));
        render_log_statistics(r, v->compute[i], k_compute_names[i]);
        if (i == (u32)VIS_COMPUTE_CULL_MESHLETS) {
            // With the voxel clusters' routing (vis_cull_meshlets.comp
            // k_voxels).
            d.specialization[0] = 1U;
            d.specialization_count = 1U;
            NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &v->cull_meshlets_voxels));
        }
        if (i == (u32)VIS_COMPUTE_RESOLVE || resolve_rt || resolve_vsm || resolve_gi) {
            // The same with the voxel test (k_voxels), and for the debug
            // views (k_debug), which always have it.
            const u32 rt_i = resolve_rt ? 1U : 0U;
            const u32 vsm_i = resolve_vsm ? 1U : 0U;
            const u32 gi_i = resolve_gi ? 1U : 0U;
            d.specialization[4] = 1U;
            NPH_TRY(
                nph_gpu_pipeline_create_compute(r->dev, &d, &v->resolve_voxels[rt_i][vsm_i][gi_i]));
            char name[96];
            (void)snprintf(name, sizeof name, "%s with the voxel test", k_compute_names[i]);
            render_log_statistics(r, v->resolve_voxels[rt_i][vsm_i][gi_i], name);
            d.specialization[3] = 1U;
            NPH_TRY(
                nph_gpu_pipeline_create_compute(r->dev, &d, &v->resolve_debug[rt_i][vsm_i][gi_i]));
            if (r->desc.max_many_lights != 0U) {
                // All three with the many lights' near fields (k_many).
                d.specialization[5] = 1U;
                d.specialization_count = 6U;
                d.specialization[3] = 1U;
                d.specialization[4] = 1U;
                NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d,
                                                        &v->resolve_many[2][rt_i][vsm_i][gi_i]));
                d.specialization[3] = 0U;
                NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d,
                                                        &v->resolve_many[1][rt_i][vsm_i][gi_i]));
                d.specialization[4] = 0U;
                NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d,
                                                        &v->resolve_many[0][rt_i][vsm_i][gi_i]));
                (void)snprintf(name, sizeof name, "%s with many lights", k_compute_names[i]);
                render_log_statistics(r, v->resolve_many[0][rt_i][vsm_i][gi_i], name);
            }
        }
    }
    // The voxel resolve's variants, specialised as the resolve's.
    for (u32 i = 0; i < 16U; ++i) {
        const u32 debug = i >> 3U;
        const u32 rt = (i >> 2U) & 1U;
        const u32 map = (i >> 1U) & 1U;
        const u32 gi_on = i & 1U;
        if ((rt != 0U && !r->rt.supported) || (map != 0U && !vsm) || (gi_on != 0U && !gi)) {
            continue;
        }
        NphGpuComputePipelineDesc d;
        memset(&d, 0, sizeof d);
        d.shader = v->shaders[rt != 0U ? VIS_SHADER_VOXEL_RESOLVE_RT : VIS_SHADER_VOXEL_RESOLVE];
        d.name = "vis voxel resolve";
        d.specialization[0] = v->hdr_format == RENDER_HDR_COMPACT ? 1U : 0U;
        d.specialization[1] = map;
        d.specialization[2] = gi_on;
        d.specialization[3] = debug;
        d.specialization_count = 4U;
        NPH_TRY(
            nph_gpu_pipeline_create_compute(r->dev, &d, &v->voxel_resolve[debug][rt][map][gi_on]));
        if (debug == 0U) {
            render_log_statistics(r, v->voxel_resolve[0][rt][map][gi_on], d.name);
        }
    }
    if (v->mesh_shaders) {
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_MESH_OPAQUE, VIS_SHADER_MESH_FRAG_OPAQUE,
                                       "vis raster (mesh, opaque)", &v->raster[0]));
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_MESH_MASKED, VIS_SHADER_MESH_FRAG_MASKED,
                                       "vis raster (mesh, masked)", &v->raster[1]));
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_MESH_OPAQUE, VIS_SHADER_MESH_FRAG_OPAQUE_ROP,
                                       "vis raster (mesh, opaque, ROP)", &v->raster_rop));
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_MESH_MASKED, VIS_SHADER_MESH_FRAG_MASKED_SKIN,
                                       "vis raster (mesh, masked skin)", &v->raster_skin));
    } else {
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_VERT_OPAQUE, VIS_SHADER_VERT_FRAG_OPAQUE,
                                       "vis raster (indirect, opaque)", &v->raster[0]));
        NPH_TRY(create_raster_pipeline(r, VIS_SHADER_VERT_MASKED, VIS_SHADER_VERT_FRAG_MASKED,
                                       "vis raster (indirect, masked)", &v->raster[1]));
    }
    // The blended surfaces, into the HDR format, testing the visibility
    // buffer's depth themselves: with and without shadow rays, and with and
    // without the virtual shadow map.
    for (u32 rt = 0; rt < 2U; ++rt) {
        for (u32 map = 0; map < 2U; ++map) {
            if ((rt != 0U && !r->rt.supported) || (map != 0U && !vsm)) {
                continue;
            }
            for (u32 c = 0; c < (u32)RENDER_CULL_COUNT; ++c) {
                NPH_TRY(render_forward_pipeline(r, (u32)NPH_RENDER_ALPHA_BLEND, c, v->hdr_format,
                                                rt != 0U, map != 0U, true,
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
    nph_gpu_buffer_destroy(dev, v->voxel_tiles);
    nph_gpu_buffer_destroy(dev, v->voxel_tile_bits);
    nph_gpu_buffer_destroy(dev, v->light_tiles);
    nph_gpu_buffer_destroy(dev, v->light_depths);
    for (u32 i = 0; i < (u32)VIS_COMPUTE_COUNT; ++i) {
        nph_gpu_pipeline_destroy(dev, v->compute[i]);
    }
    nph_gpu_pipeline_destroy(dev, v->cull_meshlets_voxels);
    for (u32 i = 0; i < 8U; ++i) {
        nph_gpu_pipeline_destroy(dev, v->resolve_debug[i >> 2U][(i >> 1U) & 1U][i & 1U]);
        nph_gpu_pipeline_destroy(dev, v->resolve_voxels[i >> 2U][(i >> 1U) & 1U][i & 1U]);
        for (u32 kind = 0; kind < 3U; ++kind) {
            nph_gpu_pipeline_destroy(dev, v->resolve_many[kind][i >> 2U][(i >> 1U) & 1U][i & 1U]);
        }
    }
    nph_gpu_pipeline_destroy(dev, v->page_requests);
    for (u32 i = 0; i < 16U; ++i) {
        nph_gpu_pipeline_destroy(dev,
                                 v->voxel_resolve[i >> 3U][(i >> 2U) & 1U][(i >> 1U) & 1U][i & 1U]);
    }
    nph_gpu_pipeline_destroy(dev, v->raster[0]);
    nph_gpu_pipeline_destroy(dev, v->raster[1]);
    nph_gpu_pipeline_destroy(dev, v->raster_rop);
    nph_gpu_pipeline_destroy(dev, v->raster_skin);
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
    NPH_TRY(render_taa_target_pipeline(r, t, format));
    return render_motion_target_pipeline(r, t, format);
}

// ---------------------------------------------------------------------------
// The hierarchical depth buffer
// ---------------------------------------------------------------------------

// Keeps what is sized by the target the size it needs: the light tiles'
// masks and depths, and the HZB, whose mip 0 is half the target, rounded up, with a
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
    nph_gpu_buffer_destroy(r->dev, v->voxel_tiles);
    nph_gpu_buffer_destroy(r->dev, v->voxel_tile_bits);
    nph_gpu_buffer_destroy(r->dev, v->light_tiles);
    nph_gpu_buffer_destroy(r->dev, v->light_depths);
    v->hzb = NPH_GPU_IMAGE_NULL;
    v->hzb_dirty = NPH_GPU_BUFFER_NULL;
    v->voxel_tiles = NPH_GPU_BUFFER_NULL;
    v->voxel_tile_bits = NPH_GPU_BUFFER_NULL;
    v->light_tiles = NPH_GPU_BUFFER_NULL;
    v->light_depths = NPH_GPU_BUFFER_NULL;
    v->hzb_valid = 0;
    v->prev_frame = 0U;
    v->light_tiles_x = (width + (u32)NPH_RENDER_LIGHT_TILE - 1U) / (u32)NPH_RENDER_LIGHT_TILE;
    v->light_tiles_y = (height + (u32)NPH_RENDER_LIGHT_TILE - 1U) / (u32)NPH_RENDER_LIGHT_TILE;
    NphGpuBufferDesc ld;
    memset(&ld, 0, sizeof ld);
    const u64 tiles = (u64)v->light_tiles_x * v->light_tiles_y;
    // The masks for opaque surfaces, then for blended ones.
    ld.size = 2U * tiles * v->light_words * sizeof(u32);
    // A transfer source too: the render tests read the grid back.
    ld.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    ld.memory = NPH_GPU_MEMORY_DEVICE;
    ld.name = "vis light tiles";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &ld, &v->light_tiles));
    ld.size = tiles * 2U * sizeof(u32);
    ld.name = "vis light depths";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &ld, &v->light_depths));
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
    NPH_ASSERT(v->hzb_mips <= (u32)NPH_VIS_HZB_MAX_MIPS); // vis_hzb_mips.comp's storage indices
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

// The resolve's tiles that hold voxel pixels (as many as it has tiles) and a
// bit each, made for the first frame that may draw voxel clusters at this
// size (ensure_hzb drops them with the size) and kept after.
static NphResult ensure_voxel_tiles(NphRenderer *r, u32 width, u32 height)
{
    RenderVis *v = &r->vis;
    if (!v->frame_voxels || !nph_gpu_buffer_is_null(v->voxel_tiles)) {
        return NPH_RESULT_OK;
    }
    nph_gpu_buffer_destroy(r->dev, v->voxel_tile_bits);
    v->voxel_tile_bits = NPH_GPU_BUFFER_NULL;
    v->resolve_tiles_x = (width + (u32)NPH_VIS_RESOLVE_TILE - 1U) / (u32)NPH_VIS_RESOLVE_TILE;
    v->resolve_tiles_y = (height + (u32)NPH_VIS_RESOLVE_TILE - 1U) / (u32)NPH_VIS_RESOLVE_TILE;
    const u64 resolve_tiles = (u64)v->resolve_tiles_x * v->resolve_tiles_y;
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = resolve_tiles * sizeof(u32);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "vis voxel tiles";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &v->voxel_tiles));
    bd.size = (resolve_tiles + 31U) / 32U * sizeof(u32);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.name = "vis voxel tile bits";
    return nph_gpu_buffer_create(r->dev, &bd, &v->voxel_tile_bits);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

// The cells' requests of the frame that last completed in this slot
// (ADR-0063 stage 3), as a level each (NPH_RENDER_CELL_NO_REQUEST: none).
static void read_requests(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    const u32 slot = r->frame_slot;
    if (v->request_frame[slot] == 0U) {
        return;
    }
    const u32 *levels = (const u32 *)nph_gpu_buffer_map(r->dev, v->request_readback[slot]);
    for (u32 c = 0; c < r->desc.max_cells; ++c) {
        v->cell_requested[c] =
            levels[c] < NPH_RENDER_CELL_MAX_LEVELS ? (u8)levels[c] : (u8)NPH_RENDER_CELL_NO_REQUEST;
    }
    v->requests_from = v->request_frame[slot];
}

u32 nph_render_cell_request(const NphRenderer *r, NphRenderCell cell)
{
    NPH_ASSERT(r != NULL);
    const u32 slot = render_slot_of(&r->cell_pool, cell.value);
    if (slot == NPH_U32_MAX || !r->vis.supported || r->vis.requests_from == 0U) {
        return NPH_RENDER_CELL_NO_REQUEST;
    }
    return r->vis.cell_requested[slot];
}

u64 nph_render_cell_requests_frame(const NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    return r->vis.supported && r->vis.requests_from != 0U ? r->vis.requests_from - 1U : NPH_U64_MAX;
}

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
    // The small masked clusters a phase, where they went to compute (vis_args.comp).
    for (u32 p = 0; p < 2U; ++p) {
        const u32 small = c->raster_count[p][NPH_VIS_RASTER_SOFTWARE_MASKED];
        s->vis_clusters_sw += !v->mesh_shaders || small >= r->desc.sw_masked_clusters ? small : 0U;
    }
    s->vis_triangles = c->triangles;
    s->vis_voxel_clusters =
        c->raster_count[0][NPH_VIS_RASTER_VOXEL] + c->raster_count[1][NPH_VIS_RASTER_VOXEL];
    s->vis_voxel_bricks = c->voxel_bricks;
    s->vis_instances_culled_frustum = c->instances_culled_frustum;
    s->vis_instances_culled_occlusion = c->instances_culled_occlusion;
    s->vis_meshlets_culled_frustum = c->meshlets_culled_frustum;
    s->vis_meshlets_culled_cone = c->meshlets_culled_cone;
    s->vis_meshlets_culled_occlusion = c->meshlets_culled_occlusion;
    s->vis_lod_groups = c->lod_groups_tested;
    s->vis_lod_nodes = c->lod_nodes_visited;
    s->vis_lod_clusters_culled = c->lod_clusters_culled;
    s->vis_lights = c->lights_global + c->lights_local;
    s->vis_overflow = c->overflow;
    s->vis_cells_proxied = c->cells_proxied;
    s->vis_instances_proxied = c->instances_proxied;
    const u32 fresh = c->overflow & ~v->overflow_logged;
    if (fresh != 0U) {
        NPH_LOG_WARN(&NPH_LOG_RENDER,
                     "%s: GPU culling dropped work (frame %llu):%s%s%s%s; raise the capacities "
                     "in NphRendererDesc (said once per list)",
                     r->name, (unsigned long long)s->vis_frame,
                     (fresh & (u32)NPH_VIS_OVERFLOW_MESHLETS) != 0U ? " max_cull_meshlets" : "",
                     (fresh & (u32)NPH_VIS_OVERFLOW_CLUSTERS) != 0U ? " max_visible_clusters" : "",
                     (fresh & (u32)NPH_VIS_OVERFLOW_DEFERRED) != 0U ? " max_deferred" : "",
                     (fresh & (u32)NPH_VIS_OVERFLOW_LOD_NODES) != 0U ? " max_lod_nodes" : "");
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
    const NphMat4 view_proj = nph_mat4_mul(*proj, *view_m);
    vd->view_proj = view_proj;
    vd->prev_view = v->prev_view;
    vd->prev_view_proj = v->prev_view_proj;
    // The camera's move since the previous frame, taken in doubles (the
    // GPU's high and low floats give the same), and the clip spaces joined
    // across it for still points (vis_previous_uv).
    NphMat4 inv = nph_mat4_identity();
    NPH_IGNORE_RESULT(nph_mat4_inverse(vd->view_proj, &inv));
    const NphVec3 moved = nph_dvec3_relative(view->camera.position, v->prev_camera);
    vd->reproject =
        nph_mat4_mul(v->prev_view_proj, nph_mat4_mul(nph_mat4_from_translation(moved), inv));
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
    vd->cells = nph_gpu_buffer_address(dev, v->cells);
    vd->cell_requests = nph_gpu_buffer_address(dev, v->cell_requests);
    // Cells first where the renderer holds any (ADR-0064).
    vd->cell_runs = nph_gpu_buffer_address(dev, v->cell_runs);
    vd->cell_members = nph_gpu_buffer_address(dev, v->cell_members);
    vd->cell_work = nph_gpu_buffer_address(dev, v->cell_work);
    vd->cell_listed = nph_gpu_buffer_address(dev, v->cell_listed);
    vd->cell_slots = v->cells_first_off ? 0U : v->cell_slots;
    vd->max_cell_work = d->max_instances;
    vd->loose_first = v->loose.capacity != 0U ? v->loose.range.offset : 0U;
    vd->loose_count = v->loose.count;
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
    vd->skin_fraction = d->masked_skin;
    vd->sw_masked_min = d->sw_masked_clusters;
    vd->hzb_dirty = nph_gpu_buffer_address(dev, v->hzb_dirty);
    vd->hzb_tiles_x = v->hzb_tiles_x;
    vd->voxel_tiles = nph_gpu_buffer_address(dev, v->voxel_tiles);
    vd->voxel_tile_bits = nph_gpu_buffer_address(dev, v->voxel_tile_bits);
    vd->resolve_tiles_x = v->resolve_tiles_x;
    // The levels of detail (vis_lod_group_too_coarse): the pixels a unit
    // spans at a distance of one, over the threshold. A threshold of 0
    // turns them off (NPH_VIS_VIEW_LOD unset) and leaves the largest float.
    for (u32 q = 0; q < (u32)NPH_VIS_LOD_QUEUES; ++q) {
        vd->lod_queue[q] = region(dev, v->lod_queue, (u64)q * d->max_lod_nodes * 8U);
    }
    const u64 flat_cull = (u64)NPH_VIS_LOD_QUEUES * d->max_lod_nodes * 8U;
    vd->lod_group_list[0] = region(dev, v->lod_queue, flat_cull);
    vd->lod_group_list[1] = region(dev, v->lod_queue, flat_cull + (u64)d->max_lod_groups * 8U);
    vd->lod_chunk_list = region(dev, v->lod_queue, flat_cull + 2ULL * d->max_lod_groups * 8U);
    vd->max_lod_nodes = d->max_lod_nodes;
    vd->max_lod_groups = d->max_lod_groups;
    vd->max_lod_chunks = RENDER_LOD_CHUNKS(d->max_lod_groups);
    vd->max_lod_flat_groups = d->max_lod_flat_groups;
    const f32 pixels_per_unit = nph_f32_abs(proj->c[1].y) * 0.5F * (f32)height;
    const f32 per_threshold =
        view->lod_error > 0.0F ? pixels_per_unit / view->lod_error : NPH_F32_MAX;
    vd->lod = nph_vec4(nph_f32_is_finite(per_threshold) ? per_threshold : NPH_F32_MAX,
                       view->camera.znear, 0.0F, 0.0F);

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
    vd->light_depths = nph_gpu_buffer_address(dev, v->light_depths);
    vd->light_slots = r->light_pool.bump;
    vd->light_tiles_y = v->light_tiles_y;
    NphRenderLightGrid *g = render_frame_at(r, r->layout.light_grid_offset);
    memset(g, 0, sizeof *g);
    g->globals = work + ll->globals;
    g->locals = work + ll->locals;
    g->zbins = work + ll->zbins;
    g->tiles = nph_gpu_buffer_address(dev, v->light_tiles);
    g->tiles_blended =
        g->tiles + (u64)v->light_tiles_x * v->light_tiles_y * v->light_words * (u64)sizeof(u32);
    g->counts = vd->counters + offsetof(NphVisCounters, lights_global);
    g->znear = view->camera.znear;
    g->zbin_scale = (f32)NPH_RENDER_ZBINS / (f32)NPH_RENDER_ZBIN_OCTAVES;
    g->tiles_x = v->light_tiles_x;
    g->tile_count = v->light_tiles_x * v->light_tiles_y;
    NphRenderViewData *rv = render_frame_at(r, r->layout.view_offset);
    rv->light_grid = vd->light_grid;

    // The resolve's push constants (NphVisResolvePush), from the same values.
    NphVisResolvePush *rp = &v->resolve_push;
    memset(rp, 0, sizeof *rp);
    rp->view_proj = view_proj;
    rp->view = frame + r->layout.vis_view_offset;
    rp->render_view = frame + r->layout.view_offset;
    rp->clusters = nph_gpu_buffer_address(dev, v->clusters);
    rp->instances = nph_gpu_buffer_address(dev, v->instances);
    rp->meshes = nph_gpu_buffer_address(dev, v->meshes);
    rp->inv_width = 1.0F / (f32)width;
    rp->inv_height = 1.0F / (f32)height;
    rp->size = width | (height << 16U);
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
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->cell_requests), 0U, NPH_GPU_WHOLE_SIZE,
                            NPH_VIS_CELL_NO_REQUEST);
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->light_grid),
                            r->vis.light_layout.histogram, (u64)NPH_RENDER_ZBINS * sizeof(u32), 0U);
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->hzb_dirty), 0U, NPH_GPU_WHOLE_SIZE,
                            r->vis.hzb_full_rebuild ? NPH_U32_MAX : 0U);
    // The hierarchy steps' arguments and the flat cull's: no workgroups until
    // whatever fills their lists raises them, and one in y and z.
    const u32 no_steps[NPH_VIS_LOD_QUEUES][4] = {
        {0U, 1U, 1U, 0U}, {0U, 1U, 1U, 0U}, {0U, 1U, 1U, 0U}};
    const u32 no_groups[4][4] = {
        {0U, 1U, 1U, 0U}, {0U, 1U, 1U, 0U}, {0U, 1U, 1U, 0U}, {0U, 1U, 1U, 0U}};
    NPH_STATIC_ASSERT(sizeof no_steps == sizeof(((NphVisArgs *)NULL)->lod_node_args),
                      "a set of arguments a queue");
    NPH_STATIC_ASSERT(sizeof no_groups == sizeof(((NphVisArgs *)NULL)->lod_group_args) +
                                              sizeof(((NphVisArgs *)NULL)->lod_chunk_args),
                      "a set of arguments a phase, of each list");
    NPH_STATIC_ASSERT(offsetof(NphVisArgs, lod_chunk_args) ==
                          offsetof(NphVisArgs, lod_group_args) +
                              sizeof(((NphVisArgs *)NULL)->lod_group_args),
                      "the flat cull's arguments follow one another");
    nph_gpu_cmd_update_buffer(cmd, nph_gpu_rg_buffer(graph, f->args),
                              (u64)offsetof(NphVisArgs, lod_node_args), no_steps, sizeof no_steps);
    nph_gpu_cmd_update_buffer(cmd, nph_gpu_rg_buffer(graph, f->args),
                              (u64)offsetof(NphVisArgs, lod_group_args), no_groups,
                              sizeof no_groups);
    // The voxel resolve's: no tile listed yet, and one in y and z.
    if (r->vis.frame_voxels) {
        nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->args),
                                (u64)offsetof(NphVisArgs, voxel_resolve_args), sizeof(u32), 0U);
        nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->args),
                                (u64)offsetof(NphVisArgs, voxel_resolve_args) + sizeof(u32),
                                2U * sizeof(u32), 1U);
        nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, f->voxel_tile_bits), 0U,
                                NPH_GPU_WHOLE_SIZE, 0U);
    }
    if (!r->vis.mesh_shaders) {
        const NphGpuClearValue zero = nph_gpu_clear_color(0.0F, 0.0F, 0.0F, 0.0F);
        nph_gpu_cmd_clear_color_image(cmd, nph_gpu_rg_image(graph, f->visbuffer),
                                      NPH_GPU_LAYOUT_TRANSFER_DST, &zero, 0U, 1U, 0U, 1U);
    }
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

// The world partition's cells, first (vis_cull_cells.comp; ADR-0064): the
// cells to list, an invocation a cell slot.
static void cull_cells_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, (u32)NPH_VIS_CELLS_TEST);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_CELLS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    dispatch_items(cmd, nph_max_u32(r->vis.cell_slots, 1U));
}

// Their runs into the work list, a workgroup a listed cell.
static void expand_cells_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, (u32)NPH_VIS_CELLS_EXPAND);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_CELLS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  (u64)offsetof(NphVisArgs, cell_expand_args));
}

// Phase 0: the instances the cell cull listed and those of no cell, or,
// without cells, every instance slot.
static void cull_instances0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_INSTANCES]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    if (r->vis.frame.cells_first) {
        nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                      (u64)offsetof(NphVisArgs, instance0_args));
    } else {
        dispatch_items(cmd, nph_max_u32(r->instance_pool.bump, 1U));
    }
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

ARGS_EXECUTE(args_cells_execute, NPH_VIS_ARGS_CELLS)
ARGS_EXECUTE(args_phase1_execute, NPH_VIS_ARGS_PHASE1)
ARGS_EXECUTE(args_raster1_execute, NPH_VIS_ARGS_RASTER1)
ARGS_EXECUTE(args_phase2_execute, NPH_VIS_ARGS_PHASE2)
ARGS_EXECUTE(args_phase2_mesh_execute, NPH_VIS_ARGS_PHASE2_MESH)
ARGS_EXECUTE(args_raster2_execute, NPH_VIS_ARGS_RASTER2)

#define ARGS_OFFSET(field) ((u64)offsetof(NphVisArgs, field))

// A step of the hierarchies (vis_cull_nodes.comp) in a phase: from a queue
// into the next, emptying the one after, dispatched with the workgroups
// whatever filled the queue raised its arguments to.
static void cull_nodes_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase,
                              u32 queue)
{
    NphVisPush p = push_of(r, phase, 0U);
    p.arg0 = queue;
    p.arg1 = (queue + 1U) % (u32)NPH_VIS_LOD_QUEUES;
    p.arg2 = (queue + 2U) % (u32)NPH_VIS_LOD_QUEUES;
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_NODES]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(lod_node_args) + (u64)queue * 16U);
}

#define CULL_NODES_EXECUTE(name, phase, queue)                                                     \
    static void name(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)                         \
    {                                                                                              \
        cull_nodes_record((NphRenderer *)user, graph, cmd, (phase), (queue));                      \
    }

CULL_NODES_EXECUTE(cull_nodes00_execute, 0U, 0U)
CULL_NODES_EXECUTE(cull_nodes01_execute, 0U, 1U)
CULL_NODES_EXECUTE(cull_nodes02_execute, 0U, 2U)
CULL_NODES_EXECUTE(cull_nodes10_execute, 1U, 0U)
CULL_NODES_EXECUTE(cull_nodes11_execute, 1U, 1U)
CULL_NODES_EXECUTE(cull_nodes12_execute, 1U, 2U)

// The flat cull of the groups of the levels each instance's distance may take,
// in a phase: its work items, then the chunks of its long runs. Nothing between
// them: neither reads what the other writes.
static void cull_groups_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    const u64 args = 16U * (u64)phase;
    NphVisPush p = push_of(r, phase, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_CULL_GROUPS]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(lod_group_args) + args);
    p.arg0 = 1U;
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(lod_chunk_args) + args);
}

static void cull_groups0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    cull_groups_record((NphRenderer *)user, graph, cmd, 0U);
}

static void cull_groups1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    cull_groups_record((NphRenderer *)user, graph, cmd, 1U);
}

// The meshlet cull, with the voxel clusters' routing where they may appear.
static NphGpuPipeline cull_meshlets_pipeline(const NphRenderer *r)
{
    return r->vis.frame_voxels ? r->vis.cull_meshlets_voxels
                               : r->vis.compute[VIS_COMPUTE_CULL_MESHLETS];
}

static void cull_meshlets0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphVisPush p = push_of(r, 0U, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, cull_meshlets_pipeline(r));
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
    nph_gpu_cmd_bind_pipeline(cmd, cull_meshlets_pipeline(r));
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.args),
                                  ARGS_OFFSET(cull_args) + 16U);
}

static void raster_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase,
                          u32 kinds, bool rop)
{
    RenderVis *v = &r->vis;
    const VisFrame *f = &v->frame;
    // The mesh path's pass has a depth attachment, and the graph begins
    // rendering around it; the vertex path has none.
    if (!v->mesh_shaders) {
        NphGpuRenderingDesc rd;
        memset(&rd, 0, sizeof rd);
        rd.width = f->width;
        rd.height = f->height;
        nph_gpu_cmd_begin_rendering(cmd, &rd);
    }
    NphVisPush p = push_of(r, phase, 0U);
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    if (!v->mesh_shaders) {
        nph_gpu_cmd_bind_index_buffer(cmd, nph_gpu_rg_buffer(graph, f->index_pool), 0U,
                                      NPH_GPU_INDEX_U16);
    }
    // The hardware kinds: opaque, then the masked skin (writing depth), then
    // the other masked clusters, and the small ones where they are too few for
    // their dispatch (vis_args.comp, whose arguments draw none otherwise).
    static const u32 k_order[4] = {NPH_VIS_RASTER_OPAQUE, NPH_VIS_RASTER_SKIN,
                                   NPH_VIS_RASTER_MASKED, NPH_VIS_RASTER_SOFTWARE_MASKED};
    for (u32 o = 0; o < 4U; ++o) {
        const u32 k = k_order[o];
        if ((kinds & (1U << k)) == 0U) {
            continue;
        }
        const bool small = k == NPH_VIS_RASTER_SOFTWARE_MASKED;
        nph_gpu_cmd_bind_pipeline(cmd, k == NPH_VIS_RASTER_SKIN ? v->raster_skin
                                       : small                  ? v->raster[1]
                                       : rop && k == 0U         ? v->raster_rop
                                                                : v->raster[k]);
        p.mode = k;
        nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
        const u64 kind = (u64)phase * (u64)NPH_VIS_RASTER_KINDS + k;
        if (v->mesh_shaders) {
            nph_gpu_cmd_draw_mesh_tasks_indirect(cmd, nph_gpu_rg_buffer(graph, f->args),
                                                 small ? ARGS_OFFSET(sw_masked_hw_args) +
                                                             (u64)phase * 16U
                                                       : ARGS_OFFSET(raster_args) + kind * 16U,
                                                 1U, 16U);
        } else {
            const u64 list = (u64)r->desc.max_visible_clusters * sizeof(NphVisDraw);
            nph_gpu_cmd_draw_indexed_indirect_count(
                cmd, nph_gpu_rg_buffer(graph, f->draws), list * ((u64)phase * 2U + k),
                nph_gpu_rg_buffer(graph, f->counters),
                (u64)offsetof(NphVisCounters, raster_count) + kind * 4U,
                r->desc.max_visible_clusters, (u32)sizeof(NphVisDraw));
        }
    }
    if (!v->mesh_shaders) {
        nph_gpu_cmd_end_rendering(cmd);
    }
}

// The small masked clusters of a phase, in a pass of their own.
static void sw_masked_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    const VisFrame *f = &r->vis.frame;
    NphVisPush p = push_of(r, phase, 0U);
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_SW_RASTER_MASKED]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(
        cmd, nph_gpu_rg_buffer(graph, f->args),
        ARGS_OFFSET(raster_args) +
            ((u64)phase * (u64)NPH_VIS_RASTER_KINDS + (u64)NPH_VIS_RASTER_SOFTWARE_MASKED) * 16U);
}

static void sw_masked0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    sw_masked_record((NphRenderer *)user, graph, cmd, 0U);
}

static void sw_masked1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    sw_masked_record((NphRenderer *)user, graph, cmd, 1U);
}

// The software raster of a phase: a workgroup per small opaque cluster.
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

// The voxel raster of a phase: a workgroup per voxel cluster.
static void voxel_raster_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    const VisFrame *f = &r->vis.frame;
    NphVisPush p = push_of(r, phase, 0U);
    p.arg0 = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.compute[VIS_COMPUTE_VOXEL_RASTER]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(
        cmd, nph_gpu_rg_buffer(graph, f->args),
        ARGS_OFFSET(raster_args) +
            ((u64)phase * (u64)NPH_VIS_RASTER_KINDS + (u64)NPH_VIS_RASTER_VOXEL) * 16U);
}

static void voxel_raster0_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    voxel_raster_record((NphRenderer *)user, graph, cmd, 0U);
}

static void voxel_raster1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    voxel_raster_record((NphRenderer *)user, graph, cmd, 1U);
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
    // The mesh path draws phase 0's opaque clusters through the ROP here and
    // its masked ones in a pass of their own.
    NphRenderer *r = (NphRenderer *)user;
    raster_record(r, graph, cmd, 0U, r->vis.mesh_shaders ? 1U : 3U, r->vis.mesh_shaders);
}

static void raster0_masked_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    raster_record((NphRenderer *)user, graph, cmd, 0U,
                  (1U << NPH_VIS_RASTER_MASKED) | (1U << NPH_VIS_RASTER_SKIN) |
                      (1U << NPH_VIS_RASTER_SOFTWARE_MASKED),
                  false);
}

static void raster1_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    raster_record(r, graph, cmd, 1U,
                  r->vis.mesh_shaders
                      ? (1U << NPH_VIS_RASTER_OPAQUE) | (1U << NPH_VIS_RASTER_MASKED) |
                            (1U << NPH_VIS_RASTER_SKIN) | (1U << NPH_VIS_RASTER_SOFTWARE_MASKED)
                      : 3U,
                  false);
}

// The pyramid: mip 0 from the visibility buffer's depth (vis_hzb.comp),
// then the mips past it six at a time, each dispatch from the mip before
// them (vis_hzb_mips.comp). After phase 1, mip 0 is rebuilt only in the
// tiles phase 1 drew into (vis_hzb.comp mode 2), and the first mips only in
// the tiles above those: elsewhere the pyramid after phase 0 already holds
// the frame's depth.
static void hzb_record(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuCmd *cmd, u32 phase)
{
    RenderVis *v = &r->vis;
    NphGpuDevice *dev = r->dev;
    const NphGpuImage hzb = nph_gpu_rg_image(graph, v->frame.hzb);
    // The next dispatch reads what this one wrote (the image stays in
    // GENERAL).
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                                   (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_READ};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;

    nph_gpu_cmd_bind_pipeline(cmd, v->compute[VIS_COMPUTE_HZB]);
    NphVisPush p = push_of(r, phase, phase == 0U ? 0U : 2U);
    p.arg0 = nph_gpu_image_storage_index(dev, nph_gpu_rg_image(graph, v->frame.visbuffer), 0U);
    p.arg1 = nph_gpu_image_storage_index(dev, hzb, 0U);
    p.arg4 = NPH_VIS_NO_AO;
    if (r->ao.frame_active) {
        // The occlusion's depths' mips 0 and 1 (vis_hzb.comp).
        const NphGpuImage depths = nph_gpu_rg_image(graph, r->ao.frame_depths);
        p.arg4 = nph_gpu_image_storage_index(dev, depths, 0U);
        p.arg2 = nph_gpu_image_storage_index(dev, depths, 1U);
    }
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    if (phase == 0U) {
        // Mip 0 is half the target, rounded up.
        nph_gpu_cmd_dispatch(cmd, (v->hzb_width + 15U) / 16U, (v->hzb_height + 15U) / 16U, 1U);
    } else {
        // A workgroup per tile: 16 x 16 mip-0 texels.
        nph_gpu_cmd_dispatch(cmd, v->hzb_tiles_x, v->hzb_tiles_y, 1U);
    }

    NphVisHzbMipsPush mp;
    memset(&mp, 0, sizeof mp);
    mp.view = v->frame.view_address;
    mp.phase = phase;
    mp.mips = v->hzb_mips;
    mp.width = v->hzb_width;
    mp.height = v->hzb_height;
    for (u32 i = 0; i < v->hzb_mips; ++i) {
        mp.storage[i] = nph_gpu_image_storage_index(dev, hzb, i);
    }
    nph_gpu_cmd_bind_pipeline(cmd, v->compute[VIS_COMPUTE_HZB_MIPS]);
    for (u32 base = 0; base + 1U < v->hzb_mips; base += (u32)NPH_VIS_HZB_MIPS_LEVELS) {
        nph_gpu_cmd_barrier(cmd, &bd);
        // Tiles of the base mip, rounded down (the last takes the rest).
        mp.base = base;
        mp.tiles_x =
            nph_max_u32(nph_max_u32(v->hzb_width >> base, 1U) / (u32)NPH_VIS_HZB_MIPS_TILE, 1U);
        mp.tiles_y =
            nph_max_u32(nph_max_u32(v->hzb_height >> base, 1U) / (u32)NPH_VIS_HZB_MIPS_TILE, 1U);
        nph_gpu_cmd_push_constants(cmd, &mp, sizeof mp);
        nph_gpu_cmd_dispatch(cmd, mp.tiles_x, mp.tiles_y, 1U);
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

NphResult render_vis_paged_pipelines(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    if (!v->supported || v->paged_ready) {
        return NPH_RESULT_OK;
    }
    NphGpuComputePipelineDesc d;
    memset(&d, 0, sizeof d);
    d.shader = v->shaders[VIS_SHADER_PAGE_REQUESTS];
    d.name = "vis page requests";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &v->page_requests));
    render_log_statistics(r, v->page_requests, d.name);
    v->paged_ready = 1;
    return NPH_RESULT_OK;
}

// One invocation for each of the target's 8 x 8 tiles, 64 a workgroup
// (vis_page_requests.comp).
static void page_requests_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const VisFrame *f = &r->vis.frame;
    NphVisResolvePush p = r->vis.resolve_push;
    p.vis_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.page_requests);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    const u32 tiles_x = (f->width + 7U) / 8U;
    const u32 tiles_y = (f->height + 7U) / 8U;
    nph_gpu_cmd_dispatch(cmd, (tiles_x + 7U) / 8U, (tiles_y + 7U) / 8U, 1U);
}

static void resolve_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const VisFrame *f = &r->vis.frame;
    NphVisResolvePush p = r->vis.resolve_push;
    p.hdr_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->hdr), 0U);
    p.vis_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    p.ao_index = r->ao.frame_active ? render_ao_index(r, graph) : NPH_VIS_NO_AO;
    // By shadow rays, the virtual shadow map, and the radiance cache.
    static const VisCompute k_resolve[2][2][2] = {
        {{VIS_COMPUTE_RESOLVE, VIS_COMPUTE_RESOLVE_GI},
         {VIS_COMPUTE_RESOLVE_VSM, VIS_COMPUTE_RESOLVE_VSM_GI}},
        {{VIS_COMPUTE_RESOLVE_RT, VIS_COMPUTE_RESOLVE_RT_GI},
         {VIS_COMPUTE_RESOLVE_RT_VSM, VIS_COMPUTE_RESOLVE_RT_VSM_GI}}};
    const u32 rt = r->rt.frame_shadows ? 1U : 0U;
    const u32 vsm = r->vsm.frame_active ? 1U : 0U;
    const u32 gi = r->gi.frame_active ? 1U : 0U;
    const u32 kind = r->vis.debug_view ? 2U : r->vis.frame_voxels ? 1U : 0U;
    if (r->many.frame_active) {
        nph_gpu_cmd_bind_pipeline(cmd, r->vis.resolve_many[kind][rt][vsm][gi]);
    } else {
        nph_gpu_cmd_bind_pipeline(cmd, kind == 2U   ? r->vis.resolve_debug[rt][vsm][gi]
                                       : kind == 1U ? r->vis.resolve_voxels[rt][vsm][gi]
                                                    : r->vis.compute[k_resolve[rt][vsm][gi]]);
    }
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, (f->width + 7U) / 8U, (f->height + 7U) / 8U, 1U);
}

// The voxel pixels of the tiles the resolve listed: a workgroup a tile.
static void voxel_resolve_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const VisFrame *f = &r->vis.frame;
    NphVisResolvePush p = r->vis.resolve_push;
    p.hdr_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->hdr), 0U);
    p.vis_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, f->visbuffer), 0U);
    p.ao_index = r->ao.frame_active ? render_ao_index(r, graph) : NPH_VIS_NO_AO;
    const u32 debug = r->vis.debug_view ? 1U : 0U;
    const u32 rt = r->rt.frame_shadows ? 1U : 0U;
    const u32 vsm = r->vsm.frame_active ? 1U : 0U;
    const u32 gi = r->gi.frame_active ? 1U : 0U;
    nph_gpu_cmd_bind_pipeline(cmd, r->vis.voxel_resolve[debug][rt][vsm][gi]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, nph_gpu_rg_buffer(graph, f->args),
                                  ARGS_OFFSET(voxel_resolve_args));
}

static void stats_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const NphGpuBufferCopy region = {0U, 0U, sizeof(NphVisCounters)};
    nph_gpu_cmd_copy_buffer(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.counters),
                            nph_gpu_rg_buffer(graph, r->vis.frame.stats), &region, 1U);
    const NphGpuBufferCopy requests = {0U, 0U,
                                       (u64)nph_max_u32(r->desc.max_cells, 1U) * sizeof(u32)};
    nph_gpu_cmd_copy_buffer(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.cell_requests),
                            nph_gpu_rg_buffer(graph, r->vis.frame.request_readback), &requests, 1U);
}

u32 render_vis_visbuffer_index(const NphRenderer *r, NphGpuRenderGraph *graph)
{
    return nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, r->vis.frame.visbuffer));
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
    NphGpuRgBuffer cells;
    NphGpuRgBuffer material_map;
    NphGpuRgBuffer cull_meshlets;
    NphGpuRgBuffer deferred;
    NphGpuRgBuffer clusters;
    NphGpuRgBuffer lists;
    NphGpuRgBuffer lod_queue;
    NphGpuRgBuffer cell_runs; // cells first (ADR-0064)
    NphGpuRgBuffer cell_members;
    NphGpuRgBuffer cell_work;
    NphGpuRgBuffer cell_listed;
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
    nph_gpu_rg_read_buffer(p, g->cells, NPH_GPU_RG_USE_SHADER_READ);
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
    const bool rop = r->vis.mesh_shaders && phase == 0U;
    if (rop) {
        // Phase 0's opaque clusters through the ROP, into the visibility
        // buffer, which the pass clears.
        NphGpuRgAttachment c;
        memset(&c, 0, sizeof c);
        c.load = NPH_GPU_LOAD_OP_CLEAR;
        c.store = NPH_GPU_STORE_OP_STORE;
        nph_gpu_rg_color_attachment(p, f->visbuffer, &c);
    } else {
        nph_gpu_rg_write_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    }
    if (r->vis.mesh_shaders) {
        // Cleared by phase 0 (reverse-Z: far is 0), kept for phase 1. The
        // software raster's clusters never reach it: fragments behind them
        // are not rejected, only lose their atomic, as before.
        NphGpuRgAttachment a;
        memset(&a, 0, sizeof a);
        a.load = phase == 0U ? NPH_GPU_LOAD_OP_CLEAR : NPH_GPU_LOAD_OP_LOAD;
        a.store = phase == 0U ? NPH_GPU_STORE_OP_STORE : NPH_GPU_STORE_OP_DONT_CARE;
        a.clear.ds.depth = 0.0F;
        nph_gpu_rg_depth_attachment(p, f->raster_depth, &a, 0);
    }
    if (rop) {
        // The masked clusters, with atomics, against the opaque ones' depth.
        p = add_pass(graph, r, "vis raster 0 masked", NPH_GPU_RG_PASS_GRAPHICS,
                     raster0_masked_execute);
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
        NphGpuRgAttachment a;
        memset(&a, 0, sizeof a);
        a.load = NPH_GPU_LOAD_OP_LOAD;
        a.store = NPH_GPU_STORE_OP_STORE;
        nph_gpu_rg_depth_attachment(p, f->raster_depth, &a, 0);
    }

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

    // The small masked clusters, likewise.
    p = add_pass(graph, r, phase == 0U ? "vis sw raster 0 masked" : "vis sw raster 1 masked",
                 NPH_GPU_RG_PASS_COMPUTE, phase == 0U ? sw_masked0_execute : sw_masked1_execute);
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

    // The voxel clusters, in compute, into the same visibility buffer: the
    // pixel's ray from the camera through its point on the near plane, which
    // the render view holds the inverse projection for. Without a mesh with
    // voxel levels the cull lists none, and the pass is left out.
    if (!r->vis.frame_voxels) {
        return NPH_RESULT_OK;
    }
    p = add_pass(graph, r, phase == 0U ? "vis voxel raster 0" : "vis voxel raster 1",
                 NPH_GPU_RG_PASS_COMPUTE,
                 phase == 0U ? voxel_raster0_execute : voxel_raster1_execute);
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
    // The counters: the local lights' count (the light tiles' depths), and
    // in phase 1 the full rebuild's flag.
    nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    if (phase == 1U) {
        nph_gpu_rg_read_buffer(p, f->hzb_dirty, NPH_GPU_RG_USE_SHADER_READ);
    }
    // Mip 0 requests the virtual shadow map's pages, and writes the ambient
    // occlusion's depths (vis_hzb.comp).
    render_vsm_request_pages(r, p);
    if (r->ao.frame_active) {
        nph_gpu_rg_write_image(p, r->ao.frame_depths, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 2U);
    }
    // And bounds the light tiles' depths.
    nph_gpu_rg_write_buffer(p, f->light_depths, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_image(p, f->hzb, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, NPH_GPU_ALL_MIPS);
    return NPH_RESULT_OK;
}

// The light grid: the frame's light updates, then the lights in view and
// their bins. Nothing here reads the visibility buffer; the tiles' masks
// wait for the frame's depths (declare_light_tiles).
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
    return NPH_RESULT_OK;
}

// The light tiles' masks, culled against the depths the depth pyramid's
// passes bound (vis_light_tiles.comp): after both phases.
static NphResult declare_light_tiles(NphGpuRenderGraph *graph, NphRenderer *r)
{
    const VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p =
        add_pass(graph, r, "vis light tiles", NPH_GPU_RG_PASS_COMPUTE, light_tiles_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->light_grid, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->light_depths, NPH_GPU_RG_USE_SHADER_READ);
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
    render_gi_read(r, p);
    if (r->ao.frame_active) {
        nph_gpu_rg_read_image(p, r->ao.frame_out, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    }
    nph_gpu_rg_write_image(p, f->hdr, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    // The pages the opaque pixels ask for, in a pass of their own
    // (vis_page_requests.comp; ADR-0065).
    if (!nph_gpu_rg_buffer_is_null(r->paging.frame_requests)) {
        NphGpuRgPass *q =
            add_pass(graph, r, "vis page requests", NPH_GPU_RG_PASS_COMPUTE, page_requests_execute);
        if (q == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_image(q, f->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
        nph_gpu_rg_read_buffer(q, g->clusters, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(q, g->instances, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(q, g->meshes, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_write_buffer(q, r->paging.frame_requests, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    if (!r->vis.frame_voxels) {
        return NPH_RESULT_OK;
    }
    // The tiles holding voxel pixels, listed for the voxel resolve, its
    // count, and its workgroups.
    nph_gpu_rg_write_buffer(p, f->voxel_tiles, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, f->voxel_tile_bits, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_STORAGE_WRITE);

    // The voxel pixels of those tiles, after the triangles' pixels.
    p = add_pass(graph, r, "vis voxel resolve", NPH_GPU_RG_PASS_COMPUTE, voxel_resolve_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, f->voxel_tiles, NPH_GPU_RG_USE_SHADER_READ);
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
    render_gi_read(r, p);
    if (r->ao.frame_active) {
        nph_gpu_rg_read_image(p, r->ao.frame_out, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    }
    nph_gpu_rg_write_image(p, f->hdr, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    return NPH_RESULT_OK;
}

// The steps down the hierarchies of the levels of detail in a phase, after
// its instance cull: one per two node levels of the deepest live hierarchy,
// each from one queue into the next (vis_cull_nodes.comp). The groups they take
// append their clusters to the phase's meshlet list.
static NphResult declare_nodes(NphGpuRenderGraph *graph, NphRenderer *r, const VisGraph *g,
                               u32 phase, u32 steps)
{
    static const NphGpuRgExecuteFn k_cull[2][NPH_VIS_LOD_QUEUES] = {
        {cull_nodes00_execute, cull_nodes01_execute, cull_nodes02_execute},
        {cull_nodes10_execute, cull_nodes11_execute, cull_nodes12_execute}};
    static const char *const k_names[2] = {"vis cull nodes 0", "vis cull nodes 1"};
    const VisFrame *f = &r->vis.frame;
    for (u32 i = 0; i < steps; ++i) {
        const u32 queue = i % (u32)NPH_VIS_LOD_QUEUES;
        NphGpuRgPass *p =
            add_pass(graph, r, k_names[phase], NPH_GPU_RG_PASS_COMPUTE, k_cull[phase][queue]);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        read_scene(p, g, f->hzb);
        // The arguments it is dispatched with, and the next steps' it writes.
        nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_STORAGE_WRITE);
        nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
        nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
        // Reads one queue and writes the other.
        nph_gpu_rg_write_buffer(p, g->lod_queue, NPH_GPU_RG_USE_STORAGE_WRITE);
        nph_gpu_rg_write_buffer(p, phase == 0U ? g->cull_meshlets : g->deferred,
                                NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    return NPH_RESULT_OK;
}

// The flat cull in a phase, after its instance cull: what the instance cull
// listed, and in phase 0 what it defers to phase 1 (its work items and their
// arguments).
static NphResult declare_groups(NphGpuRenderGraph *graph, NphRenderer *r, const VisGraph *g,
                                u32 phase)
{
    static const NphGpuRgExecuteFn k_cull[2] = {cull_groups0_execute, cull_groups1_execute};
    static const char *const k_names[2] = {"vis cull groups 0", "vis cull groups 1"};
    const VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p = add_pass(graph, r, k_names[phase], NPH_GPU_RG_PASS_COMPUTE, k_cull[phase]);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    read_scene(p, g, f->hzb);
    nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
    if (phase == 0U) {
        nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_STORAGE_WRITE);
        nph_gpu_rg_write_buffer(p, g->lod_queue, NPH_GPU_RG_USE_STORAGE_WRITE);
    } else {
        nph_gpu_rg_read_buffer(p, g->lod_queue, NPH_GPU_RG_USE_SHADER_READ);
    }
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, phase == 0U ? g->cull_meshlets : g->deferred,
                            NPH_GPU_RG_USE_STORAGE_WRITE);
    return NPH_RESULT_OK;
}

// The steps down the deepest of these hierarchies.
static u32 lod_steps_of(const u32 *depths)
{
    for (u32 d = RENDER_LOD_MAX_DEPTH; d > 0U; --d) {
        if (depths[d - 1U] != 0U) {
            return (d + 1U) / 2U;
        }
    }
    return 0U;
}

u32 render_vis_lod_steps(const NphRenderer *r)
{
    return lod_steps_of(r->vis.lod_depths);
}

// The steps down the deepest hierarchy the camera may walk, when the view
// walks them.
static u32 lod_steps(const NphRenderer *r, u32 flags)
{
    return (flags & (u32)NPH_VIS_VIEW_LOD) != 0U ? lod_steps_of(r->vis.lod_walk_depths) : 0U;
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
    nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_TRANSFER_DST);
    nph_gpu_rg_write_buffer(p, f->hzb_dirty, NPH_GPU_RG_USE_TRANSFER_DST);
    nph_gpu_rg_write_buffer(p, f->cell_requests, NPH_GPU_RG_USE_TRANSFER_DST);
    if (v->frame_voxels) {
        nph_gpu_rg_write_buffer(p, f->voxel_tile_bits, NPH_GPU_RG_USE_TRANSFER_DST);
    }
    nph_gpu_rg_write_buffer(p, f->light_grid, NPH_GPU_RG_USE_TRANSFER_DST);
    if (!v->mesh_shaders) {
        nph_gpu_rg_write_image(p, f->visbuffer, NPH_GPU_RG_USE_TRANSFER_DST, 0U, 1U);
    }

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

    // Phase 0: the cells first where the renderer holds any (ADR-0064),
    // then the instances they list and those of no cell; else every slot.
    if (f->cells_first) {
        p = add_pass(graph, r, "vis cull cells", NPH_GPU_RG_PASS_COMPUTE, cull_cells_execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_buffer(p, g->cells, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, g->cell_runs, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_write_buffer(p, g->cell_listed, NPH_GPU_RG_USE_SHADER_WRITE);
        nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
        if (args_pass(graph, r, "vis args cells", args_cells_execute) == NULL) {
            return NPH_RESULT_FULL;
        }
        p = add_pass(graph, r, "vis expand cells", NPH_GPU_RG_PASS_COMPUTE, expand_cells_execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
        nph_gpu_rg_read_buffer(p, f->counters, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, g->cell_listed, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, g->cell_members, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_write_buffer(p, g->cell_work, NPH_GPU_RG_USE_SHADER_WRITE);
    }
    p = add_pass(graph, r, "vis cull instances 0", NPH_GPU_RG_PASS_COMPUTE,
                 cull_instances0_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    read_scene(p, g, f->hzb);
    if (f->cells_first) {
        nph_gpu_rg_read_buffer(p, f->args, NPH_GPU_RG_USE_INDIRECT);
        nph_gpu_rg_read_buffer(p, g->cell_work, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, g->cell_members, NPH_GPU_RG_USE_SHADER_READ);
    }
    nph_gpu_rg_write_buffer(p, f->counters, NPH_GPU_RG_USE_STORAGE_WRITE);
    nph_gpu_rg_write_buffer(p, g->cull_meshlets, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_buffer(p, g->deferred, NPH_GPU_RG_USE_SHADER_WRITE);
    nph_gpu_rg_write_buffer(p, f->cell_requests, NPH_GPU_RG_USE_STORAGE_WRITE);
    if (f->lod_steps != 0U || f->lod_flat) {
        // The flat cull's groups, the hierarchies' roots, and their workgroups.
        nph_gpu_rg_write_buffer(p, g->lod_queue, NPH_GPU_RG_USE_SHADER_WRITE);
        nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    if (f->lod_flat) {
        NPH_TRY(declare_groups(graph, r, g, 0U));
    }
    NPH_TRY(declare_nodes(graph, r, g, 0U, f->lod_steps));
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
    nph_gpu_rg_write_buffer(p, f->cell_requests, NPH_GPU_RG_USE_STORAGE_WRITE);
    if (f->lod_steps != 0U || f->lod_flat) {
        // The flat cull's groups, the hierarchies' roots, and their workgroups.
        nph_gpu_rg_write_buffer(p, g->lod_queue, NPH_GPU_RG_USE_SHADER_WRITE);
        nph_gpu_rg_write_buffer(p, f->args, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    if (f->lod_flat) {
        NPH_TRY(declare_groups(graph, r, g, 1U));
    }
    NPH_TRY(declare_nodes(graph, r, g, 1U, f->lod_steps));
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
    NPH_TRY(declare_hzb(graph, r, 1U));
    return declare_light_tiles(graph, r);
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
    read_requests(r);
    render_vsm_read_stats(r);
    v->frame_voxels = v->voxel_meshes != 0U ? 1 : 0;
    NphResult res = ensure_hzb(r, width, height);
    if (res == NPH_RESULT_OK) {
        res = ensure_voxel_tiles(r, width, height);
    }
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
    if (v->mesh_shaders && (view->vis_flags & (u32)NPH_RENDER_VIS_NO_SKIN) == 0U) {
        flags |= (u32)NPH_VIS_VIEW_SKIN;
    }
    if ((view->vis_flags & (u32)NPH_RENDER_VIS_NO_LIGHT_GRID) == 0U) {
        flags |= (u32)NPH_VIS_VIEW_LIGHT_GRID;
    }
    if (view->lod_error > 0.0F) {
        flags |= (u32)NPH_VIS_VIEW_LOD;
    }
    write_view(r, view, width, height, view_m, proj, flags);
    v->debug_view = view->debug_view != NPH_RENDER_VIEW_LIT ? 1 : 0;
    memset(f, 0, sizeof *f);
    f->view_address = r->frame_addresses[r->frame_slot] + r->layout.vis_view_offset;
    f->cells_first = v->cell_slots != 0U && !v->cells_first_off ? 1 : 0;
    f->update_count = render_vis_write_updates(r);
    f->light_update_count = render_vis_write_light_updates(r);
    f->width = width;
    f->height = height;
    f->lod_steps = lod_steps(r, flags);
    f->lod_flat = (flags & (u32)NPH_VIS_VIEW_LOD) != 0U && v->lod_flat_meshes != 0U;

    // Resources.
    NphGpuImageDesc vd;
    memset(&vd, 0, sizeof vd);
    vd.format = RENDER_VIS_FORMAT;
    vd.width = width;
    vd.height = height;
    vd.mip_count = 1U;
    vd.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    if (v->mesh_shaders) {
        vd.usage |= (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT;
    }
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
    f->raster_depth = NPH_GPU_RG_IMAGE_NULL;
    if (v->mesh_shaders) {
        NphGpuImageDesc dd = vd;
        dd.format = RENDER_DEPTH_FORMAT;
        dd.sampled_format = NPH_GPU_FORMAT_UNDEFINED;
        dd.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
        dd.name = "vis raster depth";
        f->raster_depth = nph_gpu_rg_create_image(graph, &dd);
    }
    f->hzb = nph_gpu_rg_import_image(graph, v->hzb);
    f->hzb_dirty = nph_gpu_rg_import_buffer(graph, v->hzb_dirty);
    if (v->frame_voxels) {
        f->voxel_tiles = nph_gpu_rg_import_buffer(graph, v->voxel_tiles);
        f->voxel_tile_bits = nph_gpu_rg_import_buffer(graph, v->voxel_tile_bits);
    }
    f->counters = nph_gpu_rg_import_buffer(graph, v->counters);
    f->args = nph_gpu_rg_import_buffer(graph, v->args);
    f->stats = nph_gpu_rg_import_buffer(graph, v->stats[r->frame_slot]);
    f->cell_requests = nph_gpu_rg_import_buffer(graph, v->cell_requests);
    f->request_readback = nph_gpu_rg_import_buffer(graph, v->request_readback[r->frame_slot]);
    f->lights = nph_gpu_rg_import_buffer(graph, v->lights);
    f->light_grid = nph_gpu_rg_import_buffer(graph, v->light_grid);
    f->light_tiles = nph_gpu_rg_import_buffer(graph, v->light_tiles);
    f->light_depths = nph_gpu_rg_import_buffer(graph, v->light_depths);
    if (!v->mesh_shaders) {
        f->draws = nph_gpu_rg_import_buffer(graph, v->draws);
        f->index_pool = nph_gpu_rg_import_buffer(graph, v->index_pool);
    }
    VisGraph g;
    g.instances = nph_gpu_rg_import_buffer(graph, v->instances);
    g.prev_transforms = nph_gpu_rg_import_buffer(graph, v->prev_transforms);
    g.meshes = nph_gpu_rg_import_buffer(graph, v->meshes);
    g.cells = nph_gpu_rg_import_buffer(graph, v->cells);
    g.material_map = nph_gpu_rg_import_buffer(graph, v->material_map);
    g.cull_meshlets = nph_gpu_rg_import_buffer(graph, v->cull_meshlets);
    g.deferred = nph_gpu_rg_import_buffer(graph, v->deferred);
    g.clusters = nph_gpu_rg_import_buffer(graph, v->clusters);
    g.lists = nph_gpu_rg_import_buffer(graph, v->lists);
    g.lod_queue = nph_gpu_rg_import_buffer(graph, v->lod_queue);
    g.cell_runs = nph_gpu_rg_import_buffer(graph, v->cell_runs);
    g.cell_members = nph_gpu_rg_import_buffer(graph, v->cell_members);
    g.cell_work = nph_gpu_rg_import_buffer(graph, v->cell_work);
    g.cell_listed = nph_gpu_rg_import_buffer(graph, v->cell_listed);
    f->cell_runs = g.cell_runs;
    f->cell_members = g.cell_members;
    f->instances = g.instances;
    f->prev_transforms = g.prev_transforms;
    f->clusters = g.clusters;
    f->cells = g.cells;
    if (nph_gpu_rg_image_is_null(f->visbuffer) || nph_gpu_rg_image_is_null(f->hdr) ||
        nph_gpu_rg_image_is_null(f->hzb) || nph_gpu_rg_buffer_is_null(g.lists) ||
        nph_gpu_rg_buffer_is_null(f->stats) || nph_gpu_rg_buffer_is_null(f->light_tiles) ||
        nph_gpu_rg_buffer_is_null(f->light_depths) ||
        (v->frame_voxels && (nph_gpu_rg_buffer_is_null(f->voxel_tiles) ||
                             nph_gpu_rg_buffer_is_null(f->voxel_tile_bits)))) {
        NPH_PROFILE_END();
        return NPH_RESULT_FULL;
    }

    if (r->ao.frame_active) {
        // The ambient occlusion's depths, which the pyramid's passes start.
        res = render_ao_declare_depths(r, graph);
    }
    if (res == NPH_RESULT_OK) {
        res = declare_culling(graph, r, &g);
    }
    if (res == NPH_RESULT_OK && r->vsm.frame_active && blend_requests != NULL) {
        // The blended surfaces' pages, against the opaque depth (the
        // visibility buffer's, which the transparent pass tests too).
        res = render_vsm_declare_blend_requests(r, graph, f->visbuffer, blend_requests);
    }
    if (res == NPH_RESULT_OK && r->vsm.frame_active) {
        // The sun's virtual shadow map: its pages drawn where the depth
        // pyramid's passes asked for them.
        res = render_vsm_declare_pages(r, graph, g.instances, g.meshes);
    }
    if (res == NPH_RESULT_OK && r->ao.frame_active) {
        // The ambient occlusion, which the resolve reads.
        res = render_ao_declare(r, graph);
    }
    if (res == NPH_RESULT_OK && r->rt.frame_active) {
        // Rays: the instance records from the GPU scene, then the builds,
        // before the radiance cache and the resolve trace.
        res = render_rt_declare(r, graph, g.instances, g.meshes, f->counters, &f->tlas);
    }
    if (res == NPH_RESULT_OK && r->gi.frame_active) {
        // The radiance cache, whose cells the resolve reads.
        RenderGiInputs in;
        in.visbuffer = f->visbuffer;
        in.clusters = g.clusters;
        in.instances = g.instances;
        in.meshes = g.meshes;
        in.material_map = g.material_map;
        in.tlas = f->tlas;
        in.env = env;
        in.sh = sh;
        res = render_gi_declare(r, graph, &in);
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
        nph_gpu_rg_read_buffer(p, f->cell_requests, NPH_GPU_RG_USE_TRANSFER_SRC);
        nph_gpu_rg_write_buffer(p, f->request_readback, NPH_GPU_RG_USE_TRANSFER_DST);
        v->stats_frame[r->frame_slot] = nph_gpu_frame_index(dev) + 1U;
        v->request_frame[r->frame_slot] = nph_gpu_frame_index(dev) + 1U;
        // Next frame's phase 0 tests against this frame's HZB, from this
        // frame's view.
        v->prev_view = *view_m;
        v->prev_view_proj = nph_mat4_mul(plain, *view_m);
        v->prev_projection = nph_vec4(proj->c[0].x, proj->c[1].y, view->camera.znear, 0.0F);
        v->prev_camera = view->camera.position;
        v->prev_jitter = jitter;
        v->prev_frame = nph_gpu_frame_index(dev) + 1U;
        v->hzb_valid = 1;
        *out_hdr = f->hdr;
    }
    if (stats != NULL) {
        const NphRenderFrameStats *last = &v->stats_last;
        stats->vis_frame = last->vis_frame;
        stats->vis_clusters = last->vis_clusters;
        stats->vis_clusters_sw = last->vis_clusters_sw;
        stats->vis_triangles = last->vis_triangles;
        stats->vis_voxel_clusters = last->vis_voxel_clusters;
        stats->vis_voxel_bricks = last->vis_voxel_bricks;
        stats->vis_instances_culled_frustum = last->vis_instances_culled_frustum;
        stats->vis_instances_culled_occlusion = last->vis_instances_culled_occlusion;
        stats->vis_meshlets_culled_frustum = last->vis_meshlets_culled_frustum;
        stats->vis_meshlets_culled_cone = last->vis_meshlets_culled_cone;
        stats->vis_meshlets_culled_occlusion = last->vis_meshlets_culled_occlusion;
        stats->vis_lod_groups = last->vis_lod_groups;
        stats->vis_lod_nodes = last->vis_lod_nodes;
        stats->vis_lod_clusters_culled = last->vis_lod_clusters_culled;
        stats->vis_lod_steps = f->lod_steps;
        stats->vis_lights = last->vis_lights;
        stats->vis_overflow = last->vis_overflow;
        stats->vis_cells_proxied = last->vis_cells_proxied;
        stats->vis_instances_proxied = last->vis_instances_proxied;
        stats->vsm_requested = last->vsm_requested;
        stats->vsm_drawn = last->vsm_drawn;
        stats->vsm_resident = last->vsm_resident;
        stats->vsm_evicted = last->vsm_evicted;
        stats->vsm_draws = last->vsm_draws;
        stats->vsm_voxel_draws = last->vsm_voxel_draws;
        stats->vsm_meshlets = last->vsm_meshlets;
        stats->vsm_nodes = last->vsm_nodes;
        stats->vsm_wanted = last->vsm_wanted;
        stats->vsm_budget = last->vsm_budget;
        stats->vsm_overflow = last->vsm_overflow;
    }
    NPH_PROFILE_END();
    return res;
}

// The transparent surfaces of a production frame: forward pipelines over
// the resolved HDR image, testing the visibility buffer's depth themselves
// (forward_frag.glsl k_vis_depth). Only called when blended draws exist.
NphResult render_vis_declare_transparent(NphRenderer *r, NphGpuRenderGraph *graph,
                                         NphGpuRgExecuteFn draw, NphGpuRgImage env,
                                         NphGpuRgImage specular, NphGpuRgImage lut,
                                         NphGpuRgBuffer sh)
{
    VisFrame *f = &r->vis.frame;
    NphGpuRgPass *p = add_pass(graph, r, "transparent", NPH_GPU_RG_PASS_GRAPHICS, draw);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_LOAD;
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_color_attachment(p, f->hdr, &a);
    nph_gpu_rg_read_image(p, f->visbuffer, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, env, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(p, specular, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(p, lut, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_buffer(p, sh, NPH_GPU_RG_USE_SHADER_READ);
    if (!nph_gpu_rg_buffer_is_null(r->paging.frame_requests)) {
        nph_gpu_rg_write_buffer(p, r->paging.frame_requests, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    read_lights(p, f);
    if (r->rt.frame_active) {
        nph_gpu_rg_read_buffer(p, f->tlas, NPH_GPU_RG_USE_ACCEL_READ);
    }
    render_vsm_read(r, p);
    return NPH_RESULT_OK;
}

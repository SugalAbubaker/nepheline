// SPDX-License-Identifier: Apache-2.0
// The production path's ambient occlusion (render/renderer.h
// NphRenderView.ao_radius; docs/render.md "Ambient occlusion";
// docs/adr/0038-ambient-occlusion.md).
//
// Ground-truth ambient occlusion (Jimenez, Wu, Pesce & Jarabo, "Practical
// Realtime Strategies for Accurate Indirect Occlusion", ATVI-TR-16-01,
// 2016) at full resolution, one occlusion a pixel: the depth pyramid's
// passes copy the frame's depths and average them into a mip as they read
// them (vis_hzb.comp), and a pass adds the coarser mips for the far samples
// (ao_depth.comp); each pixel finds the
// horizons of one slice of its hemisphere and integrates the visible arc in
// closed form (ao.comp); and a bilateral 5 x 5 average gathers the 16
// directions its neighbours took (ao_denoise.comp). The resolve reads its
// pixel's and darkens the environment's light by it, the diffuse part with
// the light the occluders reflect back. The slices turn through 6 rotations
// from frame to frame under the temporal resolve, which averages them.
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderAoPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");

// The reverse-Z depths; each slice's shares with the distance, packed in a
// word; the denoised occlusion, a half float.
#define AO_DEPTH_FORMAT  NPH_GPU_FORMAT_R32_SFLOAT
#define AO_PACKED_FORMAT NPH_GPU_FORMAT_R32_UINT
#define AO_OUT_FORMAT    NPH_GPU_FORMAT_R16_SFLOAT
// The slices' rotations, which the temporal resolve averages.
#define AO_ROTATIONS 6U
// The passes' workgroups (ao_depth.comp, ao.comp, ao_denoise.comp).
#define AO_GROUP 16U
NPH_STATIC_ASSERT((1U << (NPH_RENDER_AO_MIPS - 1)) == 2U * AO_GROUP, "the depths' alignment");
#define AO_DEPTH_ALIGN (1U << (NPH_RENDER_AO_MIPS - 1))

// The depths' extent: the frame's rounded up to a multiple of
// 2^(NPH_RENDER_AO_MIPS - 1), which every mip then halves exactly
// (ao_depth.comp).
static u32 depth_extent(u32 extent)
{
    return (extent + AO_DEPTH_ALIGN - 1U) / AO_DEPTH_ALIGN * AO_DEPTH_ALIGN;
}

NphResult render_ao_create(NphRenderer *r)
{
    RenderAo *a = &r->ao;
    NPH_TRY(render_load_shader_file(r, "ao_depth.comp", &a->depth_shader));
    NPH_TRY(render_load_shader_file(r, "ao.comp", &a->ao_shader));
    NPH_TRY(render_load_shader_file(r, "ao_denoise.comp", &a->denoise_shader));
    NphGpuComputePipelineDesc d;
    memset(&d, 0, sizeof d);
    d.shader = a->depth_shader;
    d.name = "ao depth";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &a->depth));
    memset(&d, 0, sizeof d);
    d.shader = a->ao_shader;
    d.name = "ao";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &a->ao));
    render_log_statistics(r, a->ao, "ao");
    memset(&d, 0, sizeof d);
    d.shader = a->denoise_shader;
    d.name = "ao denoise";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &a->denoise));
    render_log_statistics(r, a->denoise, "ao denoise");
    return NPH_RESULT_OK;
}

void render_ao_destroy(NphRenderer *r)
{
    RenderAo *a = &r->ao;
    nph_gpu_pipeline_destroy(r->dev, a->denoise);
    nph_gpu_pipeline_destroy(r->dev, a->ao);
    nph_gpu_pipeline_destroy(r->dev, a->depth);
    nph_gpu_shader_destroy(r->dev, a->denoise_shader);
    nph_gpu_shader_destroy(r->dev, a->ao_shader);
    nph_gpu_shader_destroy(r->dev, a->depth_shader);
    memset(a, 0, sizeof *a);
}

bool render_ao_frame_begin(NphRenderer *r, const NphRenderView *view, const NphMat4 *proj,
                           u32 width, u32 height, bool production)
{
    RenderAo *a = &r->ao;
    a->frame_active = 0;
    // It darkens only the environment's light: what shades with that light
    // shows it where there is any, and the occlusion debug view always.
    const bool environment = !view->disable_ibl && r->env.source != NPH_RENDER_ENV_NONE;
    const bool shown = view->debug_view == NPH_RENDER_VIEW_OCCLUSION ||
                       (environment && (view->debug_view == NPH_RENDER_VIEW_LIT ||
                                        view->debug_view == NPH_RENDER_VIEW_INDIRECT));
    if (!production || !(view->ao_radius > 0.0F) || !shown) {
        return false;
    }
    // A point's view-space position at distance d under the screen position
    // c: d ((ndc + c2) / P_ii, -1) with ndc = 2 c / extent - 1 (P maps
    // (x, z) to ndc x = P00 x / d - c2x: the jitter's and the lens's column).
    NphRenderAoPush *p = &a->frame_push;
    memset(p, 0, sizeof *p);
    const f32 p00 = proj->c[0].x;
    const f32 p11 = proj->c[1].y;
    p->unproject = nph_vec4(2.0F / ((f32)width * p00), 2.0F / ((f32)height * p11),
                            (proj->c[2].x - 1.0F) / p00, (proj->c[2].y - 1.0F) / p11);
    p->project = nph_vec2(0.5F * (f32)width * p00, 0.5F * (f32)height * p11);
    p->znear = view->camera.znear;
    p->radius = view->ao_radius;
    p->radius_pixels = view->ao_radius * 0.5F * (f32)height * nph_f32_abs(p11);
    p->max_radius = (f32)NPH_RENDER_AO_MAX_RADIUS * (f32)height;
    p->width = width;
    p->height = height;
    p->depth_texel = nph_vec2(1.0F / (f32)depth_extent(width), 1.0F / (f32)depth_extent(height));
    // The slices turn only where the temporal resolve averages them: a still
    // view without it keeps one set, which does not shimmer.
    p->noise = r->taa.frame_active ? (u32)(nph_gpu_frame_index(r->dev) % AO_ROTATIONS) : 0U;
    p->point_sampler = nph_gpu_sampler_index(
        r->dev, r->material_samplers[NPH_RENDER_WRAP_CLAMP][NPH_RENDER_WRAP_CLAMP][1]);
    p->linear_sampler = nph_gpu_sampler_index(r->dev, r->clamp_sampler);
    a->frame_active = 1;
    return true;
}

u32 render_ao_index(const NphRenderer *r, NphGpuRenderGraph *graph)
{
    return nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, r->ao.frame_out));
}

static void dispatch(NphGpuCmd *cmd, NphGpuPipeline pipeline, const NphRenderAoPush *p, u32 group)
{
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, p, (u32)sizeof *p);
    nph_gpu_cmd_dispatch(cmd, (p->width + group - 1U) / group, (p->height + group - 1U) / group,
                         1U);
}

static void depth_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderAo *a = &r->ao;
    NphRenderAoPush p = a->frame_push;
    const NphGpuImage depths = nph_gpu_rg_image(graph, a->frame_depths);
    for (u32 m = 0; m < (u32)NPH_RENDER_AO_MIPS; ++m) {
        p.depth_mips[m] = nph_gpu_image_storage_index(r->dev, depths, m);
    }
    // An invocation per texel of mip 2 (ao_depth.comp).
    nph_gpu_cmd_bind_pipeline(cmd, a->depth);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    const u32 w = depth_extent(p.width) / 4U;
    const u32 h = depth_extent(p.height) / 4U;
    nph_gpu_cmd_dispatch(cmd, (w + AO_GROUP - 1U) / AO_GROUP, (h + AO_GROUP - 1U) / AO_GROUP, 1U);
}

static void ao_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderAo *a = &r->ao;
    NphRenderAoPush p = a->frame_push;
    p.depths_index = nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, a->frame_depths));
    p.raw_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, a->frame_raw), 0U);
    dispatch(cmd, a->ao, &p, AO_GROUP);
}

static void denoise_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderAo *a = &r->ao;
    NphRenderAoPush p = a->frame_push;
    p.raw_index = nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, a->frame_raw));
    p.out_index = nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, a->frame_out), 0U);
    // A 2 x 2 block an invocation (ao_denoise.comp).
    dispatch(cmd, a->denoise, &p, 2U * AO_GROUP);
}

static NphGpuRgPass *add_pass(NphGpuRenderGraph *graph, NphRenderer *r, const char *name,
                              NphGpuRgExecuteFn execute)
{
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = name;
    pd.type = NPH_GPU_RG_PASS_COMPUTE;
    pd.execute = execute;
    pd.user = r;
    return nph_gpu_rg_add_pass(graph, &pd);
}

NphResult render_ao_declare_depths(NphRenderer *r, NphGpuRenderGraph *graph)
{
    RenderAo *a = &r->ao;
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = AO_DEPTH_FORMAT;
    id.width = depth_extent(a->frame_push.width);
    id.height = depth_extent(a->frame_push.height);
    id.mip_count = (u32)NPH_RENDER_AO_MIPS;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    id.name = "ao depths";
    a->frame_depths = nph_gpu_rg_create_image(graph, &id);
    return nph_gpu_rg_image_is_null(a->frame_depths) ? NPH_RESULT_FULL : NPH_RESULT_OK;
}

NphResult render_ao_declare(NphRenderer *r, NphGpuRenderGraph *graph)
{
    RenderAo *a = &r->ao;
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    id.width = a->frame_push.width;
    id.height = a->frame_push.height;
    id.mip_count = 1U;
    id.format = AO_PACKED_FORMAT;
    id.name = "ao raw";
    a->frame_raw = nph_gpu_rg_create_image(graph, &id);
    id.format = AO_OUT_FORMAT;
    id.name = "ao";
    a->frame_out = nph_gpu_rg_create_image(graph, &id);
    if (nph_gpu_rg_image_is_null(a->frame_raw) || nph_gpu_rg_image_is_null(a->frame_out)) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgPass *p = add_pass(graph, r, "ao depth", depth_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, a->frame_depths, NPH_GPU_RG_USE_STORAGE_READ, 1U, 1U);
    nph_gpu_rg_write_image(p, a->frame_depths, NPH_GPU_RG_USE_STORAGE_WRITE, 2U,
                           (u32)NPH_RENDER_AO_MIPS - 2U);
    p = add_pass(graph, r, "ao", ao_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, a->frame_depths, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_write_image(p, a->frame_raw, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    p = add_pass(graph, r, "ao denoise", denoise_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, a->frame_raw, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_write_image(p, a->frame_out, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    return NPH_RESULT_OK;
}

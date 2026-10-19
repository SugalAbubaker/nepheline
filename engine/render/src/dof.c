// SPDX-License-Identifier: Apache-2.0
// Depth of field on both pipelines (render/renderer.h NphRenderView.aperture;
// docs/render.md "Depth of field"; docs/adr/0037-depth-of-field.md).
//
// A thin lens of focal length f and f-number N, focused at z_f, blurs a
// point z away into a circle of confusion on the sensor of diameter
// f^2 |z - z_f| / (N z (z_f - f)) (Potmesil & Chakravarty, "A Lens and
// Aperture Camera Model for Synthetic Image Generation", SIGGRAPH 1981). In
// pixels, signed negative in front of the focus, the radius is
// K (1 - z_f / z) with K = f^2 H / (2 N (z_f - f) h) for a sensor h tall
// and H pixels; under the reverse-Z depth d = znear / z it is linear in d.
// After the scene and before bloom (which then glows from the blurred
// image) and the final pass (whose temporal resolve averages the gathers'
// turning spirals), four passes make an HDR image with depth of field from
// the one drawn: the setup (each pixel's depth, the half-resolution colour,
// each tile's extremes), the neighbourhoods, the gather at half resolution
// for the tiles with large circles, and the resolve, which copies the tiles
// in focus, gathers those slightly out of focus at full resolution, and
// composites the rest from the gather's layers.
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderDofPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");

// A full-frame sensor's height, where the view leaves it zero.
#define DOF_SENSOR 0.024F

// The half-resolution colour and depth, the layers, and the tiles; the
// depths at full resolution in half floats where they can be stored to
// (rounded within 2^-11 of themselves: at the focus, within the circle at
// infinity over 2048 of the circle).
#define DOF_FORMAT NPH_GPU_FORMAT_R16G16B16A16_SFLOAT

// The resolve's workgroup: a quarter of a tile (dof_resolve.comp).
#define DOF_RESOLVE_GROUP ((u32)NPH_RENDER_DOF_TILE / 2U)

f32 nph_render_view_focal_length(const NphRenderView *view)
{
    const f32 sensor = view->sensor_height > 0.0F ? view->sensor_height : DOF_SENSOR;
    return sensor / (2.0F * nph_f32_tan(0.5F * view->camera.yfov));
}

NphResult render_dof_create(NphRenderer *r)
{
    RenderDof *d = &r->dof;
    static const char *const files[4] = {"dof_setup.comp", "dof_tiles.comp", "dof_gather.comp",
                                         "dof_resolve.comp"};
    for (u32 i = 0; i < 4U; ++i) {
        NPH_TRY(render_load_shader_file(r, files[i], &d->shaders[i]));
    }
    const u32 need = (u32)NPH_GPU_FORMAT_FEATURE_STORAGE | (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED;
    d->full_depth_format = nph_gpu_format_supports(r->dev, NPH_GPU_FORMAT_R16_SFLOAT, need)
                               ? NPH_GPU_FORMAT_R16_SFLOAT
                               : NPH_GPU_FORMAT_R32_SFLOAT;
    static const char *const setup_names[2] = {"dof setup (depth image)",
                                               "dof setup (visibility buffer)"};
    static const char *const resolve_names[2] = {"dof resolve", "dof resolve (dithered)"};
    NphGpuComputePipelineDesc p;
    for (u32 v = 0; v < 2U; ++v) {
        memset(&p, 0, sizeof p);
        p.shader = d->shaders[0];
        p.specialization[0] = v;
        p.specialization_count = 1U;
        p.name = setup_names[v];
        NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &p, &d->setup[v]));
        memset(&p, 0, sizeof p);
        p.shader = d->shaders[3];
        p.specialization[0] = v;
        p.specialization_count = 1U;
        p.name = resolve_names[v];
        NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &p, &d->resolve[v]));
    }
    memset(&p, 0, sizeof p);
    p.shader = d->shaders[1];
    p.name = "dof tiles";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &p, &d->tiles));
    memset(&p, 0, sizeof p);
    p.shader = d->shaders[2];
    p.name = "dof gather";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &p, &d->gather));
    render_log_statistics(r, d->gather, "dof gather");
    render_log_statistics(r, d->resolve[1], "dof resolve");
    return NPH_RESULT_OK;
}

void render_dof_destroy(NphRenderer *r)
{
    RenderDof *d = &r->dof;
    for (u32 v = 0; v < 2U; ++v) {
        nph_gpu_pipeline_destroy(r->dev, d->setup[v]);
        nph_gpu_pipeline_destroy(r->dev, d->resolve[v]);
    }
    nph_gpu_pipeline_destroy(r->dev, d->tiles);
    nph_gpu_pipeline_destroy(r->dev, d->gather);
    for (u32 i = 0; i < 4U; ++i) {
        nph_gpu_shader_destroy(r->dev, d->shaders[i]);
    }
    memset(d, 0, sizeof *d);
}

bool render_dof_frame_begin(NphRenderer *r, const NphRenderView *view, u32 height)
{
    RenderDof *d = &r->dof;
    d->frame_active = 0;
    if (!(view->aperture > 0.0F) || view->debug_view != NPH_RENDER_VIEW_LIT) {
        return false;
    }
    const f32 sensor = view->sensor_height > 0.0F ? view->sensor_height : DOF_SENSOR;
    const f32 f = nph_render_view_focal_length(view);
    const f32 focus = view->focus_distance;
    // The radius at infinity in pixels, and the circle as bias + scale *
    // depth: K (1 - z_f / z) with 1 / z = d / znear.
    const f32 k = 0.5F * f * f * (f32)height / (view->aperture * (focus - f) * sensor);
    d->frame_bias = k;
    d->frame_scale = -k * focus / view->camera.znear;
    d->frame_active = 1;
    return true;
}

static NphRenderDofPush dof_push(const NphRenderer *r)
{
    const RenderDof *d = &r->dof;
    NphGpuDevice *dev = r->dev;
    NphRenderDofPush p;
    memset(&p, 0, sizeof p);
    p.coc_bias = d->frame_bias;
    p.coc_scale = d->frame_scale;
    p.point_sampler = nph_gpu_sampler_index(
        dev, r->material_samplers[NPH_RENDER_WRAP_CLAMP][NPH_RENDER_WRAP_CLAMP][1]);
    p.linear_sampler = nph_gpu_sampler_index(dev, r->clamp_sampler);
    p.width = d->frame_width;
    p.height = d->frame_height;
    p.tiles_x = d->frame_tiles_x;
    p.tiles_y = d->frame_tiles_y;
    p.noise = (u32)(nph_gpu_frame_index(dev) % 64U);
    p.inv_size = nph_vec2(1.0F / (f32)d->frame_width, 1.0F / (f32)d->frame_height);
    return p;
}

static u32 sampled(const NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage image)
{
    return nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, image));
}

static u32 storage(const NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage image)
{
    return nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, image), 0U);
}

static void setup_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderDof *d = &r->dof;
    NphRenderDofPush p = dof_push(r);
    p.depth_index = sampled(r, graph, d->frame_depth);
    p.color_index = sampled(r, graph, d->frame_hdr);
    p.half_index = storage(r, graph, d->frame_half);
    p.full_depth_index = storage(r, graph, d->frame_full_depth);
    p.tiles_index = storage(r, graph, d->frame_tiles);
    nph_gpu_cmd_bind_pipeline(cmd, d->setup[d->frame_visbuffer ? 1U : 0U]);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    // A workgroup per tile.
    nph_gpu_cmd_dispatch(cmd, d->frame_tiles_x, d->frame_tiles_y, 1U);
}

static void tiles_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderDof *d = &r->dof;
    NphRenderDofPush p = dof_push(r);
    p.tiles_index = storage(r, graph, d->frame_tiles);
    p.neighbours_index = storage(r, graph, d->frame_neighbours);
    nph_gpu_cmd_bind_pipeline(cmd, d->tiles);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    nph_gpu_cmd_dispatch(cmd, (d->frame_tiles_x + 7U) / 8U, (d->frame_tiles_y + 7U) / 8U, 1U);
}

static void gather_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderDof *d = &r->dof;
    NphRenderDofPush p = dof_push(r);
    p.half_index = sampled(r, graph, d->frame_half);
    p.neighbours_index = sampled(r, graph, d->frame_neighbours);
    p.far_index = storage(r, graph, d->frame_far);
    p.near_index = storage(r, graph, d->frame_near);
    nph_gpu_cmd_bind_pipeline(cmd, d->gather);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    // A workgroup per tile.
    nph_gpu_cmd_dispatch(cmd, d->frame_tiles_x, d->frame_tiles_y, 1U);
}

static void resolve_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderDof *d = &r->dof;
    NphRenderDofPush p = dof_push(r);
    p.color_index = sampled(r, graph, d->frame_hdr);
    p.full_depth_index = sampled(r, graph, d->frame_full_depth);
    p.neighbours_index = sampled(r, graph, d->frame_neighbours);
    p.far_index = sampled(r, graph, d->frame_far);
    p.near_index = sampled(r, graph, d->frame_near);
    p.out_index = storage(r, graph, d->frame_out);
    nph_gpu_cmd_bind_pipeline(cmd, d->resolve[d->frame_hdr_format == RENDER_HDR_COMPACT ? 1U : 0U]);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    nph_gpu_cmd_dispatch(cmd, (d->frame_width + DOF_RESOLVE_GROUP - 1U) / DOF_RESOLVE_GROUP,
                         (d->frame_height + DOF_RESOLVE_GROUP - 1U) / DOF_RESOLVE_GROUP, 1U);
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

NphResult render_dof_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage hdr,
                             NphGpuFormat hdr_format, NphGpuRgImage depth, bool visbuffer,
                             u32 width, u32 height, NphGpuRgImage *out)
{
    RenderDof *d = &r->dof;
    d->frame_hdr = hdr;
    d->frame_hdr_format = hdr_format;
    d->frame_depth = depth;
    d->frame_visbuffer = visbuffer ? 1 : 0;
    d->frame_width = width;
    d->frame_height = height;
    const u32 hw = (width + 1U) / 2U;
    const u32 hh = (height + 1U) / 2U;
    const u32 tile = (u32)NPH_RENDER_DOF_TILE / 2U; // in half-resolution pixels
    d->frame_tiles_x = (hw + tile - 1U) / tile;
    d->frame_tiles_y = (hh + tile - 1U) / tile;
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = DOF_FORMAT;
    id.width = hw;
    id.height = hh;
    id.mip_count = 1U;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    id.name = "dof half";
    d->frame_half = nph_gpu_rg_create_image(graph, &id);
    id.name = "dof far";
    d->frame_far = nph_gpu_rg_create_image(graph, &id);
    id.name = "dof near";
    d->frame_near = nph_gpu_rg_create_image(graph, &id);
    id.width = d->frame_tiles_x;
    id.height = d->frame_tiles_y;
    id.name = "dof tiles";
    d->frame_tiles = nph_gpu_rg_create_image(graph, &id);
    id.name = "dof neighbourhoods";
    d->frame_neighbours = nph_gpu_rg_create_image(graph, &id);
    id.format = d->full_depth_format;
    id.width = width;
    id.height = height;
    id.name = "dof depths";
    d->frame_full_depth = nph_gpu_rg_create_image(graph, &id);
    // What the frame's later passes read as its HDR image: sampled.
    id.format = hdr_format;
    id.name = "dof hdr colour";
    d->frame_out = nph_gpu_rg_create_image(graph, &id);
    if (nph_gpu_rg_image_is_null(d->frame_half) || nph_gpu_rg_image_is_null(d->frame_far) ||
        nph_gpu_rg_image_is_null(d->frame_near) || nph_gpu_rg_image_is_null(d->frame_tiles) ||
        nph_gpu_rg_image_is_null(d->frame_neighbours) ||
        nph_gpu_rg_image_is_null(d->frame_full_depth) || nph_gpu_rg_image_is_null(d->frame_out)) {
        return NPH_RESULT_FULL;
    }

    NphGpuRgPass *p = add_pass(graph, r, "dof setup", setup_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, hdr, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, depth, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_half, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_full_depth, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_tiles, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    p = add_pass(graph, r, "dof tiles", tiles_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, d->frame_tiles, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_neighbours, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    p = add_pass(graph, r, "dof gather", gather_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, d->frame_half, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, d->frame_neighbours, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_far, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_near, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    p = add_pass(graph, r, "dof resolve", resolve_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, hdr, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, d->frame_full_depth, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, d->frame_neighbours, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, d->frame_far, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, d->frame_near, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_write_image(p, d->frame_out, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    *out = d->frame_out;
    return NPH_RESULT_OK;
}

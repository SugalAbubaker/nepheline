// SPDX-License-Identifier: Apache-2.0
// The production path's motion blur (render/renderer.h NphRenderView.shutter;
// docs/render.md "Motion blur"; docs/adr/0036-motion-blur.md).
//
// A frame that blurs measures every pixel's motion since the previous frame
// from the visibility buffer, as the temporal resolve does, and keeps each
// tile's longest blur (motion.comp); each tile then takes the dominant blur
// of the tiles around it (motion_tiles.comp); and every pixel gathers along
// those blurs (motion_blur.frag; Guertin, McGuire & Nowrouzezahrai, "A Fast
// and Stable Feature-Aware Motion Blur Filter", HPG 2014), fused with the
// tonemap into the caller's target. It blurs after the temporal resolve,
// whose history it reads: the resolve then sees the frame's surfaces sharp,
// as its reprojection and clipping assume, and no frame's blur is carried
// into the next one's history.
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderMotionPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(sizeof(NphRenderMotionBlurPush) <= NPH_GPU_PUSH_CONSTANT_BYTES,
                  "push constants fit");

// A view's zero frame time: a frame at the §10.1 target (post.c's too).
#define MOTION_SECONDS (1.0F / 240.0F)

// The motion image: each pixel's blur and view distance packed in a word
// (motion_common.glsl). The tiles: a blur and its spread in half floats.
#define MOTION_FORMAT NPH_GPU_FORMAT_R32_UINT
#define TILES_FORMAT  NPH_GPU_FORMAT_R16G16B16A16_SFLOAT

NphResult render_motion_create(NphRenderer *r)
{
    RenderMotion *m = &r->motion;
    NPH_TRY(render_load_shader_file(r, "motion.comp", &m->motion_shader));
    NPH_TRY(render_load_shader_file(r, "motion_tiles.comp", &m->tiles_shader));
    NPH_TRY(render_load_shader_file(r, "motion_blur.frag", &m->blur_shader));
    NphGpuComputePipelineDesc d;
    memset(&d, 0, sizeof d);
    d.shader = m->motion_shader;
    d.name = "motion";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &m->motion));
    render_log_statistics(r, m->motion, "motion");
    memset(&d, 0, sizeof d);
    d.shader = m->tiles_shader;
    d.name = "motion tiles";
    return nph_gpu_pipeline_create_compute(r->dev, &d, &m->tiles);
}

void render_motion_destroy(NphRenderer *r)
{
    RenderMotion *m = &r->motion;
    nph_gpu_pipeline_destroy(r->dev, m->tiles);
    nph_gpu_pipeline_destroy(r->dev, m->motion);
    nph_gpu_shader_destroy(r->dev, m->blur_shader);
    nph_gpu_shader_destroy(r->dev, m->tiles_shader);
    nph_gpu_shader_destroy(r->dev, m->motion_shader);
    memset(m, 0, sizeof *m);
}

NphResult render_motion_target_pipeline(NphRenderer *r, RenderTargetPipelines *t,
                                        NphGpuFormat format)
{
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_FULLSCREEN_VERT];
    d.fragment = r->motion.blur_shader;
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.name = "motion blur";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->motion_blur));
    render_log_statistics(r, t->motion_blur, "motion blur");
    return NPH_RESULT_OK;
}

bool render_motion_frame_begin(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                               bool production)
{
    RenderMotion *m = &r->motion;
    m->frame_active = 0;
    if (!production || !(view->shutter > 0.0F) || view->debug_view != NPH_RENDER_VIEW_LIT ||
        view->camera_cut) {
        return false;
    }
    // The motion is measured against the previous frame's view: it must be
    // this path's, the frame just before, in this extent.
    const RenderVis *v = &r->vis;
    if (v->prev_frame != nph_gpu_frame_index(r->dev) || v->target_width != width ||
        v->target_height != height) {
        return false;
    }
    const f32 seconds = view->frame_seconds > 0.0F ? view->frame_seconds : MOTION_SECONDS;
    // The shutter is open over the frame's instant, half before and half
    // after: the blur reaches half the motion in that time either way.
    m->frame_scale = 0.5F * view->shutter / seconds;
    m->frame_active = 1;
    return true;
}

static NphRenderMotionPush motion_push(const NphRenderer *r, NphGpuRenderGraph *graph)
{
    const RenderMotion *m = &r->motion;
    NphRenderMotionPush p;
    memset(&p, 0, sizeof p);
    p.view = r->vis.frame.view_address;
    p.scale = m->frame_scale;
    p.tiles_index =
        nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, m->frame_tiles), 0U);
    p.tiles_x = m->frame_tiles_x;
    p.tiles_y = m->frame_tiles_y;
    return p;
}

static void motion_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderMotion *m = &r->motion;
    NphRenderMotionPush p = motion_push(r, graph);
    p.visbuffer_index =
        nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, r->vis.frame.visbuffer));
    p.point_sampler = nph_gpu_sampler_index(
        r->dev, r->material_samplers[NPH_RENDER_WRAP_CLAMP][NPH_RENDER_WRAP_CLAMP][1]);
    p.motion_index =
        nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, m->frame_motion), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, m->motion);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    // A workgroup per tile.
    nph_gpu_cmd_dispatch(cmd, m->frame_tiles_x, m->frame_tiles_y, 1U);
}

static void tiles_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderMotion *m = &r->motion;
    NphRenderMotionPush p = motion_push(r, graph);
    p.neighbours_index =
        nph_gpu_image_storage_index(r->dev, nph_gpu_rg_image(graph, m->frame_neighbours), 0U);
    nph_gpu_cmd_bind_pipeline(cmd, m->tiles);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    // An invocation per tile, 8 x 8 a workgroup.
    nph_gpu_cmd_dispatch(cmd, (m->frame_tiles_x + 7U) / 8U, (m->frame_tiles_y + 7U) / 8U, 1U);
}

static void blur_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderMotion *m = &r->motion;
    NphGpuDevice *dev = r->dev;
    const VisFrame *f = &r->vis.frame;
    NphRenderMotionBlurPush p;
    memset(&p, 0, sizeof p);
    p.color_index = nph_gpu_image_sampled_index(dev, nph_gpu_rg_image(graph, m->frame_color));
    p.motion_index = nph_gpu_image_sampled_index(dev, nph_gpu_rg_image(graph, m->frame_motion));
    p.neighbours_index =
        nph_gpu_image_sampled_index(dev, nph_gpu_rg_image(graph, m->frame_neighbours));
    p.tonemapper = r->tonemap_push.tonemapper;
    p.encode_srgb = r->tonemap_push.encode_srgb;
    p.noise = (u32)(nph_gpu_frame_index(dev) % 64U);
    const RenderPostComposite c = render_post_composite(r, graph);
    p.sampler_index = c.sampler_index;
    if (m->frame_glare) {
        p.bloom_index = c.bloom_index;
        p.bloom = c.bloom;
        p.bloom_scale = c.bloom_scale;
        p.bloom_uv = c.bloom_uv;
    }
    p.width = f->width;
    p.height = f->height;
    p.inv_size = nph_vec2(1.0F / (f32)f->width, 1.0F / (f32)f->height);
    nph_gpu_cmd_bind_pipeline(cmd, r->frame_target->motion_blur);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
}

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

NphResult render_motion_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage out,
                                NphGpuRgImage color, bool glare)
{
    RenderMotion *m = &r->motion;
    const VisFrame *f = &r->vis.frame;
    m->frame_color = color;
    m->frame_glare = glare ? 1 : 0;
    m->frame_tiles_x = (f->width + (u32)NPH_RENDER_MB_TILE - 1U) / (u32)NPH_RENDER_MB_TILE;
    m->frame_tiles_y = (f->height + (u32)NPH_RENDER_MB_TILE - 1U) / (u32)NPH_RENDER_MB_TILE;
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = MOTION_FORMAT;
    d.width = f->width;
    d.height = f->height;
    d.mip_count = 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    d.name = "motion";
    m->frame_motion = nph_gpu_rg_create_image(graph, &d);
    d.format = TILES_FORMAT;
    d.width = m->frame_tiles_x;
    d.height = m->frame_tiles_y;
    d.name = "motion tiles";
    m->frame_tiles = nph_gpu_rg_create_image(graph, &d);
    d.name = "motion neighbourhoods";
    m->frame_neighbours = nph_gpu_rg_create_image(graph, &d);
    if (nph_gpu_rg_image_is_null(m->frame_motion) || nph_gpu_rg_image_is_null(m->frame_tiles) ||
        nph_gpu_rg_image_is_null(m->frame_neighbours)) {
        return NPH_RESULT_FULL;
    }

    // Every pixel's motion, from the visibility buffer and the GPU scene's
    // transforms, and every tile's longest blur.
    NphGpuRgPass *p = add_pass(graph, r, "motion", NPH_GPU_RG_PASS_COMPUTE, motion_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, f->visbuffer, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_buffer(p, f->clusters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->prev_transforms, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_write_image(p, m->frame_motion, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
    nph_gpu_rg_write_image(p, m->frame_tiles, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    // Every tile's neighbourhood's dominant blur.
    p = add_pass(graph, r, "motion tiles", NPH_GPU_RG_PASS_COMPUTE, tiles_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(p, m->frame_tiles, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    nph_gpu_rg_write_image(p, m->frame_neighbours, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    // The gather and the tonemap, into the target.
    p = add_pass(graph, r, "motion blur", NPH_GPU_RG_PASS_GRAPHICS, blur_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_DONT_CARE; // every pixel is written
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_color_attachment(p, out, &a);
    nph_gpu_rg_read_image(p, color, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, m->frame_motion, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, m->frame_neighbours, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    if (glare) {
        render_post_read(r, p);
    }
    return NPH_RESULT_OK;
}

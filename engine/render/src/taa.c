// SPDX-License-Identifier: Apache-2.0
// The production path's temporal anti-aliasing (render/renderer.h
// NPH_RENDER_AA_TEMPORAL; docs/render.md "Temporal anti-aliasing";
// docs/adr/0034-temporal-anti-aliasing.md): the jitter, the history, and
// the temporal resolve, fused with the tonemap (shaders/render/taa.frag).
//
// A frame that resolves temporally renders with its projection moved by
// the next of RENDER_TAA_PHASES sub-pixel offsets of a Halton (2, 3)
// sequence (Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014),
// every pass alike: culling and the light grid move their rectangles by
// it, so nothing is culled or binned against a position the raster does
// not draw at. The resolve writes the display colour and the next history;
// the two history images take turns. The history continues only from the
// frame just before, resolved temporally into the same extent, without a
// camera cut; otherwise the frame stands alone (its filtered samples).
#include <nepheline/core/log.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderTaaPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");

// The radical inverse of `i` in `base`: the Halton sequence's point i.
static f32 halton(u32 i, u32 base)
{
    f32 f = 1.0F;
    f32 r = 0.0F;
    while (i > 0U) {
        f /= (f32)base;
        r += f * (f32)(i % base);
        i /= base;
    }
    return r;
}

NphVec2 render_taa_jitter(u64 sequence)
{
    // Points 1 to RENDER_TAA_PHASES: point 0 is the origin.
    const u32 i = (u32)(sequence % RENDER_TAA_PHASES) + 1U;
    return nph_vec2(halton(i, 2U) - 0.5F, halton(i, 3U) - 0.5F);
}

void render_taa_weights(NphVec2 jitter, f32 out[9])
{
    f32 sum = 0.0F;
    for (u32 i = 0; i < 9U; ++i) {
        const f32 dx = (f32)((i32)(i % 3U) - 1) - jitter.x;
        const f32 dy = (f32)((i32)(i / 3U) - 1) - jitter.y;
        // taa_filter_weight: a Gaussian with a box filter's spread.
        out[i] = nph_f32_exp(-6.0F * (dx * dx + dy * dy));
        sum += out[i];
    }
    for (u32 i = 0; i < 9U; ++i) {
        out[i] /= sum;
    }
}

NphResult render_taa_create(NphRenderer *r)
{
    RenderTaa *t = &r->taa;
    NPH_TRY(render_load_shader_file(r, "taa.frag", &t->shader));
    NphGpuSamplerDesc s;
    nph_gpu_sampler_desc_default(&s);
    s.mip_filter = NPH_GPU_FILTER_NEAREST;
    s.address_u = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    s.address_v = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    s.address_w = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    s.name = "taa history";
    return nph_gpu_sampler_create(r->dev, &s, &t->sampler);
}

static void destroy_history(NphRenderer *r)
{
    RenderTaa *t = &r->taa;
    for (u32 i = 0; i < 2U; ++i) {
        nph_gpu_image_destroy(r->dev, t->history[i]); // deferred past the frames in flight
        t->history[i] = NPH_GPU_IMAGE_NULL;
    }
    t->width = 0U;
    t->height = 0U;
    t->valid = 0;
}

void render_taa_destroy(NphRenderer *r)
{
    RenderTaa *t = &r->taa;
    destroy_history(r);
    nph_gpu_sampler_destroy(r->dev, t->sampler);
    nph_gpu_shader_destroy(r->dev, t->shader);
    memset(t, 0, sizeof *t);
}

NphResult render_taa_target_pipeline(NphRenderer *r, RenderTargetPipelines *t, NphGpuFormat format)
{
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_FULLSCREEN_VERT];
    d.fragment = r->taa.shader;
    d.color_formats[0] = format;
    d.color_formats[1] = r->vis.hdr_format;
    d.color_count = 2U;
    // Whether the history is B10G11R11 and dithered (taa.frag k_dither).
    d.specialization[0] = r->vis.hdr_format == RENDER_HDR_COMPACT ? 1U : 0U;
    d.specialization_count = 1U;
    d.name = "taa";
    return nph_gpu_pipeline_create_graphics(r->dev, &d, &t->taa);
}

// The history images for a target of `width` x `height`.
static NphResult ensure_history(NphRenderer *r, u32 width, u32 height)
{
    RenderTaa *t = &r->taa;
    if (!nph_gpu_image_is_null(t->history[0]) && t->width == width && t->height == height) {
        return NPH_RESULT_OK;
    }
    destroy_history(r);
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = r->vis.hdr_format;
    d.width = width;
    d.height = height;
    d.mip_count = 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    d.name = "taa history";
    for (u32 i = 0; i < 2U; ++i) {
        const NphResult res = nph_gpu_image_create(r->dev, &d, &t->history[i]);
        if (res != NPH_RESULT_OK) {
            destroy_history(r);
            return res;
        }
    }
    t->format = d.format;
    t->width = width;
    t->height = height;
    return NPH_RESULT_OK;
}

NphResult render_taa_frame_begin(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                                 bool production, NphVec2 *out_jitter)
{
    RenderTaa *t = &r->taa;
    t->frame_active = 0;
    t->frame_history = 0;
    t->frame_jitter = nph_vec2(0.0F, 0.0F);
    *out_jitter = t->frame_jitter;
    if (!production || view->anti_aliasing != NPH_RENDER_AA_TEMPORAL ||
        view->debug_view != NPH_RENDER_VIEW_LIT) {
        // The history no longer follows the frames.
        t->valid = 0;
        return NPH_RESULT_OK;
    }
    NPH_TRY(ensure_history(r, width, height));
    const u64 frame = nph_gpu_frame_index(r->dev);
    t->frame_active = 1;
    t->frame_history = t->valid && t->last_frame == frame && !view->camera_cut ? 1 : 0;
    t->frame_jitter = render_taa_jitter(t->sequence);
    *out_jitter = t->frame_jitter;
    return NPH_RESULT_OK;
}

static void taa_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    NphRenderTaaPush p = r->taa.push;
    p.hdr_index = nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, r->hdr_rg));
    p.visbuffer_index = render_vis_visbuffer_index(r, graph);
    nph_gpu_cmd_bind_pipeline(cmd, r->frame_target->taa);
    nph_gpu_cmd_push_constants(cmd, &p, (u32)sizeof p);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
}

// The debug lines over the resolved image (render/debug_draw.h): the
// depth-tested ones against the visibility buffer, then the overlay.
static void lines_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderTargetPipelines *t = r->frame_target;
    NphRenderPush push = r->push;
    push.flags = r->tonemap_push.encode_srgb != 0U ? 0U : (u32)NPH_RENDER_PUSH_LINEAR_OUTPUT;
    if (r->debug_draw_counts[0] != 0U) {
        push.visbuffer_index = render_vis_visbuffer_index(r, graph);
        nph_gpu_cmd_bind_pipeline(cmd, t->vis_debug_depth);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw(cmd, r->debug_draw_counts[0] * 2U, 1U, 0U, 0U);
    }
    if (r->debug_draw_counts[1] != 0U) {
        nph_gpu_cmd_bind_pipeline(cmd, t->vis_debug_overlay);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw(cmd, r->debug_draw_counts[1] * 2U, 1U, r->desc.debug_lines * 2U, 0U);
    }
}

static NphGpuRgPass *add_pass(NphGpuRenderGraph *graph, NphRenderer *r, const char *name,
                              NphGpuRgExecuteFn execute)
{
    NphGpuRgPassDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = NPH_GPU_RG_PASS_GRAPHICS;
    d.execute = execute;
    d.user = r;
    return nph_gpu_rg_add_pass(graph, &d);
}

NphResult render_taa_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage out,
                             f32 exposure)
{
    RenderTaa *t = &r->taa;
    const VisFrame *f = &r->vis.frame;
    NphGpuDevice *dev = r->dev;
    const NphGpuImage last = t->history[t->write ^ 1U];
    const NphGpuRgImage next = nph_gpu_rg_import_image(graph, t->history[t->write]);
    const NphGpuRgImage prev =
        t->frame_history ? nph_gpu_rg_import_image(graph, last) : NPH_GPU_RG_IMAGE_NULL;
    NphGpuRgPass *p = add_pass(graph, r, "taa", taa_execute);
    if (p == NULL || nph_gpu_rg_image_is_null(next) ||
        (t->frame_history && nph_gpu_rg_image_is_null(prev))) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_DONT_CARE; // every pixel is written
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_color_attachment(p, out, &a);
    nph_gpu_rg_color_attachment(p, next, &a);
    nph_gpu_rg_read_image(p, r->hdr_rg, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_image(p, f->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
    nph_gpu_rg_read_buffer(p, f->clusters, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, f->prev_transforms, NPH_GPU_RG_USE_SHADER_READ);
    if (t->frame_history) {
        nph_gpu_rg_read_image(p, prev, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    }

    NphRenderTaaPush *push = &t->push;
    memset(push, 0, sizeof *push);
    push->view = f->view_address;
    f32 w[9];
    render_taa_weights(t->frame_jitter, w);
    push->weights0 = nph_vec4(w[0], w[1], w[2], w[3]);
    push->weights1 = nph_vec4(w[4], w[5], w[6], w[7]);
    push->weight8 = w[8];
    // The history was resolved with its frame's exposure: brought to this one's.
    push->history_scale = t->frame_history ? exposure / t->exposure : 0.0F;
    push->jitter = nph_vec2(t->frame_jitter.x / (f32)t->width, t->frame_jitter.y / (f32)t->height);
    push->history_index = nph_gpu_image_sampled_index(dev, last);
    push->sampler_index = nph_gpu_sampler_index(dev, t->sampler);
    push->tonemapper = r->tonemap_push.tonemapper;
    push->encode_srgb = r->tonemap_push.encode_srgb;
    push->noise = (u32)(t->sequence % 64U);

    if (r->debug_draw_counts[0] != 0U || r->debug_draw_counts[1] != 0U) {
        NphGpuRgPass *lines = add_pass(graph, r, "debug lines", lines_execute);
        if (lines == NULL) {
            return NPH_RESULT_FULL;
        }
        a.load = NPH_GPU_LOAD_OP_LOAD;
        nph_gpu_rg_color_attachment(lines, out, &a);
        if (r->debug_draw_counts[0] != 0U) {
            nph_gpu_rg_read_image(lines, f->visbuffer, NPH_GPU_RG_USE_STORAGE_READ, 0U, 1U);
        }
    }

    // The next frame continues from this one.
    t->exposure = exposure;
    t->write ^= 1U;
    t->valid = 1;
    t->last_frame = nph_gpu_frame_index(dev) + 1U;
    t->sequence += 1U;
    return NPH_RESULT_OK;
}

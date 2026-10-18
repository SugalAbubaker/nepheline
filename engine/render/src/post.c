// SPDX-License-Identifier: Apache-2.0
// Bloom and the exposure (render/renderer.h NphRenderView.bloom and
// .exposure; docs/render.md "Bloom and exposure";
// docs/adr/0035-bloom-and-exposure.md).
//
// Bloom is the HDR image's glare: a chain of mips from half the frame's
// size, each the 13-tap downsample of the one before (Jimenez, "Next
// Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH
// 2014), then summed back up through 3 x 3 tents into mip 0, which the
// final pass (the tonemap, or the temporal resolve) mixes into the image.
// The first downsample also counts the frame's luminance into a histogram
// when the exposure is metered; a one-workgroup pass averages it between
// its percentiles, and the CPU reads that back NPH_GPU_FRAMES_IN_FLIGHT
// frames later and moves the exposure toward it at the view's speed, so
// every shader keeps reading the exposure from the view record.
#include <nepheline/core/log.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <stddef.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderPostPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(sizeof(NphRenderExposureData) == sizeof(u32) * (NPH_RENDER_EXPOSURE_BINS + 4U),
                  "NphRenderExposureData matches its GLSL layout");

// The EV100 range an automatic exposure stays in (the Camera component's).
#define POST_MIN_EV100 (-6.0F)
#define POST_MAX_EV100 20.0F
// Defaults for the view's zeros.
#define POST_SPEED   1.5F            // e-foldings per second
#define POST_SECONDS (1.0F / 240.0F) // a frame at the §10.1 target

void render_exposure_average(const u32 *histogram, f32 *out_log2, u32 *out_count)
{
    u32 total = 0U;
    for (u32 b = 0; b < (u32)NPH_RENDER_EXPOSURE_BINS; ++b) {
        total += histogram[b];
    }
    // As exposure.comp: the part of each bin between the fractions.
    const f32 lo = (f32)total * (f32)NPH_RENDER_EXPOSURE_LOW;
    const f32 hi = (f32)total * (f32)NPH_RENDER_EXPOSURE_HIGH;
    const f32 bin_log2 = (f32)NPH_RENDER_EXPOSURE_RANGE_LOG2 / (f32)NPH_RENDER_EXPOSURE_BINS;
    f32 below = 0.0F;
    f32 sum = 0.0F;
    f32 weight = 0.0F;
    for (u32 b = 0; b < (u32)NPH_RENDER_EXPOSURE_BINS; ++b) {
        const f32 n = (f32)histogram[b];
        const f32 take = nph_f32_max(nph_f32_min(below + n, hi) - nph_f32_max(below, lo), 0.0F);
        sum += take * ((f32)NPH_RENDER_EXPOSURE_MIN_LOG2 + ((f32)b + 0.5F) * bin_log2);
        weight += take;
        below += n;
    }
    *out_log2 = weight > 0.0F ? sum / weight : (f32)NPH_RENDER_EXPOSURE_MIN_LOG2;
    *out_count = total;
}

f32 render_exposure_target_ev100(f32 average_log2, f32 exposure, f32 compensation)
{
    // The metered pixels' mean log2 luminance, without the pre-exposure,
    // as the EV100 that exposes it as an averaging meter would: EV100 =
    // log2(L S / K), with S = 100 and K = 12.5 (Lagarde & de Rousiers,
    // "Moving Frostbite to PBR", SIGGRAPH 2014, §5.1); a positive
    // compensation is brighter, a lower EV100.
    const f32 log2_luminance = average_log2 - nph_f32_log2(exposure);
    const f32 ev100 = log2_luminance + nph_f32_log2(100.0F / 12.5F) - compensation;
    return nph_f32_clamp(ev100, POST_MIN_EV100, POST_MAX_EV100);
}

NphResult render_post_create(NphRenderer *r)
{
    RenderPost *p = &r->post;
    NPH_TRY(render_load_shader_file(r, "bloom.comp", &p->bloom_shader));
    NPH_TRY(render_load_shader_file(r, "exposure.comp", &p->exposure_shader));
    // The chain in B10G11R11 where it can be stored to and filtered: half
    // the bytes of RGBA16F, rounded at random so its energy holds.
    const u32 need =
        (u32)NPH_GPU_FORMAT_FEATURE_STORAGE | (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED_LINEAR;
    p->format = nph_gpu_format_supports(r->dev, RENDER_HDR_COMPACT, need) ? RENDER_HDR_COMPACT
                                                                          : RENDER_HDR_FORMAT;
    static const char *const names[NPH_RENDER_POST_UP + 1] = {"bloom down first", "bloom down",
                                                              "bloom up"};
    NphGpuComputePipelineDesc d;
    for (u32 mode = 0; mode <= (u32)NPH_RENDER_POST_UP; ++mode) {
        memset(&d, 0, sizeof d);
        d.shader = p->bloom_shader;
        d.specialization[0] = p->format == RENDER_HDR_COMPACT ? 1U : 0U;
        d.specialization[1] = mode;
        d.specialization_count = 2U;
        d.name = names[mode];
        NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &p->bloom[mode]));
        render_log_statistics(r, p->bloom[mode], names[mode]);
    }
    memset(&d, 0, sizeof d);
    d.shader = p->exposure_shader;
    d.name = "exposure";
    NPH_TRY(nph_gpu_pipeline_create_compute(r->dev, &d, &p->exposure));
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(NphRenderExposureData);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC |
               (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "exposure metering";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &p->metering));
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.memory = NPH_GPU_MEMORY_READBACK;
    bd.name = "exposure readback";
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &p->readback[i]));
    }
    return NPH_RESULT_OK;
}

void render_post_destroy(NphRenderer *r)
{
    RenderPost *p = &r->post;
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        nph_gpu_buffer_destroy(r->dev, p->readback[i]);
    }
    nph_gpu_buffer_destroy(r->dev, p->metering);
    nph_gpu_pipeline_destroy(r->dev, p->exposure);
    for (u32 mode = 0; mode <= (u32)NPH_RENDER_POST_UP; ++mode) {
        nph_gpu_pipeline_destroy(r->dev, p->bloom[mode]);
    }
    nph_gpu_shader_destroy(r->dev, p->exposure_shader);
    nph_gpu_shader_destroy(r->dev, p->bloom_shader);
    memset(p, 0, sizeof *p);
}

f32 render_post_frame_begin(NphRenderer *r, const NphRenderView *view)
{
    RenderPost *p = &r->post;
    const u32 slot = r->frame_slot;
    // Bloom and the metering belong to the lit image: a debug view shows
    // the scene's data as they are, and would mislead the meter. Its frames
    // keep the automatic exposure moving toward the last metering, so the
    // lit view comes back as it left.
    const bool lit = view->debug_view == NPH_RENDER_VIEW_LIT;
    const bool automatic = view->exposure == NPH_RENDER_EXPOSURE_AUTO;
    p->frame_bloom = lit && view->bloom > 0.0F ? 1 : 0;
    p->frame_meter = lit && automatic ? 1 : 0;
    p->frame_chain = NPH_GPU_RG_IMAGE_NULL;
    p->frame_levels = 0U;
    p->frame_weight = view->bloom;
    if (!automatic) {
        // A manual exposure: the next automatic one starts again from its
        // view's EV100, with none of the meterings taken before.
        p->adapted = 0;
        p->metered = 0;
        memset(p->readback_frame, 0, sizeof p->readback_frame);
        p->frame_ev100 = view->ev100;
        return p->frame_ev100;
    }
    // The metering this slot's last frame left, NPH_GPU_FRAMES_IN_FLIGHT
    // frames ago and now complete.
    if (p->readback_frame[slot] != 0U) {
        const NphRenderExposureData *m =
            (const NphRenderExposureData *)nph_gpu_buffer_map(r->dev, p->readback[slot]);
        if (m->count != 0U) {
            p->metered_ev100 = render_exposure_target_ev100(
                m->average_log2, p->readback_exposure[slot], view->exposure_compensation);
            p->metered = 1;
        }
        p->readback_frame[slot] = 0U;
    }
    if (!p->adapted) {
        p->ev100 = view->ev100;
        p->adapted = 1;
    }
    if (p->metered) {
        const f32 seconds = view->frame_seconds > 0.0F ? view->frame_seconds : POST_SECONDS;
        const f32 speed = view->exposure_speed > 0.0F ? view->exposure_speed : POST_SPEED;
        p->ev100 += (p->metered_ev100 - p->ev100) * (1.0F - nph_f32_exp(-seconds * speed));
    }
    p->frame_ev100 = p->ev100;
    return p->frame_ev100;
}

static void post_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    const RenderPostPass *pass = (const RenderPostPass *)user;
    NphRenderer *r = pass->renderer;
    const RenderPost *p = &r->post;
    NphGpuDevice *dev = r->dev;
    NphRenderPostPush push;
    memset(&push, 0, sizeof push);
    push.noise = (u32)(nph_gpu_frame_index(dev) % 64U);
    push.sampler_index = nph_gpu_sampler_index(dev, r->clamp_sampler);
    if (pass->mode == (u32)NPH_RENDER_POST_AVERAGE) {
        push.exposure = nph_gpu_buffer_address(dev, p->metering);
        nph_gpu_cmd_bind_pipeline(cmd, p->exposure);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
        return;
    }
    const NphGpuImage chain = nph_gpu_rg_image(graph, p->frame_chain);
    const bool first = pass->mode == (u32)NPH_RENDER_POST_DOWN_FIRST;
    // The source: the HDR image for the first level, else the chain's
    // mip before (down) or after (up).
    const u32 src_mip =
        first ? 0U : (pass->mode == (u32)NPH_RENDER_POST_DOWN ? pass->mip - 1U : pass->mip + 1U);
    const NphGpuImage src = first ? nph_gpu_rg_image(graph, r->hdr_rg) : chain;
    const NphGpuImageDesc *sd = nph_gpu_image_desc(dev, src);
    push.src_index = nph_gpu_image_sampled_index(dev, src);
    push.src_lod = (f32)src_mip;
    push.src_texel = nph_vec2(1.0F / (f32)nph_gpu_mip_extent(sd->width, src_mip),
                              1.0F / (f32)nph_gpu_mip_extent(sd->height, src_mip));
    const NphGpuImageDesc *cd = nph_gpu_image_desc(dev, chain);
    push.dst_index = nph_gpu_image_storage_index(dev, chain, pass->mip);
    push.dst_width = nph_gpu_mip_extent(cd->width, pass->mip);
    push.dst_height = nph_gpu_mip_extent(cd->height, pass->mip);
    if (first && p->frame_meter) {
        push.exposure = nph_gpu_buffer_address(dev, p->metering);
    }
    nph_gpu_cmd_bind_pipeline(cmd, p->bloom[pass->mode]);
    nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
    // A downsample's invocation writes 2 x 2 texels (bloom.comp), an
    // upsample's one; 8 x 8 invocations a group.
    const u32 span = pass->mode == (u32)NPH_RENDER_POST_UP ? 8U : 16U;
    nph_gpu_cmd_dispatch(cmd, (push.dst_width + span - 1U) / span,
                         (push.dst_height + span - 1U) / span, 1U);
}

static void clear_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    const NphRenderer *r = (const NphRenderer *)user;
    nph_gpu_cmd_fill_buffer(cmd, r->post.metering, 0U, sizeof(NphRenderExposureData), 0U);
}

static void readback_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    const NphRenderer *r = (const NphRenderer *)user;
    const NphGpuBufferCopy region = {0U, 0U, sizeof(NphRenderExposureData)};
    nph_gpu_cmd_copy_buffer(cmd, r->post.metering, r->post.readback[r->frame_slot], &region, 1U);
}

static NphGpuRgPass *add_pass(NphGpuRenderGraph *graph, const char *name, NphGpuRgPassType type,
                              NphGpuRgExecuteFn execute, void *user)
{
    NphGpuRgPassDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.execute = execute;
    d.user = user;
    return nph_gpu_rg_add_pass(graph, &d);
}

// The names of the chain's passes, by mip (static strings, as the graph wants).
static const char *const k_down_names[NPH_RENDER_BLOOM_LEVELS] = {
    "bloom down 0", "bloom down 1", "bloom down 2", "bloom down 3", "bloom down 4", "bloom down 5"};
static const char *const k_up_names[NPH_RENDER_BLOOM_LEVELS] = {
    "bloom up 0", "bloom up 1", "bloom up 2", "bloom up 3", "bloom up 4", "bloom up 5"};

NphResult render_post_declare(NphRenderer *r, NphGpuRenderGraph *graph, u32 width, u32 height)
{
    RenderPost *p = &r->post;
    if (!p->frame_bloom && !p->frame_meter) {
        return NPH_RESULT_OK;
    }
    // The chain: mip 0 half the frame, as many levels as fit (only the
    // first when only the exposure is metered).
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = p->format;
    d.width = nph_max_u32((width + 1U) / 2U, 1U);
    d.height = nph_max_u32((height + 1U) / 2U, 1U);
    const u32 fit = nph_gpu_mip_count(d.width, d.height, 1U);
    d.mip_count = p->frame_bloom ? nph_min_u32(fit, (u32)NPH_RENDER_BLOOM_LEVELS) : 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    d.name = "bloom chain";
    p->frame_chain = nph_gpu_rg_create_image(graph, &d);
    if (nph_gpu_rg_image_is_null(p->frame_chain)) {
        return NPH_RESULT_FULL;
    }
    p->frame_levels = d.mip_count;
    NphGpuRgBuffer metering = NPH_GPU_RG_BUFFER_NULL;
    if (p->frame_meter) {
        metering = nph_gpu_rg_import_buffer(graph, p->metering);
        NphGpuRgPass *c =
            add_pass(graph, "exposure clear", NPH_GPU_RG_PASS_TRANSFER, clear_execute, r);
        if (c == NULL || nph_gpu_rg_buffer_is_null(metering)) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(c, metering, NPH_GPU_RG_USE_TRANSFER_DST);
    }
    u32 at = 0U;
    for (u32 mip = 0; mip < p->frame_levels; ++mip) {
        RenderPostPass *pass = &p->passes[at++];
        pass->renderer = r;
        pass->mode = mip == 0U ? (u32)NPH_RENDER_POST_DOWN_FIRST : (u32)NPH_RENDER_POST_DOWN;
        pass->mip = mip;
        NphGpuRgPass *g =
            add_pass(graph, k_down_names[mip], NPH_GPU_RG_PASS_COMPUTE, post_execute, pass);
        if (g == NULL) {
            return NPH_RESULT_FULL;
        }
        if (mip == 0U) {
            nph_gpu_rg_read_image(g, r->hdr_rg, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
            if (p->frame_meter) {
                nph_gpu_rg_write_buffer(g, metering, NPH_GPU_RG_USE_STORAGE_WRITE);
            }
        } else {
            nph_gpu_rg_read_image(g, p->frame_chain, NPH_GPU_RG_USE_SAMPLED, mip - 1U, 1U);
        }
        nph_gpu_rg_write_image(g, p->frame_chain, NPH_GPU_RG_USE_STORAGE_WRITE, mip, 1U);
    }
    // Coarsest first: mip levels - 2 down to 0.
    for (u32 up = 1U; up < p->frame_levels; ++up) {
        const u32 mip = p->frame_levels - 1U - up;
        RenderPostPass *pass = &p->passes[at++];
        pass->renderer = r;
        pass->mode = (u32)NPH_RENDER_POST_UP;
        pass->mip = mip;
        NphGpuRgPass *g =
            add_pass(graph, k_up_names[mip], NPH_GPU_RG_PASS_COMPUTE, post_execute, pass);
        if (g == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_image(g, p->frame_chain, NPH_GPU_RG_USE_SAMPLED, mip + 1U, 1U);
        // Read and written in place.
        nph_gpu_rg_write_image(g, p->frame_chain, NPH_GPU_RG_USE_STORAGE_WRITE, mip, 1U);
    }
    if (p->frame_meter) {
        RenderPostPass *pass = &p->passes[at++];
        pass->renderer = r;
        pass->mode = (u32)NPH_RENDER_POST_AVERAGE;
        pass->mip = 0U;
        NphGpuRgPass *g =
            add_pass(graph, "exposure average", NPH_GPU_RG_PASS_COMPUTE, post_execute, pass);
        NphGpuRgPass *c =
            add_pass(graph, "exposure readback", NPH_GPU_RG_PASS_TRANSFER, readback_execute, r);
        const NphGpuRgBuffer copy = nph_gpu_rg_import_buffer(graph, p->readback[r->frame_slot]);
        if (g == NULL || c == NULL || nph_gpu_rg_buffer_is_null(copy)) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(g, metering, NPH_GPU_RG_USE_STORAGE_WRITE);
        nph_gpu_rg_read_buffer(c, metering, NPH_GPU_RG_USE_TRANSFER_SRC);
        nph_gpu_rg_write_buffer(c, copy, NPH_GPU_RG_USE_TRANSFER_DST);
        p->readback_frame[r->frame_slot] = nph_gpu_frame_index(r->dev) + 1U;
        p->readback_exposure[r->frame_slot] = nph_render_exposure_from_ev100(p->frame_ev100);
    }
    return NPH_RESULT_OK;
}

void render_post_read(const NphRenderer *r, NphGpuRgPass *pass)
{
    if (r->post.frame_bloom) {
        nph_gpu_rg_read_image(pass, r->post.frame_chain, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    }
}

RenderPostComposite render_post_composite(const NphRenderer *r, NphGpuRenderGraph *graph)
{
    const RenderPost *p = &r->post;
    RenderPostComposite c;
    memset(&c, 0, sizeof c);
    c.sampler_index = nph_gpu_sampler_index(r->dev, r->clamp_sampler);
    if (p->frame_bloom && p->frame_levels != 0U) {
        const NphGpuImage chain = nph_gpu_rg_image(graph, p->frame_chain);
        const NphGpuImageDesc *d = nph_gpu_image_desc(r->dev, chain);
        c.bloom_index = nph_gpu_image_sampled_index(r->dev, chain);
        c.bloom = p->frame_weight;
        c.bloom_scale = 1.0F / (f32)p->frame_levels;
        // Mip 0's texel covers 2 x 2 pixels (its size rounds the frame's
        // half up), so pixel p lies at (p + 0.5) / 2 in its texels.
        c.bloom_uv = nph_vec2(0.5F / (f32)d->width, 0.5F / (f32)d->height);
    }
    return c;
}

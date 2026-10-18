// SPDX-License-Identifier: Apache-2.0
// The production path's virtual shadow map (render/renderer.h;
// docs/render.md "Shadows"; ADR-0033; shaders/shared/nph_vsm.h): the sun,
// the first live directional light that casts shadows, gets a clipmap of
// NPH_VSM_LEVELS levels around the camera whose pages exist only where a
// pixel needs them and stay drawn from frame to frame until something they
// show changes.
//
// Each frame that has it:
//   1. the CPU sets the clipmap up (vsm_frame_begin): light space's axes
//      and anchor, each level's window of pages around the camera and its
//      depth reference, and what must be dropped (everything when the sun
//      turned, the anchor moved, or the previous production frame had no
//      map; a level when its depth reference moved);
//   2. `vsm clear`, then `vsm invalidate` before the scatter: the pages
//      this frame's instance updates touch become dirty;
//   3. the depth pyramid's mip-0 passes request the pages the pixels read
//      (vis_hzb.comp);
//   4. `vsm update`, `vsm collect`, `vsm alloc`: the page tables against
//      the windows and the requests, physical pages for the new ones, the
//      list of pages to draw;
//   3b. with blended surfaces, which the visibility buffer does not hold,
//      `vsm requests (blended)` rasterises them against the opaque depth
//      to request theirs (vsm_request.frag);
//   5. `vsm cull instances`, `vsm cull meshlets`: the casters of those
//      pages, a draw per (meshlet, page); `vsm fixup` keeps pages dirty
//      whose draws did not all fit;
//   6. `vsm raster`: those pages cleared, then drawn, into the pool;
//   7. the resolve samples the map for the sun (vsm_common.glsl).
//
// Light space is anchored within VSM_ANCHOR_RADIUS of the camera, so 32-bit
// coordinates hold anywhere in a 64-bit world; levels are positioned in
// double precision on the CPU and reach the shaders as small offsets.
#include <nepheline/core/log.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphVsmEntry) == 16U, "NphVsmEntry matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVsmPhys) == 8U, "NphVsmPhys matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVsmLevel) == 48U, "NphVsmLevel matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVsmCounters) == 112U, "NphVsmCounters matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVsmArgs) == 144U, "NphVsmArgs matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphVsmView) == 1056U, "NphVsmView matches its GLSL layout");
NPH_STATIC_ASSERT(NPH_VSM_LEVEL_ENTRIES == NPH_VSM_WINDOW * NPH_VSM_WINDOW, "a window's entries");
NPH_STATIC_ASSERT(NPH_VSM_LEVEL_WORDS * 32 == NPH_VSM_LEVEL_ENTRIES, "a level's bitmask");
NPH_STATIC_ASSERT(NPH_VSM_ROW_WORDS * 32 == NPH_VSM_WINDOW, "a row of a level's bitmask");

// Level 0's window side in metres: pages of 3.125 cm and texels of 0.24 mm,
// finer than a pixel's footprint at half a metre on a 4K display. Each
// level doubles it; the sixteenth spans 131 km.
#define VSM_LEVEL0_SIZE 4.0

// How far the camera may move from light space's anchor before every page
// is dropped and the anchor moves to the camera: positions within it keep
// 0.25 mm in 32 bits.
#define VSM_ANCHOR_RADIUS 2048.0

// The receiver's offset along its normal, in texels of the level it reads.
#define VSM_TEXEL_BIAS 1.5F

// The resolution bias's limit (each step halves the resolution), and the
// frames in a row whose pages would have fitted a step finer, with room to
// spare, before it is lowered: overflow coarsens the levels at once, room
// refines them only slowly, so the bias does not oscillate.
#define VSM_MAX_LOD_BIAS 3U
#define VSM_ROOMY_FRAMES 120U

// How near a page's edge (in texels) a request also takes the next page:
// the shading's offset along the normal, which moves a point at most that
// far across the light, and half a texel of slack.
#define VSM_REQUEST_MARGIN (VSM_TEXEL_BIAS + 0.5F)

#define VSM_BITMASK_BYTES ((u64)NPH_VSM_LEVELS * NPH_VSM_LEVEL_WORDS * sizeof(u32))
#define VSM_ENTRIES       ((u64)NPH_VSM_LEVELS * NPH_VSM_LEVEL_ENTRIES)

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1U) / a * a;
}

// The work buffer: the counters, the three bitmasks and the render rows
// first (cleared together at the start of a frame), then the arguments,
// the lists, and the culling's items.
static void layout_work(const NphRendererDesc *d, bool vis_draws, VsmLayout *out)
{
    memset(out, 0, sizeof *out);
    u64 at = 0U;
    out->counters = at;
    at += sizeof(NphVsmCounters);
    out->requests = at;
    at += VSM_BITMASK_BYTES;
    out->dirty = at;
    at += VSM_BITMASK_BYTES;
    out->render = at;
    at += VSM_BITMASK_BYTES;
    out->render_rows = at;
    at += (u64)NPH_VSM_LEVELS * NPH_VSM_ROW_WORDS * sizeof(u32);
    out->clear_size = at;
    at = align_up(at, 16U);
    out->args = at;
    at += sizeof(NphVsmArgs);
    out->alloc = at;
    at += VSM_ENTRIES * sizeof(u32);
    out->free = at;
    at += (u64)d->vsm_pages * sizeof(u32);
    out->candidate = at;
    at += (u64)d->vsm_pages * sizeof(u32);
    out->rendered = at;
    at += (u64)d->vsm_pages * sizeof(u32);
    at = align_up(at, 16U);
    out->meshlets = at;
    at += (u64)d->max_vsm_meshlets * 8U;
    at = align_up(at, 16U);
    out->draws = at;
    at += (u64)d->max_vsm_draws * 16U;
    out->vis_draws = at;
    if (vis_draws) {
        at += (u64)d->max_vsm_draws * sizeof(NphVisDraw);
    }
    out->size = align_up(at, 16U);
}

// The production path decides it as it is created (render_vis_create), so
// its resolve and blended pipelines have the variants that read the map.
// The shading filters the pool's depth comparisons bilinearly.
bool render_vsm_device_supported(const NphRenderer *r)
{
    const NphGpuFeatures *f = &nph_gpu_device_info(r->dev)->features;
    return !r->desc.disable_virtual_shadows && f->depth_clamp && f->clip_distance &&
           nph_gpu_format_supports(r->dev, RENDER_DEPTH_FORMAT,
                                   (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED |
                                       (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED_LINEAR |
                                       (u32)NPH_GPU_FORMAT_FEATURE_DEPTH_ATTACHMENT);
}

static NphResult load(NphRenderer *r, const char *file, NphGpuShader *out)
{
    return render_load_shader_file(r, file, out);
}

static NphResult create_pipelines(NphRenderer *r)
{
    RenderVsm *v = &r->vsm;
    NphGpuDevice *dev = r->dev;
    NPH_TRY(load(r, "vsm_pages.comp", &v->shaders[0]));
    NPH_TRY(load(r, "vsm_cull.comp", &v->shaders[1]));
    NPH_TRY(load(r, r->vis.mesh_shaders ? "vsm_raster.mesh" : "vsm_raster.vert", &v->shaders[2]));
    NPH_TRY(load(r, "vsm_clear.vert", &v->shaders[3]));
    NPH_TRY(load(r, "vsm_request.frag", &v->shaders[4]));
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = v->shaders[0];
    cd.name = "vsm pages";
    NPH_TRY(nph_gpu_pipeline_create_compute(dev, &cd, &v->pages));
    cd.shader = v->shaders[1];
    cd.name = "vsm cull";
    NPH_TRY(nph_gpu_pipeline_create_compute(dev, &cd, &v->cull));
    // Depth only, into the pool: casters from both sides, their depth
    // clamped (nearer the light than the near plane still shadows).
    NphGpuGraphicsPipelineDesc gd;
    memset(&gd, 0, sizeof gd);
    if (r->vis.mesh_shaders) {
        gd.mesh = v->shaders[2];
    } else {
        gd.vertex = v->shaders[2];
    }
    gd.topology = NPH_GPU_TOPOLOGY_TRIANGLE_LIST;
    gd.cull_mode = NPH_GPU_CULL_NONE;
    gd.depth_format = RENDER_DEPTH_FORMAT;
    gd.depth.test = 1;
    gd.depth.write = 1;
    gd.depth.compare = NPH_GPU_COMPARE_LESS;
    gd.depth.clamp = 1;
    gd.name = "vsm raster";
    NPH_TRY(nph_gpu_pipeline_create_graphics(dev, &gd, &v->raster));
    memset(&gd, 0, sizeof gd);
    gd.vertex = v->shaders[3];
    gd.topology = NPH_GPU_TOPOLOGY_TRIANGLE_LIST;
    gd.cull_mode = NPH_GPU_CULL_NONE;
    gd.depth_format = RENDER_DEPTH_FORMAT;
    gd.depth.test = 1;
    gd.depth.write = 1;
    gd.depth.compare = NPH_GPU_COMPARE_ALWAYS;
    gd.name = "vsm clear";
    NPH_TRY(nph_gpu_pipeline_create_graphics(dev, &gd, &v->clear));
    // The blended draws' requests: the forward vertex stage, both faces,
    // in front of the opaque surfaces (reverse-Z), nothing written but the
    // requests.
    memset(&gd, 0, sizeof gd);
    gd.vertex = r->shaders[RENDER_SHADER_FORWARD_VERT];
    gd.fragment = v->shaders[4];
    gd.topology = NPH_GPU_TOPOLOGY_TRIANGLE_LIST;
    gd.cull_mode = NPH_GPU_CULL_NONE;
    gd.depth_format = RENDER_DEPTH_FORMAT;
    gd.depth.test = 1;
    gd.depth.compare = NPH_GPU_COMPARE_GREATER;
    gd.name = "vsm request";
    return nph_gpu_pipeline_create_graphics(dev, &gd, &v->request);
}

NphResult render_vsm_create(NphRenderer *r)
{
    RenderVsm *v = &r->vsm;
    v->sun_slot = NPH_U32_MAX;
    if (!v->supported) {
        return NPH_RESULT_OK;
    }
    NphGpuDevice *dev = r->dev;
    const NphRendererDesc *d = &r->desc;
    v->pool_pages = d->vsm_pages;
    const u32 rows = (v->pool_pages + (u32)NPH_VSM_POOL_ROW - 1U) / (u32)NPH_VSM_POOL_ROW;
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.type = NPH_GPU_IMAGE_2D;
    id.format = RENDER_DEPTH_FORMAT;
    id.width = (u32)NPH_VSM_POOL_ROW * (u32)NPH_VSM_PAGE;
    id.height = rows * (u32)NPH_VSM_PAGE;
    id.mip_count = 1U;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    id.name = "vsm pool";
    NPH_TRY(nph_gpu_image_create(dev, &id, &v->pool));
    // Bilinear percentage-closer filtering: the stored depth against the
    // receiver's, lit where the receiver is no farther from the light.
    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    sd.min_filter = NPH_GPU_FILTER_LINEAR;
    sd.mag_filter = NPH_GPU_FILTER_LINEAR;
    sd.mip_filter = NPH_GPU_FILTER_NEAREST;
    sd.address_u = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.address_v = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.address_w = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.compare_enable = 1;
    sd.compare_op = NPH_GPU_COMPARE_LESS_OR_EQUAL;
    sd.name = "vsm compare";
    NPH_TRY(nph_gpu_sampler_create(dev, &sd, &v->compare_sampler));
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.size = VSM_ENTRIES * sizeof(NphVsmEntry);
    bd.name = "vsm page table";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &v->entries));
    bd.size = (u64)v->pool_pages * sizeof(NphVsmPhys);
    bd.name = "vsm physical pages";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &v->phys));
    layout_work(d, !r->vis.mesh_shaders, &v->layout);
    bd.size = v->layout.size;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_INDIRECT |
               (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    bd.name = "vsm work";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &v->work));
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        memset(&bd, 0, sizeof bd);
        bd.size = sizeof(NphVsmCounters);
        bd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
        bd.memory = NPH_GPU_MEMORY_READBACK;
        bd.name = "vsm statistics";
        NPH_TRY(nph_gpu_buffer_create(dev, &bd, &v->stats[i]));
    }
    v->initialize = 1;
    NPH_TRY(create_pipelines(r));
    NPH_LOG_INFO(&NPH_LOG_RENDER,
                 "%s: virtual shadow map: %u pages of %u x %u texels (%.1f MB), %u levels", r->name,
                 v->pool_pages, (u32)NPH_VSM_PAGE, (u32)NPH_VSM_PAGE,
                 (f64)id.width * (f64)id.height * 4.0 / 1.0e6, (u32)NPH_VSM_LEVELS);
    return NPH_RESULT_OK;
}

void render_vsm_destroy(NphRenderer *r)
{
    RenderVsm *v = &r->vsm;
    NphGpuDevice *dev = r->dev;
    nph_gpu_pipeline_destroy(dev, v->pages);
    nph_gpu_pipeline_destroy(dev, v->cull);
    nph_gpu_pipeline_destroy(dev, v->raster);
    nph_gpu_pipeline_destroy(dev, v->clear);
    nph_gpu_pipeline_destroy(dev, v->request);
    for (u32 i = 0; i < NPH_ARRAY_COUNT(v->shaders); ++i) {
        nph_gpu_shader_destroy(dev, v->shaders[i]);
    }
    nph_gpu_image_destroy(dev, v->pool);
    nph_gpu_sampler_destroy(dev, v->compare_sampler);
    nph_gpu_buffer_destroy(dev, v->entries);
    nph_gpu_buffer_destroy(dev, v->phys);
    nph_gpu_buffer_destroy(dev, v->work);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        nph_gpu_buffer_destroy(dev, v->stats[i]);
    }
    memset(v, 0, sizeof *v);
}

// ---------------------------------------------------------------------------
// The sun
// ---------------------------------------------------------------------------

static bool sun_candidate(const RenderLight *l)
{
    return l->handle != 0U && l->desc.shadows && l->desc.type == NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
}

// The sun is the candidate in the lowest slot, so a change elsewhere can
// only make a new candidate ahead of it the sun; only the sun ceasing to
// be one (or going) looks further, from its own slot on.
void render_vsm_light_changed(NphRenderer *r, u32 slot)
{
    RenderVsm *v = &r->vsm;
    if (!v->supported) {
        return;
    }
    u32 sun = v->sun_slot;
    if (slot == sun) {
        if (!sun_candidate(&r->lights[slot])) {
            sun = NPH_U32_MAX;
            for (u32 i = slot + 1U; i < r->light_pool.bump; ++i) {
                if (sun_candidate(&r->lights[i])) {
                    sun = i;
                    break;
                }
            }
        }
    } else if (sun_candidate(&r->lights[slot]) && (sun == NPH_U32_MAX || slot < sun)) {
        sun = slot;
    }
    if (sun == v->sun_slot) {
        return;
    }
    // Both records change: the sun's says so (color.w 2), the old one's no
    // longer does.
    const u32 old = v->sun_slot;
    v->sun_slot = sun;
    if (old != NPH_U32_MAX && r->lights[old].handle != 0U) {
        render_vis_light_changed(r, old);
    }
    if (sun != NPH_U32_MAX) {
        render_vis_light_changed(r, sun);
    }
}

// ---------------------------------------------------------------------------
// A frame
// ---------------------------------------------------------------------------

static f64 dot3(const f64 a[3], NphVec3 b)
{
    return a[0] * (f64)b.x + a[1] * (f64)b.y + a[2] * (f64)b.z;
}

bool render_vsm_frame_begin(NphRenderer *r, const NphRenderView *view, const NphMat4 *proj,
                            const NphMat4 *inv_view_proj, u32 height, bool production)
{
    RenderVsm *v = &r->vsm;
    v->frame_active = 0;
    if (!v->supported || !production || (view->vis_flags & (u32)NPH_RENDER_VIS_RT_SHADOWS) != 0U ||
        v->sun_slot == NPH_U32_MAX) {
        // Instances may change unseen: the next frame that has it starts
        // over.
        v->continuous = 0;
        return false;
    }
    const NphVec3 d = nph_vec3_normalize(r->lights[v->sun_slot].desc.direction);
    const NphDVec3 camera = view->camera.position;
    bool reset = !v->continuous || nph_f32_to_bits(d.x) != nph_f32_to_bits(v->direction.x) ||
                 nph_f32_to_bits(d.y) != nph_f32_to_bits(v->direction.y) ||
                 nph_f32_to_bits(d.z) != nph_f32_to_bits(v->direction.z);
    const NphDVec3 from_anchor = {camera.x - v->anchor.x, camera.y - v->anchor.y,
                                  camera.z - v->anchor.z};
    if (reset || from_anchor.x * from_anchor.x + from_anchor.y * from_anchor.y +
                         from_anchor.z * from_anchor.z >
                     VSM_ANCHOR_RADIUS * VSM_ANCHOR_RADIUS) {
        v->anchor = camera;
        reset = true;
    }
    // Light space: u and v across the light, d along it.
    const NphVec3 up =
        nph_f32_abs(d.y) < 0.99F ? nph_vec3(0.0F, 1.0F, 0.0F) : nph_vec3(1.0F, 0.0F, 0.0F);
    const NphVec3 u = nph_vec3_normalize(nph_vec3_cross(d, up));
    const NphVec3 w = nph_vec3_cross(u, d);
    const f64 c[3] = {camera.x - v->anchor.x, camera.y - v->anchor.y, camera.z - v->anchor.z};
    const f64 cu = dot3(c, u);
    const f64 cv = dot3(c, w);
    const f64 cd = dot3(c, d);

    NphVsmView *vv = render_frame_at(r, r->layout.vsm_view_offset);
    memset(vv, 0, sizeof *vv);
    vv->axis_u = nph_vec4_from_vec3(u, 0.0F);
    vv->axis_v = nph_vec4_from_vec3(w, 0.0F);
    vv->axis_d = nph_vec4_from_vec3(d, 0.0F);
    // Light space turns camera-relative space: its rows are the axes.
    vv->light_from_clip =
        nph_mat4_mul(nph_mat4_from_mat3(nph_mat3_from_rows(u, w, d)), *inv_view_proj);
    const u32 rows = (v->pool_pages + (u32)NPH_VSM_POOL_ROW - 1U) / (u32)NPH_VSM_POOL_ROW;
    vv->pool_scale = nph_vec4(1.0F / (f32)(NPH_VSM_POOL_ROW * NPH_VSM_PAGE),
                              1.0F / (f32)(rows * (u32)NPH_VSM_PAGE), 0.0F, 0.0F);
    for (u32 i = 0; i < (u32)NPH_VSM_LEVELS; ++i) {
        NphVsmLevel *l = &vv->levels[i];
        const f64 page = VSM_LEVEL0_SIZE * (f64)(1U << i) / (f64)NPH_VSM_WINDOW;
        // Half the level's depth range: receivers that read the level lie
        // within a seventh of its side of the camera, the reference within
        // a quarter of the range.
        const f64 half_depth = page * (f64)NPH_VSM_WINDOW * 0.5;
        const f64 wx = nph_f64_floor(cu / page) - (f64)NPH_VSM_WINDOW * 0.5;
        const f64 wy = nph_f64_floor(cv / page) - (f64)NPH_VSM_WINDOW * 0.5;
        const f64 step = half_depth * 0.5;
        const f64 reference = nph_f64_round(cd / step) * step;
        l->reset = !reset && reference != v->depth_reference[i] ? 1U : 0U;
        v->depth_reference[i] = reference;
        l->window_x = (i32)wx;
        l->window_y = (i32)wy;
        // The camera in the level's frame (nph_vsm.h): in pages from the
        // window's corner, and the depth it would store.
        const f64 depth_scale = 1.0 / (2.0 * half_depth);
        l->origin = nph_vec4((f32)(cu / page - wx), (f32)(cv / page - wy),
                             (f32)((cd - reference) * depth_scale + 0.5), 0.0F);
        l->page_size = (f32)page;
        l->inv_page_size = (f32)(1.0 / page);
        l->depth_scale = (f32)depth_scale;
    }
    // A pixel's footprint per metre of distance (the projection's vertical
    // scale is 1 / tan(fov / 2), negative where Vulkan's clip space turns y
    // down) over level 0's texel.
    const f64 texel0 = VSM_LEVEL0_SIZE / ((f64)NPH_VSM_WINDOW * (f64)NPH_VSM_PAGE);
    // The bias makes every pixel's footprint 2^bias times larger.
    vv->lod_scale = (f32)(2.0 / (nph_f64_abs((f64)proj->c[1].y) * (f64)height) / texel0 *
                          (f64)(1U << v->lod_bias));
    v->frame_bias = v->lod_bias;
    vv->texel_bias = VSM_TEXEL_BIAS;
    vv->margin = VSM_REQUEST_MARGIN / (f32)NPH_VSM_PAGE;
    const u64 work = nph_gpu_buffer_address(r->dev, v->work);
    const VsmLayout *lo = &v->layout;
    vv->entries = nph_gpu_buffer_address(r->dev, v->entries);
    vv->phys = nph_gpu_buffer_address(r->dev, v->phys);
    vv->requests = work + lo->requests;
    vv->dirty = work + lo->dirty;
    vv->render = work + lo->render;
    vv->render_rows = work + lo->render_rows;
    vv->alloc = work + lo->alloc;
    vv->free = work + lo->free;
    vv->candidate = work + lo->candidate;
    vv->rendered = work + lo->rendered;
    vv->meshlets = work + lo->meshlets;
    vv->draws = work + lo->draws;
    vv->vis_draws = work + lo->vis_draws;
    vv->counters = work + lo->counters;
    vv->args = work + lo->args;
    vv->pool_pages = v->pool_pages;
    vv->pool_index = nph_gpu_image_sampled_index(r->dev, v->pool);
    vv->compare_sampler = nph_gpu_sampler_index(r->dev, v->compare_sampler);
    vv->frame = ++v->frames;
    vv->max_meshlets = r->desc.max_vsm_meshlets;
    vv->max_draws = r->desc.max_vsm_draws;
    vv->reset = reset ? 1U : 0U;
    v->direction = d;
    v->continuous = 1;
    v->frame_active = 1;
    v->view_address = r->frame_addresses[r->frame_slot] + r->layout.vsm_view_offset;
    return true;
}

// The statistics of the frame that last completed in this slot.
void render_vsm_read_stats(NphRenderer *r)
{
    RenderVsm *v = &r->vsm;
    const u32 slot = r->frame_slot;
    NphRenderFrameStats *s = &r->vis.stats_last;
    if (!v->supported || v->stats_frame[slot] == 0U) {
        // That frame had no map.
        s->vsm_requested = 0U;
        s->vsm_drawn = 0U;
        s->vsm_resident = 0U;
        s->vsm_evicted = 0U;
        s->vsm_draws = 0U;
        s->vsm_overflow = 0U;
        return;
    }
    const NphVsmCounters *c = (const NphVsmCounters *)nph_gpu_buffer_map(r->dev, v->stats[slot]);
    s->vsm_requested = c->requested;
    s->vsm_drawn = nph_min_u32(c->render_count, v->pool_pages);
    // Held after the frame's allocations: those that took free pages add
    // to what was held (the others took them from their owners).
    const u32 free_pages = nph_min_u32(c->free_count, v->pool_pages);
    s->vsm_resident = v->pool_pages - free_pages + nph_min_u32(c->alloc_count, free_pages);
    s->vsm_evicted = c->evicted;
    s->vsm_draws = nph_min_u32(c->draw_count, r->desc.max_vsm_draws);
    s->vsm_overflow = c->overflow;
    const u32 fresh = c->overflow & ~v->overflow_logged;
    if (fresh != 0U) {
        NPH_LOG_WARN(&NPH_LOG_RENDER,
                     "%s: the virtual shadow map dropped work:%s%s%s; raise the capacities in "
                     "NphRendererDesc (said once each)",
                     r->name, (fresh & (u32)NPH_VSM_OVERFLOW_POOL) != 0U ? " vsm_pages" : "",
                     (fresh & (u32)NPH_VSM_OVERFLOW_MESHLETS) != 0U ? " max_vsm_meshlets" : "",
                     (fresh & (u32)NPH_VSM_OVERFLOW_DRAWS) != 0U ? " max_vsm_draws" : "");
        v->overflow_logged |= fresh;
    }
    // The resolution bias, from frames that drew with the current one (the
    // two in flight since a change still show the old one).
    if (v->stats_bias[slot] == v->lod_bias) {
        if (c->overflow != 0U) {
            if (v->lod_bias < VSM_MAX_LOD_BIAS) {
                ++v->lod_bias;
                NPH_LOG_DEBUG(&NPH_LOG_RENDER,
                              "%s: the virtual shadow map overflowed; resolution bias %u", r->name,
                              v->lod_bias);
            }
            v->roomy = 0U;
        } else if (v->lod_bias != 0U && (u64)c->requested * 4U * 4U <= (u64)v->pool_pages * 3U) {
            // A step finer needs up to four times the pages: within three
            // quarters of the pool.
            if (++v->roomy >= VSM_ROOMY_FRAMES) {
                --v->lod_bias;
                v->roomy = 0U;
            }
        } else {
            v->roomy = 0U;
        }
    }
    v->stats_frame[slot] = 0U;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

static NphVisPush push_of(const NphRenderer *r, u32 mode, u32 phase)
{
    NphVisPush p;
    memset(&p, 0, sizeof p);
    p.view = r->vis.frame.view_address;
    p.mode = mode;
    p.phase = phase;
    return p;
}

static void dispatch_items(NphGpuCmd *cmd, u32 items)
{
    const u32 groups = (items + 63U) / 64U;
    const u32 x = nph_max_u32(nph_min_u32(groups, 65535U), 1U);
    nph_gpu_cmd_dispatch(cmd, x, (groups + x - 1U) / x, 1U);
}

static void pages_record(NphRenderer *r, NphGpuCmd *cmd, u32 mode, u32 phase, u32 arg0)
{
    NphVisPush p = push_of(r, mode, phase);
    p.arg0 = arg0;
    nph_gpu_cmd_bind_pipeline(cmd, r->vsm.pages);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
}

static void clear_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    RenderVsm *v = &r->vsm;
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, v->frame.work), 0U, v->layout.clear_size,
                            0U);
    if (v->frame_initialize) {
        // No entry holds a page; no page has an owner.
        nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, v->frame.entries), 0U,
                                NPH_GPU_WHOLE_SIZE, 0U);
        nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, v->frame.phys), 0U,
                                NPH_GPU_WHOLE_SIZE, NPH_VSM_NO_OWNER);
    }
}

static void invalidate_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    pages_record(r, cmd, NPH_VSM_PAGES_INVALIDATE, 0U, r->vis.frame.update_count);
    // An invocation per update and level (y).
    nph_gpu_cmd_dispatch(cmd, (r->vis.frame.update_count + 63U) / 64U, (u32)NPH_VSM_LEVELS, 1U);
}

static void update_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    pages_record(r, cmd, NPH_VSM_PAGES_UPDATE, 0U, 0U);
    // Every entry exactly: the shader takes no bounds.
    nph_gpu_cmd_dispatch(cmd, (u32)(VSM_ENTRIES / 64U), 1U, 1U);
}

static void collect_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    pages_record(r, cmd, NPH_VSM_PAGES_COLLECT, 0U, 0U);
    dispatch_items(cmd, r->vsm.pool_pages);
}

#define ARGS_OFFSET(field) ((u64)offsetof(NphVsmArgs, field))

static void alloc_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    RenderVsm *v = &r->vsm;
    const NphGpuBuffer work = nph_gpu_rg_buffer(graph, v->frame.work);
    pages_record(r, cmd, NPH_VSM_PAGES_ARGS, NPH_VSM_ARGS_ALLOC, 0U);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                                   (u32)NPH_GPU_STAGE_INDIRECT | (u32)NPH_GPU_STAGE_COMPUTE,
                                   (u32)NPH_GPU_ACCESS_INDIRECT_READ |
                                       (u32)NPH_GPU_ACCESS_SHADER_READ};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
    pages_record(r, cmd, NPH_VSM_PAGES_ALLOC, 0U, 0U);
    nph_gpu_cmd_dispatch_indirect(cmd, work, v->layout.args + ARGS_OFFSET(alloc_args));
}

static void cull_instances_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    RenderVsm *v = &r->vsm;
    const NphGpuBuffer work = nph_gpu_rg_buffer(graph, v->frame.work);
    // Only the levels with pages to draw: none in a frame that keeps every
    // page it needs.
    pages_record(r, cmd, NPH_VSM_PAGES_ARGS, NPH_VSM_ARGS_INSTANCE, 0U);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                                   (u32)NPH_GPU_STAGE_INDIRECT | (u32)NPH_GPU_STAGE_COMPUTE,
                                   (u32)NPH_GPU_ACCESS_INDIRECT_READ |
                                       (u32)NPH_GPU_ACCESS_SHADER_READ};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
    const NphVisPush p = push_of(r, NPH_VSM_CULL_INSTANCES, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, v->cull);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, work, v->layout.args + ARGS_OFFSET(instance_args));
}

static void cull_meshlets_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    RenderVsm *v = &r->vsm;
    const NphGpuBuffer work = nph_gpu_rg_buffer(graph, v->frame.work);
    pages_record(r, cmd, NPH_VSM_PAGES_ARGS, NPH_VSM_ARGS_MESHLET, 0U);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                                   (u32)NPH_GPU_STAGE_INDIRECT | (u32)NPH_GPU_STAGE_COMPUTE,
                                   (u32)NPH_GPU_ACCESS_INDIRECT_READ |
                                       (u32)NPH_GPU_ACCESS_SHADER_READ};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
    const NphVisPush p = push_of(r, NPH_VSM_CULL_MESHLETS, 0U);
    nph_gpu_cmd_bind_pipeline(cmd, v->cull);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch_indirect(cmd, work, v->layout.args + ARGS_OFFSET(meshlet_args));
}

static void fixup_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    pages_record(r, cmd, NPH_VSM_PAGES_FIXUP, 0U, 0U);
    dispatch_items(cmd, r->vsm.pool_pages);
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                                   (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_READ};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
    pages_record(r, cmd, NPH_VSM_PAGES_ARGS, NPH_VSM_ARGS_DRAW, 0U);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
}

static void raster_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    RenderVsm *v = &r->vsm;
    const NphGpuBuffer work = nph_gpu_rg_buffer(graph, v->frame.work);
    const NphVisPush p = push_of(r, 0U, 0U);
    // The pages drawn this frame cleared, then their casters.
    nph_gpu_cmd_bind_pipeline(cmd, v->clear);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_draw_indirect(cmd, work, v->layout.args + ARGS_OFFSET(clear_args), 1U, 16U);
    nph_gpu_cmd_bind_pipeline(cmd, v->raster);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    if (r->vis.mesh_shaders) {
        nph_gpu_cmd_draw_mesh_tasks_indirect(cmd, work, v->layout.args + ARGS_OFFSET(draw_args), 1U,
                                             16U);
    } else {
        nph_gpu_cmd_bind_index_buffer(cmd, nph_gpu_rg_buffer(graph, r->vis.frame.index_pool), 0U,
                                      NPH_GPU_INDEX_U16);
        nph_gpu_cmd_draw_indexed_indirect_count(cmd, work, v->layout.vis_draws, work,
                                                v->layout.counters +
                                                    (u64)offsetof(NphVsmCounters, draw_count),
                                                r->desc.max_vsm_draws, (u32)sizeof(NphVisDraw));
    }
}

static void stats_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    RenderVsm *v = &r->vsm;
    const NphGpuBufferCopy region = {v->layout.counters, 0U, sizeof(NphVsmCounters)};
    nph_gpu_cmd_copy_buffer(cmd, nph_gpu_rg_buffer(graph, v->frame.work),
                            nph_gpu_rg_buffer(graph, v->frame.stats), &region, 1U);
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

NphResult render_vsm_declare_begin(NphRenderer *r, NphGpuRenderGraph *graph,
                                   NphGpuRgBuffer instances, NphGpuRgBuffer meshes)
{
    RenderVsm *v = &r->vsm;
    NPH_ASSERT(v->frame_active);
    v->frame.work = nph_gpu_rg_import_buffer(graph, v->work);
    v->frame.entries = nph_gpu_rg_import_buffer(graph, v->entries);
    v->frame.phys = nph_gpu_rg_import_buffer(graph, v->phys);
    v->frame.pool = nph_gpu_rg_import_image(graph, v->pool);
    v->frame.stats = nph_gpu_rg_import_buffer(graph, v->stats[r->frame_slot]);
    if (nph_gpu_rg_buffer_is_null(v->frame.work) || nph_gpu_rg_buffer_is_null(v->frame.entries) ||
        nph_gpu_rg_buffer_is_null(v->frame.phys) || nph_gpu_rg_image_is_null(v->frame.pool) ||
        nph_gpu_rg_buffer_is_null(v->frame.stats)) {
        return NPH_RESULT_FULL;
    }
    v->frame_initialize = v->initialize;
    v->initialize = 0;
    NphGpuRgPass *p = add_pass(graph, r, "vsm clear", NPH_GPU_RG_PASS_TRANSFER, clear_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_write_buffer(p, v->frame.work, NPH_GPU_RG_USE_TRANSFER_DST);
    if (v->frame_initialize) {
        nph_gpu_rg_write_buffer(p, v->frame.entries, NPH_GPU_RG_USE_TRANSFER_DST);
        nph_gpu_rg_write_buffer(p, v->frame.phys, NPH_GPU_RG_USE_TRANSFER_DST);
    }
    if (r->vis.frame.update_count != 0U) {
        // Before the scatter replaces the records it reads.
        p = add_pass(graph, r, "vsm invalidate", NPH_GPU_RG_PASS_COMPUTE, invalidate_execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_buffer(p, instances, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_read_buffer(p, meshes, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_write_buffer(p, v->frame.work, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
    return NPH_RESULT_OK;
}

void render_vsm_request_pages(const NphRenderer *r, NphGpuRgPass *pass)
{
    if (r->vsm.frame_active) {
        nph_gpu_rg_write_buffer(pass, r->vsm.frame.work, NPH_GPU_RG_USE_STORAGE_WRITE);
    }
}

NphResult render_vsm_declare_blend_requests(NphRenderer *r, NphGpuRenderGraph *graph,
                                            NphGpuRgImage depth, NphGpuRgExecuteFn draw)
{
    RenderVsm *v = &r->vsm;
    NPH_ASSERT(v->frame_active);
    NphGpuRgPass *p = add_pass(graph, r, "vsm requests (blended)", NPH_GPU_RG_PASS_GRAPHICS, draw);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_LOAD;
    a.store = NPH_GPU_STORE_OP_NONE; // read only
    nph_gpu_rg_depth_attachment(p, depth, &a, 1);
    nph_gpu_rg_write_buffer(p, v->frame.work, NPH_GPU_RG_USE_STORAGE_WRITE);
    return NPH_RESULT_OK;
}

NphResult render_vsm_declare_pages(NphRenderer *r, NphGpuRenderGraph *graph,
                                   NphGpuRgBuffer instances, NphGpuRgBuffer meshes)
{
    RenderVsm *v = &r->vsm;
    NPH_ASSERT(v->frame_active);
    static const struct {
        const char *name;
        NphGpuRgExecuteFn execute;
    } k_compute[] = {
        {"vsm update", update_execute},
        {"vsm collect", collect_execute},
        {"vsm alloc", alloc_execute},
        {"vsm cull instances", cull_instances_execute},
        {"vsm cull meshlets", cull_meshlets_execute},
        {"vsm fixup", fixup_execute},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(k_compute); ++i) {
        NphGpuRgPass *p =
            add_pass(graph, r, k_compute[i].name, NPH_GPU_RG_PASS_COMPUTE, k_compute[i].execute);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_buffer(p, v->frame.work, NPH_GPU_RG_USE_STORAGE_WRITE);
        nph_gpu_rg_write_buffer(p, v->frame.entries, NPH_GPU_RG_USE_STORAGE_WRITE);
        nph_gpu_rg_write_buffer(p, v->frame.phys, NPH_GPU_RG_USE_STORAGE_WRITE);
        if (i >= 3U) {
            nph_gpu_rg_read_buffer(p, instances, NPH_GPU_RG_USE_SHADER_READ);
            nph_gpu_rg_read_buffer(p, meshes, NPH_GPU_RG_USE_SHADER_READ);
        }
    }
    NphGpuRgPass *p = add_pass(graph, r, "vsm raster", NPH_GPU_RG_PASS_GRAPHICS, raster_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_LOAD; // the pages kept from earlier frames
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_depth_attachment(p, v->frame.pool, &a, 0);
    nph_gpu_rg_read_buffer(p, v->frame.work, NPH_GPU_RG_USE_INDIRECT);
    nph_gpu_rg_read_buffer(p, v->frame.entries, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, instances, NPH_GPU_RG_USE_SHADER_READ);
    nph_gpu_rg_read_buffer(p, meshes, NPH_GPU_RG_USE_SHADER_READ);
    if (!r->vis.mesh_shaders) {
        nph_gpu_rg_read_buffer(p, r->vis.frame.index_pool, NPH_GPU_RG_USE_INDEX);
    }
    p = add_pass(graph, r, "vsm statistics", NPH_GPU_RG_PASS_TRANSFER, stats_execute);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_buffer(p, v->frame.work, NPH_GPU_RG_USE_TRANSFER_SRC);
    nph_gpu_rg_write_buffer(p, v->frame.stats, NPH_GPU_RG_USE_TRANSFER_DST);
    v->stats_frame[r->frame_slot] = nph_gpu_frame_index(r->dev) + 1U;
    v->stats_bias[r->frame_slot] = v->frame_bias;
    return NPH_RESULT_OK;
}

void render_vsm_read(const NphRenderer *r, NphGpuRgPass *pass)
{
    const RenderVsm *v = &r->vsm;
    if (!v->frame_active) {
        return;
    }
    nph_gpu_rg_read_image(pass, v->frame.pool, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_buffer(pass, v->frame.entries, NPH_GPU_RG_USE_SHADER_READ);
}

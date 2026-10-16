// SPDX-License-Identifier: Apache-2.0
// Debug lines (render/debug_draw.h). Each of the RENDER_DEBUG_BUFFERS
// upload buffers holds two lists of `debug_lines` lines: the depth-tested
// list from vertex 0, the overlay list from vertex 2 * debug_lines. Adding
// a line reserves its slot with one atomic add on the list's counter and
// writes straight into the mapped buffer; nph_render_frame (or, in a
// pipelined frame loop, nph_render_debug_flip) freezes the counts and
// moves the writers to the next buffer.
#include <nepheline/core/log.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <string.h>

NphResult render_debug_create(NphRenderer *r)
{
    for (u32 i = 0; i < RENDER_DEBUG_BUFFERS; ++i) {
        NphGpuBufferDesc d;
        memset(&d, 0, sizeof d);
        d.size = (u64)r->desc.debug_lines * 4U * sizeof(NphRenderDebugVertex); // 2 lists x 2 ends
        d.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
        d.memory = NPH_GPU_MEMORY_UPLOAD;
        d.name = "debug lines";
        NPH_TRY(nph_gpu_buffer_create(r->dev, &d, &r->debug_buffers[i]));
        r->debug_maps[i] = (NphRenderDebugVertex *)nph_gpu_buffer_map(r->dev, r->debug_buffers[i]);
        r->debug_addresses[i] = nph_gpu_buffer_address(r->dev, r->debug_buffers[i]);
    }
    r->debug_origin = nph_dvec3_zero();
    r->debug_frame_origin = nph_dvec3_zero();
    nph_atomic_store_u32(&r->debug_count[0], 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&r->debug_count[1], 0U, NPH_MEMORY_ORDER_RELAXED);
    return NPH_RESULT_OK;
}

void render_debug_destroy(NphRenderer *r)
{
    for (u32 i = 0; i < RENDER_DEBUG_BUFFERS; ++i) {
        nph_gpu_buffer_destroy(r->dev, r->debug_buffers[i]);
    }
}

void render_debug_begin_frame(NphRenderer *r)
{
    const u32 cap = r->desc.debug_lines;
    for (u32 list = 0; list < 2U; ++list) {
        const u32 n = nph_atomic_exchange_u32(&r->debug_count[list], 0U, NPH_MEMORY_ORDER_ACQ_REL);
        r->debug_draw_counts[list] = n < cap ? n : cap;
    }
    r->debug_draw_buffer = r->debug_write;
    r->debug_frame_origin = r->debug_origin;
    // The next buffer was last read by the frame RENDER_DEBUG_BUFFERS - 1
    // ago, which has completed: frame_begin waited for it.
    r->debug_write = (r->debug_write + 1U) % RENDER_DEBUG_BUFFERS;
}

void nph_render_debug_flip(NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    if (!nph_gpu_frame_is_active(r->dev)) {
        NPH_ASSERT_MSG(false, "nph_render_debug_flip belongs inside a frame");
        return;
    }
    const u64 frame = nph_gpu_frame_index(r->dev) + 1U;
    if (r->debug_flip_frame == frame) {
        // A second rotation would hand the writers the buffer the previous
        // frame may still be drawing from.
        NPH_ASSERT_MSG(false, "nph_render_debug_flip called twice in one frame");
        return;
    }
    render_debug_begin_frame(r);
    r->debug_flip_frame = frame;
}

void nph_render_debug_set_origin(NphRenderer *r, NphDVec3 origin)
{
    NPH_ASSERT(r != NULL);
    r->debug_origin = origin;
}

NphDVec3 nph_render_debug_origin(const NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    return r->debug_origin;
}

u32 nph_render_debug_line_count(const NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    const u32 cap = r->desc.debug_lines;
    u32 total = 0U;
    for (u32 list = 0; list < 2U; ++list) {
        const u32 n = nph_atomic_load_u32(&r->debug_count[list], NPH_MEMORY_ORDER_RELAXED);
        total += n < cap ? n : cap;
    }
    return total;
}

bool nph_render_debug_line(NphRenderer *r, NphVec3 a, NphVec3 b, u32 rgba, u32 flags)
{
    NPH_ASSERT(r != NULL);
    const u32 list = (flags & (u32)NPH_RENDER_DEBUG_OVERLAY) != 0U ? 1U : 0U;
    const u32 cap = r->desc.debug_lines;
    const u32 index = nph_atomic_fetch_add_u32(&r->debug_count[list], 1U, NPH_MEMORY_ORDER_RELAXED);
    if (index >= cap) {
        // Keep the counter from wrapping on a runaway caller.
        nph_atomic_store_u32(&r->debug_count[list], cap, NPH_MEMORY_ORDER_RELAXED);
        return false;
    }
    NphRenderDebugVertex *v =
        r->debug_maps[r->debug_write] + (u64)list * cap * 2U + (u64)index * 2U;
    v[0].x = a.x;
    v[0].y = a.y;
    v[0].z = a.z;
    v[0].color = rgba;
    v[1].x = b.x;
    v[1].y = b.y;
    v[1].z = b.z;
    v[1].color = rgba;
    return true;
}

static const u8 g_box_edges[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, // along x
    {0, 2}, {1, 3}, {4, 6}, {5, 7}, // along y
    {0, 4}, {1, 5}, {2, 6}, {3, 7}, // along z
};

// Corner i of a box: bit 0 selects max x, bit 1 max y, bit 2 max z.
static NphVec3 box_corner(NphAABB b, u32 i)
{
    return nph_vec3((i & 1U) != 0U ? b.max.x : b.min.x, (i & 2U) != 0U ? b.max.y : b.min.y,
                    (i & 4U) != 0U ? b.max.z : b.min.z);
}

static void box_lines(NphRenderer *r, const NphVec3 corners[8], u32 rgba, u32 flags)
{
    for (u32 e = 0; e < 12U; ++e) {
        NPH_IGNORE_RESULT(nph_render_debug_line(r, corners[g_box_edges[e][0]],
                                                corners[g_box_edges[e][1]], rgba, flags));
    }
}

void nph_render_debug_aabb(NphRenderer *r, NphAABB box, u32 rgba, u32 flags)
{
    NphVec3 c[8];
    for (u32 i = 0; i < 8U; ++i) {
        c[i] = box_corner(box, i);
    }
    box_lines(r, c, rgba, flags);
}

void nph_render_debug_box(NphRenderer *r, const NphAffine *transform, NphAABB local, u32 rgba,
                          u32 flags)
{
    NPH_ASSERT(transform != NULL);
    NphVec3 c[8];
    for (u32 i = 0; i < 8U; ++i) {
        c[i] = nph_affine_transform_point(*transform, box_corner(local, i));
    }
    box_lines(r, c, rgba, flags);
}

void nph_render_debug_sphere(NphRenderer *r, NphVec3 center, f32 radius, u32 rgba, u32 flags,
                             u32 segments)
{
    const u32 n = segments != 0U ? segments : 32U;
    // A great circle in each axis plane.
    for (u32 plane = 0; plane < 3U; ++plane) {
        NphVec3 prev = nph_vec3_zero();
        for (u32 i = 0; i <= n; ++i) {
            const f32 angle = (f32)i / (f32)n * 2.0F * NPH_PI;
            const f32 s = nph_f32_sin(angle) * radius;
            const f32 c = nph_f32_cos(angle) * radius;
            const NphVec3 offset = plane == 0U   ? nph_vec3(c, s, 0.0F)
                                   : plane == 1U ? nph_vec3(c, 0.0F, s)
                                                 : nph_vec3(0.0F, c, s);
            const NphVec3 p = nph_vec3_add(center, offset);
            if (i != 0U) {
                NPH_IGNORE_RESULT(nph_render_debug_line(r, prev, p, rgba, flags));
            }
            prev = p;
        }
    }
}

void nph_render_debug_axes(NphRenderer *r, const NphAffine *transform, f32 size, u32 flags)
{
    NPH_ASSERT(transform != NULL);
    const NphVec3 o = nph_affine_translation(*transform);
    static const u32 colors[3] = {NPH_RENDER_COLOR_RED, NPH_RENDER_COLOR_GREEN,
                                  NPH_RENDER_COLOR_BLUE};
    for (u32 axis = 0; axis < 3U; ++axis) {
        const NphVec3 dir = nph_vec3_normalize_or_zero(nph_affine_column(*transform, axis));
        NPH_IGNORE_RESULT(nph_render_debug_line(r, o, nph_vec3_add(o, nph_vec3_scale(dir, size)),
                                                colors[axis], flags));
    }
}

void nph_render_debug_arrow(NphRenderer *r, NphVec3 from, NphVec3 to, u32 rgba, u32 flags)
{
    NPH_IGNORE_RESULT(nph_render_debug_line(r, from, to, rgba, flags));
    const NphVec3 d = nph_vec3_sub(to, from);
    const f32 len = nph_vec3_length(d);
    if (len <= 0.0F) {
        return;
    }
    const NphVec3 dir = nph_vec3_scale(d, 1.0F / len);
    const NphVec3 u = nph_vec3_normalize(nph_vec3_perpendicular(dir));
    const NphVec3 v = nph_vec3_cross(dir, u);
    const f32 head = len * 0.15F;
    const NphVec3 back = nph_vec3_sub(to, nph_vec3_scale(dir, head));
    const NphVec3 spokes[4] = {u, nph_vec3_neg(u), v, nph_vec3_neg(v)};
    for (u32 i = 0; i < 4U; ++i) {
        const NphVec3 p = nph_vec3_add(back, nph_vec3_scale(spokes[i], head * 0.5F));
        NPH_IGNORE_RESULT(nph_render_debug_line(r, to, p, rgba, flags));
    }
}

void nph_render_debug_cross(NphRenderer *r, NphVec3 p, f32 size, u32 rgba, u32 flags)
{
    const f32 h = size * 0.5F;
    NPH_IGNORE_RESULT(nph_render_debug_line(r, nph_vec3_sub(p, nph_vec3(h, 0, 0)),
                                            nph_vec3_add(p, nph_vec3(h, 0, 0)), rgba, flags));
    NPH_IGNORE_RESULT(nph_render_debug_line(r, nph_vec3_sub(p, nph_vec3(0, h, 0)),
                                            nph_vec3_add(p, nph_vec3(0, h, 0)), rgba, flags));
    NPH_IGNORE_RESULT(nph_render_debug_line(r, nph_vec3_sub(p, nph_vec3(0, 0, h)),
                                            nph_vec3_add(p, nph_vec3(0, 0, h)), rgba, flags));
}

void nph_render_debug_grid(NphRenderer *r, NphVec3 center, f32 cell_size, u32 cells, u32 rgba,
                           u32 flags)
{
    const f32 half = cell_size * (f32)cells * 0.5F;
    for (u32 i = 0; i <= cells; ++i) {
        const f32 o = -half + cell_size * (f32)i;
        NPH_IGNORE_RESULT(nph_render_debug_line(r, nph_vec3_add(center, nph_vec3(o, 0.0F, -half)),
                                                nph_vec3_add(center, nph_vec3(o, 0.0F, half)), rgba,
                                                flags));
        NPH_IGNORE_RESULT(nph_render_debug_line(r, nph_vec3_add(center, nph_vec3(-half, 0.0F, o)),
                                                nph_vec3_add(center, nph_vec3(half, 0.0F, o)), rgba,
                                                flags));
    }
}

void nph_render_debug_frustum(NphRenderer *r, const NphRenderCamera *camera, f32 aspect,
                              f32 far_distance, u32 rgba, u32 flags)
{
    NPH_ASSERT(r != NULL && camera != NULL);
    const NphMat3 rot = nph_mat3_from_quat(nph_quat_normalize_or_identity(camera->orientation));
    const NphVec3 origin = nph_dvec3_relative(camera->position, r->debug_origin);
    const f32 ty = nph_f32_tan(camera->yfov * 0.5F);
    const f32 dist[2] = {camera->znear, far_distance};
    NphVec3 c[8];
    for (u32 i = 0; i < 8U; ++i) {
        // Bit 0: right, bit 1: top, bit 2: far (the box_corner order).
        const f32 d = dist[(i >> 2U) & 1U];
        const f32 x = ((i & 1U) != 0U ? 1.0F : -1.0F) * ty * aspect * d;
        const f32 y = ((i & 2U) != 0U ? 1.0F : -1.0F) * ty * d;
        c[i] = nph_vec3_add(origin, nph_mat3_mul_vec3(rot, nph_vec3(x, y, -d)));
    }
    box_lines(r, c, rgba, flags);
}

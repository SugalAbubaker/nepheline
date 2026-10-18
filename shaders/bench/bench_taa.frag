// SPDX-License-Identifier: Apache-2.0
// The production path's temporal resolve fused with the tonemap, as a
// pattern of reads, arithmetic, and writes (shaders/render/taa_common.glsl;
// docs/benchmarks.md "GPU benchmarks"): per pixel, the 3 x 3 neighbourhoods
// of the current HDR colour and of the visibility buffer's depth, the
// history's five Catmull-Rom taps where a constant motion takes them, and
// two outputs, the display colour and the new history. k_mode chooses how
// the neighbourhoods are read:
//   0: nine fetches each;
//   1: four each, shared across the 2 x 2 quad with subgroup quad swaps:
//      the quad's four neighbourhoods span 4 x 4 texels, each lane fetches
//      a 2 x 2 block of them and receives from its three partners the five
//      texels its own neighbourhood needs;
//   2: the cross, five fetches each.
#version 460
#extension GL_KHR_shader_subgroup_quad : require
#include "../render/taa_common.glsl"
#include "../render/tonemap.glsl"
#include "nph_shared.h"

layout(constant_id = 0) const uint k_mode = 0u;
layout(constant_id = 1) const bool k_dither = true; // the history is B10G11R11

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_history;

layout(push_constant, scalar) uniform Push { NphBenchPush pc; };

ivec2 g_size;

vec3 fetch_color(ivec2 p)
{
    return texelFetch(nph_textures_2d[pc.texture_index], clamp(p, ivec2(0), g_size - 1), 0).rgb;
}

// The visibility buffer as R32G32_UINT: the depth's bits in the high word.
float fetch_depth(ivec2 p)
{
    return uintBitsToFloat(
        texelFetch(nph_utextures_2d[pc.texture2_index], clamp(p, ivec2(0), g_size - 1), 0).y);
}

// The bench's jitter, in pixels.
const vec2 k_jitter = vec2(0.25, -0.125);

void add(inout TaaNeighbourhood n, inout float nearest, inout ivec2 nearest_offset, vec3 color,
         float depth, ivec2 offset)
{
    taa_neighbourhood_add(n, color, taa_filter_weight(vec2(offset) - k_jitter));
    if (depth > nearest) {
        nearest = depth;
        nearest_offset = offset;
    }
}

void main()
{
    g_size = textureSize(nph_textures_2d[pc.texture_index], 0);
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    TaaNeighbourhood n = taa_neighbourhood_begin();
    float nearest = 0.0;
    ivec2 nearest_offset = ivec2(0);
    if (k_mode == 1u) {
        const ivec2 lane = pixel & 1;
        const ivec2 base = pixel + lane - 1; // this lane's 2 x 2 block
        vec3 a[4];
        float d[4];
        for (int i = 0; i < 4; ++i) {
            const ivec2 q = base + ivec2(i & 1, i >> 1);
            a[i] = fetch_color(q);
            d[i] = fetch_depth(q);
            add(n, nearest, nearest_offset, a[i], d[i], q - pixel);
        }
        // Each lane sends the column next to its horizontal partner, the
        // row next to its vertical one, and the corner next to its
        // diagonal one.
        const int hc = lane.x == 0 ? 1 : 0;
        const int vr = lane.y == 0 ? 2 : 0;
        const int hx = lane.x == 0 ? 1 : -1;
        const int vy = lane.y == 0 ? 1 : -1;
        add(n, nearest, nearest_offset, subgroupQuadSwapHorizontal(a[hc]),
            subgroupQuadSwapHorizontal(d[hc]), ivec2(hx, lane.y - 1));
        add(n, nearest, nearest_offset, subgroupQuadSwapHorizontal(a[hc + 2]),
            subgroupQuadSwapHorizontal(d[hc + 2]), ivec2(hx, lane.y));
        add(n, nearest, nearest_offset, subgroupQuadSwapVertical(a[vr]),
            subgroupQuadSwapVertical(d[vr]), ivec2(lane.x - 1, vy));
        add(n, nearest, nearest_offset, subgroupQuadSwapVertical(a[vr + 1]),
            subgroupQuadSwapVertical(d[vr + 1]), ivec2(lane.x, vy));
        add(n, nearest, nearest_offset, subgroupQuadSwapDiagonal(a[hc + vr]),
            subgroupQuadSwapDiagonal(d[hc + vr]), ivec2(hx, vy));
    } else {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (k_mode == 2u && x != 0 && y != 0) {
                    continue;
                }
                const ivec2 o = ivec2(x, y);
                add(n, nearest, nearest_offset, fetch_color(pixel + o), fetch_depth(pixel + o), o);
            }
        }
    }
    // A constant motion, nudged by the nearest sample so that the search
    // stays live (the real pass reprojects the nearest sample).
    const vec2 motion = pc.color.xy + vec2(nearest_offset) * (nearest * 1e-3);
    const vec2 size = vec2(g_size);
    const vec2 inv_size = 1.0 / size;
    const vec2 uv = (vec2(pixel) + 0.5 - motion) * inv_size;
    const vec3 history = taa_rgb_to_ycocg(
        taa_history_catmull_rom(pc.texture3_index, pc.sampler_index, uv, size, inv_size));
    const vec3 current = n.filtered / n.weight;
    const vec3 resolved = taa_ycocg_to_rgb(
        taa_blend(current, taa_clip(n, history), taa_current_weight(length(motion))));
    out_color = vec4(nph_tonemap(resolved, NPH_RENDER_TONEMAP_ACES, true), 1.0);
    out_history = vec4(
        k_dither ? nph_dither_b10g11r11(resolved, nph_gradient_noise(gl_FragCoord.xy)) : resolved,
        1.0);
}

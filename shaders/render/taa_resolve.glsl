// SPDX-License-Identifier: Apache-2.0
// The production path's temporal resolve (docs/render.md "Temporal
// anti-aliasing"; ADR-0034; the arithmetic is taa_common.glsl's): one
// full-screen pass blends this frame's jittered samples into the history of
// the earlier ones and writes the new history. taa.frag also writes the
// display colour, fused with the tonemap, so the HDR image is read once and
// the caller's target is written as a colour attachment, which keeps its
// framebuffer compression; taa_history.frag (TAA_HISTORY_ONLY) writes the
// history alone, for the motion blur that reads it (motion_blur.frag).
//
// The history is read where the surface the pixel sees was a frame ago.
// The motion is the nearest surface's in the 3 x 3 neighbourhood (Karis
// 2014): a moving edge then carries its motion into the background beside
// it, where the background's own would reproject the edge's history and
// leave a trail (vis_previous_uv rebuilds where it was).
#include "taa_common.glsl"
#include "tonemap.glsl"
#include "vis_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderTaaPush pc; };

// Whether the history is B10G11R11, whose coarse steps are rounded at
// random (its expected value kept, so the accumulation does not drift).
layout(constant_id = 0) const bool k_dither = false;

layout(location = 0) in vec2 in_ndc;
// (out_state: x the pixel's unrest, y the frames its history holds over 255, zw its surface's
// key in two halves; taa_unrest, taa_count_after, taa_key_pack.)
#ifdef TAA_HISTORY_ONLY
layout(location = 0) out vec4 out_history;
layout(location = 1) out vec4 out_state;
#else
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_history;
layout(location = 2) out vec4 out_state;
#endif

float filter_weight(int i)
{
    return i < 4 ? pc.weights0[i] : (i < 8 ? pc.weights1[i - 4] : pc.weight8);
}

// A neighbour of the pixel at `uv`, its texel's centre: the point sampler
// clamps to the edge, so a neighbour past it reads the edge's texel, as a
// clamp of the texel's coordinates would, and the offsets are the
// instruction's immediates (constant expressions, hence a macro).
#define TAA_NEIGHBOUR(n, uv, x, y)                                                                 \
    taa_neighbourhood_add(                                                                         \
        n,                                                                                         \
        textureLodOffset(sampler2D(nph_textures_2d[pc.hdr_index], nph_samplers[pc.point_sampler]), \
                         uv, 0.0, ivec2(x, y))                                                     \
            .rgb,                                                                                  \
        filter_weight(((y) + 1) * 3 + (x) + 1))

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 last = ivec2(view.v.viewport.xy) - 1;
    // The neighbourhood's colour, row by row.
    const vec2 centre = gl_FragCoord.xy * view.v.viewport.zw;
    TaaNeighbourhood n = taa_neighbourhood_begin();
    TAA_NEIGHBOUR(n, centre, -1, -1);
    TAA_NEIGHBOUR(n, centre, 0, -1);
    TAA_NEIGHBOUR(n, centre, 1, -1);
    TAA_NEIGHBOUR(n, centre, -1, 0);
    TAA_NEIGHBOUR(n, centre, 0, 0);
    TAA_NEIGHBOUR(n, centre, 1, 0);
    TAA_NEIGHBOUR(n, centre, -1, 1);
    TAA_NEIGHBOUR(n, centre, 0, 1);
    TAA_NEIGHBOUR(n, centre, 1, 1);
    // The bloom (bloom.comp), mixed into the current frame before the
    // history meets it, so the history carries it and steadies it too; it
    // varies slowly, so the pixel's own serves its neighbours.
    if (pc.bloom > 0.0) {
        const vec3 glare =
            textureLod(sampler2D(nph_textures_2d[pc.bloom_index], nph_samplers[pc.sampler_index]),
                       gl_FragCoord.xy * pc.bloom_uv, 0.0)
                .rgb *
            pc.bloom_scale;
        taa_neighbourhood_mix(n, taa_rgb_to_ycocg(glare), pc.bloom);
    }
    // Its nearest surface: the visibility buffer's depths (its high words,
    // reverse-Z, so the largest is the nearest and the sky's 0 the
    // farthest; the bits of non-negative floats order as the floats do) by
    // four gathers of 2 x 2 texels, each at the corner its block shares
    // (offsets from the pixel's own corner, the instruction's immediates),
    // then the nearest texel's whole record. A gather's order: (x0, y1),
    // (x1, y1), (x1, y0), (x0, y0); of the 16 texels, the 9 around the pixel
    // are tested, in the order of the blocks, the first of equal depths kept.
    // The nearest is kept as its offset from the pixel, (dx + 1) + 4 (dy + 1).
    const vec2 corner = vec2(pixel) * view.v.viewport.zw;
    uint nearest_depth = 0u;
    int nearest_offset = 1 + 4 * 1;
#define TAA_GATHER(x, y)                                                                           \
    textureGatherOffset(usampler2D(nph_utextures_2d[pc.visbuffer_index],                           \
                                   nph_samplers[pc.point_sampler]),                                \
                        corner, ivec2(x, y), 1)
#define TAA_NEAREST(depth, dx, dy)                                                                 \
    if ((depth) > nearest_depth) {                                                                 \
        nearest_depth = (depth);                                                                   \
        nearest_offset = ((dx) + 1) + 4 * ((dy) + 1);                                              \
    }
    const uvec4 d00 = TAA_GATHER(0, 0);
    TAA_NEAREST(d00.x, -1, 0)
    TAA_NEAREST(d00.y, 0, 0)
    TAA_NEAREST(d00.z, 0, -1)
    TAA_NEAREST(d00.w, -1, -1)
    const uvec4 d10 = TAA_GATHER(2, 0);
    TAA_NEAREST(d10.x, 1, 0)
    TAA_NEAREST(d10.w, 1, -1)
    const uvec4 d01 = TAA_GATHER(0, 2);
    TAA_NEAREST(d01.z, 0, 1)
    TAA_NEAREST(d01.w, -1, 1)
    const uvec4 d11 = TAA_GATHER(2, 2);
    TAA_NEAREST(d11.w, 1, 1)
    const ivec2 nearest_at =
        clamp(pixel + ivec2(nearest_offset & 3, nearest_offset >> 2) - 1, ivec2(0), last);
    const uint64_t nearest =
        packUint2x32(texelFetch(nph_utextures_2d[pc.visbuffer_index], nearest_at, 0).xy);
    // The weights sum to 1.
    vec3 resolved = n.filtered;
    float unrest = 0.0;
    float count = 1.0; // the frames the new history holds: this one, without a history
    vec2 then;
    float depth_then;
    if (pc.history_scale > 0.0 &&
        vis_previous_point(view, nearest_at, nearest, then, depth_then)) {
        // The nearest surface's motion without jitter (its sample lies the
        // jitter away from its pixel's centre) is how far back this
        // pixel's history lies.
        const vec2 inv_size = view.v.viewport.zw;
        const vec2 motion = (vec2(nearest_at) + 0.5) * inv_size - pc.jitter - then;
        const vec2 uv = (vec2(pixel) + 0.5) * inv_size - motion;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            const vec3 history =
                taa_rgb_to_ycocg(taa_history_cubic(pc.history_index, pc.sampler_index, uv,
                                                   view.v.viewport.xy, inv_size) *
                                 pc.history_scale);
            const vec2 pixels = abs(motion * view.v.viewport.xy);
            const float speed = max(pixels.x, pixels.y);
            const float moving = clamp(speed / TAA_MOVING_SPEED, 0.0, 1.0);
            // The unrest the history's pixel left (taa_unrest) and the frames it held, with the
            // history's own taps' sampler: its texel's where the pixel stood still.
            const vec2 state = textureLod(sampler2D(nph_textures_2d[pc.unrest_index],
                                                    nph_samplers[pc.sampler_index]),
                                          uv, 0.0)
                                   .rg;
            const float was = state.x;
            const float still = taa_stillness(speed);
            // Whether the history there shows this surface: the keys of the four history
            // pixels about the point against the point's own a frame ago. If so it is clipped
            // as a still pixel's is, however it moved; what a nearer surface has just
            // uncovered takes the moving pixel's box, held to its frame.
            const vec4 keys = taa_keys_unpack(
                textureGather(sampler2D(nph_textures_2d[pc.unrest_index],
                                        nph_samplers[pc.point_sampler]),
                              uv, 2),
                textureGather(sampler2D(nph_textures_2d[pc.unrest_index],
                                        nph_samplers[pc.point_sampler]),
                              uv, 3));
            const bool same = taa_history_valid(keys, taa_depth_key(depth_then));
            const float weight = taa_count_weight(speed, state.y * 255.0);
            resolved = taa_blend(n.filtered,
                                 taa_clip_unrest(n, history, same ? 0.0 : moving, was * still),
                                 weight);
            count = taa_count_after(weight);
            unrest = taa_unrest(was, n.filtered.x, history.x, still);
        }
    }
    vec3 rgb = max(taa_ycocg_to_rgb(resolved), vec3(0.0));
    // A non-finite colour would stay in the history for good (any channel's
    // makes the sum non-finite).
    const float sum = rgb.r + rgb.g + rgb.b;
    if (isnan(sum) || isinf(sum)) {
        rgb = vec3(0.0);
    }
#ifndef TAA_HISTORY_ONLY
    out_color = vec4(nph_tonemap(rgb, pc.tonemapper, pc.encode_srgb != 0u), 1.0);
#endif
    // Interleaved gradient noise (Jimenez 2014) moved by 5.588238 pixels a
    // frame, so the rounding differs from frame to frame (Wolfe,
    // "Interleaved Gradient Noise: A Different Kind of Low Discrepancy
    // Sequence", 2022).
    out_history = vec4(k_dither ? nph_dither_b10g11r11(rgb, nph_gradient_noise(
                                                                gl_FragCoord.xy +
                                                                5.588238 * float(pc.noise)))
                                : rgb,
                       1.0);
    // The unrest less half an 8-bit step, so that it falls to nothing where the frames agree
    // (7/8 of a few steps, rounded, is those steps again); and the key of the surface whose
    // motion the pixel's history follows, the nearest about it (the sky's: the farthest).
    out_state = vec4(max(unrest - 0.5 / 255.0, 0.0), count / 255.0,
                     taa_key_pack(taa_depth_key(vis_depth(nearest))));
}

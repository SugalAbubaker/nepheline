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
#ifdef TAA_HISTORY_ONLY
layout(location = 0) out vec4 out_history;
#else
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_history;
#endif

float filter_weight(int i)
{
    return i < 4 ? pc.weights0[i] : (i < 8 ? pc.weights1[i - 4] : pc.weight8);
}

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 last = ivec2(view.v.viewport.xy) - 1;
    // The neighbourhood's colour.
    TaaNeighbourhood n = taa_neighbourhood_begin();
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), last);
            const vec3 c = texelFetch(nph_textures_2d[pc.hdr_index], q, 0).rgb;
            taa_neighbourhood_add(n, c, filter_weight((y + 1) * 3 + x + 1));
        }
    }
    // The bloom (bloom.comp), mixed into the current frame before the
    // history meets it, so the history carries it and steadies it too; it
    // varies slowly, so the pixel's own serves its neighbours.
    if (pc.bloom > 0.0) {
        const vec3 glare =
            textureLod(sampler2D(nph_textures_2d[pc.bloom_index], nph_samplers[pc.sampler_index]),
                       (vec2(pixel) + 0.5) * pc.bloom_uv, 0.0)
                .rgb *
            pc.bloom_scale;
        taa_neighbourhood_mix(n, taa_rgb_to_ycocg(glare), pc.bloom);
    }
    // Its nearest surface: the visibility buffer's depths (its high words,
    // reverse-Z, so the largest is the nearest and the sky's 0 the
    // farthest; the bits of non-negative floats order as the floats do) by
    // four gathers of 2 x 2 texels, each at the corner its block shares,
    // then the nearest texel's whole record.
    uint nearest_depth = 0u;
    ivec2 nearest_at = pixel;
    for (int by = 0; by < 2; ++by) {
        for (int bx = 0; bx < 2; ++bx) {
            const ivec2 corner = pixel + ivec2(2 * bx, 2 * by);
            const uvec4 d =
                textureGather(usampler2D(nph_utextures_2d[pc.visbuffer_index],
                                         nph_samplers[pc.point_sampler]),
                              vec2(corner) * view.v.viewport.zw, 1);
            // A gather's order: (x0, y1), (x1, y1), (x1, y0), (x0, y0).
            const ivec2 at[4] = ivec2[4](corner + ivec2(-1, 0), corner, corner + ivec2(0, -1),
                                         corner - 1);
            for (int i = 0; i < 4; ++i) {
                if (all(lessThanEqual(abs(at[i] - pixel), ivec2(1))) && d[i] > nearest_depth) {
                    nearest_depth = d[i];
                    nearest_at = at[i];
                }
            }
        }
    }
    nearest_at = clamp(nearest_at, ivec2(0), last);
    const uint64_t nearest =
        packUint2x32(texelFetch(nph_utextures_2d[pc.visbuffer_index], nearest_at, 0).xy);
    // The weights sum to 1.
    vec3 resolved = n.filtered;
    vec2 then;
    if (pc.history_scale > 0.0 && vis_previous_uv(view, nearest_at, nearest, then)) {
        // The nearest surface's motion without jitter (its sample lies the
        // jitter away from its pixel's centre) is how far back this
        // pixel's history lies.
        const vec2 inv_size = view.v.viewport.zw;
        const vec2 motion = (vec2(nearest_at) + 0.5) * inv_size - pc.jitter - then;
        const vec2 uv = (vec2(pixel) + 0.5) * inv_size - motion;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            const vec3 history =
                taa_rgb_to_ycocg(taa_history_catmull_rom(pc.history_index, pc.sampler_index, uv,
                                                         view.v.viewport.xy, inv_size) *
                                 pc.history_scale);
            const vec2 pixels = abs(motion * view.v.viewport.xy);
            resolved = taa_blend(n.filtered, taa_clip(n, history),
                                 taa_current_weight(max(pixels.x, pixels.y)));
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
}

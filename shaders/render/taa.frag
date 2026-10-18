// SPDX-License-Identifier: Apache-2.0
// The production path's temporal resolve, fused with the tonemap
// (docs/render.md "Temporal anti-aliasing"; ADR-0034; the arithmetic is
// taa_common.glsl's): one full-screen pass blends this frame's jittered
// samples into the history of the earlier ones and writes both the new
// history and the display colour, so the HDR image is read once and the
// caller's target is written as a colour attachment, which keeps its
// framebuffer compression.
//
// The history is read where the surface the pixel sees was a frame ago.
// The motion is the nearest surface's in the 3 x 3 neighbourhood (Karis
// 2014): a moving edge then carries its motion into the background beside
// it, where the background's own would reproject the edge's history and
// leave a trail. The surface's point is rebuilt from the visibility
// buffer's depth in camera-relative space and taken through the previous
// frame's view and projection, without its jitter: through the previous
// transform for an instance that moved (NPH_VIS_INSTANCE_MOVED), with the
// camera alone otherwise, and for the sky's points at infinity with the
// camera's rotation.
#version 460
#include "taa_common.glsl"
#include "tonemap.glsl"
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

layout(push_constant, scalar) uniform Push { NphRenderTaaPush pc; };

// Whether the history is B10G11R11, whose coarse steps are rounded at
// random (its expected value kept, so the accumulation does not drift).
layout(constant_id = 0) const bool k_dither = false;

layout(location = 0) in vec2 in_ndc;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_history;

float filter_weight(int i)
{
    return i < 4 ? pc.weights0[i] : (i < 8 ? pc.weights1[i - 4] : pc.weight8);
}

// Where the surface seen at pixel `q` (visibility-buffer texel `v`, 0 for
// the sky) lay in the previous frame, in UV without jitter; false when it
// lay behind the previous camera.
bool previous_uv(VisViewRef view, ivec2 q, uint64_t v, out vec2 uv)
{
    ViewRef rview = ViewRef(view.v.render_view);
    const vec2 ndc = (vec2(q) + 0.5) * view.v.viewport.zw * 2.0 - 1.0;
    // Homogeneous and camera relative, through this frame's jittered
    // projection, which the sample at q was drawn with; w is 0 at infinity.
    const vec4 x = rview.v.inv_view_proj * vec4(ndc, v == 0ul ? 0.0 : vis_depth(v), 1.0);
    vec4 clip;
    bool moved = false;
    if (v != 0ul) {
        const uint slot = VisClustersRef(view.v.clusters).c[vis_cluster(v)].instance;
        const NphVisInstance inst = VisInstancesRef(view.v.instances).i[slot];
        if ((inst.flags & NPH_VIS_INSTANCE_MOVED) != 0u) {
            moved = true;
            const vec3 t = vis_relative_translation(inst.transform, view.v.camera_high.xyz,
                                                    view.v.camera_low.xyz);
            // The point in the instance's own space (the inverse of the
            // linear part is the normal matrix transposed), out again
            // through the previous transform, relative to the previous camera.
            const vec3 local = transpose(vis_normal_matrix(inst.transform)) * (x.xyz / x.w - t);
            const NphVisTransform pt = VisTransformsRef(view.v.prev_transforms).t[slot];
            const vec3 then = vis_transform_vector(pt, local) +
                              vis_relative_translation(pt, view.v.prev_camera_high.xyz,
                                                       view.v.prev_camera_low.xyz);
            clip = view.v.prev_view_proj * vec4(then, 1.0);
        }
    }
    if (!moved) {
        // This frame's camera less the previous one's, high and low floats
        // apart (Cozzi & Ring 2011), moves a still point into the previous
        // camera's space; a point at infinity does not move with it.
        const vec3 camera_motion = (view.v.camera_high.xyz - view.v.prev_camera_high.xyz) +
                                   (view.v.camera_low.xyz - view.v.prev_camera_low.xyz);
        clip = view.v.prev_view_proj * vec4(x.xyz + camera_motion * x.w, x.w);
    }
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return clip.w > 0.0;
}

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 last = ivec2(view.v.viewport.xy) - 1;
    // The neighbourhood's colour, and its nearest surface: the visibility
    // buffer's packing orders by depth first, and the largest is the
    // nearest (reverse-Z), the sky's 0 the farthest.
    TaaNeighbourhood n = taa_neighbourhood_begin();
    uint64_t nearest = 0ul;
    ivec2 nearest_at = pixel;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), last);
            const vec3 c = texelFetch(nph_textures_2d[pc.hdr_index], q, 0).rgb;
            taa_neighbourhood_add(n, c, filter_weight((y + 1) * 3 + x + 1));
            const uint64_t v = imageLoad(nph_u64images_2d[pc.visbuffer_index], q).x;
            if (v > nearest) {
                nearest = v;
                nearest_at = q;
            }
        }
    }
    // The weights sum to 1.
    vec3 resolved = n.filtered;
    vec2 then;
    if (pc.history_scale > 0.0 && previous_uv(view, nearest_at, nearest, then)) {
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
            resolved = taa_blend(n.filtered, taa_clip(n, history),
                                 taa_current_weight(length(motion * view.v.viewport.xy)));
        }
    }
    vec3 rgb = max(taa_ycocg_to_rgb(resolved), vec3(0.0));
    // A non-finite colour would stay in the history for good.
    rgb = mix(rgb, vec3(0.0), bvec3(any(isnan(rgb)) || any(isinf(rgb))));
    out_color = vec4(nph_tonemap(rgb, pc.tonemapper, pc.encode_srgb != 0u), 1.0);
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

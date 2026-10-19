// SPDX-License-Identifier: Apache-2.0
// The motion blur's gather, fused with the tonemap (docs/render.md "Motion
// blur"; ADR-0036). The tiles, the two sampling directions, and the
// relative depth test are those of Guertin, McGuire & Nowrouzezahrai, "A
// Fast and Stable Feature-Aware Motion Blur Filter" (HPG 2014), after
// McGuire, Hennessy, Bukowski & Osman, "A Reconstruction Filter for
// Plausible Motion Blur" (I3D 2012); the samples' weights are the shutter's
// own (ADR-0036 measures both against it).
//
// Each pixel takes samples along its neighbourhood's dominant blur
// (motion_tiles.comp) and, every other one, along a second direction: its
// own blur's, or the dominant one's perpendicular where it barely moves, so
// that motion across the dominant direction is not missed. While the
// shutter is open, every surface point sweeps the segment its blur spans,
// and covers a pixel on that segment for 1 / (2 reach) of the time per pixel
// of the segment; a sample, standing for `span` pixels of the line sampled,
// covers the pixel for its share of that. So, by depth against the pixel's
// own surface:
//   - samples in front cover the pixel for the time their sweeps cross it;
//   - samples behind show through for the time the pixel's own surface,
//     sweeping its own segment, has left it to them;
//   - samples of the pixel's own surface give the colours that pass over it.
// The pixel is the front layer over its own surface, which in turn lets the
// background through for the time it is away. A box over a still
// background, or still over a moving one, then blurs as the shutter's
// average does, and a moving box's interior blurs its texture. Each
// sample's sweep is weighted by how well its motion lines up with the
// direction sampled. Where the whole neighbourhood moves as one (a turning
// camera), every sample blurs alike, and the pixel is the plain average of
// the colour along its blur, by bilinear taps.
#version 460
#include "motion_common.glsl"
#include "tonemap.glsl"

layout(push_constant, scalar) uniform Push { NphRenderMotionBlurPush pc; };

layout(location = 0) in vec2 in_ndc;
layout(location = 0) out vec4 out_color;

// A pixel whose own blur reaches less than this many pixels past half a
// pixel turns its second direction toward the dominant one's perpendicular
// (Guertin et al. 2014 Eq. 1, gamma).
#define MB_DIRECTION_BLEND 1.5
// A sample is wholly in front of the pixel once it is nearer by this
// fraction of the nearer distance, and partly within it: a relative test,
// whatever the scene's scale (Guertin et al. 2014 §5, who blend over the
// whole nearer distance; that let a box 60 % farther count 40 % in front of
// one before it).
#define MB_DEPTH_SOFTNESS 0.05
// Near a tile's edge a pixel takes the neighbouring tile's dominant blur at
// random, so neighbouring tiles' blurs meet as noise, not a seam (Guertin
// et al. 2014 §4.2): a chance of a half at the edge, falling to none this
// far into the tile.
#define MB_EDGE_BLEND (0.25 * float(NPH_RENDER_MB_TILE))
// A neighbourhood moves as one while every blur in it lies within half a
// pixel of the dominant one, or within this fraction of its length: a
// turning camera's field varies smoothly across the view (by 73 % from the
// centre to the edge of a 90-degree one), and averaging along the dominant
// blur then lengthens a pixel's own by no more than that.
#define MB_UNIFORM 0.1

uint motion_at(ivec2 p)
{
    return texelFetch(nph_utextures_2d[pc.motion_index], p, 0).x;
}

// How much of `za` lies in front of `zb` (view distances): 1 when nearer or
// level, 0 once farther by MB_DEPTH_SOFTNESS of the nearer one.
float in_front(float za, float zb)
{
    return clamp(1.0 - (za - zb) / (MB_DEPTH_SOFTNESS * min(za, zb)), 0.0, 1.0);
}

// Whether a sweep reaching `reach` pixels either way passes a point `d`
// away, its end softened over a tenth of it (McGuire et al. 2012's
// cylinder).
float sweeps(float d, float reach)
{
    return 1.0 - smoothstep(0.95 * reach, 1.05 * reach, d);
}

vec3 colour_at(ivec2 p)
{
    return texelFetch(nph_textures_2d[pc.color_index], p, 0).rgb;
}

vec3 blur(ivec2 p, vec2 dominant, float reach, float spread, float jitter)
{
    if (spread < max(0.5, MB_UNIFORM * reach)) {
        // The neighbourhood moves as one: the average along the blur, at the
        // centres of n equal parts of it (a pixel apart, up to two for the
        // longest blurs), all moved by the jitter.
        const uint n = clamp(2u * uint(ceil(reach)), uint(NPH_RENDER_MB_MIN_SAMPLES),
                             uint(NPH_RENDER_MB_UNIFORM_SAMPLES));
        vec3 sum = vec3(0.0);
        for (uint i = 0u; i < n; ++i) {
            const float t = (float(i) + 0.5 + jitter) / float(n) * 2.0 - 1.0;
            const vec2 uv = (vec2(p) + 0.5 + t * dominant) * pc.inv_size;
            sum += textureLod(sampler2D(nph_textures_2d[pc.color_index],
                                        nph_samplers[pc.sampler_index]),
                              uv, 0.0)
                       .rgb;
        }
        return sum / float(n);
    }
    // Two directions of n / 2 samples each, a pixel apart until the bound.
    const uint n = clamp(4u * uint(ceil(reach)), uint(NPH_RENDER_MB_MIN_SAMPLES),
                         uint(NPH_RENDER_MB_MAX_SAMPLES));
    const float span = 4.0 * reach / float(n); // the pixels of line a sample stands for
    const ivec2 last = ivec2(pc.width, pc.height) - 1;
    const vec3 colour = colour_at(p);
    const uint own_motion = motion_at(p);
    const vec2 own_blur = nph_motion_blur(own_motion);
    const float own_z = nph_motion_distance(own_motion);
    const float own_reach = max(length(own_blur), 0.5);
    // A unit vector where the pixel moves, shorter where it barely does.
    const vec2 own_dir = own_blur / own_reach;
    // The two directions: the dominant blur's, and the pixel's own blur's,
    // which turns toward the dominant one's perpendicular (on the pixel's
    // side) as the pixel's own blur shrinks.
    const vec2 wn = dominant / reach;
    vec2 wp = vec2(-wn.y, wn.x);
    if (dot(wp, own_blur) < 0.0) {
        wp = -wp;
    }
    const vec2 wc = normalize(
        mix(wp, own_dir, clamp((own_reach - 0.5) / MB_DIRECTION_BLEND, 0.0, 1.0)));
    // Per direction (x: the dominant one, y: the second), the time the
    // front layer covers the pixel and the time the pixel's surface leaves
    // it to the background; the colours pooled.
    vec2 cover = vec2(0.0);
    vec2 leave = vec2(0.0);
    vec3 front_sum = vec3(0.0);
    float front_weight = 0.0;
    vec3 back_sum = vec3(0.0);
    float back_weight = 0.0;
    // The pixel's own surface: its own colour for the time its own point
    // covers it, then those of the points of its surface that sweep over it.
    vec3 own_sum = colour / (2.0 * own_reach);
    float own_weight = 1.0 / (2.0 * own_reach);
    for (uint i = 0u; i < n; ++i) {
        const float t = (float(i) + 0.5 + jitter) / float(n) * 2.0 - 1.0;
        const bool second = (i & 1u) != 0u;
        const vec2 dir = second ? wc : wn;
        const float d = abs(t) * reach;
        const ivec2 s = clamp(ivec2(floor(vec2(p) + 0.5 + t * reach * dir)), ivec2(0), last);
        const uint m = motion_at(s);
        const vec2 sample_blur = nph_motion_blur(m);
        const float sample_z = nph_motion_distance(m);
        const vec3 c = colour_at(s);
        const float sample_reach = max(length(sample_blur), 0.5);
        // In front of the pixel, behind it, and level with it.
        const float f = in_front(sample_z, own_z);
        const float b = in_front(own_z, sample_z);
        // The time the sample's point covers the pixel, sweeping its own
        // segment; and the time the pixel's surface, sweeping its own,
        // spends over the sample's place (a blur runs both ways along its
        // line, so only the alignment's size counts).
        const float its_sweep = abs(dot(sample_blur, dir)) / sample_reach *
                                sweeps(d, sample_reach) / (2.0 * sample_reach) * span;
        const float own_sweep =
            abs(dot(own_dir, dir)) * sweeps(d, own_reach) / (2.0 * own_reach) * span;
        const float wf = f * (1.0 - b) * its_sweep;
        const float wb = b * (1.0 - f) * own_sweep;
        const float wo = f * b * own_sweep;
        cover += second ? vec2(0.0, wf) : vec2(wf, 0.0);
        leave += second ? vec2(0.0, wb) : vec2(wb, 0.0);
        front_sum += c * wf;
        front_weight += wf;
        back_sum += c * wb;
        back_weight += wb;
        own_sum += c * wo;
        own_weight += wo;
    }
    // The directions' estimates: of the same line where they coincide (the
    // mean), of different ones where they part (the larger).
    const float together = abs(dot(wn, wc));
    const float front_alpha =
        min(mix(max(cover.x, cover.y), 0.5 * (cover.x + cover.y), together), 1.0);
    const float back_alpha =
        min(mix(max(leave.x, leave.y), 0.5 * (leave.x + leave.y), together), 1.0);
    const vec3 front = front_weight > 0.0 ? front_sum / front_weight : colour;
    const vec3 back = back_weight > 0.0 ? back_sum / back_weight : colour;
    return mix(mix(own_sum / own_weight, back, back_alpha), front, front_alpha);
}

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const ivec2 tiles = (ivec2(pc.width, pc.height) + NPH_RENDER_MB_TILE - 1) / NPH_RENDER_MB_TILE;
    // Interleaved gradient noise, moving every frame (Jimenez 2014): the
    // samples' jitter, and, along the other axis, the chance of taking the
    // tile across a near edge.
    const float jitter = nph_gradient_noise(gl_FragCoord.xy + 5.588238 * float(pc.noise)) - 0.5;
    const float chance = nph_gradient_noise(gl_FragCoord.yx + 3.141592 * float(pc.noise));
    ivec2 tile = p / NPH_RENDER_MB_TILE;
    const ivec2 inside = p - tile * NPH_RENDER_MB_TILE;
    const ivec2 toward = ivec2(2 * inside.x < NPH_RENDER_MB_TILE ? -1 : 1,
                               2 * inside.y < NPH_RENDER_MB_TILE ? -1 : 1);
    // Pixels to the nearer of the tile's edges in x and in y.
    const ivec2 edge = min(inside, NPH_RENDER_MB_TILE - 1 - inside);
    const bool across_x = edge.x <= edge.y;
    const float to_edge = float(across_x ? edge.x : edge.y) + 0.5;
    if (chance < 0.5 * max(1.0 - to_edge / MB_EDGE_BLEND, 0.0)) {
        tile += across_x ? ivec2(toward.x, 0) : ivec2(0, toward.y);
        tile = clamp(tile, ivec2(0), tiles - 1);
    }
    const vec4 nb = texelFetch(nph_textures_2d[pc.neighbours_index], tile, 0);
    const float reach = length(nb.xy);
    vec3 hdr = reach < 0.5 ? colour_at(p) : blur(p, nb.xy, reach, nb.z, jitter);
    if (pc.bloom > 0.0) {
        // Without a temporal resolve to have mixed it in (taa.frag), the
        // bloom chain's mip 0, as the tonemap pass mixes it.
        const vec3 glare = textureLod(sampler2D(nph_textures_2d[pc.bloom_index],
                                                nph_samplers[pc.sampler_index]),
                                      gl_FragCoord.xy * pc.bloom_uv, 0.0)
                               .rgb *
                           pc.bloom_scale;
        hdr = mix(hdr, glare, pc.bloom);
    }
    out_color = vec4(nph_tonemap(hdr, pc.tonemapper, pc.encode_srgb != 0u), 1.0);
}

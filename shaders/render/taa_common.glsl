// SPDX-License-Identifier: Apache-2.0
// Temporal anti-aliasing at native resolution (CLAUDE.md §8.6.8; docs/render.md
// "Temporal anti-aliasing"): the arithmetic shared by the production path's
// temporal resolve and its microbenchmark (benchmarks/gpu, bench_taa.*).
//
// Every frame renders with its projection shifted by a sub-pixel offset, so
// a pixel's samples over time cover its area; each frame's resolve blends
// the new sample into the history of the earlier ones, read where the
// surface the pixel sees was a frame ago (Karis, "High Quality Temporal
// Supersampling", SIGGRAPH 2014):
//   - the current frame is filtered over the pixel's 3 x 3 neighbourhood,
//     each sample weighted by its distance from the pixel's centre (the
//     samples lie at the jittered positions; Karis 2014), with a Gaussian
//     of a box filter's spread (taa_filter_weight);
//   - the history is read with a Catmull-Rom filter in five bilinear taps,
//     its four corner taps dropped (Jimenez, "Filmic SMAA: Sharp Morphological
//     and Temporal Antialiasing", SIGGRAPH 2016), which keeps it sharp
//     where bilinear reads would blur it a little every frame;
//   - history that no longer belongs to the neighbourhood (a disocclusion,
//     a change of lighting) is clipped in YCoCg toward the centre of the box
//     the neighbourhood's mean and standard deviation span, inside its
//     extremes (Salvi, "An Excursion in Temporal Supersampling", GDC 2016;
//     Pedersen, "Temporal Reprojection Anti-Aliasing in INSIDE", GDC 2016);
//   - the current sample and the history blend with weights divided by
//     1 + luma, so that a bright sub-pixel highlight does not flicker as
//     the jitter finds and loses it (Karis 2014), the current frame's
//     weight growing with the motion (taa_current_weight).
#ifndef NPH_TAA_COMMON_GLSL
#define NPH_TAA_COMMON_GLSL

#include "render_common.glsl"

// The current frame's weight in the blend (before the luma weights): a
// history of about 16 frames, 67 ms at 240 Hz.
#define TAA_CURRENT_WEIGHT 0.0625
// The clipping box's half-extent in the neighbourhood's standard
// deviations (Salvi 2016).
#define TAA_VARIANCE_GAMMA 1.0
// The box's smallest half-extent, as a fraction of its centre's luma: the
// history's quantisation (taa_clip).
#define TAA_BOX_FLOOR (1.0 / 32.0)
// The current frame's weight where the history moved TAA_MOVING_SPEED
// pixels or more (taa_current_weight): a history of about 4 frames.
#define TAA_MOVING_WEIGHT 0.25
#define TAA_MOVING_SPEED  1.0

// YCoCg (Malvar & Sullivan, "YCoCg-R: A Color Space with RGB Reversibility
// and Low Dynamic Range", 2003): luma and two chroma axes, in which the
// neighbourhood's box fits the colours more tightly than in RGB.
vec3 taa_rgb_to_ycocg(vec3 c)
{
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)),
                dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 taa_ycocg_to_rgb(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The neighbourhood of a pixel in the current frame, in YCoCg: the first
// and second moments and the extremes for the clipping box, and the
// filtered colour.
struct TaaNeighbourhood {
    vec3 m1;
    vec3 m2;
    vec3 lo;
    vec3 hi;
    vec3 filtered;
    float weight;
    float count;
};

TaaNeighbourhood taa_neighbourhood_begin()
{
    TaaNeighbourhood n;
    n.m1 = vec3(0.0);
    n.m2 = vec3(0.0);
    n.lo = vec3(1e30);
    n.hi = vec3(-1e30);
    n.filtered = vec3(0.0);
    n.weight = 0.0;
    n.count = 0.0;
    return n;
}

// The reconstruction filter's weight of a sample lying `offset` pixels
// from the pixel's centre: a Gaussian with a box filter's spread (sigma
// 1 / sqrt(12) pixels), so the samples a pixel gathers over the jitter
// sequence average to its area. Karis 2014's wider fit of the
// Blackman-Harris window (sigma 0.47) steadies a frame's own sample but
// softens edges: against a 4 x 4 supersampled image a still view converged
// to 0.76 levels of error with it and 0.39 with this one (test_taa.c; ADR-0034).
// C twin: render_taa_weights, which the pass's push constants carry,
// normalised, for the frame's jitter.
float taa_filter_weight(vec2 offset)
{
    return exp(-6.0 * dot(offset, offset));
}

// Adds the sample `rgb` with the reconstruction filter's weight `w`.
void taa_neighbourhood_add(inout TaaNeighbourhood n, vec3 rgb, float w)
{
    const vec3 c = taa_rgb_to_ycocg(rgb);
    n.m1 += c;
    n.m2 += c * c;
    n.lo = min(n.lo, c);
    n.hi = max(n.hi, c);
    n.filtered += c * w;
    n.weight += w;
    n.count += 1.0;
}

// One bilinear tap of the history (its indices are the same for every
// invocation: push constants).
vec3 taa_tap(uint texture_index, uint sampler_index, vec2 uv)
{
    return textureLod(sampler2D(nph_textures_2d[texture_index], nph_samplers[sampler_index]), uv,
                      0.0)
        .rgb;
}

// The history at `uv` through a Catmull-Rom filter: the 4 x 4 texels
// around the point, weighted separably, read as five bilinear taps whose
// positions fold each pair of inner weights into one tap; the four corners
// carry the smallest weights and are dropped, the rest renormalised
// (Jimenez 2016). Negative lobes can ring below zero: clamped.
vec3 taa_history_catmull_rom(uint texture_index, uint sampler_index, vec2 uv, vec2 size,
                             vec2 inv_size)
{
    const vec2 p = uv * size;
    const vec2 t1 = floor(p - 0.5) + 0.5;
    const vec2 f = p - t1;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 t0 = (t1 - 1.0) * inv_size;
    const vec2 t3 = (t1 + 2.0) * inv_size;
    const vec2 t12 = (t1 + w2 / w12) * inv_size;
    const float a = w12.x * w0.y;
    const float b = w0.x * w12.y;
    const float c = w12.x * w12.y;
    const float d = w3.x * w12.y;
    const float e = w12.x * w3.y;
    const vec3 sum = taa_tap(texture_index, sampler_index, vec2(t12.x, t0.y)) * a +
                     taa_tap(texture_index, sampler_index, vec2(t0.x, t12.y)) * b +
                     taa_tap(texture_index, sampler_index, t12) * c +
                     taa_tap(texture_index, sampler_index, vec2(t3.x, t12.y)) * d +
                     taa_tap(texture_index, sampler_index, vec2(t12.x, t3.y)) * e;
    return max(sum / (a + b + c + d + e), vec3(0.0));
}

// `history` (YCoCg) clipped to the neighbourhood: toward the centre of the
// box of mean +- TAA_VARIANCE_GAMMA standard deviations within the
// neighbourhood's extremes, stopping where the segment leaves the box
// (Pedersen 2016's clip rather than a clamp, which would shift the hue).
vec3 taa_clip(TaaNeighbourhood n, vec3 history)
{
    const vec3 mean = n.m1 / n.count;
    const vec3 sigma = sqrt(max(n.m2 / n.count - mean * mean, vec3(0.0)));
    const vec3 lo = max(n.lo, mean - TAA_VARIANCE_GAMMA * sigma);
    const vec3 hi = min(n.hi, mean + TAA_VARIANCE_GAMMA * sigma);
    const vec3 centre = 0.5 * (lo + hi);
    // At least the history's own rounding: B10G11R11 keeps 5 bits of blue
    // mantissa, so a grey history carries chroma up to 1/32 of its luma,
    // which a flat neighbourhood's degenerate box would otherwise read as
    // far outside and clip the whole history, luma and all, to the centre.
    const vec3 extent = max(0.5 * (hi - lo), vec3(max(centre.x, 0.0) * TAA_BOX_FLOOR + 1e-6));
    const vec3 v = history - centre;
    const vec3 a = abs(v) / extent;
    const float m = max(max(a.x, a.y), a.z);
    return m > 1.0 ? centre + v / m : history;
}

// The current frame's weight for a pixel whose history moved `speed`
// pixels: every frame that reads the history at a fractional offset
// softens its edges a little (the Catmull-Rom filter interpolates exactly
// only at texel centres), so a moving pixel keeps a shorter history (Yang,
// Liu & Salvi, "A Survey of Temporal Antialiasing Techniques", Computer
// Graphics Forum 2020).
float taa_current_weight(float speed)
{
    return mix(TAA_CURRENT_WEIGHT, TAA_MOVING_WEIGHT, clamp(speed / TAA_MOVING_SPEED, 0.0, 1.0));
}

// The new history (YCoCg): the filtered current sample and the clipped
// history, `current_weight` of the first, each weighted by 1 / (1 + luma).
vec3 taa_blend(vec3 current, vec3 history, float current_weight)
{
    const float wc = current_weight / (1.0 + max(current.x, 0.0));
    const float wh = (1.0 - current_weight) / (1.0 + max(history.x, 0.0));
    return (current * wc + history * wh) / (wc + wh);
}

#endif

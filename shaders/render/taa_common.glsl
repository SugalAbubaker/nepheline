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
//     samples lie at the jittered positions), with a Gaussian fit of the
//     Blackman-Harris window (Karis 2014);
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
//     the jitter finds and loses it (Karis 2014).
#ifndef NPH_TAA_COMMON_GLSL
#define NPH_TAA_COMMON_GLSL

#include "render_common.glsl"

// The current frame's weight in the blend (before the luma weights): a
// history of about 16 frames, 67 ms at 240 Hz.
#define TAA_CURRENT_WEIGHT 0.0625
// The clipping box's half-extent in the neighbourhood's standard
// deviations (Salvi 2016).
#define TAA_VARIANCE_GAMMA 1.0

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

// Adds the sample `rgb` lying `offset` pixels from the pixel's centre.
void taa_neighbourhood_add(inout TaaNeighbourhood n, vec3 rgb, vec2 offset)
{
    const vec3 c = taa_rgb_to_ycocg(rgb);
    n.m1 += c;
    n.m2 += c * c;
    n.lo = min(n.lo, c);
    n.hi = max(n.hi, c);
    // A Gaussian of sigma 0.47 pixels fitted to the Blackman-Harris window
    // (Karis 2014).
    const float w = exp(-2.29 * dot(offset, offset));
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
    const vec3 extent = max(0.5 * (hi - lo), vec3(1e-7));
    const vec3 v = history - centre;
    const vec3 a = abs(v) / extent;
    const float m = max(max(a.x, a.y), a.z);
    return m > 1.0 ? centre + v / m : history;
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

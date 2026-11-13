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
//   - the history is read with a cubic filter in five bilinear taps, its
//     four corner taps dropped (Jimenez, "Filmic SMAA: Sharp Morphological
//     and Temporal Antialiasing", SIGGRAPH 2016), a little sharper than
//     Catmull-Rom's (TAA_HISTORY_TENSION), which keeps it sharp where
//     bilinear reads would blur it a little every frame;
//   - history that no longer belongs to the neighbourhood (a disocclusion,
//     a change of lighting) is clipped in YCoCg toward the centre of the box
//     the neighbourhood's mean and standard deviation span, inside its
//     extremes (Salvi, "An Excursion in Temporal Supersampling", GDC 2016;
//     Pedersen, "Temporal Reprojection Anti-Aliasing in INSIDE", GDC 2016).
//     Where the history did not move, or is of the same surface by its
//     distance (taa_history_valid: each history pixel keeps the distance of
//     the surface it showed, and a pixel whose own surface was not what the
//     history shows where it was has been uncovered; ADR-0095), the box is
//     wider and not held to the extremes (ADR-0092): such a history is its
//     own surface's, and
//     detail finer than a pixel (a far building's window frames) lies
//     outside the extremes of every frame whose samples miss it, so a tight
//     box clipped it away and the next frame found it again: a shimmer of
//     whatever stands still far off;
//     A still pixel also keeps how far its frames' samples have lain from
//     its history lately, its unrest (taa_unrest), and the box is as much
//     wider: a pattern at the pixels' own pitch (a far fence's bars, a far
//     front's windows) puts every sample of a neighbourhood on a bar in one
//     frame and between two in the next, so the neighbourhood is flat either
//     way, its box a point, and the history clipped to it followed each
//     frame: the patch blinked. What a pixel's own frames span is not what
//     its history has to leave;
//   - the current sample and the history blend with weights divided by
//     1 + luma, so that a bright sub-pixel highlight does not flicker as
//     the jitter finds and loses it (Karis 2014), the current frame's
//     weight growing with the motion (taa_current_weight) and never under
//     one of the frames the history holds (taa_count_weight): a history
//     that has just stopped moving, or begun, is the mean of its frames
//     until it holds as many as a still one keeps.
#ifndef NPH_TAA_COMMON_GLSL
#define NPH_TAA_COMMON_GLSL

#include "render_common.glsl"

// The current frame's weight in the blend (before the luma weights) where
// the history did not move: a history of about 32 frames, 133 ms at 240 Hz
// (ADR-0092: with 16, each frame's sample of detail finer than a pixel moved
// a still pixel twice as far).
#define TAA_CURRENT_WEIGHT 0.03125
// The clipping box's half-extent in the neighbourhood's standard
// deviations (Salvi 2016).
#define TAA_VARIANCE_GAMMA 1.0
// The same for a pixel whose history did not move, whose box is not held
// within the neighbourhood's extremes either (taa_clip_box_moving).
#define TAA_STILL_GAMMA 3.0
// The box's smallest half-extent, as a fraction of its centre's luma: the
// history's quantisation (taa_clip).
#define TAA_BOX_FLOOR (1.0 / 32.0)
// A still pixel's unrest follows its frames over about 8 of them
// (taa_unrest), and widens the box by this many times itself
// (taa_clip_unrest): a third of the light and the history is not clipped.
#define TAA_UNREST_RATE  0.125
#define TAA_UNREST_GAMMA 3.0
// A pixel is still, for its unrest, where its history moved less than this many pixels, and
// less so up to it (taa_stillness): at a third of a pixel a frame an edge is across a pixel in
// three frames, and a history kept by its unrest trailed it (test_taa.c's pan).
#define TAA_UNREST_SPEED (1.0 / 16.0)
// The current frame's weight where the history moved TAA_MOVING_SPEED
// pixels or more (taa_current_weight): a history of about 4 frames.
#define TAA_MOVING_WEIGHT 0.25
#define TAA_MOVING_SPEED  0.25
// The history's cubic filter's tension (taa_history_cubic): Catmull-Rom's is
// -0.5. A history read at a fractional offset loses its finest detail every
// frame it is read, and what a history of 4 frames keeps of detail 4 pixels
// across is 3/4 with Catmull-Rom's; this one's passband is some 3 % over 1 at
// its widest and brings that back (ADR-0095: a view in motion was softer
// than the same view standing, by a fifth to a third of its fine detail,
// and thin shadows pulsed as the camera went and stopped). No further: a
// gain over 1 compounds with the history's length.
#define TAA_HISTORY_TENSION (-0.7)
// A history pixel's key (taa_depth_key): its surface's reverse-Z depth as
// this many stops of distance from the near plane, 0 to 1 in 16 bits (a
// 2700th of a percent a step).
#define TAA_KEY_STOPS 24.0
// The most frames a history pixel counts (taa_count_after): a still pixel's
// 1 / TAA_CURRENT_WEIGHT.
#define TAA_COUNT_MAX 32.0
// How far past the keys about it a point's own may lie and still be of their
// surface (taa_history_valid): 1.6 % of its distance.
#define TAA_KEY_SLACK (1.0 / 1024.0)

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
// filtered colour; and a mix every sample takes, c' = scale c + shift, which
// taa_clip_mixed applies to the box the samples span (taa_neighbourhood_mix).
struct TaaNeighbourhood {
    vec3 m1;
    vec3 m2;
    vec3 lo;
    vec3 hi;
    vec3 filtered;
    float weight;
    float count;
    float scale;
    vec3 shift;
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
    n.scale = 1.0;
    n.shift = vec3(0.0);
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

// The neighbourhood with `g` (YCoCg) mixed into every sample by `b`, as
// if each had been c' = (1 - b) c + b g (the bloom, bloom.comp). The same g
// for all, so the samples' box follows from the plain samples' (taa_clip_mixed):
// their mean and extremes take the mix too (1 - b >= 0 keeps the extremes'
// order), their deviation only its scale, and none of the moments need
// touching here. The filtered colour takes it now: the weights sum to 1,
// as taa_resolve's do (render_taa_weights).
void taa_neighbourhood_mix(inout TaaNeighbourhood n, vec3 g, float b)
{
    const float a = 1.0 - b;
    n.scale *= a;
    n.shift = a * n.shift + b * g;
    n.filtered = a * n.filtered + b * g;
}

// One bilinear tap of the history (its indices are the same for every
// invocation: push constants).
vec3 taa_tap(uint texture_index, uint sampler_index, vec2 uv)
{
    return textureLod(sampler2D(nph_textures_2d[texture_index], nph_samplers[sampler_index]), uv,
                      0.0)
        .rgb;
}

// The history at `uv` through a cubic filter (Keys, "Cubic Convolution
// Interpolation for Digital Image Processing", IEEE ASSP 1981, of tension
// TAA_HISTORY_TENSION; -0.5 is Catmull-Rom's): the 4 x 4 texels around the
// point, weighted separably, read as five bilinear taps whose positions fold
// each pair of inner weights into one tap; the four corners carry the
// smallest weights and are dropped, the rest renormalised (Jimenez 2016).
// It interpolates: at a texel's centre it is that texel. Negative lobes can
// ring below zero: clamped.
vec3 taa_history_cubic(uint texture_index, uint sampler_index, vec2 uv, vec2 size, vec2 inv_size)
{
    const vec2 p = uv * size;
    const vec2 t1 = floor(p - 0.5) + 0.5;
    const vec2 f = p - t1;
    const float a = TAA_HISTORY_TENSION;
    const vec2 w0 = a * f * (1.0 - f) * (1.0 - f);
    const vec2 w1 = 1.0 + f * f * ((a + 2.0) * f - (a + 3.0));
    const vec2 w2 = f * (-a + f * ((2.0 * a + 3.0) - (a + 2.0) * f));
    const vec2 w3 = a * f * f * (1.0 - f);
    const vec2 w12 = w1 + w2;
    const vec2 t0 = (t1 - 1.0) * inv_size;
    const vec2 t3 = (t1 + 2.0) * inv_size;
    const vec2 t12 = (t1 + w2 / w12) * inv_size;
    const float up = w12.x * w0.y;
    const float left = w0.x * w12.y;
    const float middle = w12.x * w12.y;
    const float right = w3.x * w12.y;
    const float down = w12.x * w3.y;
    const vec3 sum = taa_tap(texture_index, sampler_index, vec2(t12.x, t0.y)) * up +
                     taa_tap(texture_index, sampler_index, vec2(t0.x, t12.y)) * left +
                     taa_tap(texture_index, sampler_index, t12) * middle +
                     taa_tap(texture_index, sampler_index, vec2(t3.x, t12.y)) * right +
                     taa_tap(texture_index, sampler_index, vec2(t12.x, t3.y)) * down;
    return max(sum / (up + left + middle + right + down), vec3(0.0));
}

// The key a history's pixel keeps of the surface it showed: its reverse-Z
// `depth` (near plane 1, the sky 0) as stops of distance, 0 to 1.
float taa_depth_key(float depth)
{
    return clamp(-log2(max(depth, 1e-9)) / TAA_KEY_STOPS, 0.0, 1.0);
}

// Whether the history about a point shows the point's own surface: its key
// there, a frame ago (`expected`), lies within what the four history pixels
// about the point keep (`keys`), TAA_KEY_SLACK either side. A surface's
// depths about a point span the point's, however aslant it lies; a pixel
// that a nearer surface covered a frame ago finds only that surface's keys,
// all nearer than its own.
bool taa_history_valid(vec4 keys, float expected)
{
    const float lo = min(min(keys.x, keys.y), min(keys.z, keys.w));
    const float hi = max(max(keys.x, keys.y), max(keys.z, keys.w));
    return expected >= lo - TAA_KEY_SLACK && expected <= hi + TAA_KEY_SLACK;
}

// `history` (YCoCg) clipped to the samples' box: toward the centre of the
// box of `mean` +- TAA_VARIANCE_GAMMA standard deviations within their
// extremes [`lo`, `hi`], stopping where the segment leaves the box
// (Pedersen 2016's clip rather than a clamp, which would shift the hue).
// `moving` is how far towards TAA_MOVING_SPEED pixels the history moved (0 to 1): at 0 the box
// is `mean` +- TAA_STILL_GAMMA deviations, whatever the extremes. `spread` widens it each way.
vec3 taa_clip_box_spread(vec3 mean, vec3 sigma, vec3 extreme_lo, vec3 extreme_hi, vec3 history,
                         float moving, float spread)
{
    const float gamma = mix(TAA_STILL_GAMMA, TAA_VARIANCE_GAMMA, moving);
    const vec3 lo =
        mix(mean - gamma * sigma, max(extreme_lo, mean - gamma * sigma), moving) - spread;
    const vec3 hi =
        mix(mean + gamma * sigma, min(extreme_hi, mean + gamma * sigma), moving) + spread;
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

vec3 taa_clip_box_moving(vec3 mean, vec3 sigma, vec3 extreme_lo, vec3 extreme_hi, vec3 history,
                         float moving)
{
    return taa_clip_box_spread(mean, sigma, extreme_lo, extreme_hi, history, moving, 0.0);
}

// The same for a history that moved.
vec3 taa_clip_box(vec3 mean, vec3 sigma, vec3 extreme_lo, vec3 extreme_hi, vec3 history)
{
    return taa_clip_box_moving(mean, sigma, extreme_lo, extreme_hi, history, 1.0);
}

// `history` clipped to the neighbourhood's box (taa_clip_box).
vec3 taa_clip(TaaNeighbourhood n, vec3 history)
{
    const vec3 mean = n.m1 / n.count;
    const vec3 sigma = sqrt(max(n.m2 / n.count - mean * mean, vec3(0.0)));
    return taa_clip_box(mean, sigma, n.lo, n.hi, history);
}

// taa_clip for a neighbourhood taa_neighbourhood_mix may have mixed: the
// plain samples' statistics, then the mix they all take (its scale and shift
// are the identity's where none did, at the cost of the arithmetic).
vec3 taa_clip_mixed(TaaNeighbourhood n, vec3 history, float moving)
{
    const vec3 plain = n.m1 / n.count;
    const vec3 sigma = n.scale * sqrt(max(n.m2 / n.count - plain * plain, vec3(0.0)));
    return taa_clip_box_moving(n.scale * plain + n.shift, sigma, n.scale * n.lo + n.shift,
                               n.scale * n.hi + n.shift, history, moving);
}

// taa_clip_mixed with the box wider by the pixel's `unrest` (taa_unrest; 0 to 1) of the larger
// of the box's and the history's light, TAA_UNREST_GAMMA times.
vec3 taa_clip_unrest(TaaNeighbourhood n, vec3 history, float moving, float unrest)
{
    const vec3 plain = n.m1 / n.count;
    const vec3 sigma = n.scale * sqrt(max(n.m2 / n.count - plain * plain, vec3(0.0)));
    const vec3 mean = n.scale * plain + n.shift;
    const float spread = TAA_UNREST_GAMMA * unrest * max(max(mean.x, history.x), 0.0);
    return taa_clip_box_spread(mean, sigma, n.scale * n.lo + n.shift, n.scale * n.hi + n.shift,
                               history, moving, spread);
}

// How still a pixel whose history moved `speed` pixels is, for its unrest: 1 to 0 at
// TAA_UNREST_SPEED.
float taa_stillness(float speed)
{
    return clamp(1.0 - speed / TAA_UNREST_SPEED, 0.0, 1.0);
}

// A pixel's unrest after this frame: how far the luma of its frames' samples (`current`, the
// filtered one) has lain from its history's (`history`, before the clip), as a share of the
// larger, over the last 1 / TAA_UNREST_RATE frames or so; `was` is the history's. None where the
// history moved (`still`, taa_stillness): what a moving surface leaves behind is clipped as
// ever, so nothing trails it.
float taa_unrest(float was, float current, float history, float still)
{
    const float apart = abs(current - history) / (max(max(current, history), 0.0) + 1e-6);
    return mix(was, min(apart, 1.0), TAA_UNREST_RATE) * still;
}

// The current frame's weight for a pixel whose history moved `speed`
// pixels: every frame that reads the history at a fractional offset
// softens its edges a little (the cubic filter interpolates exactly
// only at texel centres), so a moving pixel keeps a shorter history (Yang,
// Liu & Salvi, "A Survey of Temporal Antialiasing Techniques", Computer
// Graphics Forum 2020). What a read softens grows with the square of its
// offset, and a history of 1 / weight frames is read that many times: the
// weight grows with the square of the speed, from the still pixel's to the
// moving one's at TAA_MOVING_SPEED, so that a slow pixel's history is no
// softer than a fast one's. Grown evenly with the speed, a pixel a quarter
// of a pixel a frame kept 12 frames of reads half a pixel off: a view
// walking slowly towards a wall showed it soft in a ring about where it
// headed, where the pixels crept, and sharp within and beyond (ADR-0094).
// Reached at half a pixel a frame, as it was, the ring was fainter and
// still there, a third of the finest detail gone at an eighth of a pixel a
// frame; at a quarter, what creeps keeps what the rest keeps (ADR-0095).
float taa_current_weight(float speed)
{
    const float s = clamp(speed / TAA_MOVING_SPEED, 0.0, 1.0);
    return max(TAA_CURRENT_WEIGHT, TAA_MOVING_WEIGHT * s * s);
}

// The current frame's weight for a pixel whose history moved `speed` pixels and holds `count`
// frames: taa_current_weight, and no less than the frame's share of a mean of count + 1. A
// history that moved holds 4 frames, soft from their reads; with the still pixel's weight
// alone it kept half of that softness for 22 frames after the camera stopped, a thin shadow
// coming into focus each time the view came to rest. As a mean, half is gone in 4 (ADR-0095).
// With no frame in the history the current one is all there is.
float taa_count_weight(float speed, float count)
{
    return max(taa_current_weight(speed), 1.0 / (max(count, 0.0) + 1.0));
}

// The frames a history holds after a blend of weight `weight`: a mean of 1 / weight, up to
// TAA_COUNT_MAX.
float taa_count_after(float weight)
{
    return min(1.0 / weight, TAA_COUNT_MAX);
}

// A key (taa_depth_key) as the two 8-bit halves a history's state keeps it in, each 0 to 1,
// and back from four pixels' halves (a gather of each).
vec2 taa_key_pack(float key)
{
    const uint k = uint(key * 65535.0 + 0.5);
    return vec2(float(k >> 8u), float(k & 255u)) / 255.0;
}

vec4 taa_keys_unpack(vec4 high, vec4 low)
{
    return (high * 65280.0 + low * 255.0) / 65535.0;
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

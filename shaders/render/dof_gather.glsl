// SPDX-License-Identifier: Apache-2.0
// The depth of field's gather (docs/render.md "Depth of field"; ADR-0037),
// shared by the half-resolution gather (dof_gather.comp) and the
// full-resolution one (dof_resolve.comp), which define, before including it:
//   DOF_FULL    whether the image gathered is the full-resolution one (the
//               HDR image and the depths) or the half-resolution one;
//   DOF_GROUP   the workgroup's side, in that image's pixels: a tile's or a
//               quarter of one, so that a workgroup is of one tile's class;
//   DOF_APRON   the most pixels a sample lies from its pixel: the largest
//               circle the tiles of that class hold;
//   DOF_SHARED  whether a workgroup first reads its texels and the apron
//               around them into shared memory (below), which must then fit
//               the 16 KiB every device has.
//
// Scattering as a gather (Jimenez, "Next Generation Post Processing in Call
// of Duty: Advanced Warfare", SIGGRAPH 2014), with the lens's own weights.
// Through a thin lens every surface point spreads its light evenly over its
// circle of confusion: a pixel inside the circle receives 1 / (pi c^2) of
// it per unit of the point's area. Each sample stands for an equal share of
// the disc the pixel gathers over (a spiral turned by the golden angle at
// radii sqrt((i + 0.5) / n) of it, Vogel's equal-area pattern, and the
// centre), so, by depth against the pixel's own surface:
//   - samples in front reach the pixel where their circles cover it, the
//     near layer, whose coverage is its alpha;
//   - samples level with the pixel give the colours its own circle mixes,
//     and samples behind show through where that circle holds something
//     other than its surface: the far layer is the average of the samples
//     not in front within the pixel's own circle.
// The disc's radius is the largest circle that can reach the pixel: the
// nearest one in front of the focus around it, or the pixel's own.
// Interleaved gradient noise, moving every frame, turns the spiral, which
// the temporal resolve then averages.
#ifndef NPH_DOF_GATHER_GLSL
#define NPH_DOF_GATHER_GLSL

#include "dof_common.glsl"

layout(local_size_x = DOF_GROUP, local_size_y = DOF_GROUP) in;

// The golden angle, 2 pi (2 - phi), which turns the spiral.
#define DOF_GOLDEN_ANGLE 2.39996323

// The image's pixels per full-resolution one.
#define DOF_SCALE (DOF_FULL ? 1.0 : 0.5)

// The image's extent in its own pixels.
ivec2 dof_extent()
{
    const ivec2 full = ivec2(pc.width, pc.height);
    return DOF_FULL ? full : (full + 1) / 2;
}

// A texel's colour and reverse-Z depth, at `q` within the image.
vec4 dof_fetch(ivec2 q)
{
    if (DOF_FULL) {
        return vec4(texelFetch(nph_textures_2d[pc.color_index], q, 0).rgb,
                    texelFetch(nph_textures_2d[pc.full_depth_index], q, 0).r);
    }
    return texelFetch(nph_textures_2d[pc.half_index], q, 0);
}

// A texel's circle's radius r in the image's pixels, at least half a pixel,
// and the share of its light a pixel inside that circle receives per unit
// of its area, 1 / (pi r^2); the radius half a pixel larger, the circle's
// edge soft over a pixel.
vec2 dof_reach_density(float depth)
{
    const float reach = max(abs(DOF_SCALE * dof_circle(depth)), 0.5);
    return vec2(reach + 0.5, 1.0 / (NPH_PI * reach * reach));
}

#if DOF_SHARED
// A workgroup reads its texels and DOF_APRON around them into shared memory
// once, with what every sample needs of them worked out there: a sample
// then costs a shared read, where it took a texture fetch or two and a
// reciprocal.
#define DOF_SPAN (DOF_GROUP + 2 * DOF_APRON)

// A texel as the samples take it, in half floats, pairs to a word: its
// colour, its depth, and dof_reach_density's (an array of a struct of its
// own: docs/build.md "Shaders").
struct DofTexel {
    uint rg;
    uint b_depth;
    uint reach_density;
};
shared DofTexel s_texels[DOF_SPAN * DOF_SPAN];

// Reads the workgroup's texels and their apron, clamped to the image, into
// shared memory; `origin` is the first one's place. A barrier must follow.
void dof_load(ivec2 origin)
{
    const ivec2 last = dof_extent() - 1;
    for (uint i = gl_LocalInvocationIndex; i < uint(DOF_SPAN * DOF_SPAN);
         i += uint(DOF_GROUP * DOF_GROUP)) {
        const ivec2 q =
            clamp(origin + ivec2(int(i) % DOF_SPAN, int(i) / DOF_SPAN), ivec2(0), last);
        const vec4 t = dof_fetch(q);
        s_texels[i].rg = packHalf2x16(t.rg);
        s_texels[i].b_depth = packHalf2x16(t.ba);
        s_texels[i].reach_density = packHalf2x16(dof_reach_density(t.a));
    }
}

// The texel at `q`, within the span read from `origin` (which holds every
// sample: a disc's radius is at most DOF_APRON): its colour and depth, and
// dof_reach_density's.
void dof_texel_at(ivec2 q, ivec2 origin, out vec4 colour_depth, out vec2 reach_density)
{
    const ivec2 at = q - origin;
    const DofTexel t = s_texels[at.y * DOF_SPAN + at.x];
    colour_depth = vec4(unpackHalf2x16(t.rg), unpackHalf2x16(t.b_depth));
    reach_density = unpackHalf2x16(t.reach_density);
}
#else
void dof_texel_at(ivec2 q, ivec2 origin, out vec4 colour_depth, out vec2 reach_density)
{
    colour_depth = dof_fetch(clamp(q, ivec2(0), dof_extent() - 1));
    reach_density = dof_reach_density(colour_depth.a);
}
#endif

// What a gather finds at a pixel: its far layer (its own surface over what
// that reveals behind it) and its near layer (what lies in front of it,
// premultiplied, its coverage in alpha), and its depth.
struct DofLayers {
    vec3 far;
    vec4 near;
    float depth;
};

// Gathers at pixel `p` (with DOF_SHARED, after dof_load(origin) and a
// barrier): `front` is the nearest circle in front of the focus around it
// (the image's pixels), and a sample stands for `sample_area` square pixels
// of the disc, within [min_samples, max_samples]. Texels past the image's
// edge repeat it.
DofLayers dof_gather(ivec2 p, ivec2 origin, float front, float sample_area, int min_samples,
                     int max_samples)
{
    vec4 own;
    vec2 own_rd;
    dof_texel_at(p, origin, own, own_rd);
    const float own_circle = abs(DOF_SCALE * dof_circle(own.a));
    const float reach = min(max(front, own_circle), float(DOF_APRON));
    DofLayers layers;
    layers.depth = own.a;
    if (reach < DOF_SCALE * NPH_RENDER_DOF_SHARP) {
        layers.far = own.rgb;
        layers.near = vec4(0.0);
        return layers;
    }
    const int n = int(clamp(ceil(NPH_PI * reach * reach / sample_area), float(min_samples),
                            float(max_samples)));
    // The depth test of dof_in_front against the pixel's own depth, as
    // in_front = s * relative - offset: a sample is wholly in front once
    // nearer by DOF_DEPTH_SOFTNESS of the pixel's distance.
    const float relative = 1.0 / (DOF_DEPTH_SOFTNESS * max(own.a, 1e-30));
    const float offset = own.a * relative;
    // The samples' weights are in units of a sample's share of the disc:
    // the centre's, the pixel itself, is one.
    float front_weight = 0.0;
    vec3 front_sum = vec3(0.0);
    float far_weight = 1.0;
    vec3 far_sum = own.rgb;
    // The spiral's first direction, then each next one turned by the golden
    // angle.
    const float turn = nph_gradient_noise(vec2(p) + 5.588238 * float(pc.noise)) * 2.0 * NPH_PI;
    vec2 dir = vec2(cos(turn), sin(turn));
    const vec2 golden = vec2(cos(DOF_GOLDEN_ANGLE), sin(DOF_GOLDEN_ANGLE));
    const float inv_n = 1.0 / float(n);
    for (int i = 0; i < n; ++i) {
        const float radius = reach * sqrt((float(i) + 0.5) * inv_n);
        const ivec2 q = ivec2(floor(vec2(p) + 0.5 + radius * dir));
        dir = vec2(dir.x * golden.x - dir.y * golden.y, dir.x * golden.y + dir.y * golden.x);
        vec4 s;
        vec2 rd;
        dof_texel_at(q, origin, s, rd);
        // The texel's own distance: a sharp sample's light stays within half
        // a pixel of it, so the spiral's inner points, under a pixel from the
        // centre, must not lend a neighbour's sharp colour to the pixel.
        const float d = length(vec2(q - p));
        const float in_front = clamp(s.a * relative - offset, 0.0, 1.0);
        // The sample's light at the pixel where its circle covers it, and
        // the pixel's own circle's part where it holds the sample.
        const float its = in_front * clamp(rd.x - d, 0.0, 1.0) * rd.y;
        const float mine = (1.0 - in_front) * clamp(own_rd.x - d, 0.0, 1.0);
        front_weight += its;
        front_sum += s.rgb * its;
        far_weight += mine;
        far_sum += s.rgb * mine;
    }
    layers.far = far_sum / far_weight;
    // The near layer's coverage: the light the samples bring, each standing
    // for a share of the disc's area.
    const float coverage = front_weight * NPH_PI * reach * reach / float(n + 1);
    const float alpha = min(coverage, 1.0);
    layers.near =
        vec4(front_weight > 0.0 ? front_sum / front_weight * alpha : vec3(0.0), alpha);
    return layers;
}

#endif

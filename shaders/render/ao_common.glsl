// SPDX-License-Identifier: Apache-2.0
// What the ambient occlusion's kernels share (docs/render.md "Ambient
// occlusion"; ADR-0038): their push constants, view-space positions rebuilt
// from depth, and lookups in the depths.
//
// The occlusion runs at full resolution, one value a pixel: texel (i, j) of
// every image is pixel (i, j), whose centre is at (i + 0.5, j + 0.5), in
// the pixels all positions on screen are given in here.
#ifndef NPH_AO_COMMON_GLSL
#define NPH_AO_COMMON_GLSL

#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderAoPush pc; };

// The largest half float: ao.comp stores the distances beyond it (the
// sky's) as it.
#define NPH_AO_HALF_MAX 65504.0

// The view distance of reverse-Z depth `d` (d = znear / distance); the
// sky's (depth 0) is NPH_RENDER_AO_SKY.
float ao_distance(float d)
{
    return min(pc.znear / d, NPH_RENDER_AO_SKY);
}

// The view-space position (the camera looks down -Z) of the point at view
// distance `dist` under the screen position `c` (full-resolution pixels).
vec3 ao_position(vec2 c, float dist)
{
    return dist * vec3(pc.unproject.xy * c + pc.unproject.zw, -1.0);
}

// The screen position of pixel `t`.
vec2 ao_centre(ivec2 t)
{
    return vec2(t) + 0.5;
}

// The reverse-Z depth of pixel `t`, clamped to the image.
float ao_depth(ivec2 t)
{
    return texelFetch(nph_textures_2d[pc.depths_index],
                      clamp(t, ivec2(0), ivec2(pc.width, pc.height) - 1), 0)
        .r;
}

// The reverse-Z depth at screen position `at` (pixels) from the depths' mip
// `mip`. Reverse-Z depth is affine in
// screen position across a plane, and every mip averages the depths below
// it, so where the texels around the point lie on a plane, their depths
// interpolated bilinearly put it exactly on that plane: a sample along a
// slice then lies in the slice's plane, as the horizon's integral assumes,
// where a texel's centre a little aside would rise above a plane seen at a
// grazing angle and occlude it falsely. On mip 0 (the steps near the pixel)
// the four texels are tested, and where they are not of one plane (their
// depths break the parallelogram rule: an edge, a crease) the nearest one's
// depth is taken instead; the coarser mips, sampled by the far steps, are
// interpolated as they are.
float ao_depth_at(vec2 at, int mip)
{
    // Texel i's sample lies at i + 0.5, l = at - 0.5 texels along. The
    // extent halves exactly down the mips (ao_depth.comp), so mip m's texel
    // k, which stands for its texels' centroid k 2^m + (2^m - 1) / 2, lies at
    // uv (k + 0.5) 2^m / extent: every mip has the point at uv
    // (l + 0.5) / extent.
    const vec2 l = at - 0.5;
    if (mip > 0) {
        return textureLod(sampler2D(nph_textures_2d[pc.depths_index],
                                    nph_samplers[pc.linear_sampler]),
                          (l + 0.5) * pc.depth_texel, float(mip))
            .r;
    }
    // The four texels around the point, gathered at the corner they share so
    // that the hardware's rounding of a point on a row or column of texels
    // cannot pick other ones than the weights are for. In the gather's
    // order: (x0, y1), (x1, y1), (x1, y0), (x0, y0).
    const vec2 corner = floor(l);
    const vec2 f = l - corner;
    const vec4 d = textureGather(
        sampler2D(nph_textures_2d[pc.depths_index], nph_samplers[pc.point_sampler]),
        (corner + 1.0) * pc.depth_texel, 0);
    if (abs(d.x + d.z - d.y - d.w) <= 1e-3 * max(max(d.x, d.y), max(d.z, d.w))) {
        return mix(mix(d.w, d.z, f.x), mix(d.x, d.y, f.x), f.y);
    }
    return f.y < 0.5 ? (f.x < 0.5 ? d.w : d.z) : (f.x < 0.5 ? d.x : d.y);
}

// The surface's tangent plane at pixel `t`, from `q`, a
// quantity proportional to the inverse of the view distance (reverse-Z
// depth, or 1 / distance), at the texel and its neighbours (`left`,
// `right`, `up`, `down`). Such a q is affine in screen position across a
// plane: for the plane of unit normal n through the points p with
// n . p = k, the point under screen position c is distance times its ray
// r(c) = (unproject.xy c + unproject.zw, -1) (ao_position), and its
// distance k / (n . r(c)), so q(c) = m . r(c) with m = s n / k for the
// scale s of q. The plane's m follows from q's screen gradient: m.xy is it
// over unproject.xy, and m.z what makes m . r(c) the texel's q. The
// gradient takes, on each axis, the side whose q differs the less, so as
// not to reach across an edge, and the side within the image at its edges;
// on a plane it is exact. m points away from the camera (m . r = q > 0),
// so the normal facing it is -normalize(m).
vec2 ao_gradient(ivec2 t, float q, float left, float right, float up, float down)
{
    const ivec2 last = ivec2(pc.width, pc.height) - 1;
    const bool use_right = t.x == 0 || (t.x < last.x && abs(right - q) < abs(left - q));
    const bool use_down = t.y == 0 || (t.y < last.y && abs(down - q) < abs(up - q));
    return vec2(use_right ? right - q : q - left, use_down ? down - q : q - up);
}

vec3 ao_plane(ivec2 t, float q, vec2 gradient)
{
    const vec2 m = gradient * pc.project;
    return vec3(m, dot(m, pc.unproject.xy * ao_centre(t) + pc.unproject.zw) - q);
}

#endif

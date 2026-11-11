// SPDX-License-Identifier: Apache-2.0
// What both of the reflection's passes (reflect_rays.comp, which fills a smooth pixel's reflection
// cell, and reflect.comp, which looks it up; docs/adr/0077-reflections.md) take from the pixel to
// key its cell (ADR-0086). The includer has the view's records (vis_common.glsl), the 64-bit
// images (nph_bindless_int64.glsl) and the cells (gi_common.glsl).
#ifndef NPH_REFLECT_COMMON_GLSL
#define NPH_REFLECT_COMMON_GLSL

// What keys a smooth pixel's reflection cell: a point of its surface, the surface's facing (its
// geometric normal, towards the camera), and a mirror direction (gi_reflect_oct).
struct ReflectKey {
    vec3 p;
    vec3 n;
    uvec2 d;
};

// The depth of the visibility buffer `visbuffer` (bindless storage) at `p`, or -1 off the image.
float reflect_depth(uint visbuffer, ivec2 p, ivec2 size)
{
    if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) {
        return -1.0;
    }
    return vis_depth(imageLoad(nph_u64images_2d[nonuniformEXT(visbuffer)], p).x);
}

// The depth's change a pixel along one axis across the plane a pixel of depth `d` shows, from its
// neighbours `before` and `after` on that axis (-1 off the image). Across a plane depth is affine,
// so the two sides change alike, and across a smooth surface nearly so; `ok` falls where they
// differ by more than a tenth or change by more than a twentieth of `d` a pixel. That is an edge
// (whose pixel's ray, unjittered, may meet the other surface), the image's edge, or foliage, whose
// neighbours lie on other leaves at much the same depth. (A side alone, where the other is an edge,
// keyed edges worse: the city's crossing 1.4 levels from its truth on average to 2.7.)
float reflect_gradient(float d, float before, float after, inout bool ok)
{
    const float b = d - before;
    const float a = after - d;
    ok = ok && abs(a - b) <= 0.1 * (abs(a) + abs(b)) + 1e-7 * d && abs(a) + abs(b) <= 0.1 * d;
    return 0.5 * (a + b);
}

// The key of the smooth pixel `p` (pixel centre `q` in NDC) whose depth is `d` and camera-relative
// point `x` this frame, of shading normal `n` (towards the camera), from the visibility buffer
// `visbuffer`: the same whatever the frame's jitter, so that every frame finds the cell its rays
// filled. Where a surface is seen at a grazing angle a cell can be smaller than what a pixel
// covers, and a tile's rays come once in 32 frames, at the same one of the jitter's eight
// offsets: keyed by the jittered point, a pixel would find its cell one frame in eight.
//
// Depth (reverse Z, a reciprocal of the distance) is affine across a plane on the screen, so the
// neighbours' depths give the plane's (reflect_gradient): the point is where the ray through the
// pixel's centre, without the jitter, meets it, and the facing is its normal, the surface's own,
// which neither a normal map nor a mesh's smoothed normals turn. The mirror direction is that
// ray's about the shading normal. Where the plane cannot be told (foliage, thin things), the
// pixel's own point and its shading normal: a leaf's neighbours lie on other leaves, and a plane
// through them would move its point further than the jitter does.
ReflectKey reflect_key(VisViewRef view, ViewRef rview, uint visbuffer, ivec2 p, vec2 q, float d,
                       vec3 x, vec3 n)
{
    const ivec2 size = ivec2(view.v.viewport.xy);
    bool ok = d > 0.0;
    const float gx = reflect_gradient(d, reflect_depth(visbuffer, p - ivec2(1, 0), size),
                                      reflect_depth(visbuffer, p + ivec2(1, 0), size), ok);
    const float gy = reflect_gradient(d, reflect_depth(visbuffer, p - ivec2(0, 1), size),
                                      reflect_depth(visbuffer, p + ivec2(0, 1), size), ok);
    ReflectKey k;
    k.p = x;
    k.n = n;
    if (ok) {
        // The jitter in pixels: the centre's ray lies at q + jitter in this frame's NDC.
        const vec2 shift = rview.v.jitter.xy * 0.5 * view.v.viewport.xy;
        const float dc = d + gx * shift.x + gy * shift.y;
        const vec4 c = rview.v.inv_view_proj * vec4(q + rview.v.jitter.xy, dc, 1.0);
        const vec2 step = 2.0 * view.v.viewport.zw;
        const vec4 hx = rview.v.inv_view_proj * vec4(q.x + step.x, q.y, d + gx, 1.0);
        const vec4 hy = rview.v.inv_view_proj * vec4(q.x, q.y + step.y, d + gy, 1.0);
        const vec3 face = cross(hx.xyz / hx.w - x, hy.xyz / hy.w - x);
        if (dot(face, face) > 0.0) {
            k.p = c.xyz / c.w;
            k.n = normalize(dot(face, x) > 0.0 ? -face : face);
        }
    }
    k.d = gi_reflect_oct(reflect(normalize(k.p), n));
    return k;
}

#endif

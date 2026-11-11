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
    bool stable; // from the surface's plane, so the same whatever the jitter
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
    k.stable = false;
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
            k.stable = true;
        }
    }
    k.d = gi_reflect_oct(reflect(normalize(k.p), n));
    return k;
}

// What a smooth pixel reflects: the radiance of its reflection cell (keyed by `key`) and how far it
// can be trusted, zero where there is none. The cell is that of the pixel's own direction, every
// frame (ADR-0086): its rays filled it, from this pixel and those near it. (A neighbouring
// direction's cell, which a random pick under temporal anti-aliasing once took now and then, is
// one that a flat surface's rays seldom fill: the pick fell back on the sky about half the time,
// blocks of a sky the rays never see, flickering.) Its age is set back to 0 where `touch`
// (reflect.comp, ADR-0087): a cell is kept while pixels look it up (NPH_RENDER_REFLECT_MAX_AGE),
// however long its tile waits for its turn. `missing` tells that the cell holds no sample and the
// key holds from frame to frame, for which the pixel's tile asks the next frame's rays
// (reflect_want). (Neighbouring direction bins'
// and levels' cells, blended to stand in until then, moved the city's moving views' error by no
// more than its noise: a camera's step gives a pixel new spatial cells as often as new
// directions.)
vec4 reflect_lookup(GiViewRef g, ReflectKey key, uint rough, bool touch, out bool missing)
{
    const uint slot = gi_find_in(g.g.reflect_keys, g.g.reflect_bucket_mask,
                                 gi_reflect_key(g, key.p, key.n, key.d, rough));
    vec4 own = vec4(0.0);
    if (slot != GI_NONE) {
        const uvec4 c = GiCellsRef(g.g.reflect_cells).c[slot];
        // A narrow lobe's samples hardly differ: one is enough for the smoothest, four at the end.
        own = vec4(uintBitsToFloat(c.xyz),
                   min(float(gi_word_count(c.w)) / float(1u + (rough >> 2u)), 1.0));
        if (touch && gi_word_age(c.w) != 0u) {
            atomicAnd(GiKeysRef(g.g.reflect_cells).k[slot * 4u + 3u],
                      ~(0xFFu << uint(NPH_RENDER_GI_COUNT_BITS)));
        }
    }
    // Asked for only where the key holds from frame to frame: an edge's or a leaf's follows the
    // jitter, and a cell made for it would be made anew every frame and never found again.
    missing = own.w <= 0.0 && key.stable;
    return own;
}

// The word and bit of tile `tile` (x, y) in the tiles' bits (NphRenderGiView.reflect_want).
uvec2 reflect_tile_bit(GiViewRef g, uvec2 tile)
{
    const uint index = tile.y * (g.g.reflect_width >> 3u) + tile.x;
    return uvec2(index >> 5u, 1u << (index & 31u));
}

#endif

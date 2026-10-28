// SPDX-License-Identifier: Apache-2.0
// Shared by the virtual shadow map's passes (vsm_*), the depth pyramid that
// marks its pages (vis_hzb.comp), and the shading that samples it
// (vis_resolve.glsl, forward_frag.glsl): light space, the clipmap's levels
// and pages, page requests, culling against the pages to draw, and the
// shadow test (shaders/shared/nph_vsm.h; docs/render.md "Shadows"). The
// includer includes vis_common.glsl (or render_common.glsl and the
// VisUintRef it needs) first.
//
// Two indexings of a level's pages:
//   - an entry (the page table, kept from frame to frame) is the page's
//     slot, its absolute coordinates modulo the window, so a page keeps its
//     entry while the window scrolls over it;
//   - a bit (the frame's bitmasks: requests, dirty, render) is the page's
//     place in this frame's window, row by row, so a rectangle of pages is
//     a run of words per row.
//
// The view is read through its reference, field by field, so a shader
// holds only what it uses.
#ifndef NPH_VSM_COMMON_GLSL
#define NPH_VSM_COMMON_GLSL

#include "nph_vsm.h"

NPH_BUFFER_REF(VsmViewRef) { NphVsmView v; };
NPH_BUFFER_REF(VsmEntriesRef) { NphVsmEntry e[]; };
NPH_BUFFER_REF(VsmPhysRef) { NphVsmPhys p[]; };
NPH_BUFFER_REF(VsmCountersRef) { NphVsmCounters c; };
NPH_BUFFER_REF(VsmArgsRef) { NphVsmArgs a; };
NPH_BUFFER_REF(VsmUvec2Ref) { uvec2 v[]; };
NPH_BUFFER_REF(VsmUvec4Ref) { uvec4 v[]; };
NPH_BUFFER_REF(VsmBitsRef) { uint w[]; };

#define VSM_NONE 0xFFFFFFFFu

// A caster work item is (instance slot, meshlet | level << 24), the meshlet
// under 2^24; VSM_ITEM_LOD marks a cluster of a mesh's levels that the walk
// (vsm_cull_nodes.comp) took from a group too coarse for the level, which
// vsm cull meshlets keeps only where its maker's error is fine enough.
#define VSM_ITEM_LOD        0x40000000u
#define VSM_ITEM_LEVEL_MASK 0x3Fu

// The error a caster's levels of detail may have, in texels of the level it
// is drawn into: within the receiver's offset along its normal
// (NphVsmView.texel_bias, 1.5 texels) with a texel to spare.
#define VSM_LOD_TEXELS 0.5

// floor(log2(x)) of a positive float from its exponent (-127 for 0 and
// denormals, 128 for infinity).
int vsm_floor_log2(float x)
{
    return int((floatBitsToUint(x) >> 23u) & 0xFFu) - 127;
}

// The threshold of a caster's cut in a level, in mesh units: the largest
// power of two within VSM_LOD_TEXELS of the level's texel at the instance's
// scale. A cluster is drawn into the level's pages when its maker's error
// is at most this and its consumer's more (render/cluster_lod.h): one cut
// for the whole level, the same every frame, so a page drawn stays right
// while the camera moves. Below every error of a mesh it draws level 0,
// above every one its coarsest level.
float vsm_lod_threshold(VsmViewRef r, int level, float scale)
{
    const float allowed =
        float(VSM_LOD_TEXELS) * r.v.levels[level].page_size / (float(NPH_VSM_PAGE) * scale);
    return exp2(float(vsm_floor_log2(allowed)));
}

// Whether a sphere (light-space centre, radius) spans a page of a level to
// draw this frame and does not lie wholly beyond the level's far plane. A
// sphere spanning more pages than VSM_SCAN_PAGES is taken as touching one
// without scanning the bitmask (its parts are tested).
#define VSM_SCAN_PAGES 256
bool vsm_touches_render(VsmViewRef r, int level, vec3 centre, float radius);

// How far below a level's boundary (in log2 of the footprint) the shading
// already takes the finer level. The requests rebuild a pixel's point from
// its depth, the shading interpolates it, and the two can differ by a few
// ULPs: with the shading biased to the finer side, it reads the level the
// request took or the one finer, which it falls back from to the requested
// one (vsm_shadow).
#define VSM_LEVEL_SLACK 0.001

// A camera-relative point in light space (before a level's frame).
vec3 vsm_light_space(VsmViewRef r, vec3 p)
{
    return vec3(dot(r.v.axis_u.xyz, p), dot(r.v.axis_v.xyz, p), dot(r.v.axis_d.xyz, p));
}

// A light-space point in a level's frame: x and y in pages from the
// window's corner, z the depth the pool stores (0 toward the light).
vec3 vsm_level_coords(VsmViewRef r, int level, vec3 ls)
{
    return vec3(ls.xy * r.v.levels[level].inv_page_size, ls.z * r.v.levels[level].depth_scale) +
           r.v.levels[level].origin.xyz;
}

ivec2 vsm_window(VsmViewRef r, int level)
{
    return ivec2(r.v.levels[level].window_x, r.v.levels[level].window_y);
}

// The level whose texels match a pixel's footprint `distance` metres from
// the camera: resolution-matched shadow maps (Lefohn, Sengupta & Owens,
// ACM TOG 2007), the footprint doubling with each level. `slack` moves the
// boundaries toward the coarser side (VSM_LEVEL_SLACK).
int vsm_level_of(VsmViewRef r, float distance, float slack)
{
    const float x = log2(max(distance * r.v.lod_scale, 1.0)) - slack;
    return clamp(int(ceil(x)), 0, NPH_VSM_LEVELS - 1);
}

// An absolute page's entry: its slot (coordinates modulo the window).
uint vsm_entry(int level, ivec2 page)
{
    const ivec2 s = page & (NPH_VSM_WINDOW - 1);
    return uint(level) * uint(NPH_VSM_LEVEL_ENTRIES) + uint(s.y * NPH_VSM_WINDOW + s.x);
}

// A window-relative page's bit.
uint vsm_bit_of(int level, ivec2 rel)
{
    return uint(level) * uint(NPH_VSM_LEVEL_ENTRIES) + uint(rel.y * NPH_VSM_WINDOW + rel.x);
}

// The absolute page an entry's slot holds in this frame's window of a
// level: the one inside the window whose coordinates equal the slot's
// modulo the window.
ivec2 vsm_slot_page(ivec2 window, uint slot)
{
    const ivec2 s = ivec2(int(slot % uint(NPH_VSM_WINDOW)), int(slot / uint(NPH_VSM_WINDOW)));
    return window + ((s - window) & (NPH_VSM_WINDOW - 1));
}

bool vsm_inside(vec2 c)
{
    return all(greaterThanEqual(c, vec2(0.0))) && all(lessThan(c, vec2(float(NPH_VSM_WINDOW))));
}

bool vsm_bit(uint64_t bits, uint bit)
{
    return (VsmBitsRef(bits).w[bit >> 5u] & (1u << (bit & 31u))) != 0u;
}

#ifdef NPH_VSM_REQUESTS
// (The page requests need subgroup vote, ballot and arithmetic, which the
// includer enables.)

// Sets a bit in a bitmask, once per subgroup for every distinct bit: the
// invocations with the first pending bit take it together, one of them
// sets it, and the rest go round again (a "waterfall"; neighbouring pixels
// mostly need the same page). Every active invocation of the subgroup
// calls it.
void vsm_set_bit(uint64_t bits, uint bit)
{
    bool pending = bit != VSM_NONE;
    while (subgroupAny(pending)) {
        if (pending) {
            const uint first = subgroupBroadcastFirst(bit);
            if (bit == first) {
                if (subgroupElect()) {
                    atomicOr(VsmBitsRef(bits).w[bit >> 5u], 1u << (bit & 31u));
                }
                pending = false;
            }
        }
    }
}

// The requests take every quantity they compare through the functions
// below (precise, their fused operations explicit), so a pixel's point
// and what follows from it come out the same to the bit wherever they are
// computed: one pixel at a time (vsm_request) or a block of four at once
// (vsm_block.glsl), which relies on it.

// A pixel's NDC from its coordinates, `scale` 2 / the target's extent.
vec2 vsm_ndc(ivec2 pixel, vec2 scale)
{
    precise vec2 ndc = fma(vec2(pixel) + 0.5, scale, vec2(-1.0));
    return ndc;
}

// A pixel's point in light space from its NDC and depth (not 0), through
// the view's rays (nph_vsm.h).
vec3 vsm_ray_point(VsmViewRef r, vec2 ndc, float depth)
{
    precise vec3 l =
        fma(r.v.ray_y.xyz, vec3(ndc.y), fma(r.v.ray_x.xyz, vec3(ndc.x), r.v.ray_o.xyz));
    precise vec3 p = l * (1.0 / depth);
    return p;
}

// A light-space point's squared distance from the camera times the square
// of lod_scale (light space only turns camera-relative space), whose level
// is vsm_level_of_q.
float vsm_q(VsmViewRef r, vec3 ls)
{
    precise float q = dot(ls, ls) * (r.v.lod_scale * r.v.lod_scale);
    return q;
}

// The level vsm_level_of picks without slack for a point whose vsm_q is
// `q`: ceil(log2(sqrt(q))), clamped, from q's exponent e and whether its
// mantissa is zero, without the root or the logarithm. With q = 2^e (1 +
// f), log2(sqrt(q)) is (e + log2(1 + f)) / 2, whose ceiling is (e + 1) / 2
// rounded down for f = 0 and (e + 2) / 2 rounded down otherwise. A level L
// above 0 thus takes exactly the q above 4^(L - 1).
int vsm_level_of_q(float q)
{
    const uint b = floatBitsToUint(max(q, 1.0));
    const int e = int(b >> 23u) - 127;
    return min((e + ((b & 0x7FFFFFu) != 0u ? 2 : 1)) >> 1, NPH_VSM_LEVELS - 1);
}

// A light-space point's x and y in a level's frame (vsm_level_coords).
// Increasing in the point's, the scale being positive.
vec2 vsm_level_xy(VsmViewRef r, int level, vec2 ls)
{
    precise vec2 c =
        fma(ls, vec2(r.v.levels[level].inv_page_size), r.v.levels[level].origin.xy);
    return c;
}

// A request for a level's window-relative pages lo to hi (at most one apart
// each way), packed as level | lo.x << 4 | lo.y << 11 | (hi.x > lo.x) << 18
// | (hi.y > lo.y) << 19.
uint vsm_pack_request(int level, ivec2 lo, ivec2 hi)
{
    return uint(level) | (uint(lo.x) << 4u) | (uint(lo.y) << 11u) | (uint(hi.x - lo.x) << 18u) |
           (uint(hi.y - lo.y) << 19u);
}

// The pages the shading of a point may read, as a packed request
// (vsm_pack_request), or VSM_NONE beyond every window: the finest level from
// the point's distance whose window holds it with a margin, and there its
// page with any other within the margin (r.v.margin pages: the shading's
// offset along the normal, and slack), so at most 2 x 2. `ls` is the point
// in light space.
uint vsm_request(VsmViewRef r, vec3 ls)
{
    const float m = r.v.margin;
    const float last = float(NPH_VSM_WINDOW) - m;
    for (int level = vsm_level_of_q(vsm_q(r, ls)); level < NPH_VSM_LEVELS; ++level) {
        const vec2 c = vsm_level_xy(r, level, ls.xy);
        if (all(greaterThanEqual(c, vec2(m))) && all(lessThan(c, vec2(last)))) {
            return vsm_pack_request(level, ivec2(floor(c - m)), ivec2(floor(c + m)));
        }
    }
    return VSM_NONE;
}

// Page `k` (0 to 3, x first) of a packed request's rectangle as its bit, or
// VSM_NONE where the rectangle has no such page.
uint vsm_request_bit(uint request, uint k)
{
    if (request == VSM_NONE || ((k & 1u) != 0u && (request & (1u << 18u)) == 0u) ||
        ((k & 2u) != 0u && (request & (1u << 19u)) == 0u)) {
        return VSM_NONE;
    }
    const ivec2 rel = ivec2((request >> 4u) & 0x7Fu, (request >> 11u) & 0x7Fu) +
                      ivec2(int(k & 1u), int(k >> 1u));
    return vsm_bit_of(int(request & 0xFu), rel);
}

// Sets a packed request's pages (not VSM_NONE) in a bitmask: a row of one
// page or two (neighbouring bits, in one word or across two), and the same
// in the row below where the rectangle has two rows.
void vsm_set_request(uint64_t bits, uint request)
{
    const uint bit = vsm_request_bit(request, 0u);
    const bool wide = (request & (1u << 18u)) != 0u;
    const uint run = wide ? 3u : 1u;
    const uint w = bit >> 5u;
    const uint s = bit & 31u;
    atomicOr(VsmBitsRef(bits).w[w], run << s);
    if (wide && s == 31u) {
        atomicOr(VsmBitsRef(bits).w[w + 1u], 1u);
    }
    if ((request & (1u << 19u)) != 0u) {
        const uint below = w + uint(NPH_VSM_WINDOW / 32);
        atomicOr(VsmBitsRef(bits).w[below], run << s);
        if (wide && s == 31u) {
            atomicOr(VsmBitsRef(bits).w[below + 1u], 1u);
        }
    }
}
#endif

// The window-relative pages a sphere (light-space centre, radius) spans in
// a level, as (x0, x1, y0, y1) clamped to the window; empty (x0 > x1)
// where it misses the window.
ivec4 vsm_sphere_rect(VsmViewRef r, int level, vec3 centre, float radius)
{
    const float inv = r.v.levels[level].inv_page_size;
    const vec2 c = centre.xy * inv + r.v.levels[level].origin.xy;
    const float k = radius * inv;
    const vec2 lo = floor(c - k);
    const vec2 hi = floor(c + k);
    const float last = float(NPH_VSM_WINDOW - 1);
    if (hi.x < 0.0 || hi.y < 0.0 || lo.x > last || lo.y > last) {
        return ivec4(1, 0, 0, 0);
    }
    const vec2 a = clamp(lo, vec2(0.0), vec2(last));
    const vec2 b = clamp(hi, vec2(0.0), vec2(last));
    return ivec4(int(a.x), int(b.x), int(a.y), int(b.y));
}

// Whether a sphere lies wholly beyond a level's far plane (away from the
// light), where it shadows nothing the level's pages hold. Nearer than the
// near plane it still shadows: the raster clamps its depth to 0.
bool vsm_beyond(VsmViewRef r, int level, vec3 centre, float radius)
{
    return (centre.z - radius) * r.v.levels[level].depth_scale + r.v.levels[level].origin.z > 1.0;
}

// The mask of row word `w` (columns 32 w to 32 w + 31) within [x0, x1].
uint vsm_word_mask(uint w, int x0, int x1)
{
    const uint w0 = uint(x0) >> 5u;
    const uint w1 = uint(x1) >> 5u;
    uint mask = 0xFFFFFFFFu;
    if (w == w0) {
        mask &= 0xFFFFFFFFu << (uint(x0) & 31u);
    }
    if (w == w1) {
        mask &= 0xFFFFFFFFu >> (31u - (uint(x1) & 31u));
    }
    return mask;
}

// The bits of row `y` of a level's bitmask in columns [x0, x1], word `w`.
uint vsm_row_word(uint64_t bits, int level, int y, uint w)
{
    return VsmBitsRef(bits)
        .w[uint(level) * uint(NPH_VSM_LEVEL_WORDS) + uint(y) * uint(NPH_VSM_WINDOW / 32) + w];
}

// Word `k` of a level's render rows (rows 32 k to 32 k + 31) within
// [y0, y1]: the rows with a page to draw this frame.
uint vsm_render_rows(VsmViewRef r, int level, uint k, int y0, int y1)
{
    return VsmBitsRef(r.v.render_rows).w[uint(level) * uint(NPH_VSM_ROW_WORDS) + k] &
           vsm_word_mask(k, y0, y1);
}

// Whether any page of a window-relative rectangle (x0, x1, y0, y1) of a
// level is drawn this frame. Only the rows with a page to draw are read.
bool vsm_render_any(VsmViewRef r, int level, ivec4 rect)
{
    if (rect.x > rect.y) {
        return false;
    }
    for (uint k = uint(rect.z) >> 5u; k <= uint(rect.w) >> 5u; ++k) {
        uint rows = vsm_render_rows(r, level, k, rect.z, rect.w);
        while (rows != 0u) {
            const int y = int(k * 32u + uint(findLSB(rows)));
            rows &= rows - 1u;
            for (uint w = uint(rect.x) >> 5u; w <= uint(rect.y) >> 5u; ++w) {
                if ((vsm_row_word(r.v.render, level, y, w) & vsm_word_mask(w, rect.x, rect.y)) !=
                    0u) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool vsm_touches_render(VsmViewRef r, int level, vec3 centre, float radius)
{
    if (vsm_beyond(r, level, centre, radius)) {
        return false;
    }
    const ivec4 rect = vsm_sphere_rect(r, level, centre, radius);
    const int area = (rect.y - rect.x + 1) * (rect.w - rect.z + 1);
    return rect.x <= rect.y && (area > VSM_SCAN_PAGES || vsm_render_any(r, level, rect));
}

// The physical page's first texel in the pool.
ivec2 vsm_pool_base(uint page)
{
    return ivec2(int(page % uint(NPH_VSM_POOL_ROW)), int(page / uint(NPH_VSM_POOL_ROW))) *
           NPH_VSM_PAGE;
}

#ifdef NPH_VSM_SAMPLING
// The fraction of the sun's light reaching light-space point `ls`,
// `distance` from the camera, on a surface whose geometric normal in light
// space is `nl` (unit, toward the light: nl.z <= 0): the depth test against
// the resolution-matched level holding the point's page, with 2 x 2
// percentage-closer filtering (Reeves, Salesin & Cook, SIGGRAPH 1987), the
// receiver moved off its surface along the normal by a share of the
// level's texel (normal-offset shadows; Holbert, "Saying Goodbye to Shadow
// Acne", GDC 2011). The filter stays inside the page. A
// level without the page (the request took the next coarser one:
// VSM_LEVEL_SLACK, the margin at a window's edge) passes to the next;
// after three levels whose windows hold the point, or past the last, the
// point is lit.
float vsm_shadow_light_space(VsmViewRef r, vec3 ls, float distance, vec3 nl)
{
    // The offset in a level's frame is the same at every level: a texel is
    // a fixed share of a page, and the depth range a fixed number of pages.
    const float page = 1.0 / float(NPH_VSM_PAGE);
    const vec3 offset =
        nl * (r.v.texel_bias * vec3(page, page, page / float(NPH_VSM_WINDOW)));
    int tries = 0;
    for (int level = vsm_level_of(r, distance, VSM_LEVEL_SLACK);
         level < NPH_VSM_LEVELS && tries < 3; ++level) {
        const vec3 c = vsm_level_coords(r, level, ls) + offset;
        if (!vsm_inside(c.xy)) {
            continue;
        }
        ++tries;
        // The state alone: `vsm update` has dropped every entry whose page
        // the window left, so a valid one holds this frame's page.
        const ivec2 rel = ivec2(floor(c.xy));
        const uint state = VsmEntriesRef(r.v.entries).e[vsm_entry(level, rel + vsm_window(r, level))].state;
        if ((state & (NPH_VSM_ENTRY_VALID | NPH_VSM_ENTRY_DIRTY)) != NPH_VSM_ENTRY_VALID) {
            continue;
        }
        // The page's texels, the sample point kept half a texel inside so
        // the bilinear footprint does not reach the next page in the pool.
        const vec2 at = vec2(vsm_pool_base(state & NPH_VSM_ENTRY_PAGE)) +
                        clamp((c.xy - vec2(rel)) * float(NPH_VSM_PAGE), vec2(0.5),
                              vec2(float(NPH_VSM_PAGE) - 0.5));
        // The four depths around it in one gather, each compared and
        // weighted as bilinear filtering would: the comparison done here
        // rather than by a depth-comparison sampler, which needs a depth
        // image type the bindless array does not declare (SPIR-V's
        // OpSampledImage over a Depth 0 image; lavapipe compares nothing).
        // Gather order: (0, 1), (1, 1), (1, 0), (0, 0). The pool and its
        // sampler are the same for every invocation.
        const vec4 stored = textureGather(sampler2D(nph_textures_2d[r.v.pool_index],
                                                    nph_samplers[r.v.pool_sampler]),
                                          at * r.v.pool_scale.xy, 0);
        const vec2 f = fract(at - 0.5);
        const vec4 lit = vec4(lessThanEqual(vec4(c.z), stored));
        return dot(lit, vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y),
                             (1.0 - f.x) * (1.0 - f.y)));
    }
    return 1.0;
}

// The same for camera-relative point `p` on a surface whose geometric
// normal is `face_normal` (either way round) and whose shading normal is
// `n`. Facing away from the sun the point is unlit anyway: 1.
float vsm_shadow(VsmViewRef r, vec3 p, vec3 face_normal, vec3 n)
{
    const vec3 l = -r.v.axis_d.xyz;
    if (dot(n, l) <= 0.0) {
        return 1.0;
    }
    const vec3 nl = vsm_light_space(r, dot(face_normal, l) >= 0.0 ? face_normal : -face_normal);
    return vsm_shadow_light_space(r, vsm_light_space(r, p), length(p), nl);
}
#endif

#endif

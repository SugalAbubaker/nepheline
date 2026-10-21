// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page requests for 2 x 2 blocks of pixels: the
// depth pyramid's (vis_hzb.comp), which vsm_request_test.comp holds to
// every pixel's own (docs/render.md "Shadows"). The includer includes
// vsm_common.glsl with NPH_VSM_REQUESTS and enables subgroup basic, vote,
// ballot, arithmetic and shuffle.
#ifndef NPH_VSM_BLOCK_GLSL
#define NPH_VSM_BLOCK_GLSL

// The invocation whose bit is the `n`-th (from 0) set bit of a subgroup
// ballot (`n` below their count): the word that holds it, then halving
// within the word.
uint vsm_ballot_nth(uvec4 ballot, uint n)
{
    uint word = 0u;
    for (; word < 3u && n >= bitCount(ballot[word]); ++word) {
        n -= bitCount(ballot[word]);
    }
    uint w = ballot[word];
    uint at = 0u;
    for (uint span = 16u; span != 0u; span >>= 1u) {
        const uint below = bitCount(w & ((1u << span) - 1u));
        if (n >= below) {
            n -= below;
            w >>= span;
            at += span;
        }
    }
    return 32u * word + at;
}

// Gives each distinct value of the subgroup's `value`s (VSM_NONE, the
// largest, for none) an invocation of its own, the next after the last one
// given (`round`): round after round, the lowest value any invocation still
// has goes to invocation `round` (its `mine`). With every invocation
// holding one, they first set theirs (vsm_set_request) and start again.
// Every invocation of the subgroup calls it.
void vsm_gather_requests(VsmViewRef r, uint value, inout uint mine, inout uint round)
{
    for (;;) {
        const uint lowest = subgroupMin(value);
        if (lowest == VSM_NONE) {
            break;
        }
        if (round == gl_SubgroupSize) {
            if (mine != VSM_NONE) {
                vsm_set_request(r.v.requests, mine);
            }
            mine = VSM_NONE;
            round = 0u;
        }
        if (gl_SubgroupInvocationID == round) {
            mine = lowest;
        }
        ++round;
        if (value == lowest) {
            value = VSM_NONE;
        }
    }
}

// Requests the pages the shading of the 2 x 2 pixels at `lo` may read (the
// pixels clamped to `last`), each pixel's as vsm_request has it, in
// r.v.requests. `scale` is 2 / the target's extent (vsm_ndc), `depth` the
// four pixels' depths, row by row (0 for the background, or none). Every
// invocation of the subgroup calls it.
//
// Mostly the four lie on one surface at one level, a texel or two apart.
// Then one request holds their pages: the rectangle around their points
// widened by the margin, which takes the same level, window test and pages
// as each point's own (all three only grow with the point's coordinates
// and distance), and so exactly their pages where it is one page or two
// (the pixel nearest each end takes the page there). Near a page's corner,
// where it is four, a pixel need not take the one across the corner; there,
// where the levels differ, the background shows or the rectangle is larger
// (an edge between surfaces), each pixel requests its own. Those pixels,
// four from each invocation that has them, are gathered into consecutive
// invocations (as many at a time as the subgroup has), which compute them
// together: an invocation going round all four, with the subgroup waiting
// on it, would cost the subgroup far more. Shuffles gather them, not
// shared memory, which would need a barrier in control flow some
// subgroups of the workgroup skip.
//
// Each distinct request is then set once per subgroup: gathered to an
// invocation of its own (vsm_gather_requests), and set by every invocation
// at the end, their atomics together.
void vsm_request_block(VsmViewRef r, ivec2 lo, ivec2 last, vec2 scale, vec4 depth)
{
    const ivec2 hi = min(lo + 1, last);
    const vec2 n0 = vsm_ndc(lo, scale);
    const vec2 n1 = vsm_ndc(hi, scale);
    // The four points (the columns' terms shared), their extent, and the
    // extent of their squared distances.
    const vec3 p0 = vsm_ray_point(r, n0, depth.x);
    const vec3 p1 = vsm_ray_point(r, vec2(n1.x, n0.y), depth.y);
    const vec3 p2 = vsm_ray_point(r, vec2(n0.x, n1.y), depth.z);
    const vec3 p3 = vsm_ray_point(r, n1, depth.w);
    const vec2 pmin = min(min(p0.xy, p1.xy), min(p2.xy, p3.xy));
    const vec2 pmax = max(max(p0.xy, p1.xy), max(p2.xy, p3.xy));
    const vec4 q = vec4(vsm_q(r, p0), vsm_q(r, p1), vsm_q(r, p2), vsm_q(r, p3));
    const int level = vsm_level_of_q(max(max(q.x, q.y), max(q.z, q.w)));
    // The nearest at the farthest's level too: above the boundary below it.
    const bool one_level = level == 0 || min(min(q.x, q.y), min(q.z, q.w)) >
                                             uintBitsToFloat(uint(2 * level + 125) << 23u);
    const vec2 cmin = vsm_level_xy(r, level, pmin);
    const vec2 cmax = vsm_level_xy(r, level, pmax);
    const float m = r.v.margin;
    const ivec2 a = ivec2(floor(cmin - m));
    const ivec2 b = ivec2(floor(cmax + m));
    const bool block = all(greaterThan(depth, vec4(0.0))) && one_level &&
                       all(greaterThanEqual(cmin, vec2(m))) &&
                       all(lessThan(cmax, vec2(float(NPH_VSM_WINDOW) - m))) &&
                       (b.x - a.x) + (b.y - a.y) <= 1;
    uint mine = VSM_NONE;
    uint round = 0u;
    vsm_gather_requests(r, block ? vsm_pack_request(level, a, b) : VSM_NONE, mine, round);

    const bool own = !block && any(greaterThan(depth, vec4(0.0)));
    const uvec4 owners = subgroupBallot(own);
    const uint points = 4u * subgroupBallotBitCount(owners);
    for (uint base = 0u; base < points; base += gl_SubgroupSize) {
        // This invocation's point: pixel `k` of the owner (`from`) of rank
        // item / 4. Every invocation takes part in the shuffles.
        const uint item = base + gl_SubgroupInvocationID;
        const uint from = item < points ? vsm_ballot_nth(owners, item >> 2u) : 0u;
        const uint k = item & 3u;
        const vec4 d = vec4(subgroupShuffle(depth.x, from), subgroupShuffle(depth.y, from),
                            subgroupShuffle(depth.z, from), subgroupShuffle(depth.w, from));
        const ivec2 o = ivec2(subgroupShuffle(lo.x, from), subgroupShuffle(lo.y, from));
        const float z = k == 0u ? d.x : k == 1u ? d.y : k == 2u ? d.z : d.w;
        uint point = VSM_NONE;
        if (item < points && z > 0.0) {
            const ivec2 pixel = min(o + ivec2(int(k & 1u), int(k >> 1u)), last);
            point = vsm_request(r, vsm_ray_point(r, vsm_ndc(pixel, scale), z));
        }
        vsm_gather_requests(r, point, mine, round);
    }
    if (mine != VSM_NONE) {
        vsm_set_request(r.v.requests, mine);
    }
}

#endif

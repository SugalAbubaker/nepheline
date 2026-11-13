// SPDX-License-Identifier: Apache-2.0
// Ground-truth ambient occlusion (docs/render.md "Ambient occlusion";
// ADR-0038): Jimenez, Wu, Pesce & Jarabo, "Practical Realtime Strategies
// for Accurate Indirect Occlusion", ATVI-TR-16-01, 2016, at full resolution:
// one slice a pixel.
//
// A pixel's ambient occlusion is the cosine-weighted visibility of its
// hemisphere: A = (1/pi) * integral of V(w) <n, w>+ dw, over occluders
// within `radius` metres (Eq. 2). In a slice of the hemisphere through the
// view vector at screen angle phi, the visible arc lies between the two
// horizons th1 and th2 that the depth shows, measured from the view vector;
// its cosine-weighted measure has a closed form (Eq. 7):
//   a = 1/4 (-cos(2 th1 - g) + cos g + 2 th1 sin g)
//     + 1/4 (-cos(2 th2 - g) + cos g + 2 th2 sin g),
// with g the angle from the view vector of the normal projected into the
// slice, whose length scales the slice's share (Eq. 8). The horizons are
// the highest of the samples along the slice's screen direction on either
// side (Eq. 6), within the normal's hemisphere, an occluder fading out over
// the last tenth of the radius (§4.1). The samples step evenly out to the
// radius, the far ones from the depths' coarser mips (ao_depth.comp).
//
// Each pixel takes one slice, its direction one of 16 spread over the 4 x 4
// pixels around it and turned through 6 rotations from frame to frame
// (§4.1): the denoise (ao_denoise.comp) averages a window that holds every
// direction, and the temporal resolve the rotations, 96 directions in all.
// A slice's share of an open hemisphere, |np| (cos g + g sin g) (Eq. 7 with
// the horizons at the hemisphere's edges), is 1 only on average over the
// directions; each pixel gives it with the share it found, and the denoise
// divides the one's sum by the other's, so that an open surface comes out
// open even where a window lacks some directions (an edge).
//
// Where the frame has the sun's shadow map, the same pass marches each pixel a short way
// towards the sun through the depths (docs/render.md "Contact shadows"; ADR-0076): what the map's
// texels are too coarse for, or its bias lifts off the surface. It has the point, its normal and
// the depths in hand, and the answer goes in the sign of the pixel's distance, then of the
// occlusion the denoise writes, a bit that the resolve reads with it: no memory and no fetch.
//
// With the rays' scene in the frame (ao_rt.comp; RenderAo.frame_grazed) a pixel the sun faces
// traces the sun instead of marching, a ray to its centre, and says so in a bit beside the
// answer; the denoise gathers the rays' answers about a pixel, on its plane, into the sun's
// light there, by which the resolve leaves the map unread: the rays are the sun's light whole
// (ADR-0094). Every ray goes to the sun's centre: a shadow's edge is the 5 x 5 pixels' mean, two
// pixels soft and the same every frame, and a gap of light thinner than that a faint band, as
// the sun's own width makes it. (To points of the sun's disc, a pixel's by its place in a
// lattice, a thin shadow's edge showed the lattice, a wave that crawled with the camera; left
// to the pixel alone, a gap a pixel wide was a row of dashes.) The map's shadow of what stands
// close to a surface is the march's to find, a few pixels of it and jittered, its texels lie
// stretched across a surface the sun grazes, and each of its levels draws a thin caster's shadow
// otherwise: a vine's shadow on Bistro's wall was no shape of the vine's, and changed as the
// camera moved or turned. A ray's answer is the same from anywhere.
#ifndef NPH_AO_PASS_GLSL
#define NPH_AO_PASS_GLSL

#include "ao_common.glsl"
#ifdef AO_RAYS
// ao_rt.comp: the pass with the rays' scene, for the surfaces the sun grazes (below).
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"
#include "rt_common.glsl"
#include "rt_mask.glsl"
// The visibility buffer's bindless index, in the field the denoise names its output by
// (NphRenderAoPush.out_index).
#define AO_VISIBILITY pc.out_index
// What a ray to the sun starts off its surface by, as a share of the point's distance from the
// camera (main): 2^-14, a tenth of a pixel's width at 1440p.
#define AO_RAY_SLACK (1.0 / 16384.0)
// And at least this far, in metres: an eighth of a millimetre.
#define AO_RAY_SLACK_MIN (1.0 / 8192.0)
#endif

#define AO_GROUP 16
layout(local_size_x = AO_GROUP, local_size_y = AO_GROUP) in;

// Whether the frame has the sun's map, and so the march (ao.c): a frame without it pays nothing for
// the march's code (0.035 ms of the pass in the interior when it was one pipeline).
layout(constant_id = 0) const bool k_contact = false;

// Samples on either side of a slice.
#define AO_STEPS 4
// An occluder's weight falls from 1 to 0 over this last part of the radius.
#define AO_FALLOFF 0.1
// Steps 2^AO_MIP_SHIFT pixels out and further take mip 1 of the depths,
// twice as far mip 2, and so on: a far step's texel is an eighth of its
// length, as at half resolution, and the nearer steps read the frame's own.
#define AO_MIP_SHIFT 4
// Samples a pixel's march takes towards the sun, closer together near it.
#define AO_CONTACT_STEPS 4
// A surface counts as blocking the sun only within this share of the march's length in front of
// the marched point: one further off is something else in front of it, not its occluder.
#define AO_CONTACT_THICKNESS 0.6
// What a depth must exceed the marched point's by, as a share of it, past a float's rounding.
#define AO_CONTACT_SLACK 1.0001

// The 4 x 4 ordered dither (Bayer) at (x, y) mod 4, so that neighbours
// take far-apart directions:
//    0  8  2 10
//   12  4 14  6
//    3 11  1  9
//   15  7 13  5
// the bits of x ^ y and y interleaved, lowest first (from the matrix's
// recursive definition, Bayer, "An optimum method for two-level rendition
// of continuous-tone pictures", 1973).
uint bayer(ivec2 p)
{
    const uint x = uint(p.x ^ p.y);
    const uint y = uint(p.y);
    return ((x & 1u) << 3u) | ((y & 1u) << 2u) | (x & 2u) | ((y >> 1u) & 1u);
}

// The pixel of a 4 x 4 block whose Bayer value is `k` (bayer's inverse).
ivec2 bayer_inverse(uint k)
{
    const uint x = ((k >> 3u) & 1u) | (k & 2u);
    const uint y = ((k >> 2u) & 1u) | ((k & 1u) << 1u);
    return ivec2(int(x ^ y), int(y));
}

// The pixel of the group's AO_GROUP x AO_GROUP that invocation `index` takes:
// the pixels sorted by slice direction, 16 of each, so that each half of a
// subgroup takes one direction's pixels, 4 apart (deinterleaved, as Bavoil,
// "Deinterleaved Texturing for Cache-Efficient Interleaved Sampling", NVIDIA
// 2014, but over the frame's own depths). Their steps share the direction
// and the jitter, so a step's samples lie on one lattice, a few cache lines,
// where 16 directions in a subgroup would scatter them over the radius.
ivec2 group_pixel(uint index)
{
    const uint m = index & 15u;
    return 4 * ivec2(int(m & 3u), int(m >> 2u)) + bayer_inverse(index >> 4u);
}

// The group's depths and a texel's border around them, for the normals
// (an array of a struct of its own: docs/build.md "Shaders").
#define AO_SPAN (AO_GROUP + 2)
#if (AO_SPAN % 2) != 0
#error "the span's gathers take 2 x 2 texels"
#endif
struct AoDepth {
    float d;
};
shared AoDepth s_depths[AO_SPAN * AO_SPAN];

float depth_at(ivec2 l)
{
    return s_depths[(l.y + 1) * AO_SPAN + l.x + 1].d;
}

// acos to within 6.8e-5: a cubic in |x| times sqrt(1 - |x|), reflected for
// negative x (Abramowitz & Stegun, Handbook of Mathematical Functions,
// 4.4.45).
float fast_acos(float x)
{
    const float a = abs(x);
    const float r =
        sqrt(1.0 - a) * (1.5707288 + a * (-0.2121144 + a * (0.0742610 - 0.0187293 * a)));
    return x >= 0.0 ? r : NPH_PI - r;
}

// For the denoise (ao_denoise.comp): the slice's share of the hemisphere
// left visible and its share when open, each in [0, 2) as 8 bits, and the
// distance as a half float, negative where the march or the ray found the sun blocked, its
// last bit set where it was a ray (`traced`: NPH_AO_TRACED).
void store(ivec2 t, float visible, float open, float dist, bool blocked, bool traced)
{
    const uint shares = packUnorm4x8(vec4(0.5 * visible, 0.5 * open, 0.0, 0.0)) & 0xFFFFu;
    const uint distance =
        packHalf2x16(vec2(0.0, min(dist, NPH_AO_HALF_MAX))) & (0xFFFF0000u & ~NPH_AO_TRACED);
    const uint sign_bit = blocked ? 0x80000000u : 0u;
    imageStore(nph_uimages_2d[pc.raw_index], t,
               uvec4(shares | distance | sign_bit | (traced ? NPH_AO_TRACED : 0u), 0u, 0u, 0u));
}

// Whether a surface the depths show stands between the point under screen position `c`, of
// reverse-Z depth `depth`, and the sun, within the march's length. `gradient` is the point's
// plane's depth a pixel along each axis (ao_gradient), `jitter` in [0, 1).
//
// The march's length is w times the point's distance D (NphRenderAoPush.sun), so a step of u times
// D towards the sun L takes the point from D (r, -1) (ao_position) to D (r + u L.xy, -1 + u L.z):
// a = 1 - u L.z times as far along the view (at least a half: render_ao_frame_begin bounds it), at
// screen position (c + u sun_screen) / a, with sun_screen = (L.xy + unproject.zw L.z) project, and
// depth / a, which past the near plane no depth exceeds. None of it needs D.
//
// A point marched off a plane towards the sun lies on the camera's side of it, so the plane's own
// depth under it is behind it, exactly; a sample reads the plane at a pixel's centre instead, up
// to half a pixel off on each axis, which at a grazing angle is a long way in metres but in depth,
// which is affine across a plane, is exactly `rounding`.
bool sun_blocked(vec2 c, float depth, vec2 gradient, float jitter)
{
    const uvec2 size = uvec2(pc.width, pc.height);
    const float rounding = 0.5 * (abs(gradient.x) + abs(gradient.y));
    const float marched = depth * AO_CONTACT_SLACK;
    const float thickness = AO_CONTACT_THICKNESS * pc.sun.w;
    // What a step towards the sun does on screen (above).
    const vec2 sun_screen = (pc.sun.xy + pc.unproject.zw * pc.sun.z) * pc.project;
    for (int i = 1; i <= AO_CONTACT_STEPS; ++i) {
        // (A first step shorter than a pixel would read the point's own texel: it starts further.)
        const float f = (float(i) + jitter) / float(AO_CONTACT_STEPS + 1);
        const float u = pc.sun.w * f * f;
        const float a = 1.0 - u * pc.sun.z;
        const float inv = 1.0 / a;
        // (Truncating only moves what lies under a pixel off the image onto its edge.)
        const uvec2 at = uvec2(ivec2((c + u * sun_screen) * inv));
        // Off the image nothing is known.
        if (any(greaterThanEqual(at, size))) {
            return false;
        }
        const float d = texelFetch(nph_textures_2d[pc.depths_index], ivec2(at), 0).r;
        // In front of the marched point, by more than the rounding and by no more than the
        // thickness: a D - thickness D < znear / d, and znear / D is the point's depth.
        if (d > marched * inv + rounding && (a - thickness) * d < depth) {
            return true;
        }
    }
    return false;
}

void main()
{
    const ivec2 origin = ivec2(gl_WorkGroupID.xy) * AO_GROUP - 1;
    // The span as 9 x 9 gathers of 2 x 2 texels, each at the corner its four
    // share, in the gather's order (x0, y1), (x1, y1), (x1, y0), (x0, y0): a
    // quarter of the fetches that one at a time took. The sampler clamps to
    // the depths' edge, which lies past the frame's where the frame is no
    // multiple of 32 pixels (ao_depth clamps to the frame's); only texels
    // past the frame differ, and no pixel in it reads them (ao_gradient
    // takes the side within the image).
    if (gl_LocalInvocationIndex < uint((AO_SPAN / 2) * (AO_SPAN / 2))) {
        const uint i = gl_LocalInvocationIndex;
        const ivec2 b = 2 * ivec2(int(i % uint(AO_SPAN / 2)), int(i / uint(AO_SPAN / 2)));
        const vec4 g = textureGather(
            sampler2D(nph_textures_2d[pc.depths_index], nph_samplers[pc.point_sampler]),
            vec2(origin + b + 1) * pc.depth_texel, 0);
        s_depths[(b.y + 1) * AO_SPAN + b.x].d = g.x;
        s_depths[(b.y + 1) * AO_SPAN + b.x + 1].d = g.y;
        s_depths[b.y * AO_SPAN + b.x + 1].d = g.z;
        s_depths[b.y * AO_SPAN + b.x].d = g.w;
    }
    barrier();
    const ivec2 l = group_pixel(gl_LocalInvocationIndex);
    const ivec2 t = ivec2(gl_WorkGroupID.xy) * AO_GROUP + l;
    if (any(greaterThanEqual(t, ivec2(pc.width, pc.height)))) {
        return;
    }
    const float depth = depth_at(l);
    const float dist = ao_distance(depth);
    // A point whose radius covers under a pixel sees nothing occlude it.
    const float radius_px = min(pc.radius_pixels / dist, pc.max_radius);
    if (dist >= NPH_RENDER_AO_SKY || radius_px < 1.0) {
        store(t, 1.0, 1.0, dist, false, false);
        return;
    }
    // The point is dist times its ray rc (ao_position); v, the view vector.
    const vec2 c = ao_centre(t);
    const vec3 rc = vec3(pc.unproject.xy * c + pc.unproject.zw, -1.0);
    const float inv_rc_length = inversesqrt(dot(rc, rc));
    const vec3 v = -rc * inv_rc_length;
    const vec2 gradient = ao_gradient(t, depth, depth_at(l - ivec2(1, 0)), depth_at(l + ivec2(1, 0)),
                                      depth_at(l - ivec2(0, 1)), depth_at(l + ivec2(0, 1)));
    const vec3 n = -normalize(ao_plane(t, depth, gradient));

    // The slice: a direction at angle phi around the view vector, the angles
    // spread evenly as Eq. 8 integrates over them (even screen angles would
    // crowd the slices of a point off the view's axis), and the screen
    // direction a step along it takes from the point (the derivative of
    // ao_position's inverse): the camera's rays through that screen line
    // span the slice's plane, so the samples along it lie in the slice.
    const float phi = (float(bayer(t)) + float(pc.noise) / 6.0) * (NPH_PI / 16.0);
    const vec3 b1 = normalize(cross(vec3(0.0, 1.0, 0.0), v));
    const vec3 b2 = cross(v, b1);
    const vec3 ortho = cos(phi) * b1 + sin(phi) * b2;
    const vec2 omega = normalize((ortho.xy + rc.xy * ortho.z) * pc.project);
    const vec3 axis = cross(ortho, v);
    const vec3 np = n - axis * dot(n, axis);
    const float np_length2 = dot(np, np);
    const float inv_np_length = inversesqrt(max(np_length2, 1e-12));
    const float np_length = np_length2 * inv_np_length;
    const float cos_g = clamp(dot(np, v) * inv_np_length, -1.0, 1.0);
    const float g = (dot(np, ortho) < 0.0 ? -1.0 : 1.0) * fast_acos(cos_g);

    // The horizons' cosines from the view vector, starting at the
    // hemisphere's edges (g -+ pi/2, cosines sin g and -sin g): side 0
    // against the screen direction, side 1 along it.
    const float sin_g = sin(g);
    const float low0 = sin_g;
    const float low1 = -sin_g;
    float h0 = low0;
    float h1 = low1;
    // The steps' offsets, jittered as the directions are (the transposed
    // dither), at least a pixel out.
    const float jitter =
        fract((float(bayer(t.yx)) + 0.5) / 16.0 +
              float(pc.noise) * 0.618034);
    const float inv_radius = 1.0 / pc.radius;
    // How far each side's samples reach: between the outermost texels'
    // samples, where the depths interpolate (off the image nothing is
    // known), a thousandth of a pixel's slack keeping an axis the slice
    // runs along from cutting it off.
    const vec2 lo = vec2(0.5);
    const vec2 hi = vec2(ivec2(pc.width, pc.height) - 1) + 0.5;
    const bvec2 rising = greaterThanEqual(omega, vec2(0.0));
    const vec2 per_pixel = 1.0 / max(abs(omega), vec2(1e-7));
    const vec2 room0 = (mix(hi - c, c - lo, rising) + 1e-3) * per_pixel;
    const vec2 room1 = (mix(c - lo, hi - c, rising) + 1e-3) * per_pixel;
    const float reach0 = min(room0.x, room0.y);
    const float reach1 = min(room1.x, room1.y);
    // A step of a pixel along omega turns the ray by w, within the slice's
    // plane: a sample s pixels along it at distance x lies at x (rc + s w),
    // (x - dist) rc + x s w from the point, of which (rc being -|rc| v)
    // -(x - dist) |rc| + x s (w . v) along v and x s (w . ortho) along
    // ortho, the slice's other axis.
    const vec2 w = pc.unproject.xy * omega;
    const float w_along = dot(w, v.xy);
    const float w_across = dot(w, ortho.xy);
    const float rc_length = 1.0 / inv_rc_length;
    for (int i = 0; i < AO_STEPS; ++i) {
        const float length_px = max((float(i) + jitter) / float(AO_STEPS) * radius_px, 1.0);
        // Mip floor(log2(length_px)) - AO_MIP_SHIFT + 1: the logarithm is the
        // float's exponent (length_px is at least 1).
        const int mip = clamp((floatBitsToInt(length_px) >> 23) - 127 - AO_MIP_SHIFT + 1, 0,
                              NPH_RENDER_AO_MIPS - 1);
        for (int side = 0; side < 2; ++side) {
            // Every sample is fetched, within the reach (so its texels lie in
            // the image), and one beyond it counts nothing: the fetches do not
            // wait on a branch, and the horizon is the same.
            const float reach = side == 0 ? reach0 : reach1;
            const float s = side == 0 ? -min(length_px, reach) : min(length_px, reach);
            const float x = ao_distance(ao_depth_at(c + s * omega, mip));
            const float along = x * s * w_along - (x - dist) * rc_length;
            const float across = x * s * w_across;
            const float len2 = along * along + across * across;
            const float inv_len = inversesqrt(max(len2, 1e-12));
            const float weight =
                length_px > reach
                    ? 0.0
                    : clamp((1.0 - len2 * inv_len * inv_radius) / AO_FALLOFF, 0.0, 1.0);
            const float h = along * inv_len;
            if (side == 0) {
                h0 = max(h0, mix(low0, h, weight));
            } else {
                h1 = max(h1, mix(low1, h, weight));
            }
        }
    }

    // The horizon angles (side 0 negative), within the hemisphere around
    // the projected normal, and the slice's visible arc (Eq. 7).
    const float th0 = g + max(-fast_acos(clamp(h0, -1.0, 1.0)) - g, -0.5 * NPH_PI);
    const float th1 = g + min(fast_acos(clamp(h1, -1.0, 1.0)) - g, 0.5 * NPH_PI);
    const float arc = 0.25 * (-cos(2.0 * th0 - g) + cos_g + 2.0 * th0 * sin_g) +
                      0.25 * (-cos(2.0 * th1 - g) + cos_g + 2.0 * th1 * sin_g);
    // Facing away the sun does not reach it anyway; with no sun, this is 0.
    const float nol = dot(n, pc.sun.xyz);
    bool blocked = false;
    bool traced = false;
#ifdef AO_RAYS
    if (k_contact && nol > 0.0) {
        // The point, its normal and the sun in the camera-relative world, where the rays' scene
        // is (its origin tlas_offset from the camera).
        VisViewRef view = VisViewRef(pc.view);
        ViewRef rview = ViewRef(view.v.render_view);
        // (The view matrix is a rotation: its transpose takes view space back.)
        const mat3 to_world = transpose(mat3(rview.v.view));
        const vec3 shift = rview.v.tlas_offset.xyz;
        const uint64_t tlas = rview.v.tlas;
        // Where the pixel shows what stands in for finer geometry (NPH_VIS_CLUSTER_COARSE: a
        // coarser level of its mesh, voxels, a cell's proxy), the rays find the mesh itself, which
        // lies within the view's error of what is shown (a world unit spans view.v.lod.x pixels'
        // errors at a distance of one: vis_lod_group_too_coarse). From the level's surface, a
        // ray under a fold of the mesh's cloth that its level had smoothed away met the fold:
        // patches of shadow on an awning that came and went as its level changed with the
        // camera's distance. Lifted off the surface by the error, it cleared the fold and moved
        // every shadow's edge by the lift over N.L, a ledge's on a grazed wall by tens of
        // pixels. So such a pixel's own ray finds the mesh first, within the error either side
        // of the level's surface (further along it where it is seen aslant), and the sun's ray
        // starts where it does; from the level's own point, lifted, where it finds nothing. A
        // pixel that shows the mesh itself needs none of it (most of a view near the camera, and
        // a second ray's time saved there).
        const vec3 at = dist * rc;
        const uint64_t shown = imageLoad(nph_u64images_2d[nonuniformEXT(AO_VISIBILITY)], t).x;
        const bool coarse =
            shown != 0ul && (VisClustersRef(view.v.clusters).c[vis_cluster(shown)].flags &
                             NPH_VIS_CLUSTER_COARSE) != 0u;
        const float error = 1.25 * length(at) / view.v.lod.x;
        vec3 from = at;
        if (coarse && error > 0.0) {
            const vec3 toward = -v; // the unit view ray to the point
            const float reach = error / max(dot(n, v), 0.25);
            from = at + error * n;
            rayQueryEXT probe;
            rayQueryInitializeEXT(probe, accelerationStructureEXT(tlas), gl_RayFlagsNoneEXT, 0xFFu,
                                  to_world * (at - reach * toward) + shift, 0.0, to_world * toward,
                                  2.0 * reach);
            NPH_RAY_TRAVERSE(view, probe)
            if (rayQueryGetIntersectionTypeEXT(probe, true) !=
                gl_RayQueryCommittedIntersectionNoneEXT) {
                from = at + (rayQueryGetIntersectionTEXT(probe, true) - reach) * toward;
            }
        }
        traced = true;
        // The ray leaves from a little off the surface, by a share of the point's distance from
        // the camera (AO_RAY_SLACK): the point comes from a depth, as exact as a float at that
        // distance is, where the rays' own offset (nph_offset_ray) counts units in the last place
        // of its coordinates in their scene's space, which near that space's anchor are far
        // finer. Without it a road 50 m off, level with the anchor, shadowed itself a triangle
        // here and a triangle there, others every frame, once the camera had left the anchor.
        // (At least AO_RAY_SLACK_MIN: a mesh's own space rounds a point as coarsely near the eye.)
        from += max(AO_RAY_SLACK * length(at), AO_RAY_SLACK_MIN) * n;
        blocked = nph_shadow_ray_masked(view, tlas, to_world * from + shift, to_world * n,
                                        to_world * pc.sun.xyz, 1e30) == 0.0;
    } else
#endif
    {
        blocked = k_contact && nol > 0.0 && sun_blocked(c, depth, gradient, jitter);
    }
    store(t, max(np_length * arc, 0.0), np_length * (cos_g + g * sin_g), dist, blocked, traced);
}

#endif

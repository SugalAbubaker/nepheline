// SPDX-License-Identifier: Apache-2.0
// Voxels in the production path (render/voxel.h; docs/render.md "Voxels";
// ADR-0061): the bricks and voxels of a mesh's voxel clusters, the pixel's
// ray in a mesh's space, and the traversal of a brick, the twin of
// nph_render_voxel_trace (render/src/voxel.c), with the footprints' tests.
//
// The raster (vis_voxel_raster.comp), the voxel resolve
// (vis_voxel_resolve.glsl), and the radiance cache's update (gi_update.comp)
// each trace a pixel's ray through the brick the visibility buffer names, and
// must meet the same voxel at the same depth: every value here is `precise`,
// so that no shader fuses or reorders its arithmetic differently from
// another's (the C twin rounds as C does, and agrees to within the tests'
// tolerance).
#ifndef NPH_VIS_VOXEL_GLSL
#define NPH_VIS_VOXEL_GLSL

#include "vis_common.glsl"

// A brick: two uvec4, (origin.xyz, cell) as float bits, then (mask low, mask
// high, first voxel, 0). A voxel: (colour, normal, material, box).
NPH_BUFFER_REF(VisBricksRef) { uvec4 b[]; };
NPH_BUFFER_REF(VisVoxelsRef) { uvec4 v[]; };

// render/voxel.h's constants.
#define VIS_VOXEL_BOX_STEPS       32.0
#define VIS_VOXEL_SHAPE_BOX       0u
#define VIS_VOXEL_SHAPE_ELLIPSOID 1u
#define VIS_VOXEL_SHAPE_CAPSULE   2u
#define VIS_VOXEL_RADIUS_CODES    57u
#define VIS_VOXEL_KAPPA_MAX       62756.0
// A ray crosses at most 4 + 3 + 3 cells of a brick.
#define VIS_VOXEL_MAX_STEPS 10u

struct VisVoxelHit {
    float t;    // along the ray
    uint voxel; // index into the mesh's voxels
    uint cell;  // its cell's bit in the brick, x + 4y + 16z
    // The footprint's face it entered: axis (0, 1, 2) | 4 on the box's high
    // side; for a round shape the axis of the normal's largest component
    // there, on the side the ray came from.
    uint face;
};

// A capsule's radius, as a fraction of the cell, from its code.
float vis_voxel_radius(uint code)
{
    return exp2(float(min(code, VIS_VOXEL_RADIUS_CODES - 1u)) / 8.0 - 8.0);
}

// The concentration of a voxel's lobe from its byte.
float vis_voxel_kappa(uint b)
{
    return exp2(float(min(b, 255u)) / 16.0) - 1.0;
}

// The inverse of the linear part (the normal matrix transposed), in
// precise arithmetic: the voxels' rays must be rebuilt bit for bit.
mat3 vis_voxel_inverse_linear(NphVisTransform tr)
{
    precise vec3 r0 = tr.row0.xyz;
    precise vec3 r1 = tr.row1.xyz;
    precise vec3 r2 = tr.row2.xyz;
    precise vec3 c0 = cross(r1, r2);
    precise vec3 c1 = cross(r2, r0);
    precise vec3 c2 = cross(r0, r1);
    precise float inv_det = 1.0 / dot(r0, c0);
    // The inverse's columns are the cross products of pairs of rows over the
    // determinant (r_i . c_j is the determinant where i = j, else 0).
    precise mat3 inv = mat3(c0 * inv_det, c1 * inv_det, c2 * inv_det);
    return inv;
}

// The NDC of pixel `pixel`'s centre, from the target's reciprocal size.
vec2 vis_voxel_ndc(ivec2 pixel, vec2 inv_size)
{
    precise vec2 q = (vec2(pixel) + 0.5) * inv_size * 2.0 - 1.0;
    return q;
}

// The rays of an instance's pixels, from the camera through each pixel's
// point on the near plane (depth 1 under reverse-Z with an infinite far
// plane, so a hit at parameter t lies at depth 1 / t), in the space of the
// instance whose linear part's inverse is `inv` (vis_voxel_inverse_linear)
// and whose translation from the camera is `t`: their origin, the camera's
// place there (the same for every pixel; the raster takes it once).
vec3 vis_voxel_ray_origin(mat3 inv, vec3 t)
{
    precise vec3 o = inv * (-t);
    return o;
}

// And the ray's direction there for the pixel at NDC `q`, `world_dir` the
// same in camera-relative world space (the camera at the origin).
vec3 vis_voxel_ray_dir(mat4 inv_view_proj, mat3 inv, vec2 q, out vec3 world_dir)
{
    precise vec4 p = inv_view_proj * vec4(q, 1.0, 1.0);
    precise vec3 w = p.xyz / p.w;
    precise vec3 d = inv * w;
    world_dir = w;
    return d;
}

// The capsule from `a` to `b` of radius `r` against the ray p + s d for s in
// [s_lo, s_hi] (voxel.c capsule_hit): the first point of its body and its
// ends' spheres, or s_lo where the ray is already inside; `n` the outward
// normal there, unnormalised.
bool vis_voxel_capsule(vec3 p, vec3 d, vec3 a, vec3 b, float r, float s_lo, float s_hi,
                       out float out_s, out vec3 n)
{
    precise vec3 ba = b - a;
    precise vec3 oa = p - a;
    precise float baba = dot(ba, ba);
    precise float r2 = r * r;
    precise vec3 q = oa + s_lo * d;
    precise float y = baba > 0.0 ? clamp(dot(q, ba) / baba, 0.0, 1.0) : 0.0;
    precise vec3 qn = q - y * ba;
    precise float best = 3.402823466e38;
    if (dot(qn, qn) <= r2) {
        best = s_lo;
    } else {
        precise float dd = dot(d, d);
        precise float bard = dot(ba, d);
        precise float baoa = dot(ba, oa);
        precise float doa = dot(d, oa);
        precise float oaoa = dot(oa, oa);
        precise float qa = baba * dd - bard * bard;
        if (qa > 1e-12 * baba * dd) {
            precise float qb = baba * doa - baoa * bard;
            precise float qc = baba * oaoa - baoa * baoa - r2 * baba;
            precise float h = qb * qb - qa * qc;
            if (h >= 0.0) {
                precise float sb = (-qb - sqrt(h)) / qa;
                precise float yb = baoa + sb * bard;
                if (yb >= 0.0 && yb <= baba) {
                    best = sb;
                }
            }
        }
        for (uint e = 0u; e < 2u; ++e) {
            precise vec3 oc = e == 0u ? oa : oa - ba;
            precise float hb = dot(d, oc);
            precise float hc = dot(oc, oc) - r2;
            precise float h = hb * hb - dd * hc;
            if (h >= 0.0) {
                precise float se = (-hb - sqrt(h)) / dd;
                best = min(best, se);
            }
        }
        if (!(best >= s_lo && best <= s_hi)) {
            out_s = 0.0;
            n = vec3(0.0);
            return false;
        }
    }
    precise vec3 qh = oa + best * d;
    precise float yh = baba > 0.0 ? clamp(dot(qh, ba) / baba, 0.0, 1.0) : 0.0;
    precise vec3 nh = qh - yh * ba;
    n = nh;
    out_s = best;
    return true;
}

// The footprint of a voxel (its box and material words) in the cell of
// lowest corner `base`, against the ray o + t d (brick space; `inv` the
// reciprocal of d) for t in [t0, t1]; the ray crosses the cell over [t_cell,
// t_exit] (voxel.c footprint). On a hit, t0 takes its t and `face` the face
// it entered.
bool vis_voxel_footprint(uint box, uint material, vec3 base, float step, vec3 o, vec3 d, vec3 inv,
                         float t_cell, float t_exit, inout float t0, float t1, out uint face)
{
    face = 0u;
    precise vec3 lo;
    precise vec3 hi;
    for (uint a = 0u; a < 3u; ++a) {
        lo[a] = base[a] + float((box >> (5u * a)) & 31u) * step;
        hi[a] = base[a] + float(((box >> (15u + 5u * a)) & 31u) + 1u) * step;
    }
    const uint shape = box >> 30u;
    // The ray from where it enters the cell, its t counted from there.
    precise vec3 p = o + t_cell * d;
    precise vec3 n = vec3(0.0);
    precise float s = 0.0;
    uint f = 0u;
    if (shape == VIS_VOXEL_SHAPE_CAPSULE) {
        // From the box's corner (x, y, low z) its bits name to the opposite
        // one, clipped to the cell.
        const uint diagonal = (material >> 24u) & 3u;
        precise float r = vis_voxel_radius((material >> 26u) & 63u) * (step * VIS_VOXEL_BOX_STEPS);
        precise vec3 ca =
            vec3((diagonal & 1u) != 0u ? hi.x : lo.x, (diagonal & 2u) != 0u ? hi.y : lo.y, lo.z);
        precise vec3 cb =
            vec3((diagonal & 1u) != 0u ? lo.x : hi.x, (diagonal & 2u) != 0u ? lo.y : hi.y, hi.z);
        precise float s_lo = max(t0, t_cell) - t_cell;
        precise float s_hi = min(t1, t_exit) - t_cell;
        if (!(s_lo <= s_hi)) {
            return false;
        }
        precise float cs;
        precise vec3 cn;
        if (!vis_voxel_capsule(p, d, ca, cb, r, s_lo, s_hi, cs, cn)) {
            return false;
        }
        s = cs;
        n = cn;
    } else {
        precise float bt0 = t0;
        precise float bt1 = t1;
        precise float enter = -3.402823466e38;
        for (uint a = 0u; a < 3u; ++a) {
            precise float ta = (lo[a] - o[a]) * inv[a];
            precise float tb = (hi[a] - o[a]) * inv[a];
            precise float tn = min(ta, tb);
            if (tn > enter) {
                enter = tn;
                f = a | (d[a] < 0.0 ? 4u : 0u);
            }
            bt0 = max(bt0, tn);
            bt1 = min(bt1, max(ta, tb));
        }
        if (!(bt0 <= bt1)) {
            return false;
        }
        if (shape == VIS_VOXEL_SHAPE_BOX) {
            t0 = bt0;
            face = f;
            return true;
        }
        // The ellipsoid inscribed in the box, in the unit sphere's space.
        precise vec3 half_ = 0.5 * (hi - lo);
        precise vec3 q = (p - 0.5 * (lo + hi)) / half_;
        precise vec3 k = d / half_;
        precise float qq = dot(q, q);
        precise float qk = dot(q, k);
        precise float kk = dot(k, k);
        precise float disc = qk * qk - kk * (qq - 1.0);
        if (disc < 0.0) {
            return false;
        }
        precise float root = sqrt(disc);
        precise float se = (-qk - root) / kk;
        precise float sx = (-qk + root) / kk;
        if (sx < bt0 - t_cell || se > bt1 - t_cell) {
            return false;
        }
        s = max(se, bt0 - t_cell);
        n = (q + s * k) / half_;
    }
    // The face the normal points out of most, facing back along the ray.
    float best = -1.0;
    for (uint a = 0u; a < 3u; ++a) {
        if (abs(n[a]) > best) {
            best = abs(n[a]);
            f = a | (n[a] > 0.0 ? 4u : 0u);
        }
    }
    precise float th = t_cell + s;
    t0 = th;
    face = f;
    return true;
}

// The first voxel of the brick (b0, b1) that the ray origin + t dir (mesh
// space) meets for t in [t_min, t_max], the footprints tested in the order
// the ray crosses their cells (Amanatides & Woo, "A Fast Voxel Traversal
// Algorithm for Ray Tracing", Eurographics 1987); false when it meets none.
bool vis_voxel_trace(uvec4 b0, uvec4 b1, VisVoxelsRef voxels, vec3 origin, vec3 dir, float t_min,
                     float t_max, out VisVoxelHit hit)
{
    hit.t = 0.0;
    hit.voxel = 0u;
    hit.cell = 0u;
    hit.face = 0u;
    precise float h = uintBitsToFloat(b0.w);
    precise vec3 o = origin - uintBitsToFloat(b0.xyz);
    precise vec3 d = dir;
    precise vec3 inv;
    for (uint a = 0u; a < 3u; ++a) {
        if (abs(d[a]) < 1e-20) {
            d[a] = d[a] < 0.0 ? -1e-20 : 1e-20;
        }
        inv[a] = 1.0 / d[a];
    }
    // The brick's box, [0, 4h] on each axis.
    precise float side = 4.0 * h;
    precise float t0 = t_min;
    precise float t1 = t_max;
    for (uint a = 0u; a < 3u; ++a) {
        precise float ta = (0.0 - o[a]) * inv[a];
        precise float tb = (side - o[a]) * inv[a];
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }
    if (!(t0 <= t1)) {
        return false;
    }
    // The cell the ray enters by, the t it enters there, and the steps.
    precise float t_cell = t0;
    ivec3 c;
    ivec3 stp;
    precise vec3 next;
    precise vec3 delta;
    for (uint a = 0u; a < 3u; ++a) {
        precise float pa = o[a] + t0 * d[a];
        precise float cf = floor(pa / h);
        c[a] = cf < 0.0 ? 0 : (cf > 3.0 ? 3 : int(cf));
        stp[a] = d[a] > 0.0 ? 1 : -1;
        next[a] = (float(c[a] + (stp[a] > 0 ? 1 : 0)) * h - o[a]) * inv[a];
        delta[a] = h * abs(inv[a]);
    }
    precise float step_h = h / VIS_VOXEL_BOX_STEPS;
    for (uint iter = 0u; iter < VIS_VOXEL_MAX_STEPS; ++iter) {
        const uint bit = uint(c.x) + 4u * uint(c.y) + 16u * uint(c.z);
        const uint word = bit < 32u ? b1.x : b1.y;
        if (((word >> (bit & 31u)) & 1u) != 0u) {
            const uint rank = bit < 32u ? bitCount(b1.x & ((1u << bit) - 1u))
                                        : bitCount(b1.x) + bitCount(b1.y & ((1u << (bit - 32u)) - 1u));
            const uint index = b1.z + rank;
            const uvec4 voxel = voxels.v[index];
            precise vec3 base = vec3(c) * h;
            precise float t = t0;
            uint face = 0u;
            precise float t_exit = min(min(next.x, next.y), next.z);
            if (vis_voxel_footprint(voxel.w, voxel.z, base, step_h, o, d, inv, t_cell, t_exit, t, t1,
                                    face)) {
                hit.t = t;
                hit.voxel = index;
                hit.cell = bit;
                hit.face = face;
                return true;
            }
        }
        // The nearest boundary next, the lowest axis on a tie.
        uint a = 0u;
        if (next.y < next[a]) {
            a = 1u;
        }
        if (next.z < next[a]) {
            a = 2u;
        }
        if (next[a] > t1) {
            break;
        }
        c[a] += stp[a];
        if (c[a] < 0 || c[a] > 3) {
            break;
        }
        t_cell = next[a];
        next[a] += delta[a];
    }
    return false;
}

// What the shading and the radiance cache take of a voxel pixel: the
// visibility buffer's texel `v` (a voxel cluster's) at pixel `pixel`.
struct VisVoxelSurface {
    vec3 position;    // camera relative: the hit
    vec3 face_normal; // the footprint's face entered, world space, unit, facing the camera
    float offset;     // a cell's diagonal in world units: rays and lookups start this far off
    uvec4 voxel;      // the voxel's record
    NphVisTransform transform;
};

// The voxel the visibility buffer's texel `v` shows at `pixel`, retraced
// through the brick it names exactly as the raster traced it; false where
// the ray meets none (which the precise arithmetic rules out).
bool vis_voxel_surface(VisViewRef view, mat4 inv_view_proj, uint64_t v, ivec2 pixel,
                       out VisVoxelSurface out_s)
{
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[vis_cluster(v)];
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[cluster.instance];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec4 m = VisMeshletsRef(mesh.meshlets).m[cluster.meshlet];
    const uint brick = m.x + vis_triangle(v);
    VisBricksRef bricks = VisBricksRef(mesh.voxel_bricks);
    const uvec4 b0 = bricks.b[brick * 2u];
    const uvec4 b1 = bricks.b[brick * 2u + 1u];
    const vec3 t =
        vis_relative_translation(inst.transform, view.v.camera_high.xyz, view.v.camera_low.xyz);
    const mat3 inv = vis_voxel_inverse_linear(inst.transform);
    const vec3 o = vis_voxel_ray_origin(inv, t);
    vec3 world_dir;
    const vec3 d =
        vis_voxel_ray_dir(inv_view_proj, inv, vis_voxel_ndc(pixel, view.v.viewport.zw), world_dir);
    VisVoxelHit hit;
    const bool found =
        vis_voxel_trace(b0, b1, VisVoxelsRef(mesh.voxels), o, d, 1.0, 3.402823466e38, hit);
    out_s.transform = inst.transform;
    // Where the trace finds nothing, the depth's point (and the voxel the
    // record names first) keep the shading defined.
    const float th = found ? hit.t : 1.0 / max(vis_depth(v), 1e-30);
    out_s.position = world_dir * th;
    out_s.voxel = VisVoxelsRef(mesh.voxels).v[found ? hit.voxel : b1.z];
    const uint axis = hit.face & 3u;
    vec3 n = vec3(0.0);
    n[axis] = (hit.face & 4u) != 0u ? 1.0 : -1.0;
    vec3 wn = normalize(vis_normal_matrix(inst.transform) * n);
    out_s.face_normal = dot(wn, out_s.position) > 0.0 ? -wn : wn;
    out_s.offset = uintBitsToFloat(b0.w) * vis_max_scale(inst.transform) * 1.7320508;
    return found;
}

#endif

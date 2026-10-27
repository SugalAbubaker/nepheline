// SPDX-License-Identifier: Apache-2.0
// Voxels in the production path (render/voxel.h; docs/render.md "Voxels";
// ADR-0061): the bricks and voxels of a mesh's voxel clusters, the pixel's
// ray in a mesh's space, and a voxel's test against it (vis_voxel_hit, the
// twin of render/src/voxel.c's), with the footprints' tests.
//
// The raster (vis_voxel_raster.comp) tests a pixel's ray against each voxel
// whose footprint may cover the pixel; the voxel resolve
// (vis_voxel_resolve.glsl) and the radiance cache's update (gi_update.comp)
// test it again against the cells next to the depth's point in the brick the
// visibility buffer names (vis_voxel_find), and must meet the same voxel at
// the same depth: a voxel's test depends on nothing but the ray and the
// voxel, and every value here is `precise`, so that no shader fuses or
// reorders its arithmetic differently from another's (the C twin rounds as
// C does, and agrees to within the tests' tolerance).
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
// t_exit], from the cell's planes (vis_voxel_hit; voxel.c footprint). On a
// hit, t0 takes its t and `face` the face it entered.
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

// A ray in a brick's space: its origin relative to the brick's lowest
// corner, its direction (a component smaller than 1e-20 taken as 1e-20 of
// its sign, as the C twin does), and the direction's reciprocal.
struct VisVoxelRay {
    vec3 o;
    vec3 d;
    vec3 inv;
};

// The ray origin + t dir (mesh space) in the space of the brick whose
// lowest corner is `corner`.
VisVoxelRay vis_voxel_brick_ray(vec3 corner, vec3 origin, vec3 dir)
{
    precise vec3 o = origin - corner;
    precise vec3 d = dir;
    precise vec3 inv;
    for (uint a = 0u; a < 3u; ++a) {
        if (abs(d[a]) < 1e-20) {
            d[a] = d[a] < 0.0 ? -1e-20 : 1e-20;
        }
        inv[a] = 1.0 / d[a];
    }
    VisVoxelRay r;
    r.o = o;
    r.d = d;
    r.inv = inv;
    return r;
}

// The voxel `voxel` of cell `bit` (x + 4y + 16z) of a brick of cell `h`
// against the brick-space ray `r` for t in [t_min, t_max] (voxel.c
// voxel_hit): the cell's span along the ray, from the cell's own planes, then
// its footprint (vis_voxel_footprint). The raster and the searches after it
// test a voxel apart from its neighbours, each from the same planes, so they
// meet it at the same t bit for bit. On a hit, `t` and the face entered.
bool vis_voxel_hit(uvec4 voxel, uint bit, float h, VisVoxelRay r, float t_min, float t_max,
                   out float t, out uint face)
{
    t = 0.0;
    face = 0u;
    precise vec3 base = vec3(uvec3(bit & 3u, (bit >> 2u) & 3u, bit >> 4u)) * h;
    precise float enter = t_min;
    precise float leave = t_max;
    for (uint a = 0u; a < 3u; ++a) {
        // Cells are powers of two and their corners small multiples of
        // them, so base + h is exact, as is every footprint plane.
        precise float ta = (base[a] - r.o[a]) * r.inv[a];
        precise float tb = (base[a] + h - r.o[a]) * r.inv[a];
        enter = max(enter, min(ta, tb));
        leave = min(leave, max(ta, tb));
    }
    if (!(enter <= leave)) {
        return false;
    }
    precise float th = t_min;
    if (!vis_voxel_footprint(voxel.w, voxel.z, base, h / VIS_VOXEL_BOX_STEPS, r.o, r.d, r.inv,
                             enter, leave, th, t_max, face)) {
        return false;
    }
    t = th;
    return true;
}

// The rank of the set bit `bit` among a brick mask's (m0, m1): its voxel's
// index after the brick's first.
uint vis_voxel_rank(uint m0, uint m1, uint bit)
{
    return bit < 32u ? bitCount(m0 & ((1u << bit) - 1u))
                     : bitCount(m0) + bitCount(m1 & ((1u << (bit - 32u)) - 1u));
}

// The depth of a voxel hit at parameter t along a pixel's ray (under
// reverse-Z with an infinite far plane, exactly 1 / t), the same in every
// shader: the raster writes it, and the searches after it meet it again.
float vis_voxel_depth(float t)
{
    precise float z = 1.0 / t;
    return z;
}

// The voxel of brick (b0, b1) that the ray origin + t dir (mesh space, t >=
// 1) meets first, where the visibility buffer holds `depth` for it: among
// the occupied cells of the two along each axis whose centres lie nearest
// the ray's point at that depth, which hold that point whatever its
// rounding, the point's own cell first. The first whose voxel's hit gives the
// depth again is the raster's (vis_voxel_hit, bit for bit), else the nearest
// hit among them is taken. False where it meets none.
bool vis_voxel_find(uvec4 b0, uvec4 b1, VisVoxelsRef voxels, vec3 origin, vec3 dir, float depth,
                    out VisVoxelHit hit)
{
    hit.t = 0.0;
    hit.voxel = 0u;
    hit.cell = 0u;
    hit.face = 0u;
    const float h = uintBitsToFloat(b0.w);
    const VisVoxelRay r = vis_voxel_brick_ray(uintBitsToFloat(b0.xyz), origin, dir);
    const vec3 p = (r.o + (1.0 / max(depth, 1e-30)) * r.d) / h;
    const uvec3 own = min(uvec3(clamp(floor(p), vec3(0.0), vec3(3.0))), uvec3(3u));
    const uvec3 c0 = min(uvec3(floor(clamp(p - 0.5, vec3(0.0), vec3(2.0)))), uvec3(2u));
    bool found = false;
    float best = 3.402823466e38;
    for (uint k = 0u; k < 9u; ++k) {
        const uint j = k - 1u;
        const uvec3 c = k == 0u ? own : c0 + uvec3(j & 1u, (j >> 1u) & 1u, j >> 2u);
        if (k != 0u && all(equal(c, own))) {
            continue;
        }
        const uint bit = c.x + 4u * c.y + 16u * c.z;
        const uint word = bit < 32u ? b1.x : b1.y;
        if (((word >> (bit & 31u)) & 1u) == 0u) {
            continue;
        }
        const uint index = b1.z + vis_voxel_rank(b1.x, b1.y, bit);
        float t;
        uint face;
        if (vis_voxel_hit(voxels.v[index], bit, h, r, 1.0, 3.402823466e38, t, face) && t < best) {
            best = t;
            found = true;
            hit.t = t;
            hit.voxel = index;
            hit.cell = bit;
            hit.face = face;
            if (vis_voxel_depth(t) == depth) {
                break;
            }
        }
    }
    return found;
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

// The voxel the visibility buffer's texel `v` shows at `pixel`, found again
// in the brick it names near the texel's depth (vis_voxel_find), the ray
// rebuilt exactly as the raster built it; false where the ray meets none
// (which the precise arithmetic rules out).
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
    const bool found = vis_voxel_find(b0, b1, VisVoxelsRef(mesh.voxels), o, d, vis_depth(v), hit);
    out_s.transform = inst.transform;
    // Where the search finds nothing, the depth's point (and the voxel the
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

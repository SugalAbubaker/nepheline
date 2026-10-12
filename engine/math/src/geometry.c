// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/geometry.h>

// ---------------------------------------------------------------------------
// NphAABB
// ---------------------------------------------------------------------------

NphAABB nph_aabb_from_points(const NphVec3 *points, u32 count)
{
    NphAABB b = nph_aabb_empty();
    for (u32 i = 0; i < count; ++i) {
        b = nph_aabb_expand(b, points[i]);
    }
    return b;
}

NphAABB nph_aabb_transform(NphAABB b, NphAffine m)
{
    // Arvo 1990: the new centre is the transformed centre and the new
    // half-size along each output axis is the absolute row of the linear
    // part dotted with the old half-sizes.
    const NphVec3 c = nph_affine_transform_point(m, nph_aabb_center(b));
    const NphVec3 e = nph_aabb_extents(b);
    const NphVec3 ne = nph_vec3(nph_vec3_dot(nph_vec3_abs(nph_vec4_xyz(m.r[0])), e),
                                nph_vec3_dot(nph_vec3_abs(nph_vec4_xyz(m.r[1])), e),
                                nph_vec3_dot(nph_vec3_abs(nph_vec4_xyz(m.r[2])), e));
    return nph_aabb_from_center_extents(c, ne);
}

// ---------------------------------------------------------------------------
// NphSphere
// ---------------------------------------------------------------------------

NphSphere nph_sphere_from_points(const NphVec3 *points, u32 count)
{
    if (count == 0U) {
        return nph_sphere(nph_vec3_zero(), 0.0F);
    }
    // Pass 1: the two most distant of the six axis-extreme points give the
    // initial sphere.
    u32 lo[3] = {0U, 0U, 0U};
    u32 hi[3] = {0U, 0U, 0U};
    for (u32 i = 1; i < count; ++i) {
        for (u32 axis = 0; axis < 3U; ++axis) {
            const f32 v = nph_vec3_at(points[i], axis);
            if (v < nph_vec3_at(points[lo[axis]], axis)) {
                lo[axis] = i;
            }
            if (v > nph_vec3_at(points[hi[axis]], axis)) {
                hi[axis] = i;
            }
        }
    }
    u32 best = 0U;
    f32 best_dist_sq = -1.0F;
    for (u32 axis = 0; axis < 3U; ++axis) {
        const f32 d = nph_vec3_distance_sq(points[lo[axis]], points[hi[axis]]);
        if (d > best_dist_sq) {
            best_dist_sq = d;
            best = axis;
        }
    }
    NphVec3 center = nph_vec3_scale(nph_vec3_add(points[lo[best]], points[hi[best]]), 0.5F);
    f32 radius = 0.5F * nph_f32_sqrt(best_dist_sq);

    // Pass 2: grow to enclose every point that falls outside, moving the
    // centre toward it by half the overshoot.
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 d = nph_vec3_sub(points[i], center);
        const f32 dist_sq = nph_vec3_length_sq(d);
        if (dist_sq > radius * radius) {
            const f32 dist = nph_f32_sqrt(dist_sq);
            const f32 new_radius = 0.5F * (radius + dist);
            center = nph_vec3_mul_add(center, d, (new_radius - radius) / dist);
            radius = new_radius;
        }
    }
    return nph_sphere(center, radius);
}

NphSphere nph_sphere_union(NphSphere a, NphSphere b)
{
    const NphVec3 ab = nph_vec3_sub(b.center, a.center);
    const f32 d = nph_vec3_length(ab);
    if (d + b.radius <= a.radius) {
        return a;
    }
    if (d + a.radius <= b.radius) {
        return b;
    }
    // Neither contains the other, so d > 0: the result spans from the far
    // side of a to the far side of b.
    const f32 r = 0.5F * (d + a.radius + b.radius);
    return nph_sphere(nph_vec3_mul_add(a.center, ab, (r - a.radius) / d), r);
}

NphSphere nph_sphere_transform(NphSphere s, NphAffine m)
{
    const NphMat3 l = nph_affine_linear(m);
    const f32 scale = nph_f32_sqrt(nph_f32_max3(
        nph_vec3_length_sq(l.c[0]), nph_vec3_length_sq(l.c[1]), nph_vec3_length_sq(l.c[2])));
    return nph_sphere(nph_affine_transform_point(m, s.center), s.radius * scale);
}

// ---------------------------------------------------------------------------
// NphPlane
// ---------------------------------------------------------------------------

bool nph_plane_transform(NphPlane p, NphAffine m, NphPlane *out)
{
    NphMat3 inv;
    if (!nph_mat3_inverse(nph_affine_linear(m), &inv)) {
        return false;
    }
    // A point on the plane moves with m; the normal moves with the inverse
    // transpose of the linear part (it is a covector).
    const NphVec3 p0 = nph_vec3_scale(p.normal, -p.d / nph_vec3_length_sq(p.normal));
    const NphVec3 p1 = nph_affine_transform_point(m, p0);
    const NphVec3 n1 = nph_vec3_normalize(nph_mat3_mul_vec3(nph_mat3_transpose(inv), p.normal));
    *out = nph_plane_from_point_normal(p1, n1);
    return true;
}

// ---------------------------------------------------------------------------
// NphRay
// ---------------------------------------------------------------------------

bool nph_ray_intersects_aabb(NphRay r, NphAABB b, f32 t_max, f32 *out_t_min, f32 *out_t_max)
{
    f32 t_lo = 0.0F;
    f32 t_hi = t_max;
    for (u32 axis = 0; axis < 3U; ++axis) {
        const f32 o = nph_vec3_at(r.origin, axis);
        const f32 d = nph_vec3_at(r.dir, axis);
        const f32 lo = nph_vec3_at(b.min, axis);
        const f32 hi = nph_vec3_at(b.max, axis);
        if (d == 0.0F) {
            // Parallel to this slab: a hit needs the origin inside it. The
            // explicit branch avoids the 0 * inf NaN of the pure slab form.
            if (o < lo || o > hi) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.0F / d;
        f32 t1 = (lo - o) * inv;
        f32 t2 = (hi - o) * inv;
        if (t1 > t2) {
            const f32 tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        t_lo = nph_f32_max(t_lo, t1);
        t_hi = nph_f32_min(t_hi, t2);
        if (t_lo > t_hi) {
            return false;
        }
    }
    if (out_t_min != NULL) {
        *out_t_min = t_lo;
    }
    if (out_t_max != NULL) {
        *out_t_max = t_hi;
    }
    return true;
}

bool nph_ray_intersects_sphere(NphRay r, NphSphere s, f32 *out_t)
{
    // Ericson §5.3.2, generalised to a non-unit direction (a != 1).
    const NphVec3 m = nph_vec3_sub(r.origin, s.center);
    const f32 a = nph_vec3_length_sq(r.dir);
    const f32 b = nph_vec3_dot(m, r.dir);
    const f32 c = nph_vec3_length_sq(m) - s.radius * s.radius;
    if (c > 0.0F && b > 0.0F) {
        return false; // outside and pointing away
    }
    const f32 disc = b * b - a * c;
    if (disc < 0.0F || a == 0.0F) {
        return false;
    }
    const f32 root = nph_f32_sqrt(disc);
    f32 t = (-b - root) / a;
    if (t < 0.0F) {
        t = (-b + root) / a; // origin inside: the exit point
    }
    if (t < 0.0F) {
        return false;
    }
    if (out_t != NULL) {
        *out_t = t;
    }
    return true;
}

bool nph_ray_intersects_plane(NphRay r, NphPlane p, f32 *out_t)
{
    const f32 denom = nph_vec3_dot(p.normal, r.dir);
    if (nph_f32_abs(denom) < 1e-12F) {
        return false;
    }
    const f32 t = -nph_plane_distance(p, r.origin) / denom;
    if (t < 0.0F) {
        return false;
    }
    if (out_t != NULL) {
        *out_t = t;
    }
    return true;
}

bool nph_ray_intersects_triangle(NphRay r, NphVec3 a, NphVec3 b, NphVec3 c, bool cull_backfaces,
                                 f32 *out_t, f32 *out_u, f32 *out_v)
{
    const NphVec3 e1 = nph_vec3_sub(b, a);
    const NphVec3 e2 = nph_vec3_sub(c, a);
    const NphVec3 p = nph_vec3_cross(r.dir, e2);
    const f32 det = nph_vec3_dot(e1, p);
    // det > 0 means the ray approaches the counter-clockwise front.
    if (cull_backfaces ? det <= 1e-12F : nph_f32_abs(det) <= 1e-12F) {
        return false;
    }
    const f32 inv_det = 1.0F / det;
    const NphVec3 tv = nph_vec3_sub(r.origin, a);
    const f32 u = nph_vec3_dot(tv, p) * inv_det;
    if (u < 0.0F || u > 1.0F) {
        return false;
    }
    const NphVec3 q = nph_vec3_cross(tv, e1);
    const f32 v = nph_vec3_dot(r.dir, q) * inv_det;
    if (v < 0.0F || u + v > 1.0F) {
        return false;
    }
    const f32 t = nph_vec3_dot(e2, q) * inv_det;
    if (t < 0.0F) {
        return false;
    }
    if (out_t != NULL) {
        *out_t = t;
    }
    if (out_u != NULL) {
        *out_u = u;
    }
    if (out_v != NULL) {
        *out_v = v;
    }
    return true;
}

NphVec3 nph_closest_point_on_segment(NphVec3 a, NphVec3 b, NphVec3 p, f32 *out_t)
{
    const NphVec3 ab = nph_vec3_sub(b, a);
    const f32 len_sq = nph_vec3_length_sq(ab);
    f32 t = 0.0F;
    if (len_sq > 0.0F) {
        t = nph_f32_saturate(nph_vec3_dot(nph_vec3_sub(p, a), ab) / len_sq);
    }
    if (out_t != NULL) {
        *out_t = t;
    }
    return nph_vec3_mul_add(a, ab, t);
}

NphVec3 nph_closest_point_on_triangle(NphVec3 a, NphVec3 b, NphVec3 c, NphVec3 p)
{
    // Ericson §5.1.5: find which Voronoi feature region of the triangle
    // contains p, then project onto that vertex, edge, or the face.
    const NphVec3 ab = nph_vec3_sub(b, a);
    const NphVec3 ac = nph_vec3_sub(c, a);
    const NphVec3 ap = nph_vec3_sub(p, a);
    const f32 d1 = nph_vec3_dot(ab, ap);
    const f32 d2 = nph_vec3_dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return a;
    }
    const NphVec3 bp = nph_vec3_sub(p, b);
    const f32 d3 = nph_vec3_dot(ab, bp);
    const f32 d4 = nph_vec3_dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return nph_vec3_mul_add(a, ab, d1 / (d1 - d3));
    }
    const NphVec3 cp = nph_vec3_sub(p, c);
    const f32 d5 = nph_vec3_dot(ab, cp);
    const f32 d6 = nph_vec3_dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return nph_vec3_mul_add(a, ac, d2 / (d2 - d6));
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return nph_vec3_mul_add(b, nph_vec3_sub(c, b), w);
    }
    const f32 denom = 1.0F / (va + vb + vc);
    const f32 v = vb * denom;
    const f32 w = vc * denom;
    return nph_vec3_add(a, nph_vec3_add(nph_vec3_scale(ab, v), nph_vec3_scale(ac, w)));
}

// ---------------------------------------------------------------------------
// NphFrustum
// ---------------------------------------------------------------------------

NphFrustum nph_frustum_from_matrix(const NphMat4 *clip_from_world)
{
    // A point is inside when -w <= x <= w, -w <= y <= w, 0 <= z <= w in clip
    // space. Each inequality is linear in the world point, and its
    // coefficients are a sum or difference of two matrix rows.
    const NphVec4 r0 = nph_mat4_row(*clip_from_world, 0U);
    const NphVec4 r1 = nph_mat4_row(*clip_from_world, 1U);
    const NphVec4 r2 = nph_mat4_row(*clip_from_world, 2U);
    const NphVec4 r3 = nph_mat4_row(*clip_from_world, 3U);
    const NphVec4 raw[NPH_FRUSTUM_PLANE_COUNT] = {
        nph_vec4_add(r3, r0), // x >= -w   left
        nph_vec4_sub(r3, r0), // x <=  w   right
        nph_vec4_sub(r3, r1), // y <=  w   bottom (clip +Y is down)
        nph_vec4_add(r3, r1), // y >= -w   top
        nph_vec4_sub(r3, r2), // z <=  w   near (reversed depth: near is 1)
        r2,                   // z >=  0   far (0 at infinity)
    };
    NphFrustum f;
    f.plane_count = 0U;
    for (u32 i = 0; i < NPH_FRUSTUM_PLANE_COUNT; ++i) {
        const NphPlane p = nph_plane_from_vec4(raw[i]);
        const f32 len_sq = nph_vec3_length_sq(p.normal);
        if (len_sq < 1e-16F) {
            // Only the far plane can degenerate (infinite projection).
            NPH_ASSERT(i == NPH_FRUSTUM_FAR);
            continue;
        }
        f.planes[f.plane_count] = nph_plane_normalize(p);
        f.plane_count += 1U;
    }
    // Unused slots hold a plane every point satisfies, so loops over the
    // full array (the wide kernels) need no special case.
    for (u32 i = f.plane_count; i < NPH_FRUSTUM_PLANE_COUNT; ++i) {
        f.planes[i] = nph_plane(nph_vec3_zero(), 1.0F);
    }
    return f;
}

NphIntersection nph_frustum_test_aabb(const NphFrustum *f, NphAABB b)
{
    NphIntersection result = NPH_INTERSECTION_INSIDE;
    for (u32 i = 0; i < f->plane_count; ++i) {
        const NphPlane p = f->planes[i];
        // The p-vertex is the corner furthest along the normal, the
        // n-vertex the one furthest against it.
        const NphVec3 pv =
            nph_vec3(p.normal.x >= 0.0F ? b.max.x : b.min.x, p.normal.y >= 0.0F ? b.max.y : b.min.y,
                     p.normal.z >= 0.0F ? b.max.z : b.min.z);
        if (nph_plane_distance(p, pv) < 0.0F) {
            return NPH_INTERSECTION_OUTSIDE;
        }
        const NphVec3 nv =
            nph_vec3(p.normal.x >= 0.0F ? b.min.x : b.max.x, p.normal.y >= 0.0F ? b.min.y : b.max.y,
                     p.normal.z >= 0.0F ? b.min.z : b.max.z);
        if (nph_plane_distance(p, nv) < 0.0F) {
            result = NPH_INTERSECTION_INTERSECTS;
        }
    }
    return result;
}

NphIntersection nph_frustum_test_sphere(const NphFrustum *f, NphSphere s)
{
    NphIntersection result = NPH_INTERSECTION_INSIDE;
    for (u32 i = 0; i < f->plane_count; ++i) {
        const f32 d = nph_plane_distance(f->planes[i], s.center);
        if (d < -s.radius) {
            return NPH_INTERSECTION_OUTSIDE;
        }
        if (d < s.radius) {
            result = NPH_INTERSECTION_INTERSECTS;
        }
    }
    return result;
}

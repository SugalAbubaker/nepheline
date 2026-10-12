// SPDX-License-Identifier: Apache-2.0
#pragma once

// Bounding volumes, planes, rays, frusta, and the intersection tests between
// them (CLAUDE.md §8.2; Ericson, "Real-Time Collision Detection", 2005).
//
// Every test here is the scalar reference; the wide SoA kernels in wide.h
// implement the frustum tests for four or eight volumes at a time and are
// checked against these. All functions are pure and NPH_THREADSAFE.
//
// Conventions: an NphPlane is (n, d) with n·p + d == 0 on the plane and
// n·p + d > 0 on the side the normal points to. Frustum planes point
// inward, so a point is inside when every signed distance is >= 0. Rays
// are parametrised as origin + t·dir for t >= 0; dir need not be unit
// unless a function says so.

#include <nepheline/math/affine.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAABB {
    NphVec3 min;
    NphVec3 max;
} NphAABB;

typedef struct NphSphere {
    NphVec3 center;
    f32 radius;
} NphSphere;

// n·p + d == 0. Sixteen bytes, so it loads as one SIMD register.
typedef struct NphPlane {
    NphVec3 normal;
    f32 d;
} NphPlane;

typedef struct NphRay {
    NphVec3 origin;
    NphVec3 dir;
} NphRay;

// Plane indices in NphFrustum::planes. Names refer to clip space, where
// Vulkan's Y points down: BOTTOM is the clip-space +Y side.
typedef enum NphFrustumPlane {
    NPH_FRUSTUM_LEFT = 0,
    NPH_FRUSTUM_RIGHT,
    NPH_FRUSTUM_BOTTOM,
    NPH_FRUSTUM_TOP,
    NPH_FRUSTUM_NEAR,
    NPH_FRUSTUM_FAR, // absent (plane_count == 5) for an infinite projection
    NPH_FRUSTUM_PLANE_COUNT
} NphFrustumPlane;

// Inward-facing, normalised planes. plane_count is 6 for a finite
// projection and 5 for the default infinite one (ADR-0008 §2).
typedef struct NphFrustum {
    NphPlane planes[NPH_FRUSTUM_PLANE_COUNT];
    u32 plane_count;
} NphFrustum;

typedef enum NphIntersection {
    NPH_INTERSECTION_OUTSIDE = 0,
    NPH_INTERSECTION_INTERSECTS,
    NPH_INTERSECTION_INSIDE,
} NphIntersection;

NPH_STATIC_ASSERT(sizeof(NphAABB) == 24U, "NphAABB is six floats");
NPH_STATIC_ASSERT(sizeof(NphSphere) == 16U, "NphSphere is four floats");
NPH_STATIC_ASSERT(sizeof(NphPlane) == 16U, "NphPlane is four floats");

// ---------------------------------------------------------------------------
// NphAABB
// ---------------------------------------------------------------------------

NPH_INLINE NphAABB nph_aabb(NphVec3 min, NphVec3 max)
{
    return (NphAABB){min, max};
}
// The empty box: min = +inf, max = -inf, so that union and expand work
// without a special first case.
NPH_INLINE NphAABB nph_aabb_empty(void)
{
    return (NphAABB){nph_vec3_splat(NPH_F32_INFINITY), nph_vec3_splat(-NPH_F32_INFINITY)};
}
NPH_INLINE NphAABB nph_aabb_from_point(NphVec3 p)
{
    return (NphAABB){p, p};
}
// center ± extents (half-sizes).
NPH_INLINE NphAABB nph_aabb_from_center_extents(NphVec3 center, NphVec3 extents)
{
    return (NphAABB){nph_vec3_sub(center, extents), nph_vec3_add(center, extents)};
}
// Bounds of `count` points; empty when count == 0.
NphAABB nph_aabb_from_points(const NphVec3 *points, u32 count);

// True when any min exceeds its max (the result of nph_aabb_empty or of an
// intersection of disjoint boxes).
NPH_INLINE bool nph_aabb_is_empty(NphAABB b)
{
    return b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z;
}
NPH_INLINE NphVec3 nph_aabb_center(NphAABB b)
{
    return nph_vec3_scale(nph_vec3_add(b.min, b.max), 0.5F);
}
// Half-sizes.
NPH_INLINE NphVec3 nph_aabb_extents(NphAABB b)
{
    return nph_vec3_scale(nph_vec3_sub(b.max, b.min), 0.5F);
}
// Full sizes.
NPH_INLINE NphVec3 nph_aabb_size(NphAABB b)
{
    return nph_vec3_sub(b.max, b.min);
}
NPH_INLINE f32 nph_aabb_surface_area(NphAABB b)
{
    const NphVec3 s = nph_aabb_size(b);
    return 2.0F * (s.x * s.y + s.y * s.z + s.z * s.x);
}
NPH_INLINE f32 nph_aabb_volume(NphAABB b)
{
    const NphVec3 s = nph_aabb_size(b);
    return s.x * s.y * s.z;
}
// Corner i in 0..7: bit 0 selects max.x, bit 1 max.y, bit 2 max.z.
NPH_INLINE NphVec3 nph_aabb_corner(NphAABB b, u32 i)
{
    return nph_vec3((i & 1U) != 0U ? b.max.x : b.min.x, (i & 2U) != 0U ? b.max.y : b.min.y,
                    (i & 4U) != 0U ? b.max.z : b.min.z);
}
NPH_INLINE NphAABB nph_aabb_union(NphAABB a, NphAABB b)
{
    return (NphAABB){nph_vec3_min(a.min, b.min), nph_vec3_max(a.max, b.max)};
}
// The smallest box containing b and p.
NPH_INLINE NphAABB nph_aabb_expand(NphAABB b, NphVec3 p)
{
    return (NphAABB){nph_vec3_min(b.min, p), nph_vec3_max(b.max, p)};
}
// Inflates (or deflates, for negative margin) every face by `margin`.
NPH_INLINE NphAABB nph_aabb_grow(NphAABB b, f32 margin)
{
    const NphVec3 m = nph_vec3_splat(margin);
    return (NphAABB){nph_vec3_sub(b.min, m), nph_vec3_add(b.max, m)};
}
// May be empty (see nph_aabb_is_empty) when the boxes do not overlap.
NPH_INLINE NphAABB nph_aabb_intersection(NphAABB a, NphAABB b)
{
    return (NphAABB){nph_vec3_max(a.min, b.min), nph_vec3_min(a.max, b.max)};
}
NPH_INLINE NphAABB nph_aabb_translate(NphAABB b, NphVec3 t)
{
    return (NphAABB){nph_vec3_add(b.min, t), nph_vec3_add(b.max, t)};
}
// Closed intervals: touching faces count as containment and overlap.
NPH_INLINE bool nph_aabb_contains_point(NphAABB b, NphVec3 p)
{
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z &&
           p.z <= b.max.z;
}
NPH_INLINE bool nph_aabb_contains_aabb(NphAABB outer, NphAABB inner)
{
    return nph_aabb_contains_point(outer, inner.min) && nph_aabb_contains_point(outer, inner.max);
}
NPH_INLINE bool nph_aabb_overlaps(NphAABB a, NphAABB b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
// The point of b nearest to p (p itself when inside).
NPH_INLINE NphVec3 nph_aabb_closest_point(NphAABB b, NphVec3 p)
{
    return nph_vec3_clamp(p, b.min, b.max);
}
// Squared distance from p to the box; 0 inside.
NPH_INLINE f32 nph_aabb_distance_sq(NphAABB b, NphVec3 p)
{
    return nph_vec3_distance_sq(p, nph_aabb_closest_point(b, p));
}
// The axis-aligned bounds of the transformed box (Arvo, "Transforming
// Axis-Aligned Bounding Boxes", Graphics Gems 1990): the transformed centre
// plus the absolute linear part applied to the extents. Exact for the box
// of the box's corners; conservative for the contents. b must not be empty.
NphAABB nph_aabb_transform(NphAABB b, NphAffine m);

// The sphere through the box's corners.
NPH_INLINE NphSphere nph_aabb_bounding_sphere(NphAABB b)
{
    return (NphSphere){nph_aabb_center(b), nph_vec3_length(nph_aabb_extents(b))};
}
NPH_INLINE bool nph_aabb_eq(NphAABB a, NphAABB b)
{
    return nph_vec3_eq(a.min, b.min) && nph_vec3_eq(a.max, b.max);
}
NPH_INLINE bool nph_aabb_near(NphAABB a, NphAABB b, f32 tol)
{
    return nph_vec3_near(a.min, b.min, tol) && nph_vec3_near(a.max, b.max, tol);
}

// ---------------------------------------------------------------------------
// NphSphere
// ---------------------------------------------------------------------------

NPH_INLINE NphSphere nph_sphere(NphVec3 center, f32 radius)
{
    return (NphSphere){center, radius};
}
// An approximate bounding sphere of `count` points in two passes (Ritter,
// "An Efficient Bounding Sphere", Graphics Gems 1990): within a few percent
// of optimal, never too small. Zero radius at the origin for count == 0.
NphSphere nph_sphere_from_points(const NphVec3 *points, u32 count);

NPH_INLINE bool nph_sphere_contains_point(NphSphere s, NphVec3 p)
{
    return nph_vec3_distance_sq(s.center, p) <= s.radius * s.radius;
}
NPH_INLINE bool nph_sphere_overlaps_sphere(NphSphere a, NphSphere b)
{
    const f32 r = a.radius + b.radius;
    return nph_vec3_distance_sq(a.center, b.center) <= r * r;
}
// Arvo's test: the closest point of the box to the centre is within the radius.
NPH_INLINE bool nph_sphere_overlaps_aabb(NphSphere s, NphAABB b)
{
    return nph_aabb_distance_sq(b, s.center) <= s.radius * s.radius;
}
// The smallest sphere containing both.
NphSphere nph_sphere_union(NphSphere a, NphSphere b);

// The sphere under m: the centre is transformed and the radius scaled by
// the longest column of the linear part, which is the largest scale factor
// for rotation-and-scale transforms. Shear is not supported (it would need
// the largest singular value).
NphSphere nph_sphere_transform(NphSphere s, NphAffine m);

NPH_INLINE NphAABB nph_sphere_bounding_aabb(NphSphere s)
{
    return nph_aabb_from_center_extents(s.center, nph_vec3_splat(s.radius));
}
NPH_INLINE bool nph_sphere_eq(NphSphere a, NphSphere b)
{
    return nph_vec3_eq(a.center, b.center) && a.radius == b.radius;
}
NPH_INLINE bool nph_sphere_near(NphSphere a, NphSphere b, f32 tol)
{
    return nph_vec3_near(a.center, b.center, tol) && nph_f32_near(a.radius, b.radius, tol);
}

// ---------------------------------------------------------------------------
// NphPlane
// ---------------------------------------------------------------------------

NPH_INLINE NphPlane nph_plane(NphVec3 normal, f32 d)
{
    return (NphPlane){normal, d};
}
// The plane through p with unit normal n.
NPH_INLINE NphPlane nph_plane_from_point_normal(NphVec3 p, NphVec3 n)
{
    return (NphPlane){n, -nph_vec3_dot(n, p)};
}
// The plane through three non-collinear points, with the normal following
// the right-hand rule for the order a, b, c.
NPH_INLINE NphPlane nph_plane_from_points(NphVec3 a, NphVec3 b, NphVec3 c)
{
    const NphVec3 n = nph_vec3_normalize(nph_vec3_cross(nph_vec3_sub(b, a), nph_vec3_sub(c, a)));
    return nph_plane_from_point_normal(a, n);
}
// (a, b, c, d) with a x + b y + c z + d == 0.
NPH_INLINE NphPlane nph_plane_from_vec4(NphVec4 v)
{
    return (NphPlane){nph_vec4_xyz(v), v.w};
}
NPH_INLINE NphVec4 nph_plane_to_vec4(NphPlane p)
{
    return nph_vec4_from_vec3(p.normal, p.d);
}
// Scales so the normal is unit; distances then are metric.
NPH_INLINE NphPlane nph_plane_normalize(NphPlane p)
{
    const f32 inv_len = nph_f32_rsqrt(nph_vec3_length_sq(p.normal));
    return (NphPlane){nph_vec3_scale(p.normal, inv_len), p.d * inv_len};
}
// Signed distance; positive on the normal's side. Metric for unit normals.
NPH_INLINE f32 nph_plane_distance(NphPlane p, NphVec3 point)
{
    return nph_vec3_dot(p.normal, point) + p.d;
}
// The nearest point on the plane (unit normal).
NPH_INLINE NphVec3 nph_plane_project_point(NphPlane p, NphVec3 point)
{
    return nph_vec3_mul_add(point, p.normal, -nph_plane_distance(p, point));
}
// +1, 0, or -1 for a point beyond, within `epsilon` of, or behind the plane.
NPH_INLINE i32 nph_plane_side(NphPlane p, NphVec3 point, f32 epsilon)
{
    const f32 dist = nph_plane_distance(p, point);
    return dist > epsilon ? 1 : (dist < -epsilon ? -1 : 0);
}
NPH_INLINE NphPlane nph_plane_flip(NphPlane p)
{
    return (NphPlane){nph_vec3_neg(p.normal), -p.d};
}
// The plane under the affine transform m (normals transform by the inverse
// transpose of the linear part). Returns false, leaving *out untouched, when
// the linear part is singular. The result is renormalised.
bool nph_plane_transform(NphPlane p, NphAffine m, NphPlane *out);

NPH_INLINE bool nph_plane_eq(NphPlane a, NphPlane b)
{
    return nph_vec3_eq(a.normal, b.normal) && a.d == b.d;
}
NPH_INLINE bool nph_plane_near(NphPlane a, NphPlane b, f32 tol)
{
    return nph_vec3_near(a.normal, b.normal, tol) && nph_f32_near(a.d, b.d, tol);
}

// ---------------------------------------------------------------------------
// NphRay and closest points
// ---------------------------------------------------------------------------

NPH_INLINE NphRay nph_ray(NphVec3 origin, NphVec3 dir)
{
    return (NphRay){origin, dir};
}
NPH_INLINE NphVec3 nph_ray_at(NphRay r, f32 t)
{
    return nph_vec3_mul_add(r.origin, r.dir, t);
}

// Slab test (Williams et al., "An Efficient and Robust Ray–Box
// Intersection Algorithm", JGT 2005). Returns true with the entry and exit
// parameters when the ray hits the box for some t in [0, t_max]; an origin
// inside the box yields t_min == 0. Axis-parallel rays are handled through
// the IEEE semantics of the division by zero. Either output may be NULL.
bool nph_ray_intersects_aabb(NphRay r, NphAABB b, f32 t_max, f32 *out_t_min, f32 *out_t_max);

// Nearest intersection with t >= 0 (the exit point when the origin is
// inside). dir need not be unit. out_t may be NULL.
bool nph_ray_intersects_sphere(NphRay r, NphSphere s, f32 *out_t);

// False when the ray is parallel to the plane or points away from it.
bool nph_ray_intersects_plane(NphRay r, NphPlane p, f32 *out_t);

// Möller–Trumbore (1997). With cull_backfaces the triangle is one-sided:
// a hit requires the ray to approach the counter-clockwise front. Writes the
// parameter t and the barycentric coordinates (u, v) of b and c; a point is
// (1 - u - v) a + u b + v c. Any output may be NULL.
bool nph_ray_intersects_triangle(NphRay r, NphVec3 a, NphVec3 b, NphVec3 c, bool cull_backfaces,
                                 f32 *out_t, f32 *out_u, f32 *out_v);

// The point of segment [a, b] nearest to p, and its parameter in [0, 1].
NphVec3 nph_closest_point_on_segment(NphVec3 a, NphVec3 b, NphVec3 p, f32 *out_t);

// The point of triangle abc nearest to p (Ericson §5.1.5: Voronoi regions).
NphVec3 nph_closest_point_on_triangle(NphVec3 a, NphVec3 b, NphVec3 c, NphVec3 p);

// ---------------------------------------------------------------------------
// NphFrustum
// ---------------------------------------------------------------------------

// Extracts normalised, inward-facing planes from a clip matrix (world or
// view to Vulkan clip space) by combining its rows (Gribb & Hartmann, "Fast
// Extraction of Viewing Frustum Planes from the World-View-Projection
// Matrix", 2001), adapted to Z in [0, 1] and reversed depth. A degenerate
// far plane (infinite projection) is dropped, leaving five planes.
NphFrustum nph_frustum_from_matrix(const NphMat4 *clip_from_world);

NPH_INLINE bool nph_frustum_contains_point(const NphFrustum *f, NphVec3 p)
{
    for (u32 i = 0; i < f->plane_count; ++i) {
        if (nph_plane_distance(f->planes[i], p) < 0.0F) {
            return false;
        }
    }
    return true;
}
// Classifies a box against the frustum with the p-vertex / n-vertex test
// (Assarsson & Möller, "Optimized View Frustum Culling Algorithms for
// Bounding Boxes", JGT 2000). INTERSECTS may be reported for a box that is
// actually outside (near a frustum edge); OUTSIDE and INSIDE are exact.
NphIntersection nph_frustum_test_aabb(const NphFrustum *f, NphAABB b);
NphIntersection nph_frustum_test_sphere(const NphFrustum *f, NphSphere s);

// The culling predicates: true unless the volume is certainly outside.
NPH_INLINE bool nph_frustum_intersects_aabb(const NphFrustum *f, NphAABB b)
{
    return nph_frustum_test_aabb(f, b) != NPH_INTERSECTION_OUTSIDE;
}
NPH_INLINE bool nph_frustum_intersects_sphere(const NphFrustum *f, NphSphere s)
{
    return nph_frustum_test_sphere(f, s) != NPH_INTERSECTION_OUTSIDE;
}

#ifdef __cplusplus
}
#endif

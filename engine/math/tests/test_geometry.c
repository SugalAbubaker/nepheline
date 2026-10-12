// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/geometry.h>

#include "test_math_support.h"

#define ITER 1000U

// ---------------------------------------------------------------------------
// NphAABB
// ---------------------------------------------------------------------------

NTEST(aabb, construction_and_queries)
{
    const NphAABB b = nph_aabb(nph_vec3(-1.0F, -2.0F, -3.0F), nph_vec3(1.0F, 2.0F, 3.0F));
    NTEST_ASSERT_FALSE(nph_aabb_is_empty(b));
    NTEST_ASSERT_TRUE(nph_aabb_is_empty(nph_aabb_empty()));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_from_point(nph_vec3_one()), nph_aabb(nph_vec3_one(), nph_vec3_one())));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_from_center_extents(nph_vec3_zero(), nph_vec3(1.0F, 2.0F, 3.0F)), b));
    TM_ASSERT_VEC3_NEAR(nph_aabb_center(b), nph_vec3_zero(), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_aabb_extents(b), nph_vec3(1.0F, 2.0F, 3.0F), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_aabb_size(b), nph_vec3(2.0F, 4.0F, 6.0F), 0.0F);
    NTEST_ASSERT_ULP_F32(nph_aabb_surface_area(b), 2.0F * (8.0F + 24.0F + 12.0F), 0U);
    NTEST_ASSERT_ULP_F32(nph_aabb_volume(b), 48.0F, 0U);
    // Eight distinct corners, all inside.
    for (u32 i = 0; i < 8U; ++i) {
        const NphVec3 c = nph_aabb_corner(b, i);
        NTEST_ASSERT_TRUE(nph_aabb_contains_point(b, c));
        for (u32 j = 0; j < i; ++j) {
            NTEST_ASSERT_FALSE(nph_vec3_eq(c, nph_aabb_corner(b, j)));
        }
    }
    TM_ASSERT_VEC3_NEAR(nph_aabb_corner(b, 0U), b.min, 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_aabb_corner(b, 7U), b.max, 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_aabb_corner(b, 1U), nph_vec3(1.0F, -2.0F, -3.0F), 0.0F);
    NTEST_ASSERT_TRUE(nph_aabb_near(b, b, 0.0F));
    NTEST_ASSERT_FALSE(nph_aabb_near(b, nph_aabb_from_point(nph_vec3_zero()), 1e-6F));
    NTEST_ASSERT_FALSE(nph_aabb_eq(b, nph_aabb_from_point(nph_vec3_zero())));
}

NTEST(aabb, set_operations)
{
    const NphAABB a = nph_aabb(nph_vec3(0.0F, 0.0F, 0.0F), nph_vec3(2.0F, 2.0F, 2.0F));
    const NphAABB b = nph_aabb(nph_vec3(1.0F, 1.0F, 1.0F), nph_vec3(3.0F, 3.0F, 3.0F));
    const NphAABB c = nph_aabb(nph_vec3(5.0F, 5.0F, 5.0F), nph_vec3(6.0F, 6.0F, 6.0F));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_union(a, b), nph_aabb(nph_vec3_zero(), nph_vec3_splat(3.0F))));
    NTEST_ASSERT_TRUE(nph_aabb_eq(nph_aabb_union(nph_aabb_empty(), a), a));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_expand(a, nph_vec3(-1.0F, 1.0F, 5.0F)),
                    nph_aabb(nph_vec3(-1.0F, 0.0F, 0.0F), nph_vec3(2.0F, 2.0F, 5.0F))));
    NTEST_ASSERT_TRUE(nph_aabb_eq(nph_aabb_expand(nph_aabb_empty(), nph_vec3_one()),
                                  nph_aabb_from_point(nph_vec3_one())));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_grow(a, 0.5F), nph_aabb(nph_vec3_splat(-0.5F), nph_vec3_splat(2.5F))));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_grow(a, -0.5F), nph_aabb(nph_vec3_splat(0.5F), nph_vec3_splat(1.5F))));
    NTEST_ASSERT_TRUE(nph_aabb_eq(nph_aabb_intersection(a, b),
                                  nph_aabb(nph_vec3_splat(1.0F), nph_vec3_splat(2.0F))));
    NTEST_ASSERT_TRUE(nph_aabb_is_empty(nph_aabb_intersection(a, c)));
    NTEST_ASSERT_TRUE(
        nph_aabb_eq(nph_aabb_translate(a, nph_vec3(1.0F, 2.0F, 3.0F)),
                    nph_aabb(nph_vec3(1.0F, 2.0F, 3.0F), nph_vec3(3.0F, 4.0F, 5.0F))));
    // Closed intervals: boundaries count.
    NTEST_ASSERT_TRUE(nph_aabb_contains_point(a, nph_vec3(2.0F, 0.0F, 1.0F)));
    NTEST_ASSERT_FALSE(nph_aabb_contains_point(a, nph_vec3(2.1F, 0.0F, 1.0F)));
    NTEST_ASSERT_FALSE(nph_aabb_contains_point(a, nph_vec3(1.0F, -0.1F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_aabb_contains_aabb(nph_aabb_union(a, b), a));
    NTEST_ASSERT_FALSE(nph_aabb_contains_aabb(a, b));
    NTEST_ASSERT_TRUE(nph_aabb_overlaps(a, b));
    NTEST_ASSERT_FALSE(nph_aabb_overlaps(a, c));
    NTEST_ASSERT_TRUE(
        nph_aabb_overlaps(a, nph_aabb_translate(a, nph_vec3(2.0F, 0.0F, 0.0F)))); // touching
    TM_ASSERT_VEC3_NEAR(nph_aabb_closest_point(a, nph_vec3(5.0F, 1.0F, -1.0F)),
                        nph_vec3(2.0F, 1.0F, 0.0F), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_aabb_closest_point(a, nph_vec3(1.0F, 1.0F, 1.0F)),
                        nph_vec3(1.0F, 1.0F, 1.0F), 0.0F);
    NTEST_ASSERT_ULP_F32(nph_aabb_distance_sq(a, nph_vec3(5.0F, 1.0F, -1.0F)), 10.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_aabb_distance_sq(a, nph_vec3(1.0F, 1.0F, 1.0F)), 0.0F, 0U);
    const NphSphere s = nph_aabb_bounding_sphere(a);
    TM_ASSERT_VEC3_NEAR(s.center, nph_vec3_splat(1.0F), 0.0F);
    NTEST_ASSERT_NEAR_F32(s.radius, NPH_SQRT3, 1e-6F);
}

NTEST(aabb, from_points_and_transform)
{
    NTEST_ASSERT_TRUE(nph_aabb_is_empty(nph_aabb_from_points(NULL, 0U)));
    u64 rng = 40U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 pts[16];
        for (u32 k = 0; k < 16U; ++k) {
            pts[k] = tm_rng_vec3(&rng, -10.0F, 10.0F);
        }
        const NphAABB b = nph_aabb_from_points(pts, 16U);
        for (u32 k = 0; k < 16U; ++k) {
            NTEST_ASSERT_TRUE(nph_aabb_contains_point(b, pts[k]));
        }
        // Every face touches a point.
        NphVec3 lo = nph_vec3_splat(NPH_F32_INFINITY);
        NphVec3 hi = nph_vec3_splat(-NPH_F32_INFINITY);
        for (u32 k = 0; k < 16U; ++k) {
            lo = nph_vec3_min(lo, pts[k]);
            hi = nph_vec3_max(hi, pts[k]);
        }
        NTEST_ASSERT_TRUE(nph_aabb_eq(b, nph_aabb(lo, hi)));

        // Arvo's transform equals the bounds of the transformed corners.
        const NphAffine m = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));
        NphVec3 corners[8];
        for (u32 k = 0; k < 8U; ++k) {
            corners[k] = nph_affine_transform_point(m, nph_aabb_corner(b, k));
        }
        const NphAABB expected = nph_aabb_from_points(corners, 8U);
        const NphAABB got = nph_aabb_transform(b, m);
        NTEST_ASSERT_TRUE(nph_aabb_near(got, expected, 1e-3F));
    }
    // A quarter turn about Z swaps the x and y extents.
    const NphAABB box = nph_aabb(nph_vec3(0.0F, 0.0F, 0.0F), nph_vec3(2.0F, 1.0F, 1.0F));
    const NphAABB rotated =
        nph_aabb_transform(box, nph_affine_from_quat(nph_quat_from_rotation_z(NPH_HALF_PI)));
    NTEST_ASSERT_TRUE(nph_aabb_near(
        rotated, nph_aabb(nph_vec3(-1.0F, 0.0F, 0.0F), nph_vec3(0.0F, 2.0F, 1.0F)), 1e-6F));
}

// ---------------------------------------------------------------------------
// NphSphere
// ---------------------------------------------------------------------------

NTEST(sphere, queries_and_union)
{
    const NphSphere a = nph_sphere(nph_vec3_zero(), 1.0F);
    const NphSphere b = nph_sphere(nph_vec3(1.5F, 0.0F, 0.0F), 1.0F);
    const NphSphere c = nph_sphere(nph_vec3(5.0F, 0.0F, 0.0F), 1.0F);
    const NphSphere inner = nph_sphere(nph_vec3(0.2F, 0.0F, 0.0F), 0.5F);
    NTEST_ASSERT_TRUE(nph_sphere_contains_point(a, nph_vec3(0.0F, 1.0F, 0.0F)));
    NTEST_ASSERT_FALSE(nph_sphere_contains_point(a, nph_vec3(0.0F, 1.01F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_sphere_overlaps_sphere(a, b));
    NTEST_ASSERT_FALSE(nph_sphere_overlaps_sphere(a, c));
    NTEST_ASSERT_TRUE(
        nph_sphere_overlaps_sphere(a, nph_sphere(nph_vec3(2.0F, 0.0F, 0.0F), 1.0F))); // tangent
    const NphAABB box = nph_aabb(nph_vec3_splat(1.0F), nph_vec3_splat(2.0F));
    NTEST_ASSERT_FALSE(nph_sphere_overlaps_aabb(a, box)); // corner at distance sqrt(3)
    NTEST_ASSERT_TRUE(nph_sphere_overlaps_aabb(nph_sphere(nph_vec3_zero(), 1.8F), box));
    NTEST_ASSERT_TRUE(
        nph_sphere_overlaps_aabb(nph_sphere(nph_vec3_splat(1.5F), 0.1F), box)); // inside
    // Union: containment short cuts, then the general case.
    NTEST_ASSERT_TRUE(nph_sphere_eq(nph_sphere_union(a, inner), a));
    NTEST_ASSERT_TRUE(nph_sphere_eq(nph_sphere_union(inner, a), a));
    const NphSphere u = nph_sphere_union(a, c);
    NTEST_ASSERT_NEAR_F32(u.radius, 3.5F, 1e-6F);
    TM_ASSERT_VEC3_NEAR(u.center, nph_vec3(2.5F, 0.0F, 0.0F), 1e-6F);
    u64 rng = 41U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphSphere x =
            nph_sphere(tm_rng_vec3(&rng, -5.0F, 5.0F), tm_rng_range(&rng, 0.1F, 3.0F));
        const NphSphere y =
            nph_sphere(tm_rng_vec3(&rng, -5.0F, 5.0F), tm_rng_range(&rng, 0.1F, 3.0F));
        const NphSphere un = nph_sphere_union(x, y);
        // Contains both: distance between centres plus the inner radius fits.
        NTEST_ASSERT_TRUE(nph_vec3_distance(un.center, x.center) + x.radius <= un.radius + 1e-4F);
        NTEST_ASSERT_TRUE(nph_vec3_distance(un.center, y.center) + y.radius <= un.radius + 1e-4F);
        // And is minimal: the larger input when one contains the other,
        // otherwise the span from far side to far side.
        const f32 minimal =
            nph_f32_max3(x.radius, y.radius,
                         0.5F * (nph_vec3_distance(x.center, y.center) + x.radius + y.radius));
        NTEST_ASSERT_TRUE(un.radius <= minimal + 1e-4F);
    }
    NTEST_ASSERT_TRUE(nph_aabb_eq(nph_sphere_bounding_aabb(a),
                                  nph_aabb(nph_vec3_splat(-1.0F), nph_vec3_splat(1.0F))));
    NTEST_ASSERT_TRUE(nph_sphere_near(a, nph_sphere(nph_vec3_zero(), 1.0F + 1e-7F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_sphere_near(a, b, 1e-6F));
    NTEST_ASSERT_FALSE(nph_sphere_eq(a, b));
}

NTEST(sphere, from_points_and_transform)
{
    const NphSphere none = nph_sphere_from_points(NULL, 0U);
    NTEST_ASSERT_TRUE(nph_sphere_eq(none, nph_sphere(nph_vec3_zero(), 0.0F)));
    const NphVec3 one = nph_vec3(1.0F, 2.0F, 3.0F);
    NTEST_ASSERT_TRUE(nph_sphere_eq(nph_sphere_from_points(&one, 1U), nph_sphere(one, 0.0F)));
    u64 rng = 42U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 pts[32];
        for (u32 k = 0; k < 32U; ++k) {
            pts[k] = tm_rng_vec3(&rng, -3.0F, 3.0F);
        }
        const NphSphere s = nph_sphere_from_points(pts, 32U);
        for (u32 k = 0; k < 32U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_distance(s.center, pts[k]) <= s.radius * (1.0F + 1e-5F));
        }
        // Ritter's bound is within a quarter of the box's bounding sphere.
        const NphSphere box = nph_aabb_bounding_sphere(nph_aabb_from_points(pts, 32U));
        NTEST_ASSERT_TRUE(s.radius <= 1.25F * box.radius);
    }
    const NphSphere s = nph_sphere(nph_vec3(1.0F, 0.0F, 0.0F), 2.0F);
    const NphSphere rigid = nph_sphere_transform(
        s, nph_affine_from_trs(nph_vec3(0.0F, 0.0F, 5.0F), nph_quat_from_rotation_z(NPH_HALF_PI),
                               nph_vec3_one()));
    TM_ASSERT_VEC3_NEAR(rigid.center, nph_vec3(0.0F, 1.0F, 5.0F), 1e-6F);
    NTEST_ASSERT_NEAR_F32(rigid.radius, 2.0F, 1e-6F);
    const NphSphere scaled =
        nph_sphere_transform(s, nph_affine_from_scale(nph_vec3(1.0F, 3.0F, 2.0F)));
    NTEST_ASSERT_NEAR_F32(scaled.radius, 6.0F, 1e-6F);
    TM_ASSERT_VEC3_NEAR(scaled.center, nph_vec3(1.0F, 0.0F, 0.0F), 0.0F);
}

// ---------------------------------------------------------------------------
// NphPlane
// ---------------------------------------------------------------------------

NTEST(plane, construction_distance_projection)
{
    const NphPlane p = nph_plane_from_point_normal(nph_vec3(0.0F, 2.0F, 0.0F), nph_vec3_unit_y());
    NTEST_ASSERT_ULP_F32(p.d, -2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_plane_distance(p, nph_vec3(5.0F, 3.0F, -1.0F)), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_plane_distance(p, nph_vec3(5.0F, 0.0F, -1.0F)), -2.0F, 0U);
    TM_ASSERT_VEC3_NEAR(nph_plane_project_point(p, nph_vec3(5.0F, 3.0F, -1.0F)),
                        nph_vec3(5.0F, 2.0F, -1.0F), 0.0F);
    NTEST_ASSERT_EQ_INT(nph_plane_side(p, nph_vec3(0.0F, 3.0F, 0.0F), 1e-4F), 1);
    NTEST_ASSERT_EQ_INT(nph_plane_side(p, nph_vec3(0.0F, 1.0F, 0.0F), 1e-4F), -1);
    NTEST_ASSERT_EQ_INT(nph_plane_side(p, nph_vec3(9.0F, 2.00001F, 0.0F), 1e-4F), 0);
    // Right-hand rule: (0,0,0), (1,0,0), (0,1,0) face +Z.
    const NphPlane tri =
        nph_plane_from_points(nph_vec3_zero(), nph_vec3_unit_x(), nph_vec3_unit_y());
    TM_ASSERT_VEC3_NEAR(tri.normal, nph_vec3_unit_z(), 1e-6F);
    NTEST_ASSERT_ULP_F32(tri.d, 0.0F, 0U);
    const NphPlane raw = nph_plane(nph_vec3(0.0F, 0.0F, 4.0F), 8.0F);
    const NphPlane n = nph_plane_normalize(raw);
    TM_ASSERT_VEC3_NEAR(n.normal, nph_vec3_unit_z(), 0.0F);
    NTEST_ASSERT_ULP_F32(n.d, 2.0F, 0U);
    NTEST_ASSERT_TRUE(nph_plane_eq(nph_plane_from_vec4(nph_plane_to_vec4(raw)), raw));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_plane_to_vec4(raw), nph_vec4(0.0F, 0.0F, 4.0F, 8.0F)));
    const NphPlane f = nph_plane_flip(p);
    NTEST_ASSERT_ULP_F32(nph_plane_distance(f, nph_vec3(5.0F, 3.0F, -1.0F)), -1.0F, 0U);
    NTEST_ASSERT_TRUE(nph_plane_near(p, nph_plane(nph_vec3_unit_y(), -2.0F + 1e-7F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_plane_near(p, f, 1e-6F));
    NTEST_ASSERT_FALSE(nph_plane_eq(p, f));
}

NTEST(plane, transform)
{
    NphPlane out = nph_plane(nph_vec3_unit_x(), 0.0F);
    NTEST_ASSERT_FALSE(
        nph_plane_transform(out, nph_affine_from_scale(nph_vec3(1.0F, 0.0F, 1.0F)), &out));
    NTEST_ASSERT_TRUE(nph_plane_eq(out, nph_plane(nph_vec3_unit_x(), 0.0F))); // untouched
    u64 rng = 43U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 n = tm_rng_unit_vec3(&rng);
        const NphVec3 p0 = tm_rng_vec3(&rng, -5.0F, 5.0F);
        const NphPlane p = nph_plane_from_point_normal(p0, n);
        const NphAffine m = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));
        NphPlane q;
        NTEST_ASSERT_TRUE(nph_plane_transform(p, m, &q));
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(q.normal, 1e-5F));
        // Points on p land on q, and the positive side is preserved.
        NphVec3 t;
        NphVec3 b;
        nph_vec3_orthonormal_basis(n, &t, &b);
        const NphVec3 on =
            nph_vec3_add(p0, nph_vec3_add(nph_vec3_scale(t, tm_rng_range(&rng, -3.0F, 3.0F)),
                                          nph_vec3_scale(b, tm_rng_range(&rng, -3.0F, 3.0F))));
        NTEST_ASSERT_NEAR_F32(nph_plane_distance(q, nph_affine_transform_point(m, on)), 0.0F,
                              1e-3F);
        const NphVec3 above = nph_vec3_mul_add(p0, n, 1.0F);
        NTEST_ASSERT_TRUE(nph_plane_distance(q, nph_affine_transform_point(m, above)) > 0.0F);
    }
}

// ---------------------------------------------------------------------------
// Rays and closest points
// ---------------------------------------------------------------------------

NTEST(ray, aabb)
{
    const NphAABB b = nph_aabb(nph_vec3_splat(-1.0F), nph_vec3_splat(1.0F));
    f32 t0;
    f32 t1;
    NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(-5.0F, 0.0F, 0.0F), nph_vec3_unit_x()), b, NPH_F32_INFINITY, &t0, &t1));
    NTEST_ASSERT_NEAR_F32(t0, 4.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(t1, 6.0F, 1e-6F);
    // Origin inside: entry at 0.
    NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(nph_ray(nph_vec3_zero(), nph_vec3(0.0F, 0.0F, 2.0F)),
                                              b, NPH_F32_INFINITY, &t0, &t1));
    NTEST_ASSERT_ULP_F32(t0, 0.0F, 0U);
    NTEST_ASSERT_NEAR_F32(t1, 0.5F, 1e-6F);
    // Misses: beside, behind, and beyond t_max.
    NTEST_ASSERT_FALSE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(-5.0F, 2.0F, 0.0F), nph_vec3_unit_x()), b, NPH_F32_INFINITY, NULL, NULL));
    NTEST_ASSERT_FALSE(
        nph_ray_intersects_aabb(nph_ray(nph_vec3(-5.0F, 0.0F, 0.0F), nph_vec3(-1.0F, 0.0F, 0.0F)),
                                b, NPH_F32_INFINITY, NULL, NULL));
    NTEST_ASSERT_FALSE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(-5.0F, 0.0F, 0.0F), nph_vec3_unit_x()), b, 3.0F, NULL, NULL));
    NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(-5.0F, 0.0F, 0.0F), nph_vec3_unit_x()), b, 4.0F, NULL, NULL));
    // Axis-parallel rays: inside the slab hits, outside misses, on the face hits.
    NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(0.5F, 0.5F, -5.0F), nph_vec3_unit_z()), b, NPH_F32_INFINITY, &t0, NULL));
    NTEST_ASSERT_NEAR_F32(t0, 4.0F, 1e-6F);
    NTEST_ASSERT_FALSE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(1.5F, 0.5F, -5.0F), nph_vec3_unit_z()), b, NPH_F32_INFINITY, NULL, NULL));
    NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(
        nph_ray(nph_vec3(1.0F, 0.5F, -5.0F), nph_vec3_unit_z()), b, NPH_F32_INFINITY, NULL, NULL));
    // Diagonal through a corner region.
    NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(nph_ray(nph_vec3_splat(-3.0F), nph_vec3_one()), b,
                                              NPH_F32_INFINITY, &t0, &t1));
    NTEST_ASSERT_NEAR_F32(t0, 2.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(t1, 4.0F, 1e-6F);
    u64 rng = 44U;
    for (u32 i = 0; i < ITER; ++i) {
        // A ray aimed at a point inside the box always hits before it.
        const NphVec3 o = tm_rng_vec3(&rng, -10.0F, 10.0F);
        if (nph_aabb_contains_point(b, o)) {
            continue;
        }
        const NphVec3 target = tm_rng_vec3(&rng, -0.9F, 0.9F);
        const NphRay r = nph_ray(o, nph_vec3_sub(target, o));
        NTEST_ASSERT_TRUE(nph_ray_intersects_aabb(r, b, NPH_F32_INFINITY, &t0, &t1));
        NTEST_ASSERT_TRUE(t0 > 0.0F && t0 < 1.0F && t1 > 1.0F);
        NTEST_ASSERT_TRUE(nph_aabb_contains_point(nph_aabb_grow(b, 1e-4F), nph_ray_at(r, t0)));
        NTEST_ASSERT_TRUE(nph_aabb_contains_point(nph_aabb_grow(b, 1e-4F), nph_ray_at(r, t1)));
    }
}

NTEST(ray, sphere_and_plane)
{
    const NphSphere s = nph_sphere(nph_vec3(0.0F, 0.0F, -10.0F), 2.0F);
    f32 t;
    NTEST_ASSERT_TRUE(
        nph_ray_intersects_sphere(nph_ray(nph_vec3_zero(), nph_vec3(0.0F, 0.0F, -1.0F)), s, &t));
    NTEST_ASSERT_NEAR_F32(t, 8.0F, 1e-5F);
    // Non-unit direction scales t.
    NTEST_ASSERT_TRUE(
        nph_ray_intersects_sphere(nph_ray(nph_vec3_zero(), nph_vec3(0.0F, 0.0F, -2.0F)), s, &t));
    NTEST_ASSERT_NEAR_F32(t, 4.0F, 1e-5F);
    // Inside: the exit point.
    NTEST_ASSERT_TRUE(nph_ray_intersects_sphere(nph_ray(s.center, nph_vec3_unit_x()), s, &t));
    NTEST_ASSERT_NEAR_F32(t, 2.0F, 1e-5F);
    NTEST_ASSERT_FALSE(nph_ray_intersects_sphere(
        nph_ray(nph_vec3(3.0F, 0.0F, 0.0F), nph_vec3(0.0F, 0.0F, -1.0F)), s, &t));
    NTEST_ASSERT_FALSE(
        nph_ray_intersects_sphere(nph_ray(nph_vec3_zero(), nph_vec3_unit_z()), s, NULL)); // away
    NTEST_ASSERT_FALSE(
        nph_ray_intersects_sphere(nph_ray(nph_vec3_zero(), nph_vec3_zero()), s, NULL));
    NTEST_ASSERT_TRUE(nph_ray_intersects_sphere(
        nph_ray(nph_vec3(2.0F, 0.0F, 0.0F), nph_vec3(0.0F, 0.0F, -1.0F)), s, &t)); // tangent
    NTEST_ASSERT_NEAR_F32(t, 10.0F, 1e-3F);

    const NphPlane p = nph_plane_from_point_normal(nph_vec3(0.0F, -1.0F, 0.0F), nph_vec3_unit_y());
    NTEST_ASSERT_TRUE(nph_ray_intersects_plane(
        nph_ray(nph_vec3(0.0F, 3.0F, 0.0F), nph_vec3(0.0F, -2.0F, 0.0F)), p, &t));
    NTEST_ASSERT_NEAR_F32(t, 2.0F, 1e-6F);
    NTEST_ASSERT_TRUE(nph_ray_intersects_plane(
        nph_ray(nph_vec3(0.0F, -3.0F, 0.0F), nph_vec3_unit_y()), p, &t)); // from below
    NTEST_ASSERT_NEAR_F32(t, 2.0F, 1e-6F);
    NTEST_ASSERT_FALSE(nph_ray_intersects_plane(
        nph_ray(nph_vec3(0.0F, 3.0F, 0.0F), nph_vec3_unit_y()), p, &t)); // away
    NTEST_ASSERT_FALSE(nph_ray_intersects_plane(
        nph_ray(nph_vec3(0.0F, 3.0F, 0.0F), nph_vec3_unit_x()), p, NULL)); // parallel
}

NTEST(ray, triangle)
{
    // Counter-clockwise seen from +Z: faces +Z.
    const NphVec3 a = nph_vec3(0.0F, 0.0F, 0.0F);
    const NphVec3 b = nph_vec3(2.0F, 0.0F, 0.0F);
    const NphVec3 c = nph_vec3(0.0F, 2.0F, 0.0F);
    f32 t;
    f32 u;
    f32 v;
    const NphRay down = nph_ray(nph_vec3(0.5F, 0.5F, 3.0F), nph_vec3(0.0F, 0.0F, -1.0F));
    NTEST_ASSERT_TRUE(nph_ray_intersects_triangle(down, a, b, c, true, &t, &u, &v));
    NTEST_ASSERT_NEAR_F32(t, 3.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(u, 0.25F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(v, 0.25F, 1e-6F);
    // The barycentrics reconstruct the hit point.
    const NphVec3 hit = nph_vec3_add(
        nph_vec3_add(nph_vec3_scale(a, 1.0F - u - v), nph_vec3_scale(b, u)), nph_vec3_scale(c, v));
    TM_ASSERT_VEC3_NEAR(hit, nph_ray_at(down, t), 1e-6F);
    // From below it is a back face: culled when asked, hit otherwise.
    const NphRay up = nph_ray(nph_vec3(0.5F, 0.5F, -3.0F), nph_vec3_unit_z());
    NTEST_ASSERT_FALSE(nph_ray_intersects_triangle(up, a, b, c, true, NULL, NULL, NULL));
    NTEST_ASSERT_TRUE(nph_ray_intersects_triangle(up, a, b, c, false, &t, NULL, NULL));
    NTEST_ASSERT_NEAR_F32(t, 3.0F, 1e-6F);
    // Outside the edges, behind the origin, parallel.
    NTEST_ASSERT_FALSE(nph_ray_intersects_triangle(
        nph_ray(nph_vec3(1.5F, 1.5F, 3.0F), nph_vec3(0.0F, 0.0F, -1.0F)), a, b, c, false, NULL,
        NULL, NULL));
    NTEST_ASSERT_FALSE(nph_ray_intersects_triangle(
        nph_ray(nph_vec3(-0.1F, 0.5F, 3.0F), nph_vec3(0.0F, 0.0F, -1.0F)), a, b, c, false, NULL,
        NULL, NULL));
    NTEST_ASSERT_FALSE(nph_ray_intersects_triangle(
        nph_ray(nph_vec3(0.5F, -0.1F, 3.0F), nph_vec3(0.0F, 0.0F, -1.0F)), a, b, c, false, NULL,
        NULL, NULL));
    NTEST_ASSERT_FALSE(nph_ray_intersects_triangle(
        nph_ray(nph_vec3(0.5F, 0.5F, 3.0F), nph_vec3_unit_z()), a, b, c, false, NULL, NULL, NULL));
    NTEST_ASSERT_FALSE(nph_ray_intersects_triangle(
        nph_ray(nph_vec3(0.5F, 0.5F, 3.0F), nph_vec3_unit_x()), a, b, c, false, NULL, NULL, NULL));
    u64 rng = 45U;
    for (u32 i = 0; i < ITER; ++i) {
        // Aim at a random interior point of a random triangle.
        const NphVec3 ta = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 tb = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 tc = tm_rng_vec3(&rng, -3.0F, 3.0F);
        f32 bu = tm_rng_range(&rng, 0.05F, 0.9F);
        f32 bv = tm_rng_range(&rng, 0.05F, 0.9F);
        if (bu + bv > 0.95F) {
            bu *= 0.5F;
            bv *= 0.5F;
        }
        const NphVec3 target =
            nph_vec3_add(nph_vec3_add(nph_vec3_scale(ta, 1.0F - bu - bv), nph_vec3_scale(tb, bu)),
                         nph_vec3_scale(tc, bv));
        const NphVec3 o = tm_rng_vec3(&rng, -10.0F, 10.0F);
        const NphRay r = nph_ray(o, nph_vec3_sub(target, o));
        if (nph_f32_abs(nph_vec3_dot(nph_vec3_normalize(r.dir),
                                     nph_vec3_normalize(nph_vec3_cross(
                                         nph_vec3_sub(tb, ta), nph_vec3_sub(tc, ta))))) < 0.05F) {
            continue; // nearly grazing: numerically ambiguous by construction
        }
        NTEST_ASSERT_TRUE(nph_ray_intersects_triangle(r, ta, tb, tc, false, &t, &u, &v));
        NTEST_ASSERT_NEAR_F32(t, 1.0F, 1e-3F);
        NTEST_ASSERT_NEAR_F32(u, bu, 1e-3F);
        NTEST_ASSERT_NEAR_F32(v, bv, 1e-3F);
    }
}

NTEST(ray, closest_points)
{
    const NphVec3 a = nph_vec3(0.0F, 0.0F, 0.0F);
    const NphVec3 b = nph_vec3(4.0F, 0.0F, 0.0F);
    f32 t;
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_segment(a, b, nph_vec3(1.0F, 5.0F, 0.0F), &t),
                        nph_vec3(1.0F, 0.0F, 0.0F), 0.0F);
    NTEST_ASSERT_ULP_F32(t, 0.25F, 0U);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_segment(a, b, nph_vec3(-3.0F, 1.0F, 0.0F), &t), a,
                        0.0F);
    NTEST_ASSERT_ULP_F32(t, 0.0F, 0U);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_segment(a, b, nph_vec3(9.0F, 1.0F, 0.0F), NULL), b,
                        0.0F);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_segment(a, a, nph_vec3(9.0F, 1.0F, 0.0F), &t), a,
                        0.0F); // degenerate
    NTEST_ASSERT_ULP_F32(t, 0.0F, 0U);

    // Triangle in the XY plane; every Voronoi region.
    const NphVec3 c = nph_vec3(0.0F, 4.0F, 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(-1.0F, -1.0F, 2.0F)), a,
                        0.0F);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(6.0F, -1.0F, 2.0F)), b,
                        0.0F);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(-1.0F, 6.0F, 2.0F)), c,
                        0.0F);
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(2.0F, -3.0F, 0.0F)),
                        nph_vec3(2.0F, 0.0F, 0.0F), 1e-6F); // edge ab
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(-3.0F, 2.0F, 0.0F)),
                        nph_vec3(0.0F, 2.0F, 0.0F), 1e-6F); // edge ac
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(4.0F, 4.0F, 0.0F)),
                        nph_vec3(2.0F, 2.0F, 0.0F), 1e-6F); // edge bc
    TM_ASSERT_VEC3_NEAR(nph_closest_point_on_triangle(a, b, c, nph_vec3(1.0F, 1.0F, 5.0F)),
                        nph_vec3(1.0F, 1.0F, 0.0F), 1e-6F); // face
    u64 rng = 46U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 ta = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 tb = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 tc = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 p = tm_rng_vec3(&rng, -6.0F, 6.0F);
        const NphVec3 q = nph_closest_point_on_triangle(ta, tb, tc, p);
        const f32 best = nph_vec3_distance_sq(p, q);
        // No sampled point of the triangle is closer, and q lies on it.
        for (u32 k = 0; k < 32U; ++k) {
            f32 bu = tm_rng_f32(&rng);
            f32 bv = tm_rng_f32(&rng);
            if (bu + bv > 1.0F) {
                bu = 1.0F - bu;
                bv = 1.0F - bv;
            }
            const NphVec3 s = nph_vec3_add(
                nph_vec3_add(nph_vec3_scale(ta, 1.0F - bu - bv), nph_vec3_scale(tb, bu)),
                nph_vec3_scale(tc, bv));
            NTEST_ASSERT_TRUE(nph_vec3_distance_sq(p, s) >= best - 1e-3F);
        }
        const NphPlane tri = nph_plane_from_points(ta, tb, tc);
        NTEST_ASSERT_NEAR_F32(nph_plane_distance(tri, q), 0.0F, 1e-3F);
    }
}

// ---------------------------------------------------------------------------
// NphFrustum
// ---------------------------------------------------------------------------

// Reference: the clip-space inequalities the planes are derived from.
static bool clip_inside(NphMat4 m, NphVec3 p, f32 margin)
{
    const NphVec4 c = nph_mat4_mul_vec4(m, nph_vec4_point(p));
    const f32 w = c.w;
    return w > margin && c.x >= -w + margin && c.x <= w - margin && c.y >= -w + margin &&
           c.y <= w - margin && c.z >= margin && c.z <= w - margin;
}

static bool clip_ambiguous(NphMat4 m, NphVec3 p, f32 margin)
{
    return clip_inside(m, p, -margin) != clip_inside(m, p, margin);
}

NTEST(frustum, plane_extraction_matches_clip_space)
{
    const NphMat4 inf = nph_mat4_perspective_infinite(1.1F, 1.7F, 0.3F);
    const NphMat4 fin = nph_mat4_perspective(1.1F, 1.7F, 0.3F, 50.0F);
    const NphMat4 ortho = nph_mat4_orthographic(-4.0F, 4.0F, -2.0F, 2.0F, 0.5F, 30.0F);
    const NphFrustum fi = nph_frustum_from_matrix(&inf);
    const NphFrustum ff = nph_frustum_from_matrix(&fin);
    const NphFrustum fo = nph_frustum_from_matrix(&ortho);
    NTEST_ASSERT_EQ_UINT(fi.plane_count, 5U);
    NTEST_ASSERT_EQ_UINT(ff.plane_count, 6U);
    NTEST_ASSERT_EQ_UINT(fo.plane_count, 6U);
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(ff.planes[i].normal, 1e-5F));
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(fo.planes[i].normal, 1e-5F));
    }
    // The near plane of the perspective frusta is z = -near facing -Z.
    TM_ASSERT_VEC3_NEAR(fi.planes[NPH_FRUSTUM_NEAR].normal, nph_vec3(0.0F, 0.0F, -1.0F), 1e-6F);
    NTEST_ASSERT_NEAR_F32(fi.planes[NPH_FRUSTUM_NEAR].d, -0.3F, 1e-6F);
    TM_ASSERT_VEC3_NEAR(ff.planes[NPH_FRUSTUM_FAR].normal, nph_vec3_unit_z(), 1e-6F);
    NTEST_ASSERT_NEAR_F32(ff.planes[NPH_FRUSTUM_FAR].d, 50.0F, 1e-4F);
    // The unused slot accepts everything.
    NTEST_ASSERT_TRUE(nph_plane_distance(fi.planes[NPH_FRUSTUM_FAR], nph_vec3_splat(1e6F)) > 0.0F);

    const NphMat4 mats[3] = {inf, fin, ortho};
    const NphFrustum *frusta[3] = {&fi, &ff, &fo};
    // Sampling boxes a little larger than each frustum, so both sides of
    // every plane are exercised and a good share of points lands inside.
    const NphVec3 lo[3] = {nph_vec3(-60.0F, -60.0F, -80.0F), nph_vec3(-60.0F, -60.0F, -80.0F),
                           nph_vec3(-6.0F, -3.0F, -40.0F)};
    const NphVec3 hi[3] = {nph_vec3(60.0F, 60.0F, 5.0F), nph_vec3(60.0F, 60.0F, 5.0F),
                           nph_vec3(6.0F, 3.0F, 2.0F)};
    u64 rng = 47U;
    for (u32 k = 0; k < 3U; ++k) {
        u32 inside = 0U;
        for (u32 i = 0; i < 4U * ITER; ++i) {
            const NphVec3 p =
                nph_vec3(tm_rng_range(&rng, lo[k].x, hi[k].x), tm_rng_range(&rng, lo[k].y, hi[k].y),
                         tm_rng_range(&rng, lo[k].z, hi[k].z));
            // Skip points within a clip-space margin of a boundary, where
            // rounding in either formulation could flip the answer.
            if (clip_ambiguous(mats[k], p, 1e-3F)) {
                continue;
            }
            const bool expected = clip_inside(mats[k], p, 0.0F);
            NTEST_ASSERT_TRUE(nph_frustum_contains_point(frusta[k], p) == expected);
            inside += expected ? 1U : 0U;
        }
        NTEST_ASSERT_GT_UINT(inside, 50U); // the sample actually covered the inside
    }
}

NTEST(frustum, aabb_and_sphere_classification)
{
    const NphMat4 proj = nph_mat4_perspective(NPH_HALF_PI, 1.0F, 1.0F, 100.0F);
    const NphFrustum f = nph_frustum_from_matrix(&proj);
    // At distance 10 the frustum is 20 wide; boxes well inside, straddling, outside.
    const NphAABB inside = nph_aabb(nph_vec3(-2.0F, -2.0F, -12.0F), nph_vec3(2.0F, 2.0F, -8.0F));
    const NphAABB straddle = nph_aabb(nph_vec3(8.0F, -1.0F, -12.0F), nph_vec3(12.0F, 1.0F, -8.0F));
    const NphAABB outside = nph_aabb(nph_vec3(30.0F, -1.0F, -12.0F), nph_vec3(32.0F, 1.0F, -8.0F));
    const NphAABB behind = nph_aabb(nph_vec3(-1.0F, -1.0F, 5.0F), nph_vec3(1.0F, 1.0F, 6.0F));
    const NphAABB too_far =
        nph_aabb(nph_vec3(-1.0F, -1.0F, -200.0F), nph_vec3(1.0F, 1.0F, -150.0F));
    NTEST_ASSERT_EQ_INT(nph_frustum_test_aabb(&f, inside), NPH_INTERSECTION_INSIDE);
    NTEST_ASSERT_EQ_INT(nph_frustum_test_aabb(&f, straddle), NPH_INTERSECTION_INTERSECTS);
    NTEST_ASSERT_EQ_INT(nph_frustum_test_aabb(&f, outside), NPH_INTERSECTION_OUTSIDE);
    NTEST_ASSERT_EQ_INT(nph_frustum_test_aabb(&f, behind), NPH_INTERSECTION_OUTSIDE);
    NTEST_ASSERT_EQ_INT(nph_frustum_test_aabb(&f, too_far), NPH_INTERSECTION_OUTSIDE);
    NTEST_ASSERT_TRUE(nph_frustum_intersects_aabb(&f, straddle));
    NTEST_ASSERT_FALSE(nph_frustum_intersects_aabb(&f, outside));
    NTEST_ASSERT_EQ_INT(nph_frustum_test_sphere(&f, nph_sphere(nph_vec3(0.0F, 0.0F, -10.0F), 2.0F)),
                        NPH_INTERSECTION_INSIDE);
    NTEST_ASSERT_EQ_INT(
        nph_frustum_test_sphere(&f, nph_sphere(nph_vec3(10.0F, 0.0F, -10.0F), 2.0F)),
        NPH_INTERSECTION_INTERSECTS);
    NTEST_ASSERT_EQ_INT(
        nph_frustum_test_sphere(&f, nph_sphere(nph_vec3(30.0F, 0.0F, -10.0F), 2.0F)),
        NPH_INTERSECTION_OUTSIDE);
    NTEST_ASSERT_EQ_INT(nph_frustum_test_sphere(&f, nph_sphere(nph_vec3(0.0F, 0.0F, 3.0F), 1.0F)),
                        NPH_INTERSECTION_OUTSIDE);
    NTEST_ASSERT_TRUE(
        nph_frustum_intersects_sphere(&f, nph_sphere(nph_vec3(10.0F, 0.0F, -10.0F), 2.0F)));
    NTEST_ASSERT_FALSE(
        nph_frustum_intersects_sphere(&f, nph_sphere(nph_vec3(30.0F, 0.0F, -10.0F), 2.0F)));

    // Exactness: all corners inside => INSIDE; all corners beyond one plane
    // => OUTSIDE; a box whose corners are split is never OUTSIDE.
    u64 rng = 48U;
    u32 seen_inside = 0U;
    u32 seen_outside = 0U;
    for (u32 i = 0; i < 4U * ITER; ++i) {
        const NphVec3 c =
            nph_vec3(tm_rng_range(&rng, -40.0F, 40.0F), tm_rng_range(&rng, -40.0F, 40.0F),
                     tm_rng_range(&rng, -120.0F, 10.0F));
        const NphAABB b = nph_aabb_from_center_extents(c, tm_rng_vec3(&rng, 0.1F, 6.0F));
        u32 corners_inside = 0U;
        for (u32 k = 0; k < 8U; ++k) {
            corners_inside += nph_frustum_contains_point(&f, nph_aabb_corner(b, k)) ? 1U : 0U;
        }
        const NphIntersection r = nph_frustum_test_aabb(&f, b);
        if (corners_inside == 8U) {
            NTEST_ASSERT_EQ_INT(r, NPH_INTERSECTION_INSIDE);
            seen_inside += 1U;
        } else if (corners_inside > 0U) {
            NTEST_ASSERT_EQ_INT(r, NPH_INTERSECTION_INTERSECTS);
        } else {
            NTEST_ASSERT_NE_INT(r, NPH_INTERSECTION_INSIDE);
            seen_outside += r == NPH_INTERSECTION_OUTSIDE ? 1U : 0U;
        }
        // Sphere: consistent with the box test for the inscribed sphere.
        const NphSphere s = nph_sphere(c, nph_vec3_min_component(nph_aabb_extents(b)));
        if (r == NPH_INTERSECTION_INSIDE) {
            NTEST_ASSERT_EQ_INT(nph_frustum_test_sphere(&f, s), NPH_INTERSECTION_INSIDE);
        }
        if (r == NPH_INTERSECTION_OUTSIDE) {
            NTEST_ASSERT_EQ_INT(nph_frustum_test_sphere(&f, s), NPH_INTERSECTION_OUTSIDE);
        }
    }
    NTEST_ASSERT_GT_UINT(seen_inside, 20U);
    NTEST_ASSERT_GT_UINT(seen_outside, 20U);
}

NTEST(frustum, world_space_from_view_projection)
{
    const NphVec3 eye = nph_vec3(10.0F, 5.0F, -3.0F);
    const NphVec3 target = nph_vec3(0.0F, 1.0F, 20.0F);
    const NphMat4 view = nph_mat4_look_at(eye, target, nph_vec3_unit_y());
    const NphMat4 proj = nph_mat4_perspective_infinite(1.0F, 16.0F / 9.0F, 0.1F);
    const NphMat4 clip_from_world = nph_mat4_mul(proj, view);
    const NphFrustum f = nph_frustum_from_matrix(&clip_from_world);
    NTEST_ASSERT_EQ_UINT(f.plane_count, 5U);
    NTEST_ASSERT_TRUE(nph_frustum_contains_point(&f, target));
    NTEST_ASSERT_TRUE(nph_frustum_contains_point(&f, nph_vec3_lerp(eye, target, 0.5F)));
    // Behind the camera, and in front of the near plane.
    NTEST_ASSERT_FALSE(nph_frustum_contains_point(&f, nph_vec3_lerp(eye, target, -0.5F)));
    NTEST_ASSERT_FALSE(nph_frustum_contains_point(&f, nph_vec3_lerp(eye, target, 0.001F)));
    // Far along the view direction is still inside (infinite far plane).
    NTEST_ASSERT_TRUE(nph_frustum_contains_point(&f, nph_vec3_lerp(eye, target, 1000.0F)));
    NTEST_ASSERT_EQ_INT(
        nph_frustum_test_aabb(&f, nph_aabb_from_center_extents(target, nph_vec3_splat(0.5F))),
        NPH_INTERSECTION_INSIDE);
}

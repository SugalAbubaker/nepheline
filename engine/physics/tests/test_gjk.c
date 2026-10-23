// SPDX-License-Identifier: Apache-2.0
// GJK and EPA: distances against closed forms and brute force, overlap,
// caching, and penetration depths against the separating axis test.

#include "test_physics_support.h"

static NphPhysicsProxy proxy_of(const NphPhysicsGeometry *g, NphVec3 storage[8])
{
    return nph_physics_geometry_proxy(g, storage);
}

NTEST(gjk, spheres_closed_form)
{
    const NphPhysicsGeometry a = nph_physics_geometry_sphere(0.5F);
    const NphPhysicsGeometry b = nph_physics_geometry_sphere(0.25F);
    NphVec3 sa[8];
    NphVec3 sb[8];
    const NphPhysicsProxy pa = proxy_of(&a, sa);
    const NphPhysicsProxy pb = proxy_of(&b, sb);
    NphPcg32 rng = test_rng(1U);
    for (u32 k = 0U; k < 100U; ++k) {
        const NphPhysicsPose ta = test_pose(&rng, 5.0F);
        const NphPhysicsPose tb = test_pose(&rng, 5.0F);
        NphPhysicsDistance d;
        nph_physics_distance(&pa, ta, &pb, tb, true, NULL, &d);
        const f32 centres = nph_vec3_distance(ta.position, tb.position);
        const f32 want = nph_f32_max(centres - 0.75F, 0.0F);
        NTEST_EXPECT_NEAR_F32(d.distance, want, 1e-4F);
        if (want > 0.0F) {
            NTEST_EXPECT_NEAR_F32(nph_vec3_distance(d.point_a, ta.position), 0.5F, 1e-4F);
            NTEST_EXPECT_NEAR_F32(nph_vec3_distance(d.point_b, tb.position), 0.25F, 1e-4F);
            NTEST_EXPECT_NEAR_F32(nph_vec3_distance(d.point_a, d.point_b), d.distance, 1e-4F);
        }
    }
}

NTEST(gjk, point_to_hull_matches_brute_force)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(2U);
    for (u32 h = 0U; h < 4U * TEST_SCALE; ++h) {
        NphPhysicsHull *hull =
            test_random_hull(alloc, &rng, 40U, 1.0F, test_vec3(&rng, 0.4F, 2.0F));
        const NphPhysicsGeometry g = nph_physics_geometry_hull(hull);
        NphVec3 storage[8];
        const NphPhysicsProxy ph = proxy_of(&g, storage);
        for (u32 k = 0U; k < 40U; ++k) {
            const NphVec3 p = nph_vec3_scale(test_unit(&rng), test_range(&rng, 1.5F, 6.0F));
            const NphPhysicsProxy pp = {&p, 1U, 0.0F};
            NphPhysicsDistance d;
            nph_physics_distance(&ph, nph_physics_pose_identity(), &pp, nph_physics_pose_identity(),
                                 false, NULL, &d);
            if (test_hull_plane_distance(hull, p) > 0.0F) {
                const f32 want = test_hull_surface_distance(hull, p);
                NTEST_EXPECT_NEAR_F32(d.distance, want, 1e-4F * nph_f32_max(1.0F, want));
                NTEST_EXPECT_NEAR_F32(test_hull_plane_distance(hull, d.point_a), 0.0F, 1e-4F);
            }
        }
        nph_physics_hull_free(hull, alloc);
    }
    test_alloc_end(&ta);
}

NTEST(gjk, boxes_and_capsules)
{
    // Two unit boxes 3 m apart along X, one turned about X: the gap is 1.
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3_one());
    NphVec3 s1[8];
    NphVec3 s2[8];
    const NphPhysicsProxy p1 = proxy_of(&box, s1);
    const NphPhysicsProxy p2 = proxy_of(&box, s2);
    NphPhysicsDistance d;
    nph_physics_distance(
        &p1, nph_physics_pose_identity(), &p2,
        nph_physics_pose(nph_vec3(3.0F, 0.0F, 0.0F), nph_quat_from_rotation_x(0.6F)), true, NULL,
        &d);
    NTEST_EXPECT_NEAR_F32(d.distance, 1.0F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(d.normal, nph_vec3_unit_x(), 1e-5F));
    // A capsule standing 2 m above the box's top: gap 2 - radius.
    const NphPhysicsGeometry cap = nph_physics_geometry_capsule(0.5F, 0.25F);
    NphVec3 s3[8];
    const NphPhysicsProxy p3 = proxy_of(&cap, s3);
    nph_physics_distance(&p1, nph_physics_pose_identity(), &p3,
                         nph_physics_pose(nph_vec3(0.2F, 3.5F, -0.3F), nph_quat_identity()), true,
                         NULL, &d);
    NTEST_EXPECT_NEAR_F32(d.distance, 1.75F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(d.normal, nph_vec3_unit_y(), 1e-5F));
    // Overlap: zero distance and a full simplex.
    nph_physics_distance(
        &p1, nph_physics_pose_identity(), &p2,
        nph_physics_pose(nph_vec3(1.5F, 0.2F, 0.1F), nph_quat_from_rotation_y(0.3F)), true, NULL,
        &d);
    NTEST_EXPECT_NEAR_F32(d.distance, 0.0F, 0.0F);
}

NTEST(gjk, the_cache_warm_starts)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(8U);
    NphPhysicsHull *ha = test_random_hull(alloc, &rng, 64U, 1.0F, nph_vec3_one());
    NphPhysicsHull *hb = test_random_hull(alloc, &rng, 64U, 1.0F, nph_vec3_one());
    const NphPhysicsGeometry ga = nph_physics_geometry_hull(ha);
    const NphPhysicsGeometry gb = nph_physics_geometry_hull(hb);
    NphVec3 s1[8];
    NphVec3 s2[8];
    const NphPhysicsProxy pa = proxy_of(&ga, s1);
    const NphPhysicsProxy pb = proxy_of(&gb, s2);
    NphPhysicsSimplexCache cache;
    memset(&cache, 0, sizeof cache);
    u32 cold = 0U;
    u32 warm = 0U;
    for (u32 k = 0U; k < 50U; ++k) {
        // B drifts slowly past A: coherent queries, as in a simulation.
        const f32 t = (f32)k * 0.01F;
        const NphPhysicsPose pose_b =
            nph_physics_pose(nph_vec3(2.5F + t, 0.3F - t, 0.1F), nph_quat_from_rotation_y(t));
        NphPhysicsDistance d_cold;
        NphPhysicsDistance d_warm;
        nph_physics_distance(&pa, nph_physics_pose_identity(), &pb, pose_b, false, NULL, &d_cold);
        nph_physics_distance(&pa, nph_physics_pose_identity(), &pb, pose_b, false, &cache, &d_warm);
        NTEST_EXPECT_NEAR_F32(d_warm.distance, d_cold.distance, 1e-4F);
        cold += d_cold.iterations;
        warm += d_warm.iterations;
    }
    NTEST_EXPECT_TRUE(warm < cold);
    nph_physics_hull_free(ha, alloc);
    nph_physics_hull_free(hb, alloc);
    test_alloc_end(&ta);
}

NTEST(gjk, penetration_of_spheres_and_rounded_cores)
{
    const NphPhysicsGeometry a = nph_physics_geometry_sphere(1.0F);
    NphVec3 sa[8];
    NphVec3 sb[8];
    const NphPhysicsProxy pa = proxy_of(&a, sa);
    const NphPhysicsProxy pb = proxy_of(&a, sb);
    NphPhysicsPenetration pen;
    NTEST_ASSERT_TRUE(nph_physics_penetration(
        &pa, nph_physics_pose_identity(), &pb,
        nph_physics_pose(nph_vec3(1.5F, 0.0F, 0.0F), nph_quat_identity()), &pen));
    NTEST_EXPECT_NEAR_F32(pen.depth, 0.5F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(pen.normal, nph_vec3_unit_x(), 1e-5F));
    NTEST_EXPECT_TRUE(nph_vec3_near(pen.point_a, nph_vec3(1.0F, 0.0F, 0.0F), 1e-5F));
    NTEST_EXPECT_TRUE(nph_vec3_near(pen.point_b, nph_vec3(0.5F, 0.0F, 0.0F), 1e-5F));
    NTEST_EXPECT_FALSE(nph_physics_penetration(
        &pa, nph_physics_pose_identity(), &pb,
        nph_physics_pose(nph_vec3(2.5F, 0.0F, 0.0F), nph_quat_identity()), &pen));
    // Crossed capsules whose cores pass 0.1 apart, radii 0.2 each.
    const NphPhysicsGeometry c = nph_physics_geometry_capsule(1.0F, 0.2F);
    NphVec3 sc[8];
    NphVec3 sd[8];
    const NphPhysicsProxy pc = proxy_of(&c, sc);
    const NphPhysicsProxy pd = proxy_of(&c, sd);
    NTEST_ASSERT_TRUE(nph_physics_penetration(
        &pc, nph_physics_pose_identity(), &pd,
        nph_physics_pose(nph_vec3(0.0F, 0.0F, 0.1F), nph_quat_from_rotation_z(1.5707963F)), &pen));
    NTEST_EXPECT_NEAR_F32(pen.depth, 0.3F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(pen.normal, nph_vec3_unit_z(), 1e-5F));
}

// The separating translation along n: max over A of a.n minus min over B
// of b.n (the support distance of the Minkowski difference A - B).
static f32 support_depth(const NphPhysicsProxy *pa, NphPhysicsPose ta, const NphPhysicsProxy *pb,
                         NphPhysicsPose tb, NphVec3 n)
{
    f32 hi = -1e30F;
    f32 lo = 1e30F;
    for (u32 i = 0U; i < pa->count; ++i) {
        hi = nph_f32_max(hi, nph_vec3_dot(nph_physics_pose_transform_point(ta, pa->points[i]), n));
    }
    for (u32 i = 0U; i < pb->count; ++i) {
        lo = nph_f32_min(lo, nph_vec3_dot(nph_physics_pose_transform_point(tb, pb->points[i]), n));
    }
    return hi - lo + pa->radius + pb->radius;
}

// EPA finds the minimum translation, and the axis test's normal separates
// with at most its face preference (half a slop) more; each method's depth
// is the separating translation along its own normal. The manifold's
// deepest point carries the axis depth for shallow contacts; deep ones clip
// the deepest corner away when it lies beside the reference face, which the
// solver resolves as the contact becomes shallow.
NTEST(gjk, epa_depth_matches_the_axis_test)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(13U);
    u32 checked = 0U;
    for (u32 k = 0U; k < 40U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry ga = nph_physics_geometry_box(test_vec3(&rng, 0.3F, 1.5F));
        NphPhysicsHull *hb = test_random_hull(alloc, &rng, 30U, 1.0F, test_vec3(&rng, 0.5F, 1.5F));
        const NphPhysicsGeometry gb = nph_physics_geometry_hull(hb);
        const NphPhysicsPose ta_pose = nph_physics_pose(nph_vec3_zero(), test_rotation(&rng));
        const NphPhysicsPose tb_pose = nph_physics_pose(
            nph_vec3_scale(test_unit(&rng), test_range(&rng, 0.2F, 2.4F)), test_rotation(&rng));
        NphVec3 s1[8];
        NphVec3 s2[8];
        const NphPhysicsProxy pa = proxy_of(&ga, s1);
        const NphPhysicsProxy pb = proxy_of(&gb, s2);
        NphPhysicsPenetration pen;
        NphPhysicsManifold m;
        nph_physics_collide(&ga, ta_pose, &gb, tb_pose, 0.0F, NULL, &m);
        if (nph_physics_penetration(&pa, ta_pose, &pb, tb_pose, &pen) && m.point_count > 0U &&
            pen.depth > 0.01F) {
            const f32 tol = 2e-3F + 2e-3F * pen.depth;
            const f32 axis = support_depth(&pa, ta_pose, &pb, tb_pose, m.normal);
            NTEST_EXPECT_NEAR_F32(support_depth(&pa, ta_pose, &pb, tb_pose, pen.normal), pen.depth,
                                  tol);
            NTEST_EXPECT_TRUE(axis >= pen.depth - tol);
            NTEST_EXPECT_TRUE(axis <= pen.depth + 0.5F * NPH_PHYSICS_LINEAR_SLOP + tol);
            const f32 deepest = -test_min_separation(&m);
            NTEST_EXPECT_TRUE(deepest <= axis + tol);
            if (pen.depth < 0.1F) {
                NTEST_EXPECT_NEAR_F32(deepest, axis, tol);
            }
            checked += 1U;
        }
        nph_physics_hull_free(hb, alloc);
    }
    NTEST_EXPECT_TRUE(checked >= 10U * TEST_SCALE);
    test_alloc_end(&ta);
}

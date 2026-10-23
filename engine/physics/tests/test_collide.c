// SPDX-License-Identifier: Apache-2.0
// Contact manifolds: every pair of shape types on known configurations,
// the box view against the box hull, symmetry of the argument order,
// geometric properties on random hull pairs, persistence of point ids, the
// speculative margin, and the helpers (segment distance, reduction).

#include "physics_internal.h"
#include "test_physics_support.h"

static const f32 k_spec = NPH_PHYSICS_SPECULATIVE_DISTANCE;

static NphPhysicsPose at(f32 x, f32 y, f32 z)
{
    return nph_physics_pose(nph_vec3(x, y, z), nph_quat_identity());
}

static NphPhysicsManifold collide(const NphPhysicsGeometry *a, NphPhysicsPose pa,
                                  const NphPhysicsGeometry *b, NphPhysicsPose pb)
{
    NphPhysicsManifold m;
    nph_physics_collide(a, pa, b, pb, k_spec, NULL, &m);
    test_check_manifold(&m, k_spec);
    return m;
}

NTEST(collide, box_view_matches_the_box_hull)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const NphVec3 h = nph_vec3(0.3F, 0.7F, 1.1F);
    NphPhysicsHull *hull = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(h, alloc, &hull), NPH_RESULT_OK);
    NphBoxViewStorage storage;
    const NphHullView v = nph_physics_box_view(h, &storage);
    NTEST_ASSERT_EQ_UINT(v.vertex_count, hull->vertex_count);
    NTEST_ASSERT_EQ_UINT(v.face_count, hull->face_count);
    NTEST_ASSERT_EQ_UINT(v.edge_count, hull->edge_count);
    NTEST_EXPECT_EQ_MEM(v.vertices, nph_physics_hull_vertices(hull), 8U * sizeof(NphVec3));
    NTEST_EXPECT_EQ_MEM(v.faces, nph_physics_hull_faces(hull), 6U * sizeof(u16));
    NTEST_EXPECT_EQ_MEM(v.edges, nph_physics_hull_edges(hull), 24U * sizeof(NphPhysicsHullEdge));
    for (u32 f = 0U; f < 6U; ++f) {
        NTEST_EXPECT_TRUE(nph_vec3_eq(v.planes[f].normal, nph_physics_hull_planes(hull)[f].normal));
        NTEST_EXPECT_NEAR_F32(v.planes[f].d, nph_physics_hull_planes(hull)[f].d, 1e-6F);
    }
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(collide, spheres)
{
    const NphPhysicsGeometry a = nph_physics_geometry_sphere(0.5F);
    const NphPhysicsGeometry b = nph_physics_geometry_sphere(0.3F);
    NphPhysicsManifold m = collide(&a, at(0.0F, 0.0F, 0.0F), &b, at(0.7F, 0.0F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.1F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_x(), 1e-6F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.points[0].point, nph_vec3(0.45F, 0.0F, 0.0F), 1e-6F));
    // Within the speculative margin: a point with a positive separation.
    m = collide(&a, at(0.0F, 0.0F, 0.0F), &b, at(0.0F, 0.81F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, 0.01F, 1e-6F);
    // Beyond it: nothing.
    m = collide(&a, at(0.0F, 0.0F, 0.0F), &b, at(0.0F, 0.0F, 0.85F));
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // Concentric: a normal anyway.
    m = collide(&a, at(1.0F, 1.0F, 1.0F), &b, at(1.0F, 1.0F, 1.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.8F, 1e-6F);
}

NTEST(collide, capsules)
{
    const NphPhysicsGeometry cap = nph_physics_geometry_capsule(1.0F, 0.25F);
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.25F);
    // A ball against the side and against the cap.
    NphPhysicsManifold m = collide(&cap, at(0.0F, 0.0F, 0.0F), &ball, at(0.45F, 0.3F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.05F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_x(), 1e-5F));
    m = collide(&cap, at(0.0F, 0.0F, 0.0F), &ball, at(0.0F, 1.45F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-5F));
    // Two capsules lying side by side (along Z, both turned the same way):
    // two points at the ends of the overlap.
    const NphQuat lying = nph_quat_from_rotation_x(1.5707963F);
    const NphPhysicsPose pa = nph_physics_pose(nph_vec3_zero(), lying);
    const NphPhysicsPose pb = nph_physics_pose(nph_vec3(0.0F, 0.49F, 0.5F), lying);
    m = collide(&cap, pa, &cap, pb);
    NTEST_ASSERT_EQ_UINT(m.point_count, 2U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-4F));
    for (u32 i = 0U; i < 2U; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, -0.01F, 1e-4F);
    }
    const f32 z0 = m.points[0].point.z;
    const f32 z1 = m.points[1].point.z;
    NTEST_EXPECT_NEAR_F32(nph_f32_min(z0, z1), -0.5F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(nph_f32_max(z0, z1), 1.0F, 1e-4F);
    // Crossed at right angles: one point.
    const NphPhysicsPose pc =
        nph_physics_pose(nph_vec3(0.0F, 0.0F, 0.4F), nph_quat_from_rotation_z(1.5707963F));
    m = collide(&cap, at(0.0F, 0.0F, 0.0F), &cap, pc);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.1F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_z(), 1e-5F));
}

NTEST(collide, box_and_sphere)
{
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(1.0F, 0.5F, 2.0F));
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.25F);
    // Face, edge, corner, and inside.
    NphPhysicsManifold m = collide(&box, at(0.0F, 0.0F, 0.0F), &ball, at(0.2F, 0.7F, 0.3F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.05F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-5F));
    m = collide(&box, at(0.0F, 0.0F, 0.0F), &ball, at(1.1F, 0.6F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, nph_f32_sqrt(0.02F) - 0.25F, 1e-5F);
    m = collide(&box, at(0.0F, 0.0F, 0.0F), &ball, at(1.1F, 0.6F, 2.1F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, nph_f32_sqrt(0.03F) - 0.25F, 1e-5F);
    m = collide(&box, at(0.0F, 0.0F, 0.0F), &ball, at(0.0F, 0.4F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.35F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-5F));
}

// The box path (box views, the closed-form sphere) and the hull path (the
// same box as a hull) agree.
NTEST(collide, boxes_agree_with_box_hulls)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(17U);
    for (u32 k = 0U; k < 100U * TEST_SCALE; ++k) {
        const NphVec3 ha = test_vec3(&rng, 0.2F, 1.0F);
        const NphVec3 hb = test_vec3(&rng, 0.2F, 1.0F);
        NphPhysicsHull *hull_a = NULL;
        NphPhysicsHull *hull_b = NULL;
        NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(ha, alloc, &hull_a), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(hb, alloc, &hull_b), NPH_RESULT_OK);
        const NphPhysicsGeometry ba = nph_physics_geometry_box(ha);
        const NphPhysicsGeometry bb = nph_physics_geometry_box(hb);
        const NphPhysicsGeometry ga = nph_physics_geometry_hull(hull_a);
        const NphPhysicsGeometry gb = nph_physics_geometry_hull(hull_b);
        const NphPhysicsPose pa = test_pose(&rng, 0.2F);
        const NphPhysicsPose pb = nph_physics_pose(
            nph_vec3_scale(test_unit(&rng), test_range(&rng, 0.3F, 2.0F)), test_rotation(&rng));
        const NphPhysicsManifold m1 = collide(&ba, pa, &bb, pb);
        const NphPhysicsManifold m2 = collide(&ga, pa, &gb, pb);
        NTEST_ASSERT_EQ_UINT(m1.point_count, m2.point_count);
        if (m1.point_count > 0U) {
            NTEST_EXPECT_TRUE(nph_vec3_near(m1.normal, m2.normal, 1e-5F));
            for (u32 i = 0U; i < m1.point_count; ++i) {
                NTEST_EXPECT_TRUE(nph_vec3_near(m1.points[i].point, m2.points[i].point, 1e-4F));
                NTEST_EXPECT_NEAR_F32(m1.points[i].separation, m2.points[i].separation, 1e-5F);
            }
        }
        // A sphere: the closed form against the hull path.
        const NphPhysicsGeometry ball = nph_physics_geometry_sphere(test_range(&rng, 0.1F, 0.6F));
        const NphPhysicsManifold s1 = collide(&ba, pa, &ball, pb);
        const NphPhysicsManifold s2 = collide(&ga, pa, &ball, pb);
        NTEST_ASSERT_EQ_UINT(s1.point_count, s2.point_count);
        if (s1.point_count > 0U) {
            NTEST_EXPECT_NEAR_F32(s1.points[0].separation, s2.points[0].separation, 1e-4F);
            NTEST_EXPECT_TRUE(nph_vec3_dot(s1.normal, s2.normal) > 0.999F ||
                              s1.points[0].separation < -0.05F);
        }
        nph_physics_hull_free(hull_a, alloc);
        nph_physics_hull_free(hull_b, alloc);
    }
    test_alloc_end(&ta);
}

NTEST(collide, box_resting_on_box)
{
    const NphPhysicsGeometry ground = nph_physics_geometry_box(nph_vec3(5.0F, 0.5F, 5.0F));
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    // 1 cm into the ground: four corners at -0.01.
    NphPhysicsManifold m = collide(&ground, at(0.0F, 0.0F, 0.0F), &box, at(0.3F, 0.99F, -0.2F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 4U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-6F));
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, -0.01F, 1e-5F);
        NTEST_EXPECT_NEAR_F32(m.points[i].point.y, 0.495F, 1e-5F);
        NTEST_EXPECT_NEAR_F32(nph_f32_abs(m.points[i].point.x - 0.3F), 0.5F, 1e-5F);
        NTEST_EXPECT_NEAR_F32(nph_f32_abs(m.points[i].point.z + 0.2F), 0.5F, 1e-5F);
    }
    // Turned 45 degrees about Y: still the four corners.
    m = collide(&ground, at(0.0F, 0.0F, 0.0F), &box,
                nph_physics_pose(nph_vec3(0.0F, 0.99F, 0.0F), nph_quat_from_rotation_y(0.785398F)));
    NTEST_ASSERT_EQ_UINT(m.point_count, 4U);
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_NEAR_F32(
            nph_vec3_length(nph_vec3(m.points[i].point.x, 0.0F, m.points[i].point.z)),
            nph_f32_sqrt(0.5F), 1e-4F);
    }
    // The small box under the ground (B's face is the reference): the
    // normal still points from A to B.
    m = collide(&box, at(0.3F, 0.99F, -0.2F), &ground, at(0.0F, 0.0F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 4U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_neg(nph_vec3_unit_y()), 1e-6F));
    // A small box on its edge across the big one's face: two points.
    m = collide(&ground, at(0.0F, 0.0F, 0.0F), &box,
                nph_physics_pose(nph_vec3(0.0F, 0.5F + 0.7071F - 0.01F, 0.0F),
                                 nph_quat_from_rotation_z(0.785398F)));
    NTEST_ASSERT_EQ_UINT(m.point_count, 2U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-5F));
}

NTEST(collide, edges_and_corners)
{
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    // Edge on edge: A's top edge along Z (turned 45 degrees about Z) under
    // B's bottom edge along X (turned about X): one point.
    const NphPhysicsPose pa =
        nph_physics_pose(nph_vec3_zero(), nph_quat_from_rotation_z(0.785398F));
    const f32 h = 0.70710678F;
    const NphPhysicsPose pb = nph_physics_pose(nph_vec3(0.0F, 2.0F * h - 0.02F, 0.0F),
                                               nph_quat_from_rotation_x(0.785398F));
    NphPhysicsManifold m = collide(&box, pa, &box, pb);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.02F, 1e-4F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-4F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.points[0].point, nph_vec3(0.0F, h - 0.01F, 0.0F), 1e-4F));
    // Corner to corner, 1.5 cm apart: GJK's single point with the true
    // normal, not a face's (whose axis says less).
    const NphQuat corner_up =
        nph_quat_from_to(nph_vec3_normalize(nph_vec3_one()), nph_vec3_unit_y());
    const f32 half_diag = 0.8660254F;
    const NphPhysicsPose pc = nph_physics_pose(nph_vec3_zero(), corner_up);
    const NphPhysicsPose pd =
        nph_physics_pose(nph_vec3(0.0F, 2.0F * half_diag + 0.015F, 0.0F),
                         nph_quat_mul(nph_quat_from_rotation_y(0.5F), corner_up));
    m = collide(&box, pc, &box, pd);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, 0.015F, 1e-4F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-3F));
}

NTEST(collide, argument_order_flips_the_normal)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(23U);
    NphPhysicsHull *hull = test_random_hull(alloc, &rng, 40U, 0.8F, nph_vec3(1.0F, 0.6F, 1.2F));
    const NphPhysicsGeometry shapes[4] = {
        nph_physics_geometry_sphere(0.4F), nph_physics_geometry_capsule(0.5F, 0.3F),
        nph_physics_geometry_box(nph_vec3(0.4F, 0.6F, 0.5F)), nph_physics_geometry_hull(hull)};
    for (u32 k = 0U; k < 200U; ++k) {
        const u32 i = nph_pcg32_next_bounded(&rng, 4U);
        const u32 j = nph_pcg32_next_bounded(&rng, 4U);
        const NphPhysicsPose pa = test_pose(&rng, 0.3F);
        const NphPhysicsPose pb = nph_physics_pose(
            nph_vec3_scale(test_unit(&rng), test_range(&rng, 0.2F, 1.6F)), test_rotation(&rng));
        const NphPhysicsManifold ab = collide(&shapes[i], pa, &shapes[j], pb);
        const NphPhysicsManifold ba = collide(&shapes[j], pb, &shapes[i], pa);
        NTEST_ASSERT_EQ_UINT(ab.point_count, ba.point_count);
        if (ab.point_count > 0U && i != j) {
            NTEST_EXPECT_TRUE(nph_vec3_near(ab.normal, nph_vec3_neg(ba.normal), 1e-6F));
            for (u32 p = 0U; p < ab.point_count; ++p) {
                NTEST_EXPECT_TRUE(nph_vec3_near(ab.points[p].point, ba.points[p].point, 1e-6F));
            }
        }
    }
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

// For each point, the two surface points (half the separation either way
// along the normal) lie on or inside the two hulls, and the deeper one of
// them on the reference surface.
NTEST(collide, random_hull_pairs_are_geometrically_sound)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(29U);
    u32 contacts = 0U;
    for (u32 k = 0U; k < 60U * TEST_SCALE; ++k) {
        NphPhysicsHull *ha = test_random_hull(alloc, &rng, 32U, 1.0F, test_vec3(&rng, 0.4F, 1.4F));
        NphPhysicsHull *hb = test_random_hull(alloc, &rng, 32U, 1.0F, test_vec3(&rng, 0.4F, 1.4F));
        const NphPhysicsGeometry ga = nph_physics_geometry_hull(ha);
        const NphPhysicsGeometry gb = nph_physics_geometry_hull(hb);
        const NphPhysicsPose pa = test_pose(&rng, 0.1F);
        const NphPhysicsPose pb = nph_physics_pose(
            nph_vec3_scale(test_unit(&rng), test_range(&rng, 0.5F, 2.2F)), test_rotation(&rng));
        const NphPhysicsManifold m = collide(&ga, pa, &gb, pb);
        for (u32 i = 0U; i < m.point_count; ++i) {
            const NphVec3 p = m.points[i].point;
            const f32 s = m.points[i].separation;
            const NphVec3 on_a = nph_vec3_mul_add(p, m.normal, -0.5F * s);
            const NphVec3 on_b = nph_vec3_mul_add(p, m.normal, 0.5F * s);
            const f32 tol = 2e-3F;
            NTEST_EXPECT_TRUE(test_hull_plane_distance(
                                  ha, nph_physics_pose_inverse_transform_point(pa, on_a)) <= tol);
            NTEST_EXPECT_TRUE(test_hull_plane_distance(
                                  hb, nph_physics_pose_inverse_transform_point(pb, on_b)) <= tol);
            contacts += 1U;
        }
        nph_physics_hull_free(ha, alloc);
        nph_physics_hull_free(hb, alloc);
    }
    NTEST_EXPECT_TRUE(contacts > 20U * TEST_SCALE);
    test_alloc_end(&ta);
}

NTEST(collide, capsule_and_box)
{
    const NphPhysicsGeometry ground = nph_physics_geometry_box(nph_vec3(3.0F, 0.5F, 3.0F));
    const NphPhysicsGeometry cap = nph_physics_geometry_capsule(0.6F, 0.2F);
    // Lying on the top face: two points under the core's ends.
    const NphPhysicsPose lying =
        nph_physics_pose(nph_vec3(0.4F, 0.69F, 0.0F), nph_quat_from_rotation_z(1.5707963F));
    NphPhysicsManifold m = collide(&ground, at(0.0F, 0.0F, 0.0F), &cap, lying);
    NTEST_ASSERT_EQ_UINT(m.point_count, 2U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-5F));
    for (u32 i = 0U; i < 2U; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, -0.01F, 1e-5F);
    }
    // Standing on it: one point under the lower cap.
    m = collide(&ground, at(0.0F, 0.0F, 0.0F), &cap, at(0.0F, 1.29F, 0.0F));
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.01F, 1e-5F);
    // Across a top edge of a small box, tilted: an edge contact.
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    const NphPhysicsPose across =
        nph_physics_pose(nph_vec3(0.5F + 0.12F, 0.5F + 0.12F, 0.0F),
                         nph_quat_mul(nph_quat_from_rotation_x(1.5707963F), nph_quat_identity()));
    m = collide(&box, at(0.0F, 0.0F, 0.0F), &cap, across);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    const NphVec3 diag = nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, diag, 1e-3F));
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, 0.12F * 1.4142136F - 0.2F, 1e-4F);
    // Deep inside: still a manifold, pushing out through the nearest face.
    m = collide(&ground, at(0.0F, 0.0F, 0.0F), &cap,
                nph_physics_pose(nph_vec3(0.0F, 0.3F, 0.0F), nph_quat_from_rotation_z(1.5707963F)));
    NTEST_ASSERT_TRUE(m.point_count >= 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-5F));
}

NTEST(collide, point_ids_persist_under_small_motion)
{
    const NphPhysicsGeometry ground = nph_physics_geometry_box(nph_vec3(5.0F, 0.5F, 5.0F));
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.3F, 0.4F));
    const NphPhysicsManifold m0 =
        collide(&ground, at(0.0F, 0.0F, 0.0F), &box,
                nph_physics_pose(nph_vec3(0.1F, 0.79F, 0.2F), nph_quat_from_rotation_y(0.2F)));
    const NphPhysicsManifold m1 =
        collide(&ground, at(0.0F, 0.0F, 0.0F), &box,
                nph_physics_pose(nph_vec3(0.101F, 0.7905F, 0.199F),
                                 nph_quat_mul(nph_quat_from_rotation_y(0.202F),
                                              nph_quat_from_rotation_x(0.001F))));
    NTEST_ASSERT_EQ_UINT(m0.point_count, 4U);
    NTEST_ASSERT_EQ_UINT(m1.point_count, 4U);
    for (u32 i = 0U; i < 4U; ++i) {
        bool found = false;
        for (u32 j = 0U; j < 4U; ++j) {
            found = found || m0.points[i].id == m1.points[j].id;
        }
        NTEST_EXPECT_TRUE(found);
    }
}

NTEST(collide, the_cache_ends_separated_pairs_early)
{
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    NphPhysicsCollideCache cache;
    memset(&cache, 0, sizeof cache);
    NphPhysicsManifold m;
    nph_physics_collide(&box, at(0.0F, 0.0F, 0.0F), &box, at(0.0F, 1.5F, 0.0F), k_spec, &cache, &m);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    NTEST_EXPECT_NE_UINT(cache.type, 0U);
    // Still apart: still nothing, with the cached axis.
    nph_physics_collide(&box, at(0.0F, 0.0F, 0.0F), &box, at(0.01F, 1.49F, 0.0F), k_spec, &cache,
                        &m);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // Touching now: the cache must not hide the contact.
    nph_physics_collide(&box, at(0.0F, 0.0F, 0.0F), &box, at(0.01F, 0.99F, 0.0F), k_spec, &cache,
                        &m);
    NTEST_EXPECT_EQ_UINT(m.point_count, 4U);
}

NTEST(collide, segment_distance_matches_sampling)
{
    NphPcg32 rng = test_rng(31U);
    for (u32 k = 0U; k < 200U; ++k) {
        const NphVec3 p1 = test_vec3(&rng, -1.0F, 1.0F);
        const NphVec3 q1 = test_vec3(&rng, -1.0F, 1.0F);
        const NphVec3 p2 = test_vec3(&rng, -1.0F, 1.0F);
        const NphVec3 q2 = k % 10U == 0U ? p2 : test_vec3(&rng, -1.0F, 1.0F);
        const NphPhysicsSegmentDistance d = nph_physics_segment_distance(p1, q1, p2, q2);
        f32 best = 1e30F;
        for (u32 i = 0U; i <= 64U; ++i) {
            const NphVec3 a = nph_vec3_lerp(p1, q1, (f32)i / 64.0F);
            f32 t;
            const NphVec3 b = nph_closest_point_on_segment(p2, q2, a, &t);
            best = nph_f32_min(best, nph_vec3_distance_sq(a, b));
        }
        NTEST_EXPECT_TRUE(d.distance_squared <= best + 1e-6F);
        NTEST_EXPECT_TRUE(d.distance_squared >= 0.0F);
        NTEST_EXPECT_NEAR_F32(d.distance_squared, nph_vec3_distance_sq(d.closest1, d.closest2),
                              1e-6F);
    }
}

NTEST(collide, reduction_keeps_the_largest_area)
{
    // Eight points around a square (corners and edge midpoints), the
    // deepest at a corner: the four corners remain.
    NphPhysicsManifoldPoint pts[8];
    const f32 xy[8][2] = {{1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
    for (u32 i = 0U; i < 8U; ++i) {
        pts[i].point = nph_vec3(xy[i][0], 0.0F, xy[i][1]);
        pts[i].separation = i == 4U ? -0.02F : -0.01F;
        pts[i].id = i;
    }
    const u32 n = nph_physics_manifold_reduce(pts, 8U, nph_vec3_unit_y());
    NTEST_ASSERT_EQ_UINT(n, 4U);
    NTEST_EXPECT_EQ_UINT(pts[0].id, 4U); // the deepest first
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_TRUE(pts[i].id == 0U || pts[i].id == 2U || pts[i].id == 4U || pts[i].id == 6U);
    }
    // Four or fewer: unchanged.
    NTEST_EXPECT_EQ_UINT(nph_physics_manifold_reduce(pts, 3U, nph_vec3_unit_y()), 3U);
}

// Hulls beyond the vectorised test's 64 vertices take the scalar path:
// the same soundness on 100-vertex hulls.
NTEST(collide, large_hulls_take_the_scalar_path)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(37U);
    static NphVec3 pts[400];
    NphPhysicsHull *hulls[2];
    for (u32 h = 0U; h < 2U; ++h) {
        for (u32 i = 0U; i < 400U; ++i) {
            pts[i] = nph_vec3_mul(test_unit(&rng), nph_vec3(1.0F, 0.7F, 1.2F));
        }
        hulls[h] = test_hull(alloc, pts, 400U, 100U);
        NTEST_ASSERT_TRUE(hulls[h]->vertex_count > 64U);
    }
    const NphPhysicsGeometry ga = nph_physics_geometry_hull(hulls[0]);
    const NphPhysicsGeometry gb = nph_physics_geometry_hull(hulls[1]);
    u32 contacts = 0U;
    for (u32 k = 0U; k < 10U * TEST_SCALE; ++k) {
        const NphPhysicsPose pa = test_pose(&rng, 0.1F);
        const NphPhysicsPose pb = nph_physics_pose(
            nph_vec3_scale(test_unit(&rng), test_range(&rng, 1.0F, 2.2F)), test_rotation(&rng));
        const NphPhysicsManifold m = collide(&ga, pa, &gb, pb);
        for (u32 i = 0U; i < m.point_count; ++i) {
            const NphVec3 p = m.points[i].point;
            const f32 s = m.points[i].separation;
            NTEST_EXPECT_TRUE(test_hull_plane_distance(
                                  hulls[0], nph_physics_pose_inverse_transform_point(
                                                pa, nph_vec3_mul_add(p, m.normal, -0.5F * s))) <=
                              2e-3F);
            NTEST_EXPECT_TRUE(test_hull_plane_distance(
                                  hulls[1], nph_physics_pose_inverse_transform_point(
                                                pb, nph_vec3_mul_add(p, m.normal, 0.5F * s))) <=
                              2e-3F);
            contacts += 1U;
        }
    }
    NTEST_EXPECT_TRUE(contacts > 0U);
    nph_physics_hull_free(hulls[0], alloc);
    nph_physics_hull_free(hulls[1], alloc);
    test_alloc_end(&ta);
}

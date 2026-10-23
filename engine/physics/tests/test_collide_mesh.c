// SPDX-License-Identifier: Apache-2.0
// Convex shapes against triangles and meshes (collide_mesh.c): the triangle
// view's axis test against brute force, the closed forms of spheres and
// capsules, boxes and hulls on a triangle, the triangle's rules (its side,
// active edges, active vertices), and meshes: flat floors with no sideways
// normal anywhere, a manifold per face in a crease, the deepest four of
// many, open edges, two-sided sheets, and full precision far from a mesh's
// origin.

#include "physics_internal.h"
#include "test_physics_support.h"

static const f32 k_spec = NPH_PHYSICS_SPECULATIVE_DISTANCE;
static const NphVec3 k_down = {0.0F, -1.0F, 0.0F};

static NphPhysicsPose at(f32 x, f32 y, f32 z)
{
    return nph_physics_pose(nph_vec3(x, y, z), nph_quat_identity());
}

static NphQuat yaw(f32 angle)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_y(), angle);
}

// A capsule's long axis (Y) laid along X.
static NphQuat lying(void)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_z(), 1.5707963F);
}

// The single-triangle tests' floor: facing +Y, the right angle at
// v0 = (-1, 0, -1), the legs along +Z (edge 0, on x = -1) and +X (edge 2, on
// z = -1), the hypotenuse x + z = 1 (edge 1). Vertices 1 and 2 are acute.
static NphPhysicsTriangle floor_triangle(u32 active, bool two_sided)
{
    NphPhysicsTriangle t;
    t.vertices[0] = nph_vec3(-1.0F, 0.0F, -1.0F);
    t.vertices[1] = nph_vec3(-1.0F, 0.0F, 2.0F);
    t.vertices[2] = nph_vec3(2.0F, 0.0F, -1.0F);
    t.active_edges = active;
    t.two_sided = two_sided ? 1 : 0;
    return t;
}

static NphPhysicsManifold collide_tri(const NphPhysicsGeometry *g, NphPhysicsPose pose,
                                      const NphPhysicsTriangle *t)
{
    NphPhysicsManifold m;
    nph_physics_collide_triangle(g, pose, t, k_spec, &m);
    test_check_manifold(&m, k_spec);
    return m;
}

static NphPhysicsMeshManifolds collide_mesh(const NphPhysicsGeometry *g, NphPhysicsPose pose,
                                            const NphPhysicsMesh *mesh, NphPhysicsPose mesh_pose)
{
    NphPhysicsMeshManifolds mm;
    nph_physics_collide_mesh(g, pose, mesh, mesh_pose, k_spec, &mm);
    NTEST_ASSERT_TRUE(mm.count <= NPH_PHYSICS_MESH_MAX_MANIFOLDS);
    for (u32 i = 0U; i < mm.count; ++i) {
        test_check_manifold(&mm.manifolds[i], k_spec);
        NTEST_EXPECT_TRUE(mm.manifolds[i].point_count > 0U);
        if (i > 0U) {
            NTEST_EXPECT_TRUE(test_min_separation(&mm.manifolds[i - 1U]) <=
                              test_min_separation(&mm.manifolds[i]));
        }
    }
    return mm;
}

// ---------------------------------------------------------------------------
// The triangle view's axis test
// ---------------------------------------------------------------------------

// How far the triangle lies beyond the hull's vertices along unit l.
static f32 separation_along(const NphVec3 *av, u32 count, const NphVec3 tv[3], NphVec3 l)
{
    f32 a_max = -1e30F;
    for (u32 i = 0U; i < count; ++i) {
        a_max = nph_f32_max(a_max, nph_vec3_dot(l, av[i]));
    }
    f32 b_min = 1e30F;
    for (u32 i = 0U; i < 3U; ++i) {
        b_min = nph_f32_min(b_min, nph_vec3_dot(l, tv[i]));
    }
    return b_min - a_max;
}

// The largest separation over every axis the test could use: the hull's
// face normals, the triangle's both ways, and every cross product of an
// edge of each, both ways (skipping nearly parallel pairs, as the test
// does). With no Gauss-map pruning there is nothing to get wrong, and when
// the shapes overlap it is exact: the penetration depth, negated.
static f32 brute_separation(const NphHullView *a, NphPhysicsPose pa, const NphVec3 tv[3])
{
    static NphVec3 av[NPH_PHYSICS_HULL_MAX_VERTICES];
    for (u32 i = 0U; i < a->vertex_count; ++i) {
        av[i] = nph_physics_pose_transform_point(pa, a->vertices[i]);
    }
    const u32 n = a->vertex_count;
    f32 best = -1e30F;
    for (u32 f = 0U; f < a->face_count; ++f) {
        const NphVec3 l = nph_quat_rotate(pa.rotation, a->planes[f].normal);
        best = nph_f32_max(best, separation_along(av, n, tv, l));
    }
    const NphVec3 tn =
        nph_vec3_normalize(nph_vec3_cross(nph_vec3_sub(tv[1], tv[0]), nph_vec3_sub(tv[2], tv[0])));
    best = nph_f32_max(best, separation_along(av, n, tv, tn));
    best = nph_f32_max(best, separation_along(av, n, tv, nph_vec3_neg(tn)));
    for (u32 e = 0U; e < a->edge_count; e += 2U) {
        const NphVec3 ua =
            nph_vec3_sub(av[a->edges[a->edges[e].next].origin], av[a->edges[e].origin]);
        for (u32 k = 0U; k < 3U; ++k) {
            const NphVec3 ub = nph_vec3_sub(tv[(k + 1U) % 3U], tv[k]);
            const NphVec3 x = nph_vec3_cross(ua, ub);
            const f32 len = nph_vec3_length(x);
            if (len < 0.005F * nph_f32_sqrt(nph_vec3_length_sq(ua) * nph_vec3_length_sq(ub))) {
                continue;
            }
            const NphVec3 l = nph_vec3_scale(x, 1.0F / len);
            best = nph_f32_max(best, separation_along(av, n, tv, l));
            best = nph_f32_max(best, separation_along(av, n, tv, nph_vec3_neg(l)));
        }
    }
    return best;
}

NTEST(collide_mesh, triangle_view_axis_test_matches_brute_force)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(71U);
    // A hull too big for the vectorised path, for the scalar one.
    static NphVec3 pts[300];
    for (u32 i = 0U; i < 300U; ++i) {
        pts[i] = nph_vec3_mul(test_unit(&rng), nph_vec3(0.6F, 0.4F, 0.5F));
    }
    NphPhysicsHull *big = test_hull(alloc, pts, 300U, 100U);
    NTEST_ASSERT_TRUE(big->vertex_count > 64U);
    u32 overlapping = 0U;
    const u32 trials = 300U * TEST_SCALE;
    for (u32 trial = 0U; trial < trials; ++trial) {
        NphPhysicsHull *hull = NULL;
        NphBoxViewStorage box;
        NphHullView va;
        if (trial % 7U == 0U) {
            va = nph_physics_hull_view(big);
        } else if (trial % 3U == 0U) {
            va = nph_physics_box_view(test_vec3(&rng, 0.1F, 0.6F), &box);
        } else {
            hull =
                test_random_hull(alloc, &rng, 6U + trial % 20U, 0.5F, nph_vec3(1.0F, 0.6F, 0.9F));
            va = nph_physics_hull_view(hull);
        }
        NphVec3 tv[3];
        for (u32 i = 0U; i < 3U; ++i) {
            tv[i] = test_vec3(&rng, -1.0F, 1.0F);
        }
        const NphVec3 c = nph_vec3_cross(nph_vec3_sub(tv[1], tv[0]), nph_vec3_sub(tv[2], tv[0]));
        if (nph_vec3_length_sq(c) > 1e-2F) {
            NphTriangleViewStorage ts;
            const NphHullView vb = nph_physics_triangle_view(tv, nph_vec3_normalize(c), &ts);
            const NphPhysicsPose pa = test_pose(&rng, 0.8F);
            const f32 sat = nph_physics_hulls_separation(&va, pa, &vb, nph_physics_pose_identity());
            const f32 brute = brute_separation(&va, pa, tv);
            if (brute < -1e-4F) {
                NTEST_EXPECT_NEAR_F32(sat, brute, 1e-4F);
                overlapping += 1U;
            } else {
                // Apart, both bound the gap from below; the test's axes are
                // a subset.
                NTEST_EXPECT_TRUE(sat <= brute + 1e-5F);
            }
        }
        if (hull != NULL) {
            nph_physics_hull_free(hull, alloc);
        }
    }
    NTEST_EXPECT_TRUE(overlapping > trials / 8U);
    nph_physics_hull_free(big, alloc);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// One triangle
// ---------------------------------------------------------------------------

NTEST(collide_mesh, spheres_against_a_triangle)
{
    const NphPhysicsTriangle t = floor_triangle(NPH_PHYSICS_MESH_EDGES, false);
    const NphPhysicsGeometry s = nph_physics_geometry_sphere(0.5F);
    // The face: straight down, the point midway between the surfaces.
    NphPhysicsManifold m = collide_tri(&s, at(0.0F, 0.4F, 0.0F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.1F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.points[0].point, nph_vec3(0.0F, -0.05F, 0.0F), 1e-6F));
    // Within the speculative distance, and beyond it.
    m = collide_tri(&s, at(0.0F, 0.51F, 0.0F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, 0.01F, 1e-6F);
    m = collide_tri(&s, at(0.0F, 0.53F, 0.0F), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // Beside edge 0 (active): from the edge.
    m = collide_tri(&s, at(-1.3F, 0.3F, 0.0F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, nph_f32_sqrt(0.18F) - 0.5F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3(0.70710678F, -0.70710678F, 0.0F), 1e-5F));
    // Past vertex 0: from the corner.
    m = collide_tri(&s, at(-1.2F, 0.3F, -1.2F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    const NphVec3 d = nph_vec3(-0.2F, 0.3F, -0.2F);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, nph_vec3_length(d) - 0.5F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_neg(nph_vec3_normalize(d)), 1e-5F));
    // Behind a one-sided triangle: nothing. A two-sided one pushes it down.
    m = collide_tri(&s, at(0.0F, -0.4F, 0.0F), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    const NphPhysicsTriangle sheet = floor_triangle(NPH_PHYSICS_MESH_EDGES, true);
    m = collide_tri(&s, at(0.0F, -0.4F, 0.0F), &sheet);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.1F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_y(), 1e-6F));
    // A degenerate triangle touches nothing.
    NphPhysicsTriangle flat = t;
    flat.vertices[2] = nph_vec3(-1.0F, 0.0F, 0.5F);
    m = collide_tri(&s, at(-1.0F, 0.0F, 0.0F), &flat);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
}

NTEST(collide_mesh, inactive_edges_and_vertices_leave_contacts_to_neighbours)
{
    const NphPhysicsGeometry s = nph_physics_geometry_sphere(0.5F);
    // Beside edge 0, now inactive (a neighbour continues the surface
    // across it): nothing, the neighbour carries the sphere.
    NphPhysicsTriangle t = floor_triangle(NPH_PHYSICS_MESH_EDGE_1 | NPH_PHYSICS_MESH_EDGE_2, false);
    NphPhysicsManifold m = collide_tri(&s, at(-1.3F, 0.3F, 0.0F), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // Over the seam itself, both sides carry it: straight down, the point
    // under the centre.
    m = collide_tri(&s, at(-1.0F, 0.45F, 0.3F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.05F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.points[0].point, nph_vec3(-1.0F, -0.025F, 0.3F), 1e-6F));
    // Past acute vertex 2, leaning across edge 1 only: edge 1's contact.
    // With only edge 2 active it is the neighbour's across edge 1...
    const NphVec3 c = nph_vec3(2.3F, 0.25F, -0.73F);
    const NphVec3 off = nph_vec3_sub(c, nph_vec3(2.0F, 0.0F, -1.0F));
    t = floor_triangle(NPH_PHYSICS_MESH_EDGE_2, false);
    m = collide_tri(&s, at(c.x, c.y, c.z), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // ... and with edge 1 active, its own.
    t = floor_triangle(NPH_PHYSICS_MESH_EDGE_1, false);
    m = collide_tri(&s, at(c.x, c.y, c.z), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_neg(nph_vec3_normalize(off)), 1e-5F));
    // The face itself is never an edge: inside it, straight down whatever
    // the flags.
    t = floor_triangle(0U, false);
    m = collide_tri(&s, at(0.2F, 0.45F, 0.1F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
}

NTEST(collide_mesh, capsules_against_a_triangle)
{
    const NphPhysicsTriangle t = floor_triangle(NPH_PHYSICS_MESH_EDGES, false);
    const NphPhysicsGeometry cap = nph_physics_geometry_capsule(0.3F, 0.1F);
    // Lying over the face, 1 mm into it: two points, the core's ends.
    NphPhysicsManifold m =
        collide_tri(&cap, nph_physics_pose(nph_vec3(-0.3F, 0.099F, -0.5F), lying()), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 2U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    for (u32 i = 0U; i < 2U; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, -0.001F, 1e-5F);
        NTEST_EXPECT_NEAR_F32(nph_f32_abs(m.points[i].point.x + 0.3F), 0.3F, 1e-5F);
    }
    // Lying in the triangle's plane 3 cm beside edge 0: no phantom contact
    // (the axis test's would touch it).
    const NphQuat along_z = nph_quat_from_axis_angle(nph_vec3_unit_x(), 1.5707963F);
    m = collide_tri(&cap, nph_physics_pose(nph_vec3(-1.13F, 0.0F, 0.5F), along_z), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // 1 cm beside it: edge 0's normal (it is active), two points along it.
    m = collide_tri(&cap, nph_physics_pose(nph_vec3(-1.11F, 0.0F, 0.5F), along_z), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 2U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_x(), 1e-5F));
    for (u32 i = 0U; i < 2U; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, 0.01F, 1e-5F);
    }
    // Standing on the face: one point, at the lower end.
    m = collide_tri(&cap, at(0.2F, 0.399F, 0.2F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.001F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    // Through the face: out the front by the lower end's depth.
    m = collide_tri(&cap, at(0.2F, 0.25F, 0.2F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.15F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    // Tilted across edge 0, its lower end over the face: one point, under
    // that end.
    const NphQuat tilt = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.6F);
    const f32 rise = 0.3F * nph_f32_cos(0.6F);
    m = collide_tri(&cap, nph_physics_pose(nph_vec3(-0.9F, 0.099F + rise, 0.5F), tilt), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
    NTEST_EXPECT_NEAR_F32(m.points[0].separation, -0.001F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    // Hooked under the edge from beyond it, its centre above the plane:
    // nothing, as a one-sided triangle has no underside.
    m = collide_tri(&cap, nph_physics_pose(nph_vec3(-1.25F, 0.2F, 0.5F), tilt), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
}

NTEST(collide_mesh, boxes_against_a_triangle)
{
    const NphPhysicsTriangle t = floor_triangle(NPH_PHYSICS_MESH_EDGES, false);
    const NphPhysicsTriangle t0 =
        floor_triangle(NPH_PHYSICS_MESH_EDGE_1 | NPH_PHYSICS_MESH_EDGE_2, false);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.2F, 0.2F, 0.2F));
    // Resting 1 mm into the face: four points, straight down.
    NphPhysicsManifold m = collide_tri(&box, at(0.0F, 0.199F, 0.0F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 4U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-5F));
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, -0.001F, 1e-5F);
    }
    // Half over edge 0: the corners over the face and the edge's crossings.
    m = collide_tri(&box, at(-1.0F, 0.199F, 0.0F), &t);
    NTEST_ASSERT_EQ_UINT(m.point_count, 4U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-5F));
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_TRUE(m.points[i].point.x >= -1.0F - 1e-5F);
    }
    // Behind the triangle: nothing.
    m = collide_tri(&box, at(0.0F, -0.3F, 0.0F), &t);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    // Reaching 0.5 mm over edge 0 from outside it, 1 mm deep: the box's
    // side is the shallowest axis. An active edge stops the box...
    const f32 cx = -1.0F - 0.2F + 0.0005F;
    m = collide_tri(&box, at(cx, 0.199F, 0.5F), &t);
    NTEST_ASSERT_TRUE(m.point_count > 0U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, nph_vec3_unit_x(), 1e-5F));
    NTEST_EXPECT_NEAR_F32(test_min_separation(&m), -0.0005F, 1e-5F);
    // ... an inactive one (a seam) holds it up by the part of its face over
    // the triangle, at the face's depth: never pushes it back.
    m = collide_tri(&box, at(cx, 0.199F, 0.5F), &t0);
    NTEST_ASSERT_TRUE(m.point_count > 0U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.normal, k_down, 1e-6F));
    for (u32 i = 0U; i < m.point_count; ++i) {
        NTEST_EXPECT_NEAR_F32(m.points[i].separation, -0.001F, 1e-5F);
        NTEST_EXPECT_TRUE(m.points[i].point.x >= -1.0F - 1e-5F);
    }
    // 1 cm short of a seam: nothing (the box is over the neighbour); short
    // of an active edge, the edge's speculative contact.
    m = collide_tri(&box, at(-1.21F, 0.199F, 0.5F), &t0);
    NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
    m = collide_tri(&box, at(-1.21F, 0.199F, 0.5F), &t);
    NTEST_ASSERT_TRUE(m.point_count > 0U);
    NTEST_EXPECT_TRUE(m.normal.x > 0.99F);
}

NTEST(collide_mesh, random_hulls_against_triangles_are_sound)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(72U);
    u32 contacts = 0U;
    for (u32 trial = 0U; trial < 200U * TEST_SCALE; ++trial) {
        NphPhysicsHull *hull =
            test_random_hull(alloc, &rng, 8U + trial % 24U, 0.5F, nph_vec3(1.0F, 0.5F, 0.8F));
        const NphPhysicsGeometry g = nph_physics_geometry_hull(hull);
        NphPhysicsTriangle t;
        for (u32 i = 0U; i < 3U; ++i) {
            t.vertices[i] = test_vec3(&rng, -1.0F, 1.0F);
        }
        t.active_edges = NPH_PHYSICS_MESH_EDGES;
        t.two_sided = (trial & 1U) != 0U ? 1 : 0;
        const NphVec3 c = nph_vec3_cross(nph_vec3_sub(t.vertices[1], t.vertices[0]),
                                         nph_vec3_sub(t.vertices[2], t.vertices[0]));
        const NphPhysicsPose pa = test_pose(&rng, 0.7F);
        const NphPhysicsManifold m = collide_tri(&g, pa, &t);
        if (nph_vec3_length_sq(c) > 1e-2F) {
            const NphHullView va = nph_physics_hull_view(hull);
            const f32 brute = brute_separation(&va, pa, t.vertices);
            // Never much deeper than the shapes overlap: by the hull test's
            // preference for faces over edges (half a slop), and by a slop
            // for a contact the face rule makes again against the face.
            // Nothing when apart.
            if (m.point_count > 0U) {
                NTEST_EXPECT_TRUE(test_min_separation(&m) >=
                                  brute - 1.5F * NPH_PHYSICS_LINEAR_SLOP - 1e-4F);
                // The normal from the hull leaves through the face it faces.
                NphVec3 front = nph_vec3_normalize(c);
                const NphVec3 center = nph_physics_pose_transform_point(pa, hull->centroid);
                if (nph_vec3_dot(front, nph_vec3_sub(center, t.vertices[0])) < 0.0F) {
                    front = nph_vec3_neg(front);
                }
                NTEST_EXPECT_TRUE(nph_vec3_dot(m.normal, front) <= 1e-4F);
                contacts += 1U;
            }
            if (brute > k_spec + 1e-4F) {
                NTEST_EXPECT_EQ_UINT(m.point_count, 0U);
            }
        }
        nph_physics_hull_free(hull, alloc);
    }
    NTEST_EXPECT_TRUE(contacts > 20U * TEST_SCALE);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

// The area of the convex hull of four points in the XZ plane: the largest
// over the three ways to pair them as diagonals.
static f32 footprint_area(const NphPhysicsManifold *m)
{
    const NphVec3 *p[4] = {&m->points[0].point, &m->points[1].point, &m->points[2].point,
                           &m->points[3].point};
    const u32 pairs[3][4] = {{0U, 2U, 1U, 3U}, {0U, 1U, 2U, 3U}, {0U, 3U, 1U, 2U}};
    f32 best = 0.0F;
    for (u32 k = 0U; k < 3U; ++k) {
        const NphVec3 d1 = nph_vec3_sub(*p[pairs[k][1]], *p[pairs[k][0]]);
        const NphVec3 d2 = nph_vec3_sub(*p[pairs[k][3]], *p[pairs[k][2]]);
        best = nph_f32_max(best, 0.5F * nph_f32_abs(d1.x * d2.z - d1.z * d2.x));
    }
    return best;
}

NTEST(collide_mesh, flat_floors_give_one_straight_manifold)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 8U, 0.5F, -2.0F, -2.0F, NULL, false);
    // A hexagonal prism: a hull resting on a face.
    NphVec3 prism[12];
    for (u32 i = 0U; i < 6U; ++i) {
        const f32 a = 1.0471976F * (f32)i;
        prism[i] = nph_vec3(0.3F * nph_f32_cos(a), -0.15F, 0.3F * nph_f32_sin(a));
        prism[i + 6U] = nph_vec3(prism[i].x, 0.15F, prism[i].z);
    }
    NphPhysicsHull *hull = test_hull(alloc, prism, 12U, 0U);
    const NphVec3 half = nph_vec3(0.3F, 0.2F, 0.25F);
    const NphPhysicsGeometry shapes[4] = {
        nph_physics_geometry_box(half), nph_physics_geometry_sphere(0.3F),
        nph_physics_geometry_capsule(0.25F, 0.1F), nph_physics_geometry_hull(hull)};
    const f32 bottoms[4] = {0.2F, 0.3F, 0.1F, 0.15F};
    NphPcg32 rng = test_rng(73U);
    for (u32 trial = 0U; trial < 400U * TEST_SCALE; ++trial) {
        const u32 s = trial % 4U;
        // Anywhere over the grid, turned any way, from 2 mm into it to
        // 1.5 cm above: seams and vertices under every part of the shape.
        const f32 gap = test_range(&rng, -0.002F, 0.015F);
        NphQuat q = yaw(test_range(&rng, 0.0F, 6.2831853F));
        if (s == 2U) {
            q = nph_quat_mul(q, lying());
        }
        const NphPhysicsPose pose =
            nph_physics_pose(nph_vec3(test_range(&rng, -1.2F, 1.2F), bottoms[s] + gap,
                                      test_range(&rng, -1.2F, 1.2F)),
                             q);
        const NphPhysicsMeshManifolds mm =
            collide_mesh(&shapes[s], pose, mesh, nph_physics_pose_identity());
        NTEST_ASSERT_EQ_UINT(mm.count, 1U);
        const NphPhysicsManifold *m = &mm.manifolds[0];
        NTEST_EXPECT_TRUE(nph_vec3_near(m->normal, k_down, 1e-5F));
        for (u32 i = 0U; i < m->point_count; ++i) {
            // Every point at the gap: the floor is flat.
            NTEST_EXPECT_NEAR_F32(m->points[i].separation, gap, 2e-5F);
        }
        if (s == 0U) {
            NTEST_ASSERT_EQ_UINT(m->point_count, 4U);
            NTEST_EXPECT_NEAR_F32(footprint_area(m), 4.0F * half.x * half.z, 1e-4F);
        } else if (s == 1U) {
            // One point, under the centre.
            NTEST_ASSERT_EQ_UINT(m->point_count, 1U);
            NTEST_EXPECT_NEAR_F32(m->points[0].point.x, pose.position.x, 1e-5F);
            NTEST_EXPECT_NEAR_F32(m->points[0].point.z, pose.position.z, 1e-5F);
        } else if (s == 2U) {
            // Along the core, both of its ends kept.
            NTEST_ASSERT_TRUE(m->point_count >= 2U);
            const NphVec3 axis = nph_quat_axis_y(q);
            f32 lo = 1e30F;
            f32 hi = -1e30F;
            for (u32 i = 0U; i < m->point_count; ++i) {
                const NphVec3 r = nph_vec3_sub(m->points[i].point, pose.position);
                const f32 along = nph_vec3_dot(r, axis);
                lo = nph_f32_min(lo, along);
                hi = nph_f32_max(hi, along);
                NTEST_EXPECT_NEAR_F32(nph_vec3_length(nph_vec3_reject(
                                          nph_vec3_sub(r, nph_vec3_scale(axis, along)), k_down)),
                                      0.0F, 1e-5F);
            }
            NTEST_EXPECT_NEAR_F32(lo, -0.25F, 1e-4F);
            NTEST_EXPECT_NEAR_F32(hi, 0.25F, 1e-4F);
        } else {
            NTEST_EXPECT_EQ_UINT(m->point_count, 4U);
        }
    }
    // Above the speculative distance: nothing.
    const NphPhysicsMeshManifolds none = collide_mesh(
        &shapes[0], at(0.3F, 0.2F + k_spec + 0.002F, 0.1F), mesh, nph_physics_pose_identity());
    NTEST_EXPECT_EQ_UINT(none.count, 0U);
    nph_physics_hull_free(hull, alloc);
    nph_physics_mesh_free(mesh, alloc);
    test_alloc_end(&ta);
}

NTEST(collide_mesh, a_crease_gives_a_manifold_per_face)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *mesh = test_valley_mesh(alloc);
    const NphVec3 left = nph_vec3(-0.70710678F, -0.70710678F, 0.0F); // from the shape into each
    const NphVec3 right = nph_vec3(0.70710678F, -0.70710678F, 0.0F);
    // A cube turned to rest a face on each slope, 1 mm into both.
    const NphPhysicsGeometry cube = nph_physics_geometry_box(nph_vec3_splat(0.3F));
    const NphQuat turn = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.78539816F);
    const f32 y = (0.3F - 0.001F) * 1.41421356F;
    NphPhysicsMeshManifolds mm = collide_mesh(
        &cube, nph_physics_pose(nph_vec3(0.0F, y, 0.0F), turn), mesh, nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 2U);
    bool seen[2] = {false, false};
    for (u32 i = 0U; i < 2U; ++i) {
        const NphPhysicsManifold *m = &mm.manifolds[i];
        const bool is_left = nph_vec3_near(m->normal, left, 1e-4F);
        NTEST_EXPECT_TRUE(is_left || nph_vec3_near(m->normal, right, 1e-4F));
        seen[is_left ? 0U : 1U] = true;
        NTEST_EXPECT_EQ_UINT(m->point_count, 4U);
        NTEST_EXPECT_NEAR_F32(test_min_separation(m), -0.001F, 2e-5F);
    }
    NTEST_EXPECT_TRUE(seen[0] && seen[1]);
    // A ball in the crease: a point on each slope.
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.3F);
    mm = collide_mesh(&ball, at(0.0F, y, 0.5F), mesh, nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 2U);
    for (u32 i = 0U; i < 2U; ++i) {
        NTEST_EXPECT_EQ_UINT(mm.manifolds[i].point_count, 1U);
        NTEST_EXPECT_NEAR_F32(mm.manifolds[i].points[0].separation, -0.001F, 2e-5F);
    }
    // Resting on one slope away from the crease: one manifold.
    mm = collide_mesh(&ball, at(-1.0F, 1.0F + 0.299F * 1.41421356F, 0.0F), mesh,
                      nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(mm.manifolds[0].normal, left, 1e-5F));
    nph_physics_mesh_free(mesh, alloc);
    test_alloc_end(&ta);
}

NTEST(collide_mesh, many_normals_keep_the_deepest_four)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // A hexagonal funnel: six faces from a bottom apex up to a rim, each
    // facing up and in, 60 degrees apart around the axis.
    NphVec3 v[7];
    v[0] = nph_vec3_zero();
    for (u32 k = 0U; k < 6U; ++k) {
        const f32 a = 1.0471976F * (f32)k;
        v[1U + k] = nph_vec3(2.0F * nph_f32_cos(a), 1.0F, 2.0F * nph_f32_sin(a));
    }
    u32 idx[18];
    for (u32 k = 0U; k < 6U; ++k) {
        idx[3ULL * k] = 0U;
        idx[3ULL * k + 1U] = 1U + (k + 1U) % 6U;
        idx[3ULL * k + 2U] = 1U + k;
    }
    NphPhysicsMesh *mesh = test_mesh(alloc, v, 7U, idx, 6U, false);
    // A ball in the funnel, off its axis: every face touched, some deeper.
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.5F);
    const NphPhysicsPose pose = at(0.04F, 0.57F, 0.015F);
    const NphPhysicsMeshManifolds mm = collide_mesh(&ball, pose, mesh, nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 4U);
    // Each face alone, with the mesh's own edge flags: the four deepest
    // are the pair's.
    f32 depth[6];
    for (u32 t = 0U; t < 6U; ++t) {
        NphPhysicsTriangle tri;
        nph_physics_mesh_triangle(mesh, t, tri.vertices);
        tri.active_edges = nph_physics_mesh_edges(mesh)[t];
        tri.two_sided = 0;
        const NphPhysicsManifold m = collide_tri(&ball, pose, &tri);
        NTEST_ASSERT_EQ_UINT(m.point_count, 1U);
        depth[t] = m.points[0].separation;
    }
    for (u32 i = 1U; i < 6U; ++i) {
        for (u32 j = i; j > 0U && depth[j] < depth[j - 1U]; --j) {
            const f32 x = depth[j];
            depth[j] = depth[j - 1U];
            depth[j - 1U] = x;
        }
    }
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_NEAR_F32(mm.manifolds[i].points[0].separation, depth[i], 1e-6F);
    }
    nph_physics_mesh_free(mesh, alloc);
    test_alloc_end(&ta);
}

NTEST(collide_mesh, open_edges_stop_and_seams_carry)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // A platform of two cells, x in [0, 2], its edges open (active).
    NphPhysicsMesh *platform = test_grid_mesh(alloc, 2U, 1.0F, 0.0F, -1.0F, NULL, false);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.2F, 0.2F, 0.2F));
    // Beside the platform, straddling its plane and reaching 0.5 mm over
    // its open edge: stopped, between two of its vertices and at one (where
    // a triangle whose own edges there are inside the platform must not
    // lift the box by its face).
    const f32 zs[2] = {-0.5F, 0.0F};
    NphPhysicsMeshManifolds mm;
    for (u32 k = 0U; k < 2U; ++k) {
        mm = collide_mesh(&box, at(-0.2F + 0.0005F, 0.1F, zs[k]), platform,
                          nph_physics_pose_identity());
        NTEST_ASSERT_EQ_UINT(mm.count, 1U);
        NTEST_EXPECT_TRUE(nph_vec3_near(mm.manifolds[0].normal, nph_vec3_unit_x(), 1e-5F));
    }
    // Resting on it across the seam at x = 1, reaching 0.5 mm over the
    // seam: carried, as by the flat floor.
    mm = collide_mesh(&box, at(1.0F - 0.2F + 0.0005F, 0.199F, 0.0F), platform,
                      nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(mm.manifolds[0].normal, k_down, 1e-5F));
    nph_physics_mesh_free(platform, alloc);
    test_alloc_end(&ta);
}

NTEST(collide_mesh, two_sided_sheets_face_the_shape)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *one = test_grid_mesh(alloc, 4U, 0.5F, -1.0F, -1.0F, NULL, false);
    NphPhysicsMesh *two = test_grid_mesh(alloc, 4U, 0.5F, -1.0F, -1.0F, NULL, true);
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.3F);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.2F, 0.1F, 0.3F));
    // Under the sheet: a one-sided mesh lets it pass, a two-sided one
    // pushes it down.
    NphPhysicsMeshManifolds mm =
        collide_mesh(&ball, at(0.1F, -0.29F, 0.2F), one, nph_physics_pose_identity());
    NTEST_EXPECT_EQ_UINT(mm.count, 0U);
    mm = collide_mesh(&ball, at(0.1F, -0.29F, 0.2F), two, nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(mm.manifolds[0].normal, nph_vec3_unit_y(), 1e-5F));
    NTEST_EXPECT_NEAR_F32(mm.manifolds[0].points[0].separation, -0.01F, 1e-5F);
    mm = collide_mesh(&box, nph_physics_pose(nph_vec3(-0.1F, -0.099F, 0.15F), yaw(0.4F)), two,
                      nph_physics_pose_identity());
    NTEST_ASSERT_EQ_UINT(mm.count, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(mm.manifolds[0].normal, nph_vec3_unit_y(), 1e-5F));
    NTEST_EXPECT_EQ_UINT(mm.manifolds[0].point_count, 4U);
    nph_physics_mesh_free(one, alloc);
    nph_physics_mesh_free(two, alloc);
    test_alloc_end(&ta);
}

// The same manifolds within a tolerance: count, normals, and the points
// matched by position.
static void expect_same_manifolds(const NphPhysicsMeshManifolds *a,
                                  const NphPhysicsMeshManifolds *b, f32 tol)
{
    NTEST_ASSERT_EQ_UINT(a->count, b->count);
    for (u32 i = 0U; i < a->count; ++i) {
        const NphPhysicsManifold *ma = &a->manifolds[i];
        const NphPhysicsManifold *mb = &b->manifolds[i];
        NTEST_EXPECT_TRUE(nph_vec3_near(ma->normal, mb->normal, tol));
        NTEST_ASSERT_EQ_UINT(ma->point_count, mb->point_count);
        for (u32 j = 0U; j < ma->point_count; ++j) {
            bool found = false;
            for (u32 k = 0U; k < mb->point_count && !found; ++k) {
                found = nph_vec3_near(ma->points[j].point, mb->points[k].point, tol) &&
                        nph_f32_abs(ma->points[j].separation - mb->points[k].separation) <= tol;
            }
            NTEST_EXPECT_TRUE(found);
        }
    }
}

NTEST(collide_mesh, meshes_keep_full_precision_far_from_their_origin)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // The same floor at the origin and 10 km along X (every coordinate
    // exact in 32 bits), the second placed back by its pose: the same
    // contacts to the micrometre.
    NphPhysicsMesh *near = test_grid_mesh(alloc, 6U, 0.5F, -1.5F, -1.5F, NULL, false);
    NphPhysicsMesh *far = test_grid_mesh(alloc, 6U, 0.5F, 10000.0F - 1.5F, -1.5F, NULL, false);
    const NphPhysicsPose back = at(-10000.0F, 0.0F, 0.0F);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.3F, 0.2F, 0.25F));
    const NphPhysicsGeometry cap = nph_physics_geometry_capsule(0.25F, 0.1F);
    NphPcg32 rng = test_rng(74U);
    for (u32 trial = 0U; trial < 50U * TEST_SCALE; ++trial) {
        const bool b = (trial & 1U) == 0U;
        const NphQuat q = b ? yaw(test_range(&rng, 0.0F, 6.2831853F))
                            : nph_quat_mul(yaw(test_range(&rng, 0.0F, 6.2831853F)), lying());
        const NphPhysicsPose pose =
            nph_physics_pose(nph_vec3(test_range(&rng, -0.9F, 0.9F),
                                      (b ? 0.2F : 0.1F) + test_range(&rng, -0.002F, 0.01F),
                                      test_range(&rng, -0.9F, 0.9F)),
                             q);
        const NphPhysicsGeometry *g = b ? &box : &cap;
        const NphPhysicsMeshManifolds ma = collide_mesh(g, pose, near, nph_physics_pose_identity());
        const NphPhysicsMeshManifolds mb = collide_mesh(g, pose, far, back);
        NTEST_ASSERT_EQ_UINT(ma.count, 1U);
        expect_same_manifolds(&ma, &mb, 2e-6F);
        // And the same call twice gives the same bytes.
        const NphPhysicsMeshManifolds mc = collide_mesh(g, pose, far, back);
        NTEST_ASSERT_EQ_UINT(mb.count, mc.count);
        for (u32 i = 0U; i < mb.count; ++i) {
            const NphPhysicsManifold *x = &mb.manifolds[i];
            const NphPhysicsManifold *y = &mc.manifolds[i];
            NTEST_ASSERT_EQ_UINT(x->point_count, y->point_count);
            NTEST_EXPECT_EQ_MEM(&x->normal, &y->normal, sizeof x->normal);
            NTEST_EXPECT_EQ_MEM(x->points, y->points,
                                (size_t)x->point_count * sizeof(NphPhysicsManifoldPoint));
        }
    }
    nph_physics_mesh_free(near, alloc);
    nph_physics_mesh_free(far, alloc);
    test_alloc_end(&ta);
}

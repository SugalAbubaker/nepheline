// SPDX-License-Identifier: Apache-2.0
// Convex hulls: Quickhull on known solids and random clouds (topology,
// containment, budgets, degenerate input, far-from-origin coordinates,
// slivers built again at a larger tolerance, determinism), the box hull,
// mass properties, and validation of corrupted blobs.

#include "test_physics_support.h"

#include <float.h>

static void cube_corners(NphVec3 out[8], NphVec3 h, NphVec3 offset)
{
    for (u32 i = 0U; i < 8U; ++i) {
        out[i] =
            nph_vec3_add(offset, nph_vec3((i & 1U) != 0U ? h.x : -h.x, (i & 2U) != 0U ? h.y : -h.y,
                                          (i & 4U) != 0U ? h.z : -h.z));
    }
}

// Every input point is inside the hull (behind every plane, within the
// build tolerance and the f32 rounding of the planes).
static void expect_contains(const NphPhysicsHull *hull, const NphVec3 *pts, u32 count, f32 tol)
{
    for (u32 i = 0U; i < count; ++i) {
        NTEST_EXPECT_TRUE(test_hull_plane_distance(hull, pts[i]) <= tol);
    }
}

NTEST(hull, cube_corners_make_a_box)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphVec3 pts[8 + 27];
    cube_corners(pts, nph_vec3(1.0F, 2.0F, 0.5F), nph_vec3_zero());
    // Interior points and points on faces and edges must not add anything.
    u32 n = 8U;
    for (u32 i = 0U; i < 27U; ++i) {
        const u32 gx = i % 3U;
        const u32 gy = (i / 3U) % 3U;
        const u32 gz = i / 9U;
        pts[n++] = nph_vec3((f32)gx - 1.0F, 2.0F * ((f32)gy - 1.0F), 0.5F * ((f32)gz - 1.0F));
    }
    NphPhysicsHull *hull = test_hull(alloc, pts, n, 0U);
    NTEST_EXPECT_EQ_UINT(hull->vertex_count, 8U);
    NTEST_EXPECT_EQ_UINT(hull->face_count, 6U);
    NTEST_EXPECT_EQ_UINT(hull->edge_count, 24U);
    for (u32 f = 0U; f < hull->face_count; ++f) {
        NTEST_EXPECT_EQ_UINT(nph_physics_hull_face_vertex_count(hull, f), 4U);
    }
    NTEST_EXPECT_NEAR_F32(hull->volume, 8.0F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(hull->centroid, nph_vec3_zero(), 1e-6F));
    NTEST_EXPECT_NEAR_F32(hull->inner_radius, 0.5F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(hull->outer_radius, nph_f32_sqrt(5.25F), 1e-5F);
    NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, box_inertia_matches_the_closed_form)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const NphVec3 h = nph_vec3(0.7F, 1.3F, 0.4F);
    NphVec3 pts[8];
    cube_corners(pts, h, nph_vec3_zero());
    NphPhysicsHull *hull = test_hull(alloc, pts, 8U, 0U);
    const NphPhysicsGeometry hg = nph_physics_geometry_hull(hull);
    const NphPhysicsGeometry bg = nph_physics_geometry_box(h);
    const NphPhysicsMassData mh = nph_physics_geometry_mass(&hg, 1000.0F);
    const NphPhysicsMassData mb = nph_physics_geometry_mass(&bg, 1000.0F);
    NTEST_EXPECT_NEAR_F32(mh.mass, mb.mass, mb.mass * 1e-5F);
    for (u32 c = 0U; c < 3U; ++c) {
        NTEST_EXPECT_TRUE(nph_vec3_near(mh.inertia.c[c], mb.inertia.c[c], mb.mass * 1e-5F));
    }
    // The same box made directly.
    NphPhysicsHull *made = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(h, alloc, &made), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_physics_hull_validate(made, made->byte_size, 0.0F));
    NTEST_EXPECT_NEAR_F32(made->volume, hull->volume, 1e-5F);
    for (u32 c = 0U; c < 3U; ++c) {
        NTEST_EXPECT_TRUE(nph_vec3_near(made->inertia.c[c], hull->inertia.c[c], 1e-5F));
    }
    nph_physics_hull_free(made, alloc);
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, tetrahedron)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const NphVec3 pts[4] = {
        {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    NphPhysicsHull *hull = test_hull(alloc, pts, 4U, 0U);
    NTEST_EXPECT_EQ_UINT(hull->vertex_count, 4U);
    NTEST_EXPECT_EQ_UINT(hull->face_count, 4U);
    NTEST_EXPECT_NEAR_F32(hull->volume, 1.0F / 6.0F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(hull->centroid, nph_vec3_splat(0.25F), 1e-6F));
    // The inertia of the unit tetrahedron about its centroid, density 1:
    // diagonal 1/80, products -1/480 (from the canonical moments 1/60, 1/120
    // about the origin shifted to the centroid).
    NTEST_EXPECT_NEAR_F32(hull->inertia.c[0].x, 1.0F / 80.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(hull->inertia.c[1].y, 1.0F / 80.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(hull->inertia.c[0].y, 1.0F / 480.0F, 1e-6F);
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, random_clouds_contain_every_point)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(7U);
    NphVec3 pts[400];
    const u32 rounds = 6U * TEST_SCALE;
    for (u32 round = 0U; round < rounds; ++round) {
        const u32 count = 20U + nph_pcg32_next_bounded(&rng, 380U);
        const NphVec3 scale = test_vec3(&rng, 0.3F, 3.0F);
        for (u32 i = 0U; i < count; ++i) {
            pts[i] = nph_vec3_mul(test_vec3(&rng, -1.0F, 1.0F), scale);
        }
        NphPhysicsHull *hull = test_hull(alloc, pts, count, NPH_PHYSICS_HULL_MAX_VERTICES);
        NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
        expect_contains(hull, pts, count, 8.0F * hull->tolerance + 1e-5F);
        // Every hull vertex is an input point.
        const NphVec3 *v = nph_physics_hull_vertices(hull);
        for (u32 k = 0U; k < hull->vertex_count; ++k) {
            bool found = false;
            for (u32 i = 0U; i < count && !found; ++i) {
                found = nph_vec3_eq(v[k], pts[i]);
            }
            NTEST_EXPECT_TRUE(found);
        }
        NTEST_EXPECT_TRUE(hull->volume > 0.0F);
        nph_physics_hull_free(hull, alloc);
    }
    test_alloc_end(&ta);
}

NTEST(hull, sphere_points_respect_the_budget)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(11U);
    enum {
        COUNT = 2000
    };
    static NphVec3 pts[COUNT];
    for (u32 i = 0U; i < (u32)COUNT; ++i) {
        pts[i] = test_unit(&rng);
    }
    const f32 sphere = 4.0F / 3.0F * 3.14159265F;
    f32 previous = 0.0F;
    const u32 budgets[4] = {8U, 16U, 64U, 256U};
    for (u32 b = 0U; b < 4U; ++b) {
        NphPhysicsHull *hull = test_hull(alloc, pts, (u32)COUNT, budgets[b]);
        NTEST_EXPECT_TRUE(hull->vertex_count <= budgets[b]);
        NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
        // More vertices, more of the ball: farthest-first keeps volume.
        NTEST_EXPECT_TRUE(hull->volume > previous);
        NTEST_EXPECT_TRUE(hull->volume < sphere);
        previous = hull->volume;
        nph_physics_hull_free(hull, alloc);
    }
    // 256 of 2000 points on the unit sphere cover all but a few percent.
    NTEST_EXPECT_TRUE(previous > 0.95F * sphere);
    test_alloc_end(&ta);
}

NTEST(hull, cylinder_merges_its_caps)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    enum {
        SEGMENTS = 24
    };
    NphVec3 pts[2 * SEGMENTS];
    for (u32 i = 0U; i < (u32)SEGMENTS; ++i) {
        const f32 a = 6.2831853F * (f32)i / (f32)SEGMENTS;
        pts[2ULL * i] = nph_vec3(nph_f32_cos(a), -0.5F, nph_f32_sin(a));
        pts[2ULL * i + 1U] = nph_vec3(nph_f32_cos(a), 0.5F, nph_f32_sin(a));
    }
    NphPhysicsHull *hull = test_hull(alloc, pts, 2U * (u32)SEGMENTS, 0U);
    NTEST_EXPECT_EQ_UINT(hull->vertex_count, 2U * (u32)SEGMENTS);
    // Two caps and one quad per side.
    NTEST_EXPECT_EQ_UINT(hull->face_count, (u32)SEGMENTS + 2U);
    u32 caps = 0U;
    for (u32 f = 0U; f < hull->face_count; ++f) {
        const u32 n = nph_physics_hull_face_vertex_count(hull, f);
        NTEST_EXPECT_TRUE(n == 4U || n == (u32)SEGMENTS);
        caps += n == (u32)SEGMENTS ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(caps, 2U);
    NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, duplicates_and_coplanar_points_merge)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphVec3 pts[8 * 3 + 6 * 9];
    u32 n = 0U;
    NphVec3 corners[8];
    cube_corners(corners, nph_vec3_one(), nph_vec3_zero());
    for (u32 k = 0U; k < 3U; ++k) {
        for (u32 i = 0U; i < 8U; ++i) {
            pts[n++] = corners[i];
        }
    }
    // A 3x3 grid on every face.
    for (u32 axis = 0U; axis < 3U; ++axis) {
        for (u32 side = 0U; side < 2U; ++side) {
            for (u32 g = 0U; g < 9U; ++g) {
                const u32 gu = g % 3U;
                const u32 gw = g / 3U;
                const f32 u = (f32)gu * 0.5F - 0.5F;
                const f32 w = (f32)gw * 0.5F - 0.5F;
                const f32 s = side == 0U ? -1.0F : 1.0F;
                pts[n++] = axis == 0U ? nph_vec3(s, u, w)
                                      : (axis == 1U ? nph_vec3(u, s, w) : nph_vec3(u, w, s));
            }
        }
    }
    NphPhysicsHull *hull = test_hull(alloc, pts, n, 0U);
    NTEST_EXPECT_EQ_UINT(hull->vertex_count, 8U);
    NTEST_EXPECT_EQ_UINT(hull->face_count, 6U);
    NTEST_EXPECT_NEAR_F32(hull->volume, 8.0F, 1e-5F);
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, far_from_the_origin)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphVec3 pts[8];
    cube_corners(pts, nph_vec3(0.5F, 0.5F, 0.5F), nph_vec3(1000.0F, -2000.0F, 500.0F));
    NphPhysicsHull *hull = test_hull(alloc, pts, 8U, 0U);
    NTEST_EXPECT_EQ_UINT(hull->vertex_count, 8U);
    NTEST_EXPECT_EQ_UINT(hull->face_count, 6U);
    NTEST_EXPECT_NEAR_F32(hull->volume, 1.0F, 1e-3F);
    NTEST_EXPECT_TRUE(nph_vec3_near(hull->centroid, nph_vec3(1000.0F, -2000.0F, 500.0F), 1e-3F));
    NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, degenerate_input_is_rejected)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // The builder warns about each degenerate cloud; these are expected.
    nph_log_set_level(&NPH_LOG_PHYSICS, NPH_LOG_LEVEL_ERROR);
    NphPhysicsHull *hull = NULL;
    NphPhysicsHullDesc desc;
    memset(&desc, 0, sizeof desc);
    // Too few points.
    const NphVec3 three[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    desc.points = three;
    desc.point_count = 3U;
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    // Coplanar, collinear, coincident.
    NphVec3 flat[16];
    for (u32 i = 0U; i < 16U; ++i) {
        const u32 row = i / 4U;
        flat[i] = nph_vec3((f32)(i % 4U), 0.0F, (f32)row);
    }
    desc.points = flat;
    desc.point_count = 16U;
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    for (u32 i = 0U; i < 16U; ++i) {
        flat[i] = nph_vec3((f32)i, 2.0F * (f32)i, -(f32)i);
    }
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    for (u32 i = 0U; i < 16U; ++i) {
        flat[i] = nph_vec3(3.0F, 3.0F, 3.0F);
    }
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    // A NaN, and budgets out of range.
    NphVec3 cube[8];
    cube_corners(cube, nph_vec3_one(), nph_vec3_zero());
    desc.points = cube;
    desc.point_count = 8U;
    desc.max_vertices = 3U;
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    desc.max_vertices = NPH_PHYSICS_HULL_MAX_VERTICES + 1U;
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    desc.max_vertices = 0U;
    cube[3].y = nph_f32_from_bits(0x7FC00000U);
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(hull);
    // A coordinate beyond the limit.
    cube[3].y = 2.0F * NPH_PHYSICS_MAX_COORDINATE;
    NTEST_EXPECT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_INVALID_ARGUMENT);
    // Bad box sizes.
    NTEST_EXPECT_EQ_INT(nph_physics_hull_make_box(nph_vec3(1.0F, 0.0F, 1.0F), alloc, &hull),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_physics_hull_make_box(
                            nph_vec3(1.0F, 2.0F * NPH_PHYSICS_MAX_COORDINATE, 1.0F), alloc, &hull),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_log_set_level(&NPH_LOG_PHYSICS, NPH_LOG_LEVEL_INFO);
    test_alloc_end(&ta);
}

// A sliver 4 mm thick and 128 m long (found by fuzzing): Quickhull's merges
// leave a face reflex beyond the derived tolerance, and the builder builds
// it again at four times the tolerance, which validates.
NTEST(hull, slivers_build_again_at_a_larger_tolerance)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const f32 lift = 1.0F / 256.0F;
    const NphVec3 pts[16] = {{0.0F, 0.0F, 0.0F},    {0.0F, 0.0F, 0.5F},
                             {0.0F, 0.0F, 1.0F},    {0.0F, 0.0F, 1.2773438F},
                             {0.5F, 0.0F, 0.0F},    {0.5F, 0.0F, 0.5F},
                             {0.5F, 0.0F, 1.0F},    {0.5F, 0.0F, 1.5F},
                             {1.0F, 0.0F, 0.0F},    {1.0F, 0.0F, 0.5F},
                             {1.0F, 0.0F, -128.0F}, {lift, lift, 0.0F},
                             {0.0F, lift, -128.0F}, {0.0F, lift, 0.0F},
                             {lift, lift, -128.0F}, {-127.99609F, -117.99609F, -125.00391F}};
    NphPhysicsHull *hull = test_hull(alloc, pts, 16U, 0U);
    NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
    const f32 derived = 3.0F * FLT_EPSILON * (128.0F + 117.99609F + 128.0F);
    NTEST_EXPECT_NEAR_F32(hull->tolerance, 4.0F * derived, 1e-3F * derived);
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, builds_are_deterministic)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(99U);
    NphVec3 pts[300];
    for (u32 i = 0U; i < 300U; ++i) {
        pts[i] = test_vec3(&rng, -2.0F, 2.0F);
    }
    NphPhysicsHull *a = test_hull(alloc, pts, 300U, 48U);
    NphPhysicsHull *b = test_hull(alloc, pts, 300U, 48U);
    NTEST_ASSERT_EQ_UINT(a->byte_size, b->byte_size);
    NTEST_EXPECT_EQ_MEM(a, b, a->byte_size);
    nph_physics_hull_free(a, alloc);
    nph_physics_hull_free(b, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, support_is_the_farthest_vertex)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(5U);
    NphPhysicsHull *hull = test_random_hull(alloc, &rng, 100U, 1.0F, nph_vec3(1.0F, 0.5F, 2.0F));
    const NphVec3 *v = nph_physics_hull_vertices(hull);
    for (u32 k = 0U; k < 200U; ++k) {
        const NphVec3 d = test_unit(&rng);
        const u32 s = nph_physics_hull_support(hull, d);
        for (u32 i = 0U; i < hull->vertex_count; ++i) {
            NTEST_EXPECT_TRUE(nph_vec3_dot(v[s], d) >= nph_vec3_dot(v[i], d));
        }
    }
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(hull, validate_rejects_corrupted_blobs)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsHull *hull = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(nph_vec3_one(), alloc, &hull), NPH_RESULT_OK);
    const u32 size = hull->byte_size;
    NphPhysicsHull *copy = (NphPhysicsHull *)NPH_ALLOC(alloc, size + 16U, 16U);
    NTEST_ASSERT_NOT_NULL(copy);
    nph_log_set_level(&NPH_LOG_PHYSICS, NPH_LOG_LEVEL_ERROR); // each rejection warns
    NTEST_EXPECT_FALSE(nph_physics_hull_validate(NULL, size, 0.0F));
    NTEST_EXPECT_FALSE(nph_physics_hull_validate(hull, 64U, 0.0F));       // shorter than the header
    NTEST_EXPECT_FALSE(nph_physics_hull_validate(hull, size - 1U, 0.0F)); // than byte_size
    memcpy((u8 *)copy + 4U, hull, size);
    NTEST_EXPECT_FALSE(nph_physics_hull_validate((u8 *)copy + 4U, size, 0.0F)); // misaligned
    // Each corruption on a fresh copy; none may crash, all must fail.
    for (u32 kind = 0U; kind < 13U; ++kind) {
        memcpy(copy, hull, size);
        void *edge_bytes = (u8 *)copy + copy->edges_offset;
        void *vertex_bytes = (u8 *)copy + copy->vertices_offset;
        NphPhysicsHullEdge *edges = edge_bytes;
        NphVec3 *verts = vertex_bytes;
        switch (kind) {
        case 0U:
            copy->edges_offset = copy->byte_size; // past the block
            break;
        case 1U:
            edges[3].twin = 7U;
            break;
        case 2U:
            edges[5].next = 0xFFFFU;
            break;
        case 3U:
            verts[2] = nph_vec3(5.0F, 5.0F, 5.0F); // outside the planes
            break;
        case 4U:
            copy->face_count = 5U; // breaks Euler
            break;
        case 5U:
            edges[9].face = edges[9 ^ 1].face;
            break;
        case 6U:
            copy->vertex_count = 1000U;
            break;
        case 7U:
            verts[1].x = nph_f32_from_bits(0x7FC00000U); // NaN passes every comparison
            break;
        case 8U:
            copy->tolerance = nph_f32_from_bits(0x7FC00000U); // so would any geometry
            break;
        case 9U:
            copy->volume = -1.0F;
            break;
        case 10U:
            copy->bounds.max.x = 2.0F * NPH_PHYSICS_MAX_COORDINATE;
            break;
        case 11U:
            copy->face_count = 2U * copy->vertex_count - 3U; // more than a hull can have
            break;
        default:
            copy->inertia.c[1].y = nph_f32_from_bits(0x7F800000U);
            break;
        }
        NTEST_EXPECT_FALSE(nph_physics_hull_validate(copy, size, 0.0F));
    }
    nph_log_set_level(&NPH_LOG_PHYSICS, NPH_LOG_LEVEL_INFO);
    NPH_FREE(alloc, copy, size + 16U, 16U);
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

// Lattices are the classic Quickhull trap: rows of collinear points and
// whole planes of coplanar ones.
NTEST(hull, lattices_give_their_box)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    static NphVec3 pts[11 * 11 * 11];
    const f32 scales[4] = {0.001F, 1.0F, 37.0F, 5000.0F};
    for (u32 s = 0U; s < 4U; ++s) {
        u32 n = 0U;
        for (u32 k = 0U; k < 11U; ++k) {
            for (u32 j = 0U; j < 11U; ++j) {
                for (u32 i = 0U; i < 11U; ++i) {
                    pts[n++] =
                        nph_vec3_scale(nph_vec3((f32)i, (f32)j * 0.5F, (f32)k * 2.0F), scales[s]);
                }
            }
        }
        NphPhysicsHull *hull = test_hull(alloc, pts, n, 0U);
        NTEST_EXPECT_EQ_UINT(hull->vertex_count, 8U);
        NTEST_EXPECT_EQ_UINT(hull->face_count, 6U);
        const f32 volume = 10.0F * 5.0F * 20.0F * scales[s] * scales[s] * scales[s];
        NTEST_EXPECT_NEAR_F32(hull->volume, volume, volume * 1e-4F);
        NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
        nph_physics_hull_free(hull, alloc);
    }
    test_alloc_end(&ta);
}

// Thousands of points on a sphere with noise below and above the
// tolerance: nearly every point is a candidate and faces are nearly
// coplanar everywhere.
NTEST(hull, noisy_spheres_stay_valid)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    enum {
        COUNT = 3000
    };
    static NphVec3 pts[COUNT];
    const f32 noise[3] = {0.0F, 1e-7F, 1e-4F};
    const f32 radius[3] = {0.01F, 1.0F, 300.0F};
    NphPcg32 rng = test_rng(61U);
    for (u32 c = 0U; c < 9U; ++c) {
        for (u32 i = 0U; i < (u32)COUNT; ++i) {
            const f32 r = radius[c / 3U] * (1.0F + test_range(&rng, -noise[c % 3U], noise[c % 3U]));
            pts[i] = nph_vec3_scale(test_unit(&rng), r);
        }
        NphPhysicsHull *hull = test_hull(alloc, pts, (u32)COUNT, NPH_PHYSICS_HULL_MAX_VERTICES);
        NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, hull->byte_size, 0.0F));
        NTEST_EXPECT_TRUE(hull->vertex_count > 128U);
        const f32 r = radius[c / 3U];
        NTEST_EXPECT_TRUE(hull->volume > 0.9F * 4.18879F * r * r * r);
        nph_physics_hull_free(hull, alloc);
    }
    test_alloc_end(&ta);
}

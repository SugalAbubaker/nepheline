// SPDX-License-Identifier: Apache-2.0
// Casts and overlaps against one geometry (collide.h "Casts and overlaps"):
// rays against brute force, shape casts against bisection on GJK
// distances, shapes that start in contact, a mesh's seams and sides, a
// heightfield against the same triangles as a mesh, and the surfaces' sweep
// walks against brute force.

#include "physics_internal.h"
#include "test_physics_support.h"

#include <float.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The signed distance of point p (the geometry's frame) from convex `g`:
// exact for spheres, capsules, and boxes; a hull's largest plane distance
// (exact inside and on its faces).
static f32 signed_distance(const NphPhysicsGeometry *g, NphVec3 p)
{
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        return nph_vec3_length(p) - g->sphere.radius;
    case NPH_PHYSICS_SHAPE_CAPSULE: {
        const f32 h = g->capsule.half_height;
        const f32 y = nph_f32_clamp(p.y, -h, h);
        return nph_vec3_length(nph_vec3(p.x, p.y - y, p.z)) - g->capsule.radius;
    }
    case NPH_PHYSICS_SHAPE_BOX: {
        const NphVec3 q = nph_vec3_sub(nph_vec3_abs(p), g->box.half_extents);
        const f32 outside = nph_vec3_length(nph_vec3_max(q, nph_vec3_zero()));
        return outside + nph_f32_min(nph_vec3_max_component(q), 0.0F);
    }
    case NPH_PHYSICS_SHAPE_HULL:
        return test_hull_plane_distance(g->hull, p);
    default:
        return FLT_MAX;
    }
}

// Sphere, capsule, box, or hull by `kind`, of random size.
static NphPhysicsGeometry random_convex(NphPcg32 *r, NphPhysicsHull *const hulls[2], u32 kind)
{
    switch (kind % 4U) {
    case 0U:
        return nph_physics_geometry_sphere(test_range(r, 0.1F, 1.0F));
    case 1U:
        return nph_physics_geometry_capsule(test_range(r, 0.05F, 0.8F), test_range(r, 0.1F, 0.5F));
    case 2U:
        return nph_physics_geometry_box(test_vec3(r, 0.1F, 0.9F));
    default:
        return nph_physics_geometry_hull(hulls[(kind / 4U) % 2U]);
    }
}

static void make_hulls(NphAllocator *alloc, NphPcg32 *r, NphPhysicsHull *hulls[2])
{
    hulls[0] = test_random_hull(alloc, r, 24U, 0.8F, nph_vec3(1.0F, 0.6F, 0.9F));
    hulls[1] = test_random_hull(alloc, r, 40U, 0.7F, nph_vec3(0.7F, 1.0F, 0.5F));
}

// The first fraction at which segment o + t d enters `g` (its frame): the
// first of 4096 samples inside, then bisection. Negative for none.
static f32 ray_reference(const NphPhysicsGeometry *g, NphVec3 o, NphVec3 d)
{
    const u32 samples = 4096U;
    f32 prev = 0.0F;
    for (u32 i = 1U; i <= samples; ++i) {
        const f32 t = (f32)i / (f32)samples;
        if (signed_distance(g, nph_vec3_mul_add(o, d, t)) <= 0.0F) {
            f32 lo = prev;
            f32 hi = t;
            for (u32 k = 0U; k < 40U; ++k) {
                const f32 mid = 0.5F * (lo + hi);
                if (signed_distance(g, nph_vec3_mul_add(o, d, mid)) <= 0.0F) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return hi;
        }
        prev = t;
    }
    return -1.0F;
}

// Whether `n` (unit, the geometry's frame) is the outward normal of a plane
// that supports `g` at p: no point of `g` lies farther along it.
static bool supports_at(const NphPhysicsGeometry *g, NphVec3 p, NphVec3 n, f32 tol)
{
    const NphVec3 s = nph_physics_geometry_support(g, n);
    return nph_f32_abs(nph_vec3_dot(nph_vec3_sub(s, p), n)) <= tol;
}

// The gap between the rounded surfaces of `a` at `pa` and `b` at `pb`.
static f32 gap(const NphPhysicsGeometry *a, NphPhysicsPose pa, const NphPhysicsGeometry *b,
               NphPhysicsPose pb)
{
    NphVec3 sa[NPH_PHYSICS_PROXY_STORAGE];
    NphVec3 sb[NPH_PHYSICS_PROXY_STORAGE];
    const NphPhysicsProxy xa = nph_physics_geometry_proxy(a, sa);
    const NphPhysicsProxy xb = nph_physics_geometry_proxy(b, sb);
    NphPhysicsDistance d;
    nph_physics_distance(&xa, pa, &xb, pb, true, NULL, &d);
    return d.distance;
}

static NphPhysicsPose moved(NphPhysicsPose p, NphVec3 delta, f32 t)
{
    return nph_physics_pose(nph_vec3_mul_add(p.position, delta, t), p.rotation);
}

// The first fraction at which `a` moved by `delta` touches `b`: the first of
// 2048 samples with no gap, then bisection. Negative for none.
static f32 cast_reference(const NphPhysicsGeometry *a, NphPhysicsPose pa, NphVec3 delta,
                          const NphPhysicsGeometry *b, NphPhysicsPose pb)
{
    const u32 samples = 2048U;
    f32 prev = 0.0F;
    for (u32 i = 1U; i <= samples; ++i) {
        const f32 t = (f32)i / (f32)samples;
        if (gap(a, moved(pa, delta, t), b, pb) <= 0.0F) {
            f32 lo = prev;
            f32 hi = t;
            for (u32 k = 0U; k < 30U; ++k) {
                const f32 mid = 0.5F * (lo + hi);
                if (gap(a, moved(pa, delta, mid), b, pb) <= 0.0F) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return hi;
        }
        prev = t;
    }
    return -1.0F;
}

// ---------------------------------------------------------------------------
// Rays
// ---------------------------------------------------------------------------

NTEST(cast, rays_meet_convex_geometry_where_brute_force_does)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(101U);
    NphPhysicsHull *hulls[2];
    make_hulls(alloc, &r, hulls);
    u32 hits = 0U;
    u32 misses = 0U;
    for (u32 k = 0U; k < 300U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry g = random_convex(&r, hulls, k);
        const NphPhysicsPose pose = test_pose(&r, 3.0F);
        const NphVec3 origin =
            nph_vec3_mul_add(pose.position, test_unit(&r), test_range(&r, 2.5F, 6.0F));
        const NphVec3 aim = nph_vec3_add(pose.position, test_vec3(&r, -1.1F, 1.1F));
        const NphVec3 delta = nph_vec3_scale(nph_vec3_sub(aim, origin), test_range(&r, 0.6F, 2.5F));
        const NphVec3 o = nph_physics_pose_inverse_transform_point(pose, origin);
        const NphVec3 d = nph_physics_pose_inverse_transform_vector(pose, delta);
        if (signed_distance(&g, o) <= 0.0F) {
            continue;
        }
        const f32 want = ray_reference(&g, o, d);
        NphPhysicsCastHit hit;
        const bool got = nph_physics_raycast(&g, pose, origin, delta, &hit);
        if (!got) {
            NTEST_EXPECT_TRUE(want < 0.0F);
            misses += 1U;
            continue;
        }
        // Hit: the point on the surface, the normal a supporting plane's
        // there, against the ray, and no earlier entry.
        const NphVec3 p = nph_physics_pose_inverse_transform_point(pose, hit.point);
        const NphVec3 n = nph_physics_pose_inverse_transform_vector(pose, hit.normal);
        NTEST_EXPECT_NEAR_F32(signed_distance(&g, p), 0.0F, 1e-4F);
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(hit.normal), 1.0F, 1e-5F);
        NTEST_EXPECT_TRUE(supports_at(&g, p, n, 2e-4F));
        NTEST_EXPECT_TRUE(nph_vec3_dot(hit.normal, delta) <= 0.0F);
        NTEST_EXPECT_EQ_UINT(hit.triangle, NPH_PHYSICS_NO_TRIANGLE);
        if (want >= 0.0F) {
            NTEST_EXPECT_NEAR_F32(hit.fraction, want, 2e-5F);
            hits += 1U;
        }
    }
    NTEST_LOG("rays: %u hits, %u misses", hits, misses);
    NTEST_EXPECT_TRUE(hits > 30U * TEST_SCALE && misses > 20U * TEST_SCALE);
    nph_physics_hull_free(hulls[0], alloc);
    nph_physics_hull_free(hulls[1], alloc);
    test_alloc_end(&ta);
}

// A ray's hit at `origin` + t `delta` against `g` at the identity.
static bool ray(const NphPhysicsGeometry *g, NphVec3 origin, NphVec3 delta, NphPhysicsCastHit *hit)
{
    return nph_physics_raycast(g, nph_physics_pose_identity(), origin, delta, hit);
}

static NphPhysicsCastHit expect_hit(const NphPhysicsGeometry *g, NphVec3 origin, NphVec3 delta,
                                    f32 fraction, NphVec3 normal)
{
    NphPhysicsCastHit hit;
    memset(&hit, 0, sizeof hit);
    NTEST_EXPECT_TRUE(ray(g, origin, delta, &hit));
    NTEST_EXPECT_NEAR_F32(hit.fraction, fraction, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, normal, 1e-5F));
    NTEST_EXPECT_TRUE(nph_vec3_near(hit.point, nph_vec3_mul_add(origin, delta, fraction), 1e-5F));
    return hit;
}

NTEST(cast, rays_hit_from_outside_and_from_the_surface_inwards)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsHull *cube = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(nph_vec3_splat(1.0F), alloc, &cube),
                        NPH_RESULT_OK);
    const NphPhysicsGeometry sphere = nph_physics_geometry_sphere(1.0F);
    const NphPhysicsGeometry capsule = nph_physics_geometry_capsule(1.0F, 0.5F);
    const NphPhysicsGeometry boxes[2] = {nph_physics_geometry_box(nph_vec3_splat(1.0F)),
                                         nph_physics_geometry_hull(cube)};
    const NphVec3 x = nph_vec3_unit_x();
    const NphVec3 y = nph_vec3_unit_y();
    NphPhysicsCastHit hit;
    // A sphere: from inside, nothing; from its surface inwards, a hit at 0;
    // outwards, nothing.
    NTEST_EXPECT_FALSE(ray(&sphere, nph_vec3_zero(), nph_vec3(3.0F, 0.0F, 0.0F), &hit));
    (void)expect_hit(&sphere, x, nph_vec3(-3.0F, 0.0F, 0.0F), 0.0F, x);
    NTEST_EXPECT_FALSE(ray(&sphere, x, nph_vec3(3.0F, 0.0F, 0.0F), &hit));
    hit = expect_hit(&sphere, nph_vec3(2.0F, 0.0F, 0.0F), nph_vec3(-3.0F, 0.0F, 0.0F), 1.0F / 3.0F,
                     x);
    NTEST_EXPECT_TRUE(nph_vec3_near(hit.point, x, 1e-6F));
    // Short of it, past it, and parallel beside it.
    NTEST_EXPECT_FALSE(ray(&sphere, nph_vec3(3.0F, 0.0F, 0.0F), nph_vec3(-1.5F, 0.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(ray(&sphere, nph_vec3(3.0F, 0.0F, 0.0F), nph_vec3(1.5F, 0.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(
        ray(&sphere, nph_vec3(3.0F, 1.01F, 0.0F), nph_vec3(-6.0F, 0.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(ray(&sphere, x, nph_vec3_zero(), &hit));
    // A capsule: its side, its caps, along its axis, and from inside.
    (void)expect_hit(&capsule, nph_vec3(2.0F, 0.5F, 0.0F), nph_vec3(-2.0F, 0.0F, 0.0F), 0.75F, x);
    (void)expect_hit(&capsule, nph_vec3(0.0F, 3.0F, 0.0F), nph_vec3(0.0F, -2.0F, 0.0F), 0.75F, y);
    (void)expect_hit(&capsule, nph_vec3(0.0F, -5.0F, 0.0F), nph_vec3(0.0F, 10.0F, 0.0F), 0.35F,
                     nph_vec3_neg(y));
    (void)expect_hit(&capsule, nph_vec3(0.3F, 6.0F, 0.0F), nph_vec3(0.0F, -6.0F, 0.0F),
                     (6.0F - 1.0F - nph_f32_sqrt(0.25F - 0.09F)) / 6.0F,
                     nph_vec3(0.6F, 0.8F, 0.0F));
    NTEST_EXPECT_FALSE(ray(&capsule, nph_vec3(0.0F, 0.9F, 0.0F), nph_vec3(4.0F, 0.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(ray(&capsule, nph_vec3(0.0F, 1.2F, 0.0F), nph_vec3(0.0F, 4.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(
        ray(&capsule, nph_vec3(0.6F, 5.0F, 0.0F), nph_vec3(0.0F, -10.0F, 0.0F), &hit));
    // A box, and the same box as a hull: faces, from inside, from a face
    // inwards, parallel outside a slab.
    for (u32 k = 0U; k < 2U; ++k) {
        const NphPhysicsGeometry *g = &boxes[k];
        (void)expect_hit(g, nph_vec3(3.0F, 0.5F, 0.0F), nph_vec3(-4.0F, 0.0F, 0.0F), 0.5F, x);
        (void)expect_hit(g, nph_vec3(0.2F, -2.0F, 0.3F), nph_vec3(0.0F, 2.0F, 0.0F), 0.5F,
                         nph_vec3_neg(y));
        (void)expect_hit(g, nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(-1.0F, 0.0F, 0.0F), 0.0F, x);
        NTEST_EXPECT_FALSE(ray(g, nph_vec3(0.5F, 0.0F, 0.0F), nph_vec3(3.0F, 1.0F, 0.0F), &hit));
        NTEST_EXPECT_FALSE(ray(g, nph_vec3(3.0F, 2.0F, 0.0F), nph_vec3(-6.0F, 0.0F, 0.0F), &hit));
        NTEST_EXPECT_FALSE(ray(g, nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(1.0F, 0.0F, 0.0F), &hit));
        // At 45 degrees past an edge: the face it enters by, the last slab
        // (plane) it enters.
        (void)expect_hit(g, nph_vec3(3.0F, 2.5F, 0.0F), nph_vec3(-4.0F, -4.0F, 0.0F), 0.5F, x);
        (void)expect_hit(g, nph_vec3(2.5F, 3.0F, 0.0F), nph_vec3(-4.0F, -4.0F, 0.0F), 0.5F, y);
    }
    nph_physics_hull_free(cube, alloc);
    test_alloc_end(&ta);
}

NTEST(cast, rays_from_far_away_keep_their_digits)
{
    // A ball half a metre wide, from 5 km on either side: the closest
    // approach first, then Pythagoras, so the hit keeps the ball's digits
    // rather than the origin's (whose square would swallow them).
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.5F);
    const NphPhysicsGeometry capsule = nph_physics_geometry_capsule(0.4F, 0.5F);
    for (u32 k = 0U; k < 2U; ++k) {
        const NphPhysicsGeometry *g = k == 0U ? &ball : &capsule;
        for (u32 i = 0U; i < 16U; ++i) {
            const f32 off = -0.45F + 0.06F * (f32)i;
            const NphVec3 origin = nph_vec3(-5000.0F, off, 0.1F);
            const NphVec3 delta = nph_vec3(10000.0F, 0.0F, 0.0F);
            NphPhysicsCastHit hit;
            NTEST_ASSERT_TRUE(ray(g, origin, delta, &hit));
            // The point's error is the origin's rounding (5000 m: 0.5 mm),
            // not the ball's size times the far distance.
            NTEST_EXPECT_NEAR_F32(signed_distance(g, hit.point), 0.0F, 1e-3F);
            NTEST_EXPECT_TRUE(hit.normal.x < 0.0F);
        }
    }
}

NTEST(cast, rays_against_surfaces_are_their_raycasts_placed)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(103U);
    f32 heights[17U * 17U];
    for (u32 i = 0U; i < 17U * 17U; ++i) {
        heights[i] = test_range(&r, -0.4F, 0.4F);
    }
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 16U, 0.5F, -4.0F, -4.0F, heights, false);
    NphPhysicsHeightfield *field = test_heightfield(alloc, 17U, 17U, 0.5F, heights, NULL);
    const NphPhysicsGeometry geoms[2] = {nph_physics_geometry_mesh(mesh),
                                         nph_physics_geometry_heightfield(field)};
    for (u32 k = 0U; k < 200U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry *g = &geoms[k % 2U];
        const NphPhysicsPose pose = test_pose(&r, 20.0F);
        const NphVec3 lo = nph_vec3(k % 2U == 0U ? -4.0F : 0.0F, 0.0F, k % 2U == 0U ? -4.0F : 0.0F);
        const NphVec3 o = nph_vec3_add(
            lo, nph_vec3(test_range(&r, 0.0F, 8.0F), 3.0F, test_range(&r, 0.0F, 8.0F)));
        const NphVec3 d = nph_vec3(test_range(&r, -2.0F, 2.0F), -6.0F, test_range(&r, -2.0F, 2.0F));
        NphPhysicsMeshHit want;
        const bool want_hit = k % 2U == 0U ? nph_physics_mesh_raycast(mesh, o, d, &want)
                                           : nph_physics_heightfield_raycast(field, o, d, &want);
        NphPhysicsCastHit hit;
        const bool got = nph_physics_raycast(g, pose, nph_physics_pose_transform_point(pose, o),
                                             nph_physics_pose_transform_vector(pose, d), &hit);
        NTEST_ASSERT_EQ_INT(got, want_hit);
        if (got) {
            // To the pose's rounding: the origin comes back from 20 m out.
            NTEST_EXPECT_NEAR_F32(hit.fraction, want.fraction, 1e-5F);
            NTEST_EXPECT_EQ_UINT(hit.triangle, want.original);
            NTEST_EXPECT_TRUE(nph_vec3_near(
                hit.normal, nph_physics_pose_transform_vector(pose, want.normal), 1e-5F));
        }
    }
    nph_physics_mesh_free(mesh, alloc);
    nph_physics_heightfield_free(field, alloc);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Shape casts between convex shapes
// ---------------------------------------------------------------------------

NTEST(cast, shape_casts_stop_where_bisection_finds_contact)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(107U);
    NphPhysicsHull *hulls[2];
    make_hulls(alloc, &r, hulls);
    u32 hits = 0U;
    u32 misses = 0U;
    u32 grazes = 0U;
    f32 worst_short = 0.0F;
    f32 worst_gap = 0.0F;
    for (u32 k = 0U; k < 160U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry a = random_convex(&r, hulls, k);
        const NphPhysicsGeometry b = random_convex(&r, hulls, k / 4U + 1U);
        const NphPhysicsPose pb = test_pose(&r, 2.0F);
        const NphPhysicsPose pa = nph_physics_pose(
            nph_vec3_mul_add(pb.position, test_unit(&r), test_range(&r, 3.0F, 6.0F)),
            test_rotation(&r));
        const NphVec3 aim = nph_vec3_add(pb.position, test_vec3(&r, -1.2F, 1.2F));
        const NphVec3 delta =
            nph_vec3_scale(nph_vec3_sub(aim, pa.position), test_range(&r, 0.6F, 2.0F));
        if (gap(&a, pa, &b, pb) <= 0.0F) {
            continue;
        }
        const f32 want = cast_reference(&a, pa, delta, &b, pb);
        NphPhysicsCastHit hit;
        const bool got = nph_physics_shape_cast(&a, pa, delta, &b, pb, &hit);
        if (!got) {
            NTEST_EXPECT_TRUE(want < 0.0F);
            misses += 1U;
            continue;
        }
        // At the hit the two touch: no farther apart than the cast's touch,
        // to GJK's rounding.
        const f32 g = gap(&a, moved(pa, delta, hit.fraction), &b, pb);
        worst_gap = nph_f32_max(worst_gap, g);
        NTEST_EXPECT_TRUE(g <= NPH_PHYSICS_CAST_TOUCH + 2e-6F);
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(hit.normal), 1.0F, 1e-5F);
        NTEST_EXPECT_TRUE(nph_vec3_dot(hit.normal, delta) < 0.0F);
        // The point on b's surface, the normal b's supporting plane's there:
        // none of b lies farther along it (to 0.1 mm, GJK's normal a few
        // millimetres back for polytopes, or the axis test's).
        const NphVec3 p = nph_physics_pose_inverse_transform_point(pb, hit.point);
        const NphVec3 n = nph_physics_pose_inverse_transform_vector(pb, hit.normal);
        NTEST_EXPECT_NEAR_F32(signed_distance(&b, p), 0.0F, 1e-5F);
        NTEST_EXPECT_TRUE(supports_at(&b, p, n, 1e-4F));
        if (want >= 0.0F) {
            // Never past the contact, and short of it only by the touch,
            // along the contact's normal: a cast that grazes its target (a
            // hundredth into it) stops micrometres from it and millimetres
            // of motion short, and the reference's own GJK rounding is
            // micrometres too, a ten-thousandth of the motion there.
            const f32 approach = -nph_vec3_dot(hit.normal, delta);
            NTEST_EXPECT_TRUE((hit.fraction - want) * approach <= 2e-6F + 1e-5F * approach);
            worst_short = nph_f32_max(worst_short, (want - hit.fraction) * approach);
            hits += 1U;
        } else {
            grazes += 1U; // passing within the touch
        }
    }
    NTEST_LOG("shape casts: %u hits (at most %.3g mm short along the normal, %.3g mm apart), %u "
              "misses, %u grazes",
              hits, (f64)worst_short * 1e3, (f64)worst_gap * 1e3, misses, grazes);
    NTEST_EXPECT_TRUE(hits > 40U * TEST_SCALE && misses > 10U * TEST_SCALE);
    NTEST_EXPECT_TRUE(worst_short <= NPH_PHYSICS_CAST_TOUCH + 2e-6F);
    nph_physics_hull_free(hulls[0], alloc);
    nph_physics_hull_free(hulls[1], alloc);
    test_alloc_end(&ta);
}

NTEST(cast, flat_contacts_are_met_exactly)
{
    // Faces meeting faces: the gap crossed along the plane between them
    // ends at the contact itself.
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.25F, 0.5F));
    const NphPhysicsGeometry ground = nph_physics_geometry_box(nph_vec3(5.0F, 1.0F, 5.0F));
    const NphPhysicsPose pg = nph_physics_pose(nph_vec3(0.0F, -1.0F, 0.0F), nph_quat_identity());
    const NphPhysicsGeometry movers[3] = {box, nph_physics_geometry_sphere(0.3F),
                                          nph_physics_geometry_capsule(0.5F, 0.2F)};
    const f32 bottoms[3] = {0.25F, 0.3F, 0.7F};
    for (u32 k = 0U; k < 3U; ++k) {
        const NphPhysicsPose start =
            nph_physics_pose(nph_vec3(0.3F, 2.0F, -0.2F), nph_quat_identity());
        const NphVec3 delta = nph_vec3(0.8F, -4.0F, 0.4F);
        NphPhysicsCastHit hit;
        NTEST_ASSERT_TRUE(nph_physics_shape_cast(&movers[k], start, delta, &ground, pg, &hit));
        NTEST_EXPECT_NEAR_F32(hit.fraction, (2.0F - bottoms[k]) / 4.0F, 2e-6F);
        NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3_unit_y(), 1e-5F));
        NTEST_EXPECT_NEAR_F32(hit.point.y, 0.0F, 1e-5F);
    }
}

// The cases of a box resting on the ground, 2 mm into it (as the solver
// leaves resting contacts).
NTEST(cast, shapes_in_contact_stop_only_when_moving_into_it)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsHull *cube = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(nph_vec3_splat(0.25F), alloc, &cube),
                        NPH_RESULT_OK);
    const NphPhysicsGeometry ground = nph_physics_geometry_box(nph_vec3(5.0F, 1.0F, 5.0F));
    const NphPhysicsPose pg = nph_physics_pose(nph_vec3(0.0F, -1.0F, 0.0F), nph_quat_identity());
    const NphPhysicsGeometry movers[4] = {
        nph_physics_geometry_box(nph_vec3_splat(0.25F)), nph_physics_geometry_sphere(0.25F),
        nph_physics_geometry_capsule(0.3F, 0.25F), nph_physics_geometry_hull(cube)};
    // Each standing with its lowest point 2 mm under the ground's top.
    const f32 heights[4] = {0.248F, 0.248F, 0.548F, 0.248F};
    for (u32 k = 0U; k < 4U; ++k) {
        const NphPhysicsPose start =
            nph_physics_pose(nph_vec3(0.0F, heights[k], 0.0F), nph_quat_identity());
        NphPhysicsCastHit hit;
        // Along the ground, up, and along with a rounding's worth down: free.
        NTEST_EXPECT_FALSE(nph_physics_shape_cast(&movers[k], start, nph_vec3(1.0F, 0.0F, 0.5F),
                                                  &ground, pg, &hit));
        NTEST_EXPECT_FALSE(nph_physics_shape_cast(&movers[k], start, nph_vec3(0.0F, 0.5F, 0.0F),
                                                  &ground, pg, &hit));
        NTEST_EXPECT_FALSE(nph_physics_shape_cast(&movers[k], start, nph_vec3(1.0F, -5e-6F, 0.0F),
                                                  &ground, pg, &hit));
        // Down, or along and down: stopped at once, pushed up.
        const NphVec3 downs[2] = {nph_vec3(0.0F, -0.1F, 0.0F), nph_vec3(1.0F, -0.02F, 0.0F)};
        for (u32 i = 0U; i < 2U; ++i) {
            NTEST_ASSERT_TRUE(
                nph_physics_shape_cast(&movers[k], start, downs[i], &ground, pg, &hit));
            NTEST_EXPECT_NEAR_F32(hit.fraction, 0.0F, 0.0F);
            NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3_unit_y(), 1e-4F));
        }
        // 0.3 mm above it, down by 0.1 m: the gap's fraction exactly; by
        // 0.1 mm: it ends first.
        const NphPhysicsPose above =
            nph_physics_pose(nph_vec3(0.0F, heights[k] + 0.0023F, 0.0F), nph_quat_identity());
        NTEST_ASSERT_TRUE(nph_physics_shape_cast(&movers[k], above, nph_vec3(0.0F, -0.1F, 0.0F),
                                                 &ground, pg, &hit));
        NTEST_EXPECT_NEAR_F32(hit.fraction, 0.003F, 2e-5F);
        NTEST_EXPECT_FALSE(nph_physics_shape_cast(&movers[k], above, nph_vec3(0.0F, -0.0001F, 0.0F),
                                                  &ground, pg, &hit));
    }
    nph_physics_hull_free(cube, alloc);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Shape casts against surfaces
// ---------------------------------------------------------------------------

NTEST(cast, shapes_cross_a_meshs_seams_and_land_on_them)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsHull *cube = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(nph_vec3_splat(0.25F), alloc, &cube),
                        NPH_RESULT_OK);
    // A flat floor of 0.5 m cells whose diagonals alternate, at y = 0, and the
    // same as a heightfield.
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 16U, 0.5F, -4.0F, -4.0F, NULL, false);
    f32 flat[17U * 17U];
    for (u32 i = 0U; i < 17U * 17U; ++i) {
        flat[i] = 0.0F;
    }
    NphPhysicsHeightfield *field = test_heightfield(alloc, 17U, 17U, 0.5F, flat, NULL);
    const NphPhysicsGeometry floors[2] = {nph_physics_geometry_mesh(mesh),
                                          nph_physics_geometry_heightfield(field)};
    const NphPhysicsPose at[2] = {
        nph_physics_pose_identity(),
        nph_physics_pose(nph_vec3(-4.0F, 0.0F, -4.0F), nph_quat_identity())};
    const NphPhysicsGeometry movers[4] = {
        nph_physics_geometry_box(nph_vec3_splat(0.25F)), nph_physics_geometry_sphere(0.25F),
        nph_physics_geometry_capsule(0.3F, 0.25F), nph_physics_geometry_hull(cube)};
    const f32 bottoms[4] = {0.25F, 0.25F, 0.55F, 0.25F};
    // A capsule lying down too.
    const NphQuat lying = nph_quat_from_axis_angle(nph_vec3_unit_z(), 1.5707963F);
    NphPcg32 r = test_rng(109U);
    for (u32 f = 0U; f < 2U; ++f) {
        for (u32 k = 0U; k < 5U; ++k) {
            const NphPhysicsGeometry *g = &movers[k % 4U];
            const NphQuat q = k == 4U ? lying : nph_quat_identity();
            const f32 bottom = k == 4U ? 0.25F : bottoms[k];
            for (u32 i = 0U; i < 24U; ++i) {
                // 2 mm into the floor, along it across many seams: free.
                const NphVec3 p = nph_vec3(test_range(&r, -2.5F, 0.5F), bottom - 0.002F,
                                           test_range(&r, -2.5F, 0.5F));
                const NphPhysicsPose start = nph_physics_pose(p, q);
                const NphVec3 along =
                    nph_vec3(test_range(&r, 1.0F, 2.0F), 0.0F, test_range(&r, -1.0F, 2.0F));
                NphPhysicsCastHit hit;
                NTEST_EXPECT_FALSE(
                    nph_physics_shape_cast(g, start, along, &floors[f], at[f], &hit));
                // From above onto it, wherever it lands (seams and vertices
                // among them): the floor's height and normal.
                const NphPhysicsPose high = nph_physics_pose(nph_vec3(p.x, 1.5F, p.z), q);
                const NphVec3 down = nph_vec3(along.x * 0.3F, -2.0F, along.z * 0.3F);
                NTEST_ASSERT_TRUE(nph_physics_shape_cast(g, high, down, &floors[f], at[f], &hit));
                NTEST_EXPECT_NEAR_F32(hit.fraction, (1.5F - bottom) / 2.0F, 3e-6F);
                NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3_unit_y(), 1e-4F));
                NTEST_EXPECT_NEAR_F32(hit.point.y, 0.0F, 1e-5F);
                NTEST_EXPECT_TRUE(hit.triangle < 2U * 16U * 16U);
                // From below a one-sided floor: nothing.
                const NphPhysicsPose low = nph_physics_pose(nph_vec3(p.x, -1.5F, p.z), q);
                NTEST_EXPECT_FALSE(nph_physics_shape_cast(g, low, nph_vec3(0.0F, 2.0F, 0.0F),
                                                          &floors[f], at[f], &hit));
            }
        }
    }
    nph_physics_mesh_free(mesh, alloc);
    nph_physics_heightfield_free(field, alloc);
    nph_physics_hull_free(cube, alloc);
    test_alloc_end(&ta);
}

NTEST(cast, a_meshs_slopes_and_sides)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // The valley: slopes at 45 degrees meeting at a concave crease on Z.
    NphPhysicsMesh *valley = test_valley_mesh(alloc);
    const NphPhysicsGeometry g = nph_physics_geometry_mesh(valley);
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.25F);
    const NphPhysicsPose id = nph_physics_pose_identity();
    NphPhysicsCastHit hit;
    // A ball moving sideways into the right slope meets it along its
    // normal, (-1, 1, 0) / sqrt 2.
    const NphPhysicsPose start = nph_physics_pose(nph_vec3(-0.5F, 1.5F, 0.0F), nph_quat_identity());
    NTEST_ASSERT_TRUE(
        nph_physics_shape_cast(&ball, start, nph_vec3(3.0F, 0.0F, 0.0F), &g, id, &hit));
    const f32 s = 0.70710678F;
    NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3(-s, s, 0.0F), 1e-4F));
    // Its centre then sits a radius off the slope x = y: (y - x) / sqrt 2.
    const NphVec3 c = nph_vec3_mul_add(start.position, nph_vec3(3.0F, 0.0F, 0.0F), hit.fraction);
    NTEST_EXPECT_NEAR_F32((c.x - c.y) * -s, 0.25F, 1e-4F);
    // Down the crease: both slopes at once, the first found of equals.
    const NphPhysicsPose over = nph_physics_pose(nph_vec3(0.0F, 2.0F, 0.0F), nph_quat_identity());
    NTEST_ASSERT_TRUE(
        nph_physics_shape_cast(&ball, over, nph_vec3(0.0F, -3.0F, 0.0F), &g, id, &hit));
    NTEST_EXPECT_NEAR_F32(hit.fraction, (2.0F - 0.25F / s) / 3.0F, 1e-4F);
    // A two-sided floor meets from below too, its normal facing down.
    NphPhysicsMesh *two = test_grid_mesh(alloc, 4U, 1.0F, -2.0F, -2.0F, NULL, true);
    const NphPhysicsGeometry tg = nph_physics_geometry_mesh(two);
    const NphPhysicsPose below = nph_physics_pose(nph_vec3(0.1F, -1.0F, 0.2F), nph_quat_identity());
    NTEST_ASSERT_TRUE(
        nph_physics_shape_cast(&ball, below, nph_vec3(0.0F, 2.0F, 0.0F), &tg, id, &hit));
    NTEST_EXPECT_NEAR_F32(hit.fraction, 0.375F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3_neg(nph_vec3_unit_y()), 1e-5F));
    nph_physics_mesh_free(valley, alloc);
    nph_physics_mesh_free(two, alloc);
    test_alloc_end(&ta);
}

NTEST(cast, a_heightfield_is_cast_as_its_triangles_built_as_a_mesh)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(113U);
    f32 heights[25U * 25U];
    for (u32 j = 0U; j < 25U; ++j) {
        for (u32 i = 0U; i < 25U; ++i) {
            heights[j * 25U + i] = 0.6F * nph_f32_sin(0.4F * (f32)i) * nph_f32_cos(0.3F * (f32)j) +
                                   test_range(&r, -0.05F, 0.05F);
        }
    }
    NphPhysicsHeightfield *field = test_heightfield(alloc, 25U, 25U, 0.5F, heights, NULL);
    NphPhysicsMesh *mesh = test_heightfield_as_mesh(alloc, field);
    const NphPhysicsGeometry gf = nph_physics_geometry_heightfield(field);
    const NphPhysicsGeometry gm = nph_physics_geometry_mesh(mesh);
    NphPhysicsHull *hulls[2];
    make_hulls(alloc, &r, hulls);
    u32 hits = 0U;
    for (u32 k = 0U; k < 120U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry a = random_convex(&r, hulls, k);
        // Each placed the same way, far from the origin.
        const NphPhysicsPose place =
            nph_physics_pose(nph_vec3(1000.0F, 20.0F, -3000.0F), test_rotation(&r));
        const NphVec3 local = nph_vec3(test_range(&r, 1.0F, 11.0F), test_range(&r, 1.5F, 3.0F),
                                       test_range(&r, 1.0F, 11.0F));
        const NphPhysicsPose pa = nph_physics_pose(nph_physics_pose_transform_point(place, local),
                                                   nph_quat_mul(place.rotation, test_rotation(&r)));
        const NphVec3 delta = nph_physics_pose_transform_vector(
            place, nph_vec3(test_range(&r, -2.0F, 2.0F), -4.0F, test_range(&r, -2.0F, 2.0F)));
        NphPhysicsCastHit hf;
        NphPhysicsCastHit hm;
        const bool got_f = nph_physics_shape_cast(&a, pa, delta, &gf, place, &hf);
        const bool got_m = nph_physics_shape_cast(&a, pa, delta, &gm, place, &hm);
        NTEST_ASSERT_EQ_INT(got_f, got_m);
        if (got_f) {
            NTEST_EXPECT_NEAR_F32(hf.fraction, hm.fraction, 1e-5F);
            // The same triangle (the mesh keeps the cells' order), unless two
            // tie: the walks meet them in different orders.
            if (hf.triangle == hm.triangle) {
                NTEST_EXPECT_TRUE(nph_vec3_dot(hf.normal, hm.normal) >= 0.9998F);
                hits += 1U;
            }
        }
    }
    NTEST_EXPECT_TRUE(hits > 60U * TEST_SCALE);
    nph_physics_hull_free(hulls[0], alloc);
    nph_physics_hull_free(hulls[1], alloc);
    nph_physics_mesh_free(mesh, alloc);
    nph_physics_heightfield_free(field, alloc);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// The surfaces' sweep walks
// ---------------------------------------------------------------------------

#define SWEEP_TRIANGLES (2U * 24U * 24U)

typedef struct Visited {
    u32 count[SWEEP_TRIANGLES];
    NphVec3 v[SWEEP_TRIANGLES][3];
} Visited;

static f32 note_triangle(void *user, u32 t, const NphVec3 v[3], u32 active_edges)
{
    NPH_UNUSED(active_edges);
    Visited *vis = (Visited *)user;
    if (t < SWEEP_TRIANGLES) {
        vis->count[t] += 1U;
        vis->v[t][0] = v[0];
        vis->v[t][1] = v[1];
        vis->v[t][2] = v[2];
    }
    return 1.0F;
}

static bool sweep_meets(const NphVec3 v[3], NphVec3 center, NphVec3 extent, NphVec3 delta)
{
    const NphAABB box = {nph_vec3_sub(nph_vec3_min(nph_vec3_min(v[0], v[1]), v[2]), extent),
                         nph_vec3_add(nph_vec3_max(nph_vec3_max(v[0], v[1]), v[2]), extent)};
    return nph_ray_intersects_aabb(nph_ray(center, delta), box, 1.0F, NULL, NULL);
}

NTEST(cast, sweep_walks_visit_every_triangle_the_swept_box_meets)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(127U);
    f32 heights[25U * 25U];
    for (u32 i = 0U; i < 25U * 25U; ++i) {
        heights[i] = test_range(&r, -1.0F, 1.0F);
    }
    u8 cells[24U * 24U];
    for (u32 i = 0U; i < 24U * 24U; ++i) {
        cells[i] = (u8)((test_range(&r, 0.0F, 1.0F) < 0.1F ? NPH_PHYSICS_HEIGHTFIELD_HOLE : 0U) |
                        (test_range(&r, 0.0F, 1.0F) < 0.5F ? NPH_PHYSICS_HEIGHTFIELD_FLIP : 0U));
    }
    NphPhysicsHeightfield *field = test_heightfield(alloc, 25U, 25U, 0.5F, heights, cells);
    NphPhysicsMesh *mesh = test_heightfield_as_mesh(alloc, field);
    static Visited vis;
    const NphDVec3 origin = nph_dvec3(1.25, 0.5, -0.75);
    for (u32 k = 0U; k < 100U * TEST_SCALE; ++k) {
        const bool use_mesh = (k & 1U) != 0U;
        const NphVec3 center = nph_vec3(test_range(&r, -2.0F, 14.0F), test_range(&r, -2.0F, 3.0F),
                                        test_range(&r, -2.0F, 14.0F));
        const NphVec3 extent = test_vec3(&r, 0.0F, 0.8F);
        const NphVec3 delta = test_vec3(&r, -8.0F, 8.0F);
        memset(&vis, 0, sizeof vis);
        if (use_mesh) {
            nph_physics_mesh_visit_sweep(mesh, center, extent, delta, origin, note_triangle, &vis);
        } else {
            nph_physics_heightfield_visit_sweep(field, center, extent, delta, origin, note_triangle,
                                                &vis);
        }
        const u32 n =
            use_mesh ? mesh->triangle_count : nph_physics_heightfield_triangle_count(field);
        for (u32 t = 0U; t < n; ++t) {
            NphVec3 v[3];
            if (use_mesh) {
                nph_physics_mesh_triangle(mesh, t, v);
            } else if (!nph_physics_heightfield_triangle(field, t, v, NULL)) {
                NTEST_EXPECT_EQ_UINT(vis.count[t], 0U); // a hole's
                continue;
            }
            NTEST_EXPECT_LE_UINT(vis.count[t], 1U);
            if (sweep_meets(v, center, extent, delta)) {
                NTEST_EXPECT_EQ_UINT(vis.count[t], 1U);
            }
            if (vis.count[t] == 1U) {
                // Its vertices less the origin, in 64 bits.
                for (u32 i = 0U; i < 3U; ++i) {
                    NTEST_EXPECT_TRUE(nph_vec3_near(
                        vis.v[t][i], nph_physics_relative_vertex(v[i], origin), 1e-6F));
                }
            }
        }
    }
    nph_physics_mesh_free(mesh, alloc);
    nph_physics_heightfield_free(field, alloc);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Overlaps
// ---------------------------------------------------------------------------

NTEST(cast, overlaps_agree_with_distances)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(131U);
    NphPhysicsHull *hulls[2];
    make_hulls(alloc, &r, hulls);
    f32 heights[17U * 17U];
    for (u32 i = 0U; i < 17U * 17U; ++i) {
        heights[i] = test_range(&r, -0.5F, 0.5F);
    }
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 16U, 0.5F, 0.0F, 0.0F, heights, false);
    u32 touching = 0U;
    u32 apart = 0U;
    for (u32 k = 0U; k < 200U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry a = random_convex(&r, hulls, k);
        const NphPhysicsGeometry b = random_convex(&r, hulls, k / 4U + 3U);
        const NphPhysicsPose pa = test_pose(&r, 1.2F);
        const NphPhysicsPose pb = test_pose(&r, 1.2F);
        const bool want = gap(&a, pa, &b, pb) <= 0.0F;
        NTEST_EXPECT_EQ_INT(nph_physics_overlap(&a, pa, &b, pb), want);
        touching += want ? 1U : 0U;
        apart += want ? 0U : 1U;
        // Against the mesh: any triangle within reach, from either side.
        const NphPhysicsPose pm =
            nph_physics_pose(nph_vec3(test_range(&r, 0.5F, 7.5F), test_range(&r, -0.8F, 0.8F),
                                      test_range(&r, 0.5F, 7.5F)),
                             test_rotation(&r));
        bool want_mesh = false;
        for (u32 t = 0U; t < mesh->triangle_count && !want_mesh; ++t) {
            NphVec3 v[3];
            nph_physics_mesh_triangle(mesh, t, v);
            NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE];
            const NphPhysicsProxy xa = nph_physics_geometry_proxy(&a, storage);
            const NphPhysicsProxy xt = {v, 3U, 0.0F};
            NphPhysicsDistance d;
            nph_physics_distance(&xa, pm, &xt, nph_physics_pose_identity(), true, NULL, &d);
            want_mesh = d.distance <= 0.0F;
        }
        const NphPhysicsGeometry gm = nph_physics_geometry_mesh(mesh);
        NTEST_EXPECT_EQ_INT(nph_physics_overlap(&a, pm, &gm, nph_physics_pose_identity()),
                            want_mesh);
    }
    NTEST_EXPECT_TRUE(touching > 20U * TEST_SCALE && apart > 20U * TEST_SCALE);
    nph_physics_hull_free(hulls[0], alloc);
    nph_physics_hull_free(hulls[1], alloc);
    nph_physics_mesh_free(mesh, alloc);
    test_alloc_end(&ta);
}

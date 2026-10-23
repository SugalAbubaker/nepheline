// SPDX-License-Identifier: Apache-2.0
// Geometry: mass properties (closed forms against a numeric integration,
// transforms and sums), bounds, support points, and proxies.

#include "test_physics_support.h"

// Mass properties by midpoint integration over a grid in the box
// [-extent, extent]^3, for a solid given by an inside test.
typedef bool (*InsideFn)(const void *shape, NphVec3 p);

static NphPhysicsMassData integrate(InsideFn inside, const void *shape, NphVec3 extent, u32 n)
{
    f64 mass = 0.0;
    f64 sx = 0.0;
    f64 sy = 0.0;
    f64 sz = 0.0;
    f64 xx = 0.0;
    f64 yy = 0.0;
    f64 zz = 0.0;
    f64 xy = 0.0;
    f64 yz = 0.0;
    f64 zx = 0.0;
    const f64 dx = 2.0 * (f64)extent.x / (f64)n;
    const f64 dy = 2.0 * (f64)extent.y / (f64)n;
    const f64 dz = 2.0 * (f64)extent.z / (f64)n;
    const f64 dv = dx * dy * dz;
    for (u32 k = 0U; k < n; ++k) {
        for (u32 j = 0U; j < n; ++j) {
            for (u32 i = 0U; i < n; ++i) {
                const f64 x = -(f64)extent.x + ((f64)i + 0.5) * dx;
                const f64 y = -(f64)extent.y + ((f64)j + 0.5) * dy;
                const f64 z = -(f64)extent.z + ((f64)k + 0.5) * dz;
                if (!inside(shape, nph_vec3((f32)x, (f32)y, (f32)z))) {
                    continue;
                }
                mass += dv;
                sx += x * dv;
                sy += y * dv;
                sz += z * dv;
                xx += x * x * dv;
                yy += y * y * dv;
                zz += z * z * dv;
                xy += x * y * dv;
                yz += y * z * dv;
                zx += z * x * dv;
            }
        }
    }
    NphPhysicsMassData m;
    m.mass = (f32)mass;
    m.center = nph_vec3((f32)(sx / mass), (f32)(sy / mass), (f32)(sz / mass));
    // About the origin; the shapes tested are centred on it.
    m.inertia = nph_mat3_from_cols(nph_vec3((f32)(yy + zz), (f32)(-xy), (f32)(-zx)),
                                   nph_vec3((f32)(-xy), (f32)(xx + zz), (f32)(-yz)),
                                   nph_vec3((f32)(-zx), (f32)(-yz), (f32)(xx + yy)));
    return m;
}

static bool inside_capsule(const void *shape, NphVec3 p)
{
    const NphPhysicsCapsule *c = (const NphPhysicsCapsule *)shape;
    f32 t;
    const NphVec3 q = nph_closest_point_on_segment(nph_vec3(0.0F, -c->half_height, 0.0F),
                                                   nph_vec3(0.0F, c->half_height, 0.0F), p, &t);
    return nph_vec3_distance_sq(p, q) <= c->radius * c->radius;
}

static bool inside_sphere(const void *shape, NphVec3 p)
{
    const f32 r = *(const f32 *)shape;
    return nph_vec3_length_sq(p) <= r * r;
}

static void expect_mass_near(NphPhysicsMassData got, NphPhysicsMassData want, f32 rel)
{
    NTEST_EXPECT_NEAR_F32(got.mass, want.mass, rel * want.mass);
    const f32 scale =
        nph_f32_max(want.inertia.c[0].x, nph_f32_max(want.inertia.c[1].y, want.inertia.c[2].z));
    for (u32 c = 0U; c < 3U; ++c) {
        NTEST_EXPECT_TRUE(nph_vec3_near(got.inertia.c[c], want.inertia.c[c], rel * scale));
    }
}

NTEST(geometry, sphere_and_capsule_mass_match_integration)
{
    // 160^3 midpoint samples: about 0.5 % error on these solids.
    const u32 n = NPH_CONFIG_DEBUG ? 96U : 160U;
    const f32 r = 0.6F;
    const NphPhysicsGeometry sphere = nph_physics_geometry_sphere(r);
    expect_mass_near(nph_physics_geometry_mass(&sphere, 1.0F),
                     integrate(inside_sphere, &r, nph_vec3_splat(r), n), 0.02F);
    const NphPhysicsCapsule cap = {0.8F, 0.4F};
    const NphPhysicsGeometry capsule = nph_physics_geometry_capsule(cap.half_height, cap.radius);
    expect_mass_near(nph_physics_geometry_mass(&capsule, 1.0F),
                     integrate(inside_capsule, &cap, nph_vec3(0.4F, 1.2F, 0.4F), n), 0.02F);
}

NTEST(geometry, closed_forms)
{
    const NphPhysicsGeometry s = nph_physics_geometry_sphere(2.0F);
    const NphPhysicsMassData ms = nph_physics_geometry_mass(&s, 3.0F);
    NTEST_EXPECT_NEAR_F32(ms.mass, 3.0F * 4.0F / 3.0F * 3.14159265F * 8.0F, 1e-3F);
    NTEST_EXPECT_NEAR_F32(ms.inertia.c[0].x, 0.4F * ms.mass * 4.0F, 1e-3F);
    // A capsule without height is a sphere.
    const NphPhysicsGeometry c0 = nph_physics_geometry_capsule(0.0F, 2.0F);
    const NphPhysicsMassData mc0 = nph_physics_geometry_mass(&c0, 3.0F);
    NTEST_EXPECT_NEAR_F32(mc0.mass, ms.mass, 1e-3F);
    NTEST_EXPECT_NEAR_F32(mc0.inertia.c[0].x, ms.inertia.c[0].x, 1e-2F);
    NTEST_EXPECT_NEAR_F32(mc0.inertia.c[1].y, ms.inertia.c[1].y, 1e-2F);
    const NphPhysicsGeometry b = nph_physics_geometry_box(nph_vec3(1.0F, 2.0F, 3.0F));
    const NphPhysicsMassData mb = nph_physics_geometry_mass(&b, 0.5F);
    NTEST_EXPECT_NEAR_F32(mb.mass, 24.0F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(mb.inertia.c[0].x, 24.0F / 3.0F * 13.0F, 1e-3F);
    NTEST_EXPECT_NEAR_F32(mb.inertia.c[1].y, 24.0F / 3.0F * 10.0F, 1e-3F);
    NTEST_EXPECT_NEAR_F32(mb.inertia.c[2].z, 24.0F / 3.0F * 5.0F, 1e-3F);
    NTEST_EXPECT_NEAR_F32(mb.inertia.c[0].y, 0.0F, 0.0F);
}

NTEST(geometry, mass_transform_and_sum)
{
    // Two unit-mass point-like spheres one metre either side of the origin
    // along X: the sum's tensor gains 2 kg m^2 about Y and Z (parallel
    // axis), none about X.
    const NphPhysicsGeometry s = nph_physics_geometry_sphere(0.1F);
    NphPhysicsMassData one = nph_physics_geometry_mass(&s, 1.0F);
    const f32 k = 1.0F / one.mass;
    one.mass *= k;
    one.inertia = nph_mat3_scale(one.inertia, k);
    const NphPhysicsMassData left = nph_physics_mass_transform(
        one, nph_physics_pose(nph_vec3(-1.0F, 0.0F, 0.0F), nph_quat_identity()));
    const NphPhysicsMassData right = nph_physics_mass_transform(
        one, nph_physics_pose(nph_vec3(1.0F, 0.0F, 0.0F), nph_quat_identity()));
    const NphPhysicsMassData sum = nph_physics_mass_add(left, right);
    NTEST_EXPECT_NEAR_F32(sum.mass, 2.0F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(sum.center, nph_vec3_zero(), 1e-6F));
    NTEST_EXPECT_NEAR_F32(sum.inertia.c[0].x, 2.0F * one.inertia.c[0].x, 1e-6F);
    NTEST_EXPECT_NEAR_F32(sum.inertia.c[1].y, 2.0F * one.inertia.c[1].y + 2.0F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(sum.inertia.c[2].z, 2.0F * one.inertia.c[2].z + 2.0F, 1e-5F);
    // A zero mass contributes nothing.
    NphPhysicsMassData none;
    memset(&none, 0, sizeof none);
    const NphPhysicsMassData same = nph_physics_mass_add(none, left);
    NTEST_EXPECT_TRUE(nph_vec3_eq(same.center, left.center));
    // Rotating a box a quarter turn about Z swaps its X and Y moments.
    const NphPhysicsGeometry b = nph_physics_geometry_box(nph_vec3(1.0F, 2.0F, 3.0F));
    const NphPhysicsMassData mb = nph_physics_geometry_mass(&b, 1.0F);
    const NphPhysicsMassData rb = nph_physics_mass_transform(
        mb, nph_physics_pose(nph_vec3_zero(), nph_quat_from_rotation_z(1.5707963F)));
    NTEST_EXPECT_NEAR_F32(rb.inertia.c[0].x, mb.inertia.c[1].y, 1e-3F);
    NTEST_EXPECT_NEAR_F32(rb.inertia.c[1].y, mb.inertia.c[0].x, 1e-3F);
    NTEST_EXPECT_NEAR_F32(rb.inertia.c[2].z, mb.inertia.c[2].z, 1e-3F);
}

// The box is tight: along each axis, the support point in that direction
// reaches the box's face.
static void expect_tight_aabb(const NphPhysicsGeometry *g, NphPhysicsPose pose)
{
    const NphAABB box = nph_physics_geometry_aabb(g, pose);
    const NphVec3 axes[3] = {nph_vec3_unit_x(), nph_vec3_unit_y(), nph_vec3_unit_z()};
    for (u32 a = 0U; a < 3U; ++a) {
        for (u32 s = 0U; s < 2U; ++s) {
            const NphVec3 d = s == 0U ? axes[a] : nph_vec3_neg(axes[a]);
            const NphVec3 local_d = nph_quat_rotate_inverse(pose.rotation, d);
            const NphVec3 p =
                nph_physics_pose_transform_point(pose, nph_physics_geometry_support(g, local_d));
            const f32 face = s == 0U ? nph_vec3_at(box.max, a) : nph_vec3_at(box.min, a);
            NTEST_EXPECT_NEAR_F32(nph_vec3_at(p, a), face, 1e-4F);
        }
    }
}

NTEST(geometry, aabbs_are_tight)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 rng = test_rng(3U);
    NphPhysicsHull *hull = test_random_hull(alloc, &rng, 60U, 1.0F, nph_vec3(1.0F, 2.0F, 0.5F));
    const NphPhysicsGeometry shapes[4] = {
        nph_physics_geometry_sphere(0.7F), nph_physics_geometry_capsule(1.1F, 0.3F),
        nph_physics_geometry_box(nph_vec3(0.4F, 0.9F, 1.6F)), nph_physics_geometry_hull(hull)};
    for (u32 round = 0U; round < 50U; ++round) {
        const NphPhysicsPose pose = test_pose(&rng, 10.0F);
        for (u32 s = 0U; s < 4U; ++s) {
            NTEST_ASSERT_TRUE(nph_physics_geometry_is_valid(&shapes[s]));
            expect_tight_aabb(&shapes[s], pose);
        }
    }
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(geometry, proxies_reproduce_the_support)
{
    const NphPhysicsGeometry shapes[3] = {nph_physics_geometry_sphere(0.7F),
                                          nph_physics_geometry_capsule(1.1F, 0.3F),
                                          nph_physics_geometry_box(nph_vec3(0.4F, 0.9F, 1.6F))};
    NphPcg32 rng = test_rng(4U);
    for (u32 s = 0U; s < 3U; ++s) {
        NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE];
        const NphPhysicsProxy p = nph_physics_geometry_proxy(&shapes[s], storage);
        for (u32 k = 0U; k < 100U; ++k) {
            const NphVec3 d = test_unit(&rng);
            const NphVec3 core = p.points[nph_physics_proxy_support(&p, d)];
            const NphVec3 via_proxy = nph_vec3_mul_add(core, d, p.radius);
            const NphVec3 direct = nph_physics_geometry_support(&shapes[s], d);
            NTEST_EXPECT_NEAR_F32(nph_vec3_dot(via_proxy, d), nph_vec3_dot(direct, d), 1e-5F);
        }
    }
}

NTEST(geometry, validity)
{
    NphPhysicsGeometry g = nph_physics_geometry_sphere(0.0F);
    NTEST_EXPECT_FALSE(nph_physics_geometry_is_valid(&g));
    g = nph_physics_geometry_sphere(nph_f32_from_bits(0x7F800000U));
    NTEST_EXPECT_FALSE(nph_physics_geometry_is_valid(&g));
    g = nph_physics_geometry_capsule(-1.0F, 1.0F);
    NTEST_EXPECT_FALSE(nph_physics_geometry_is_valid(&g));
    g = nph_physics_geometry_capsule(0.0F, 1.0F);
    NTEST_EXPECT_TRUE(nph_physics_geometry_is_valid(&g));
    g = nph_physics_geometry_box(nph_vec3(1.0F, -1.0F, 1.0F));
    NTEST_EXPECT_FALSE(nph_physics_geometry_is_valid(&g));
    g = nph_physics_geometry_hull(NULL);
    NTEST_EXPECT_FALSE(nph_physics_geometry_is_valid(&g));
    NTEST_EXPECT_FALSE(nph_physics_geometry_is_valid(NULL));
    const NphPhysicsGeometry b = nph_physics_geometry_box(nph_vec3(0.5F, 1.0F, 2.0F));
    NTEST_EXPECT_NEAR_F32(nph_physics_geometry_inner_radius(&b), 0.5F, 0.0F);
    NTEST_EXPECT_NEAR_F32(nph_physics_geometry_outer_radius(&b), nph_f32_sqrt(5.25F), 1e-6F);
}

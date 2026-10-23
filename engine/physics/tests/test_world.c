// SPDX-License-Identifier: Apache-2.0
// The world: integration against closed forms, resting and stacking
// stability, friction, restitution, momentum, sleep and wake, events,
// filters, destruction during contact, kinematic bodies, the gyroscopic
// term, invariants, the 64-bit world, the ISA kernels, and determinism
// across worker counts.

#include <nepheline/math/kernels.h>
#include <nepheline/platform/cpu.h>

#include "test_world_support.h"

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

NTEST(world, create_and_reject_bad_descriptions)
{
    Scene s;
    scene_begin(&s);
    NphPhysicsWorldDesc bad = nph_physics_world_desc_default();
    NphPhysicsWorld *w = NULL;
    NTEST_EXPECT_EQ_INT(nph_physics_world_create(&bad, &w),
                        NPH_RESULT_INVALID_ARGUMENT); // no alloc
    bad.alloc = s.alloc;
    bad.substeps = 0U;
    NTEST_EXPECT_EQ_INT(nph_physics_world_create(&bad, &w), NPH_RESULT_INVALID_ARGUMENT);
    bad.substeps = 4U;
    bad.contact_hertz = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_world_create(&bad, &w), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(w);
    NTEST_EXPECT_EQ_INT(nph_physics_world_step(s.w, 0.0F, NULL), NPH_RESULT_INVALID_ARGUMENT);
    // An empty world steps.
    step(s.w, NULL, 3U);
    // Handles go stale.
    const NphPhysicsBody b = add_box(s.w, nph_dvec3(0.0, 5.0, 0.0), nph_vec3_splat(0.5F));
    NTEST_EXPECT_TRUE(nph_physics_body_is_valid(s.w, b));
    nph_physics_body_destroy(s.w, b);
    NTEST_EXPECT_FALSE(nph_physics_body_is_valid(s.w, b));
    const NphPhysicsBody b2 = add_box(s.w, nph_dvec3(0.0, 5.0, 0.0), nph_vec3_splat(0.5F));
    NTEST_EXPECT_NE_UINT(b2.value, b.value); // same slot, new generation
    NTEST_EXPECT_FALSE(nph_physics_body_is_valid(s.w, b));
    scene_end(&s);
}

NTEST(world, free_fall_matches_the_integrator)
{
    Scene s;
    scene_begin(&s);
    const NphPhysicsBody b =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 100.0, 0.0), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.5F), 0.6F, 0.0F);
    step(s.w, NULL, 240U);
    // Semi-implicit Euler over n substeps of h: v = g n h, y = y0 + g h^2 n (n + 1) / 2.
    const f64 n = 240.0 * 4.0;
    const f64 h = 1.0 / 960.0;
    const NphVec3 v = nph_physics_body_linear_velocity(s.w, b);
    NTEST_EXPECT_NEAR_F32(v.y, (f32)(-9.81 * n * h), 1e-3F);
    const NphDVec3 p = nph_physics_body_position(s.w, b);
    NTEST_EXPECT_NEAR_F64(p.y, 100.0 - 9.81 * h * h * n * (n + 1.0) / 2.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(p.x, 0.0, 0.0);
    scene_end(&s);
}

NTEST(world, a_dropped_sphere_comes_to_rest_and_sleeps)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    const NphPhysicsBody b =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 2.0, 0.0), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.5F), 0.6F, 0.0F);
    step(s.w, NULL, 480U);
    const NphDVec3 p = nph_physics_body_position(s.w, b);
    NTEST_EXPECT_NEAR_F64(p.y, 0.5, 0.01);
    NTEST_EXPECT_TRUE(nph_vec3_length(nph_physics_body_linear_velocity(s.w, b)) < 0.01F);
    step(s.w, NULL, 240U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, b));
    scene_end(&s);
}

NTEST(world, a_resting_box_does_not_drift)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(1.0, 0.5, -2.0);
    bd.enable_sleep = 0;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &b), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3_splat(0.5F)));
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, b, &sd, &shape), NPH_RESULT_OK);
    step(s.w, NULL, 240U * 5U);
    const NphDVec3 p = nph_physics_body_position(s.w, b);
    NTEST_EXPECT_NEAR_F64(p.x, 1.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(p.z, -2.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(p.y, 0.5, 6e-3);
    const NphQuat q = nph_physics_body_rotation(s.w, b);
    NTEST_EXPECT_TRUE(nph_quat_angle(q) < 1e-3F);
    scene_end(&s);
}

// Warm starting by place (contact.c): a point whose id changes but whose
// place does not keeps its impulses. Of three twins of a resting box, the
// second's stored points lose their ids and the third's also move 2 cm,
// beyond the matching distance: the second steps as the first, to the bit,
// and the third, its contact started cold, does not.
NTEST(world, points_keep_their_impulses_when_their_ids_change)
{
    Scene s[3];
    for (u32 k = 0U; k < 3U; ++k) {
        NphPhysicsWorldDesc d = nph_physics_world_desc_default();
        d.enable_sleep = 0;
        scene_begin_with(&s[k], d);
        add_ground(s[k].w, 0.0);
        add_body(s[k].w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.3, 0.3, -0.2),
                 nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.4F),
                 nph_physics_geometry_box(nph_vec3(0.5F, 0.3F, 0.4F)), 0.6F, 0.0F);
        step(s[k].w, NULL, 120U);
        NTEST_ASSERT_EQ_UINT(s[k].w->awake_contacts.len, 1U);
        NTEST_ASSERT_EQ_UINT(s[k].w->contacts.data[s[k].w->awake_contacts.data[0]].point_count, 4U);
    }
    NTEST_ASSERT_EQ_UINT(nph_physics_world_hash(s[0].w), nph_physics_world_hash(s[1].w));
    scramble_contact_points(s[1].w, nph_vec3_zero());
    scramble_contact_points(s[2].w, nph_vec3(0.02F, 0.0F, 0.0F));
    for (u32 k = 0U; k < 3U; ++k) {
        step(s[k].w, NULL, 1U);
    }
    NTEST_EXPECT_EQ_UINT(nph_physics_world_hash(s[0].w), nph_physics_world_hash(s[1].w));
    NTEST_EXPECT_NE_UINT(nph_physics_world_hash(s[0].w), nph_physics_world_hash(s[2].w));
    for (u32 k = 0U; k < 3U; ++k) {
        scene_end(&s[k]);
    }
}

static void build_stack(NphPhysicsWorld *w, u32 count, NphDVec3 base, NphPhysicsBody *out)
{
    for (u32 i = 0U; i < count; ++i) {
        out[i] = add_box(w, nph_dvec3_add(base, nph_dvec3(0.0, 0.5 + (f64)i * 1.0, 0.0)),
                         nph_vec3_splat(0.5F));
    }
}

// ---------------------------------------------------------------------------
// Compounds: bodies with several shapes (ADR-0046 §11)
// ---------------------------------------------------------------------------

// A shape of `g` at `at` on `body`, turned by `q`.
static void add_part(NphPhysicsWorld *w, NphPhysicsBody body, NphPhysicsGeometry g, NphVec3 at,
                     NphQuat q)
{
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    sd.pose = nph_physics_pose(at, q);
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(w, body, &sd, &shape), NPH_RESULT_OK);
}

// A table: a top 1 m by 0.6 m by 5 cm on four legs 70 cm tall, its frame
// at the floor under its middle.
static NphPhysicsBody add_table(NphPhysicsWorld *w, NphDVec3 at, NphQuat q)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = at;
    bd.rotation = q;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(w, &bd, &b), NPH_RESULT_OK);
    add_part(w, b, nph_physics_geometry_box(nph_vec3(0.5F, 0.025F, 0.3F)),
             nph_vec3(0.0F, 0.725F, 0.0F), nph_quat_identity());
    for (u32 k = 0U; k < 4U; ++k) {
        const f32 x = (k & 1U) != 0U ? 0.45F : -0.45F;
        const f32 z = (k & 2U) != 0U ? 0.25F : -0.25F;
        add_part(w, b, nph_physics_geometry_box(nph_vec3(0.03F, 0.35F, 0.03F)),
                 nph_vec3(x, 0.35F, z), nph_quat_identity());
    }
    return b;
}

NTEST(world, compounds_sum_their_shapes_mass)
{
    Scene s;
    scene_begin(&s);
    // Two cubes 2 m apart and a ball between, turned: the parts' mass
    // properties, moved into the body's frame and summed.
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &b), NPH_RESULT_OK);
    const NphQuat turn =
        nph_quat_from_axis_angle(nph_vec3_normalize(nph_vec3(1.0F, 2.0F, 3.0F)), 0.7F);
    const NphPhysicsGeometry cube = nph_physics_geometry_box(nph_vec3_splat(0.5F));
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.4F);
    const NphPhysicsPose poses[3] = {
        nph_physics_pose(nph_vec3(-1.0F, 0.0F, 0.0F), turn),
        nph_physics_pose(nph_vec3(1.0F, 0.0F, 0.0F), nph_quat_identity()),
        nph_physics_pose(nph_vec3(0.0F, 0.3F, 0.2F), nph_quat_identity())};
    const NphPhysicsGeometry *parts[3] = {&cube, &cube, &ball};
    NphPhysicsMassData want = {0.0F, nph_vec3_zero(), nph_mat3_zero()};
    for (u32 k = 0U; k < 3U; ++k) {
        add_part(s.w, b, *parts[k], poses[k].position, poses[k].rotation);
        want = nph_physics_mass_add(
            want,
            nph_physics_mass_transform(nph_physics_geometry_mass(parts[k], 1000.0F), poses[k]));
    }
    const NphPhysicsMassData got = nph_physics_body_mass_data(s.w, b);
    NTEST_EXPECT_NEAR_F32(got.mass, 2000.0F + 1000.0F * 4.0F / 3.0F * 3.14159265F * 0.064F, 0.5F);
    NTEST_EXPECT_NEAR_F32(got.mass, want.mass, 1e-3F);
    NTEST_EXPECT_TRUE(nph_vec3_near(got.center, want.center, 1e-5F));
    for (u32 c = 0U; c < 3U; ++c) {
        NTEST_EXPECT_TRUE(nph_vec3_near(got.inertia.c[c], want.inertia.c[c], 1e-2F));
    }
    // The centre of mass is the body's, in the world.
    const NphDVec3 com = nph_physics_body_center_of_mass(s.w, b);
    NTEST_EXPECT_NEAR_F64(com.x, (f64)want.center.x, 1e-5);
    NTEST_EXPECT_NEAR_F64(com.y, (f64)want.center.y, 1e-5);
    NTEST_EXPECT_NEAR_F64(com.z, (f64)want.center.z, 1e-5);
    scene_end(&s);
}

NTEST(world, a_table_stands_on_its_legs_and_sleeps)
{
    // On a box ground and on a heightfield: a compound's every shape has
    // its own contacts, and only the legs touch.
    TestAlloc fta;
    NphAllocator *fa = test_alloc_begin(&fta);
    static f32 flat[17U * 17U];
    NphPhysicsHeightfield *field = test_heightfield(fa, 17U, 17U, 0.5F, flat, NULL);
    for (u32 floor = 0U; floor < 2U; ++floor) {
        Scene s;
        scene_begin(&s);
        if (floor == 0U) {
            add_ground(s.w, 0.0);
        } else {
            add_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(-4.0, 0.0, -4.0), nph_quat_identity(),
                     nph_physics_geometry_heightfield(field), 0.6F, 0.0F);
        }
        const NphQuat turn = nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.3F);
        const NphPhysicsBody table = add_table(s.w, nph_dvec3(0.2, 0.05, -0.1), turn);
        for (u32 i = 0U; i < 240U * 3U; ++i) {
            step(s.w, NULL, 1U);
            if (i == 120U) {
                // Four legs on the floor, four points each: the top touches
                // nothing.
                const NphPhysicsStats st = nph_physics_world_stats(s.w);
                NTEST_EXPECT_EQ_UINT(st.touching_contacts, 4U);
                NTEST_EXPECT_EQ_UINT(st.manifold_points, 16U);
            }
        }
        NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, table));
        const NphDVec3 p = nph_physics_body_position(s.w, table);
        NTEST_EXPECT_NEAR_F64(p.x, 0.2, 2e-3);
        NTEST_EXPECT_NEAR_F64(p.z, -0.1, 2e-3);
        NTEST_EXPECT_NEAR_F64(p.y, 0.0, 6e-3); // its frame at the floor
        const NphQuat q = nph_physics_body_rotation(s.w, table);
        NTEST_EXPECT_TRUE(nph_quat_angle(nph_quat_mul(nph_quat_conjugate(turn), q)) < 2e-3F);
        scene_end(&s);
    }
    nph_physics_heightfield_free(field, fa);
    test_alloc_end(&fta);
}

NTEST(world, a_compounds_shapes_never_touch_each_other)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // Two balls overlapping on one body, and a third body far off: the only
    // pairs are each ball with the ground.
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(0.0, 0.3, 0.0);
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &b), NPH_RESULT_OK);
    add_part(s.w, b, nph_physics_geometry_sphere(0.3F), nph_vec3(-0.1F, 0.0F, 0.0F),
             nph_quat_identity());
    add_part(s.w, b, nph_physics_geometry_sphere(0.3F), nph_vec3(0.1F, 0.0F, 0.0F),
             nph_quat_identity());
    step(s.w, NULL, 60U); // resting, not yet asleep
    const NphPhysicsStats st = nph_physics_world_stats(s.w);
    NTEST_EXPECT_EQ_UINT(st.contacts, 2U);
    NTEST_EXPECT_EQ_UINT(st.touching_contacts, 2U);
    step(s.w, NULL, 180U);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, b).y, 0.3, 6e-3);
    scene_end(&s);
}

NTEST(world, a_stack_of_ten_boxes_stands)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    NphPhysicsBody boxes[10];
    build_stack(s.w, 10U, nph_dvec3_zero(), boxes);
    step(s.w, NULL, 240U * 6U);
    for (u32 i = 0U; i < 10U; ++i) {
        const NphDVec3 p = nph_physics_body_position(s.w, boxes[i]);
        NTEST_EXPECT_NEAR_F64(p.x, 0.0, 0.02);
        NTEST_EXPECT_NEAR_F64(p.z, 0.0, 0.02);
        NTEST_EXPECT_NEAR_F64(p.y, 0.5 + (f64)i, 0.03);
    }
    // It settles and sleeps.
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, boxes[9]));
    scene_end(&s);
}

NTEST(world, a_pyramid_stands)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // 20 rows (210 boxes) where the build is fast enough, 10 in Debug.
    const u32 rows = TEST_SCALE >= 8U ? 20U : 10U;
    NphPhysicsBody top = NPH_PHYSICS_BODY_NULL;
    for (u32 row = 0U; row < rows; ++row) {
        const u32 count = rows - row;
        for (u32 i = 0U; i < count; ++i) {
            const f64 x = ((f64)i - (f64)(count - 1U) * 0.5) * 1.05;
            top = add_box(s.w, nph_dvec3(x, 0.5 + (f64)row, 0.0), nph_vec3_splat(0.5F));
        }
    }
    step(s.w, NULL, 240U * 5U);
    const NphDVec3 p = nph_physics_body_position(s.w, top);
    NTEST_LOG("%u rows: the top box at x %.4f, %.4f below its place", rows, p.x,
              0.5 + (f64)(rows - 1U) - p.y);
    // Soft contacts give a little under load (each layer's share, load /
    // mass * g / omega^2: below a millimetre at 120 Hz), and nothing creeps
    // sideways.
    NTEST_EXPECT_NEAR_F64(p.x, 0.0, 0.003);
    NTEST_EXPECT_NEAR_F64(p.y, 0.5 + (f64)(rows - 1U), 0.005);
    // Still, it falls asleep: nothing jitters.
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, top));
    scene_end(&s);
}

NTEST(world, a_heavy_box_rests_on_a_light_one)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // A 100:1 mass ratio: the light box carries the heavy one.
    const NphPhysicsBody light = add_box(s.w, nph_dvec3(0.0, 0.5, 0.0), nph_vec3_splat(0.5F));
    const NphPhysicsBody heavy = add_box(s.w, nph_dvec3(0.0, 1.5, 0.0), nph_vec3_splat(0.5F));
    NphPhysicsMassData m = nph_physics_body_mass_data(s.w, heavy);
    m.mass *= 100.0F;
    m.inertia = nph_mat3_scale(m.inertia, 100.0F);
    nph_physics_body_set_mass_data(s.w, heavy, &m);
    step(s.w, NULL, 240U * 3U);
    const f64 light_sink = 0.5 - nph_physics_body_position(s.w, light).y;
    const f64 heavy_sink = 1.5 - nph_physics_body_position(s.w, heavy).y;
    NTEST_LOG("the light box sinks %.2f mm, the heavy one %.2f mm", light_sink * 1e3,
              heavy_sink * 1e3);
    // A soft contact holds its load at a depth of load / effective mass *
    // g / omega^2: 101 g / (2 pi 240 Hz)^2 = 0.44 mm under the light box,
    // 100 g / (2 pi 120 Hz)^2 = 1.7 mm more between the two.
    NTEST_EXPECT_TRUE(light_sink < 0.001);
    NTEST_EXPECT_TRUE(heavy_sink < 0.004);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, heavy).x, 0.0, 1e-3);
    scene_end(&s);
}

// A box on a static ramp of angle theta with friction mu on both surfaces:
// it holds when tan(theta) < mu and slides at g (sin - mu cos) otherwise.
static f32 ramp_slide(f32 theta, f32 mu, f32 seconds, f32 *out_accel)
{
    Scene s;
    scene_begin(&s);
    const NphQuat q = nph_quat_from_rotation_z(theta);
    add_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), q,
             nph_physics_geometry_box(nph_vec3(20.0F, 0.5F, 5.0F)), mu, 0.0F);
    const NphVec3 up = nph_quat_rotate(q, nph_vec3_unit_y());
    const NphVec3 along = nph_quat_rotate(q, nph_vec3_unit_x());
    const NphDVec3 start = nph_dvec3_from_vec3(nph_vec3_scale(up, 0.5F + 0.25F - 0.001F));
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = start;
    bd.rotation = q;
    bd.enable_sleep = 0;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &b), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3(0.25F, 0.25F, 0.25F)));
    sd.friction = mu;
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, b, &sd, &shape), NPH_RESULT_OK);
    // Let it settle onto the ramp, then measure.
    step(s.w, NULL, 24U);
    const NphDVec3 p0 = nph_physics_body_position(s.w, b);
    const f32 v0 = nph_vec3_dot(nph_physics_body_linear_velocity(s.w, b), along);
    const u32 steps = (u32)(seconds * 240.0F);
    step(s.w, NULL, steps);
    const NphDVec3 p1 = nph_physics_body_position(s.w, b);
    const f32 v1 = nph_vec3_dot(nph_physics_body_linear_velocity(s.w, b), along);
    *out_accel = (v1 - v0) / ((f32)steps * DT);
    const f32 moved = nph_vec3_dot(nph_dvec3_to_vec3(nph_dvec3_sub(p1, p0)), along);
    scene_end(&s);
    return moved;
}

NTEST(world, friction_holds_below_its_angle_and_slides_above)
{
    f32 accel = 0.0F;
    // tan(20 deg) = 0.36 < 0.6: holds.
    const f32 held = ramp_slide(0.349066F, 0.6F, 2.0F, &accel);
    NTEST_EXPECT_TRUE(nph_f32_abs(held) < 0.005F);
    // tan(35 deg) = 0.70 > 0.3: slides down (-X along the ramp) at
    // g (sin 35 - 0.3 cos 35) = 3.62 m/s^2.
    const f32 slid = ramp_slide(0.610865F, 0.3F, 1.0F, &accel);
    NTEST_EXPECT_TRUE(slid < -1.0F);
    const f32 expected = 9.81F * (nph_f32_sin(0.610865F) - 0.3F * nph_f32_cos(0.610865F));
    // Coulomb's friction exactly: the box neither tips nor spins.
    NTEST_EXPECT_NEAR_F32(-accel, expected, 0.005F * expected);
}

static f32 bounce_height(f32 restitution)
{
    Scene s;
    scene_begin(&s);
    add_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, -1.0, 0.0), nph_quat_identity(),
             nph_physics_geometry_box(nph_vec3(10.0F, 1.0F, 10.0F)), 0.6F, restitution);
    const NphPhysicsBody b =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 2.5, 0.0), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.5F), 0.6F, restitution);
    // Fall 2 m, bounce, and find the peak of the first rebound.
    f64 peak = -1.0;
    bool bounced = false;
    for (u32 i = 0U; i < 240U * 3U; ++i) {
        step(s.w, NULL, 1U);
        const f64 y = nph_physics_body_position(s.w, b).y;
        const f32 vy = nph_physics_body_linear_velocity(s.w, b).y;
        if (!bounced && vy > 0.0F) {
            bounced = true;
        }
        if (bounced) {
            peak = nph_f64_max(peak, y);
            if (vy < 0.0F) {
                break;
            }
        }
    }
    scene_end(&s);
    return (f32)(peak - 0.5);
}

NTEST(world, restitution_bounces_to_its_height)
{
    // From 2 m: e = 1 comes back to 2 m, e = 0.5 to 0.5 m (e^2).
    NTEST_EXPECT_NEAR_F32(bounce_height(1.0F), 2.0F, 0.08F);
    NTEST_EXPECT_NEAR_F32(bounce_height(0.5F), 0.5F, 0.05F);
}

NTEST(world, collisions_conserve_momentum)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    const NphPhysicsBody a =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-2.0, 0.0, 0.1), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.5F), 0.3F, 0.5F);
    const NphPhysicsBody b =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(2.0, 0.2, 0.0), nph_quat_identity(),
                 nph_physics_geometry_box(nph_vec3(0.4F, 0.6F, 0.5F)), 0.3F, 0.5F);
    nph_physics_body_set_linear_velocity(s.w, a, nph_vec3(3.0F, 0.0F, 0.0F));
    nph_physics_body_set_linear_velocity(s.w, b, nph_vec3(-1.0F, 0.3F, 0.0F));
    const f32 ma = nph_physics_body_mass_data(s.w, a).mass;
    const f32 mb = nph_physics_body_mass_data(s.w, b).mass;
    const NphVec3 p0 = nph_vec3_add(nph_vec3_scale(nph_physics_body_linear_velocity(s.w, a), ma),
                                    nph_vec3_scale(nph_physics_body_linear_velocity(s.w, b), mb));
    step(s.w, NULL, 480U);
    const NphVec3 p1 = nph_vec3_add(nph_vec3_scale(nph_physics_body_linear_velocity(s.w, a), ma),
                                    nph_vec3_scale(nph_physics_body_linear_velocity(s.w, b), mb));
    // They met (A no longer moves at 3 m/s) and momentum is the same.
    NTEST_EXPECT_TRUE(nph_physics_body_linear_velocity(s.w, a).x < 2.5F);
    NTEST_EXPECT_TRUE(nph_vec3_distance(p0, p1) < 1e-3F * nph_vec3_length(p0) + 1e-3F);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Sleep, events, filters, lifetime
// ---------------------------------------------------------------------------

NTEST(world, sleepers_wake_when_touched_or_pushed)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    NphPhysicsBody boxes[3];
    build_stack(s.w, 3U, nph_dvec3_zero(), boxes);
    step(s.w, NULL, 240U * 2U);
    for (u32 i = 0U; i < 3U; ++i) {
        NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, boxes[i]));
    }
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).awake_bodies, 0U);
    // An impulse wakes the whole island.
    nph_physics_body_apply_linear_impulse(s.w, boxes[0], nph_vec3(1.0F, 0.0F, 0.0F),
                                          nph_physics_body_center_of_mass(s.w, boxes[0]));
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, boxes[2]));
    step(s.w, NULL, 240U * 3U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, boxes[2]));
    // A ball dropped on the stack wakes it.
    const NphPhysicsBody ball =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 5.0, 0.0), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.3F), 0.6F, 0.0F);
    bool woke = false;
    for (u32 i = 0U; i < 240U && !woke; ++i) {
        step(s.w, NULL, 1U);
        woke = nph_physics_body_is_awake(s.w, boxes[0]);
    }
    NTEST_EXPECT_TRUE(woke);
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, ball));
    scene_end(&s);
}

NTEST(world, events_report_touching_and_motion)
{
    Scene s;
    scene_begin(&s);
    const NphPhysicsBody ground = add_ground(s.w, 0.0);
    NphPhysicsShape ground_shape;
    NTEST_ASSERT_EQ_UINT(nph_physics_body_shapes(s.w, ground, &ground_shape, 1U), 1U);
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(0.0, 1.0, 0.0);
    bd.user_data = 1234U;
    NphPhysicsBody ball;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &ball), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(nph_physics_geometry_sphere(0.25F));
    sd.hit_events = 1;
    NphPhysicsShape ball_shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, ball, &sd, &ball_shape), NPH_RESULT_OK);
    u32 begins = 0U;
    u32 hits = 0U;
    for (u32 i = 0U; i < 240U; ++i) {
        step(s.w, NULL, 1U);
        const NphPhysicsEvents e = nph_physics_world_events(s.w);
        for (u32 k = 0U; k < e.begin_count; ++k) {
            const bool ours = (e.begin[k].shape_a.value == ball_shape.value &&
                               e.begin[k].shape_b.value == ground_shape.value) ||
                              (e.begin[k].shape_b.value == ball_shape.value &&
                               e.begin[k].shape_a.value == ground_shape.value);
            begins += ours ? 1U : 0U;
        }
        for (u32 k = 0U; k < e.hit_count; ++k) {
            NTEST_EXPECT_TRUE(e.hit[k].approach_speed > 1.0F);
            NTEST_EXPECT_NEAR_F64(e.hit[k].point.y, 0.0, 0.05);
            hits += 1U;
        }
        if (i < 10U) {
            // Awake bodies report their pose every step.
            NTEST_ASSERT_EQ_UINT(e.move_count, 1U);
            NTEST_EXPECT_EQ_UINT(e.move[0].body.value, ball.value);
            NTEST_EXPECT_EQ_UINT(e.move[0].user_data, 1234U);
        }
    }
    NTEST_EXPECT_EQ_UINT(begins, 1U);
    NTEST_EXPECT_EQ_UINT(hits, 1U);
    // Lifting it away ends the contact.
    nph_physics_body_set_transform(s.w, ball, nph_dvec3(0.0, 3.0, 0.0), nph_quat_identity());
    step(s.w, NULL, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_events(s.w).end_count, 1U);
    scene_end(&s);
}

NTEST(world, filters_and_groups_decide_who_touches)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // Two balls dropped on one another: the upper one's mask excludes the
    // lower one's category, so it passes through and both rest on the ground.
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(0.0, 0.5, 0.0);
    NphPhysicsBody lower;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &lower), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(nph_physics_geometry_sphere(0.5F));
    sd.filter.category = 2U;
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, lower, &sd, &shape), NPH_RESULT_OK);
    bd.position = nph_dvec3(0.0, 2.0, 0.0);
    NphPhysicsBody upper;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &upper), NPH_RESULT_OK);
    sd.filter.category = 4U;
    sd.filter.mask = ~2ULL;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, upper, &sd, &shape), NPH_RESULT_OK);
    step(s.w, NULL, 480U);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, upper).y, 0.5, 0.02);
    // The same negative group: never touch, whatever the masks.
    const NphPhysicsFilter fa = {1U, NPH_U64_MAX, -3};
    const NphPhysicsFilter fb = {1U, NPH_U64_MAX, -3};
    NTEST_EXPECT_FALSE(nph_physics_filter_should_collide(fa, fb));
    const NphPhysicsFilter pa = {1U, 0U, 5};
    const NphPhysicsFilter pb = {2U, 0U, 5};
    NTEST_EXPECT_TRUE(nph_physics_filter_should_collide(pa, pb));
    scene_end(&s);
}

NTEST(world, destroying_a_support_drops_what_it_held)
{
    Scene s;
    scene_begin(&s);
    const NphPhysicsBody ground = add_ground(s.w, 0.0);
    NphPhysicsBody boxes[2];
    build_stack(s.w, 2U, nph_dvec3_zero(), boxes);
    step(s.w, NULL, 240U * 2U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, boxes[1]));
    // The lower box goes: the upper one wakes and falls onto the ground.
    nph_physics_body_destroy(s.w, boxes[0]);
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, boxes[1]));
    step(s.w, NULL, 240U);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, boxes[1]).y, 0.5, 0.02);
    step(s.w, NULL, 240U);
    // The ground goes from under a sleeping box: it wakes and falls (half a
    // second of free fall is 1.2 m).
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, boxes[1]));
    nph_physics_body_destroy(s.w, ground);
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, boxes[1]));
    step(s.w, NULL, 120U);
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, boxes[1]).y < -0.5);
    scene_end(&s);
}

NTEST(world, kinematic_bodies_push_and_carry)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // A kinematic paddle sweeps into a box and pushes it along.
    NphPhysicsBodyDesc kd = nph_physics_body_desc_default();
    kd.type = NPH_PHYSICS_BODY_KINEMATIC;
    kd.position = nph_dvec3(-3.0, 0.5, 0.0);
    kd.linear_velocity = nph_vec3(2.0F, 0.0F, 0.0F);
    NphPhysicsBody paddle;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &kd, &paddle), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3(0.2F, 0.4F, 1.0F)));
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, paddle, &sd, &shape), NPH_RESULT_OK);
    const NphPhysicsBody box = add_box(s.w, nph_dvec3(0.0, 0.5, 0.0), nph_vec3_splat(0.5F));
    step(s.w, NULL, 240U * 2U);
    // The paddle moved 4 m and is not pushed back; the box went ahead of it.
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, paddle).x, 1.0, 1e-3);
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, box).x > 1.0);
    scene_end(&s);
}

NTEST(world, the_gyroscopic_term_flips_the_intermediate_axis)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    // A box with three distinct moments, spun about the intermediate axis
    // (Y) with a small perturbation: the Dzhanibekov effect flips it.
    const NphPhysicsBody b =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3_zero(), nph_quat_identity(),
                 nph_physics_geometry_box(nph_vec3(0.1F, 0.4F, 0.8F)), 0.6F, 0.0F);
    nph_physics_body_set_angular_velocity(s.w, b, nph_vec3(0.01F, 6.0F, 0.0F));
    const NphPhysicsMassData m = nph_physics_body_mass_data(s.w, b);
    f32 min_axis_y = 1.0F;
    for (u32 i = 0U; i < 240U * 6U; ++i) {
        step(s.w, NULL, 1U);
        // The body's Y axis in the world turns over: its dot with world Y
        // goes negative.
        const NphVec3 ay = nph_quat_axis_y(nph_physics_body_rotation(s.w, b));
        min_axis_y = nph_f32_min(min_axis_y, ay.y);
    }
    NTEST_EXPECT_TRUE(min_axis_y < -0.5F);
    // Energy stays close to the start: the implicit step does not gain it.
    const NphVec3 w1 = nph_quat_rotate_inverse(nph_physics_body_rotation(s.w, b),
                                               nph_physics_body_angular_velocity(s.w, b));
    const f32 e0 = 0.5F * m.inertia.c[1].y * 36.0F;
    const f32 e1 = 0.5F * nph_vec3_dot(w1, nph_mat3_mul_vec3(m.inertia, w1));
    NTEST_LOG("energy after 6 s: %.4f of the start; lowest body Y . world Y %.3f", (f64)(e1 / e0),
              (f64)min_axis_y);
    NTEST_EXPECT_TRUE(e1 <= e0 * 1.01F);
    NTEST_EXPECT_TRUE(e1 >= e0 * 0.8F);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Invariants, the 64-bit world, kernels, determinism
// ---------------------------------------------------------------------------

static void drop_pile(NphPhysicsWorld *w, u32 count, NphDVec3 base, u64 seed)
{
    NphPcg32 rng = test_rng(seed);
    for (u32 i = 0U; i < count; ++i) {
        const NphDVec3 p =
            nph_dvec3_add(base, nph_dvec3((f64)test_range(&rng, -3.0F, 3.0F), 1.0 + 0.6 * (f64)i,
                                          (f64)test_range(&rng, -3.0F, 3.0F)));
        NphPhysicsGeometry g;
        switch (i % 3U) {
        case 0U:
            g = nph_physics_geometry_box(test_vec3(&rng, 0.2F, 0.6F));
            break;
        case 1U:
            g = nph_physics_geometry_sphere(test_range(&rng, 0.2F, 0.5F));
            break;
        default:
            g = nph_physics_geometry_capsule(test_range(&rng, 0.1F, 0.5F),
                                             test_range(&rng, 0.15F, 0.3F));
            break;
        }
        add_body(w, NPH_PHYSICS_BODY_DYNAMIC, p, test_rotation(&rng), g, 0.5F, 0.1F);
    }
}

NTEST(world, invariants_hold_through_a_pile_and_edits)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    drop_pile(s.w, 30U * TEST_SCALE, nph_dvec3_zero(), 5U);
    NphPcg32 rng = test_rng(6U);
    for (u32 i = 0U; i < 400U; ++i) {
        step(s.w, NULL, 1U);
        if (i % 40U == 0U) {
            NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
        }
        if (i % 97U == 0U) {
            // Teleport something into the pile, and drop a new body.
            const NphPhysicsBody b =
                add_box(s.w, nph_dvec3(0.0, 6.0, 0.0), test_vec3(&rng, 0.2F, 0.5F));
            nph_physics_body_set_transform(s.w, b, nph_dvec3(0.5, 2.0, 0.5), test_rotation(&rng));
        }
    }
    NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
    scene_end(&s);
}

NTEST(world, islands_merge_with_sleep_off)
{
    Scene s;
    scene_begin(&s);
    s.w->desc.enable_sleep = 0;
    add_ground(s.w, 0.0);
    // Two stacks of three apart: two islands once their contacts link the
    // bodies' own, sleep or no sleep.
    for (u32 k = 0U; k < 2U; ++k) {
        for (u32 i = 0U; i < 3U; ++i) {
            add_box(s.w, nph_dvec3(4.0 * (f64)k, 0.5 + 1.01 * (f64)i, 0.0), nph_vec3_splat(0.5F));
        }
    }
    step(s.w, NULL, 120U);
    const NphPhysicsStats st = nph_physics_world_stats(s.w);
    NTEST_EXPECT_EQ_UINT(st.islands, 2U);
    NTEST_EXPECT_EQ_UINT(st.awake_bodies, 6U);
    scene_end(&s);
}

NTEST(world, a_stack_far_away_behaves_as_at_home)
{
    // The same stack at the origin and 10 000 km away: relative positions
    // agree to a fraction of a millimetre (everything is relative inside).
    NphPhysicsBody near_boxes[5];
    NphPhysicsBody far_boxes[5];
    Scene a;
    Scene b;
    scene_begin(&a);
    scene_begin(&b);
    const NphDVec3 far = nph_dvec3(1.0e7, 0.0, -3.0e6);
    add_ground(a.w, 0.0);
    add_body(b.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_add(far, nph_dvec3(0.0, -1.0, 0.0)),
             nph_quat_identity(), nph_physics_geometry_box(nph_vec3(50.0F, 1.0F, 50.0F)), 0.6F,
             0.0F);
    build_stack(a.w, 5U, nph_dvec3_zero(), near_boxes);
    build_stack(b.w, 5U, far, far_boxes);
    // A push so the motion is not trivially symmetric.
    nph_physics_body_set_linear_velocity(a.w, near_boxes[4], nph_vec3(0.5F, 0.0F, 0.2F));
    nph_physics_body_set_linear_velocity(b.w, far_boxes[4], nph_vec3(0.5F, 0.0F, 0.2F));
    step(a.w, NULL, 480U);
    step(b.w, NULL, 480U);
    for (u32 i = 0U; i < 5U; ++i) {
        const NphDVec3 pa = nph_physics_body_position(a.w, near_boxes[i]);
        const NphDVec3 pb = nph_dvec3_sub(nph_physics_body_position(b.w, far_boxes[i]), far);
        NTEST_EXPECT_NEAR_F64(pa.x, pb.x, 5e-4);
        NTEST_EXPECT_NEAR_F64(pa.y, pb.y, 5e-4);
        NTEST_EXPECT_NEAR_F64(pa.z, pb.z, 5e-4);
    }
    scene_end(&a);
    scene_end(&b);
}

NTEST(world, sse4_and_avx2_kernels_agree_to_the_bit)
{
    if (nph_simd_detect_level() < NPH_SIMD_LEVEL_AVX2) {
        NTEST_SKIP("no AVX2 on this machine");
    }
    u64 hashes[2];
    for (u32 k = 0U; k < 2U; ++k) {
        Scene s;
        scene_begin(&s);
        s.w->kernels = k == 0U ? &g_phys_solver_sse4 : &g_phys_solver_avx2;
        add_ground(s.w, 0.0);
        drop_pile(s.w, 40U, nph_dvec3_zero(), 9U);
        step(s.w, NULL, 300U);
        hashes[k] = nph_physics_world_hash(s.w);
        scene_end(&s);
    }
    NTEST_EXPECT_EQ_UINT(hashes[0], hashes[1]);
}

NTEST(world, results_do_not_depend_on_the_worker_count)
{
    const u32 bodies = 60U * TEST_SCALE;
    const u32 steps = 300U;
    u64 reference = 0U;
    // Job system workers, and the solver's threads (0: as many as the work
    // pays for, which for this small pile is few; otherwise forced, so
    // every block of every stage has its own claimant).
    const u32 workers[5] = {0U, 1U, 4U, 8U, 8U};
    const u32 forced[5] = {0U, 2U, 5U, 9U, 0U};
    for (u32 run = 0U; run < 5U; ++run) {
        NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
        Scene s;
        scene_begin(&s);
        s.w->forced_participants = forced[run];
        add_ground(s.w, 0.0);
        drop_pile(s.w, bodies, nph_dvec3_zero(), 21U);
        u32 most_workers = 0U;
        for (u32 i = 0U; i < steps; ++i) {
            step(s.w, js, 1U);
            most_workers = nph_max_u32(most_workers, nph_physics_world_stats(s.w).workers);
        }
        NTEST_LOG("%u workers, %u forced: up to %u threads in the solver", workers[run],
                  forced[run], most_workers);
        if (forced[run] >= 2U && nph_cpu_info()->logical_cores >= 2U) {
            // The helpers took part (the parallel path ran), at least once.
            NTEST_EXPECT_TRUE(most_workers >= 2U);
        }
        const u64 h = nph_physics_world_hash(s.w);
        if (run == 0U) {
            reference = h;
        } else {
            NTEST_EXPECT_EQ_UINT(h, reference);
        }
        scene_end(&s);
        nph_job_system_destroy(js);
    }
}

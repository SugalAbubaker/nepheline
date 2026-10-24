// SPDX-License-Identifier: Apache-2.0
// Continuous collision (world.h step 5; docs/physics.md "Continuous
// collision"): fast bodies against thin walls, meshes, and heightfields,
// with and without; spin through a wall; sliding fast across a mesh's
// seams with no stop; bullets against dynamic bodies; a body shot into the
// floor it rests on; slow bodies never swept; the same course on 0 to 8
// workers; and 10 000 km out.

#include <nepheline/platform/cpu.h>

#include "test_world_support.h"

static NphPhysicsWorldDesc weightless(bool continuous)
{
    NphPhysicsWorldDesc d = nph_physics_world_desc_default();
    d.gravity = nph_vec3_zero();
    d.enable_continuous = continuous ? 1 : 0;
    return d;
}

static NphPhysicsBody add_moving(NphPhysicsWorld *w, NphDVec3 p, NphQuat q, NphPhysicsGeometry g,
                                 NphVec3 v, NphVec3 omega, bool bullet)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = p;
    bd.rotation = q;
    bd.linear_velocity = v;
    bd.angular_velocity = omega;
    bd.bullet = bullet ? 1 : 0;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(w, &bd, &b), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    sd.restitution = 0.0F;
    NphPhysicsShape s;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(w, b, &sd, &s), NPH_RESULT_OK);
    return b;
}

// A wall 2 cm thick across X at x = `x` (its near face at x - 1 cm).
static void add_wall(NphPhysicsWorld *w, NphDVec3 at, f64 x)
{
    add_body(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(at.x + x, at.y, at.z), nph_quat_identity(),
             nph_physics_geometry_box(nph_vec3(0.01F, 3.0F, 3.0F)), 0.6F, 0.0F);
}

static f64 x_of(const NphPhysicsWorld *w, NphPhysicsBody b, NphDVec3 at)
{
    return nph_physics_body_position(w, b).x - at.x;
}

// Balls, boxes, capsules, and hulls at 300 m/s (1.25 m a step) at a wall
// 2 cm thick, `at` from the origin; each step lands them clear of the wall,
// so without continuous collision nothing ever touches it.
static void wall_scene(Scene *s, NphDVec3 at, const NphPhysicsGeometry shapes[4],
                       NphPhysicsBody b[4])
{
    NphPcg32 r = test_rng(302U);
    add_wall(s->w, at, 5.0);
    for (u32 k = 0U; k < 4U; ++k) {
        b[k] = add_moving(s->w, nph_dvec3(at.x + 0.3, at.y - 1.5 + (f64)k, at.z + 0.3),
                          test_rotation(&r), shapes[k], nph_vec3(300.0F, 0.0F, 0.0F),
                          test_vec3(&r, -5.0F, 5.0F), false);
    }
}

NTEST(continuous, fast_bodies_do_not_pass_through_a_thin_wall)
{
    // Through the wall without continuous collision, stopped in front of it
    // with.
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(301U);
    NphPhysicsHull *hull = test_random_hull(alloc, &r, 24U, 0.12F, nph_vec3(1.0F, 0.7F, 0.9F));
    const NphPhysicsGeometry shapes[4] = {
        nph_physics_geometry_sphere(0.05F), nph_physics_geometry_box(nph_vec3(0.05F, 0.1F, 0.08F)),
        nph_physics_geometry_capsule(0.1F, 0.05F), nph_physics_geometry_hull(hull)};
    for (u32 on = 0U; on < 2U; ++on) {
        Scene s;
        scene_begin_with(&s, weightless(on != 0U));
        NphPhysicsBody b[4];
        wall_scene(&s, nph_dvec3_zero(), shapes, b);
        u32 swept = 0U;
        u32 impacts = 0U;
        f64 farthest[4] = {0.0, 0.0, 0.0, 0.0};
        for (u32 i = 0U; i < 24U; ++i) {
            step(s.w, NULL, 1U);
            swept += nph_physics_world_stats(s.w).swept_bodies;
            impacts += nph_physics_world_stats(s.w).impact_bodies;
            for (u32 k = 0U; k < 4U; ++k) {
                farthest[k] = fmax(farthest[k], x_of(s.w, b[k], nph_dvec3_zero()));
            }
        }
        for (u32 k = 0U; k < 4U; ++k) {
            NTEST_LOG("on %u body %u: farthest %.4f, at the end x %.4f v %.3f", on, k, farthest[k],
                      x_of(s.w, b[k], nph_dvec3_zero()),
                      (f64)nph_physics_body_linear_velocity(s.w, b[k]).x);
            if (on != 0U) {
                // Never past the wall's near face (4.99), by its size. What
                // tumbles may come off it backwards, turning.
                NTEST_EXPECT_TRUE(farthest[k] < 4.99);
            } else {
                NTEST_EXPECT_TRUE(farthest[k] > 6.0); // straight through
            }
        }
        NTEST_LOG("continuous %u: %u swept, %u impacts", on, swept, impacts);
        if (on != 0U) {
            NTEST_EXPECT_TRUE(swept > 0U && impacts >= 4U);
        } else {
            NTEST_EXPECT_EQ_UINT(swept, 0U);
        }
        scene_end(&s);
    }
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

NTEST(continuous, fast_bodies_do_not_pass_through_meshes_or_heightfields)
{
    // Thrown down at 150 m/s onto a one-sided floor of triangles: a mesh and
    // a heightfield, under gravity.
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 8U, 1.0F, -4.0F, -4.0F, NULL, false);
    f32 flat[9U * 9U];
    for (u32 i = 0U; i < 9U * 9U; ++i) {
        flat[i] = 0.0F;
    }
    NphPhysicsHeightfield *field = test_heightfield(alloc, 9U, 9U, 1.0F, flat, NULL);
    const NphPhysicsGeometry floors[2] = {nph_physics_geometry_mesh(mesh),
                                          nph_physics_geometry_heightfield(field)};
    const NphDVec3 at[2] = {nph_dvec3_zero(), nph_dvec3(-4.0, 0.0, -4.0)};
    const NphPhysicsGeometry shapes[3] = {nph_physics_geometry_sphere(0.1F),
                                          nph_physics_geometry_box(nph_vec3_splat(0.1F)),
                                          nph_physics_geometry_capsule(0.15F, 0.08F)};
    for (u32 f = 0U; f < 2U; ++f) {
        Scene s;
        scene_begin(&s);
        add_body(s.w, NPH_PHYSICS_BODY_STATIC, at[f], nph_quat_identity(), floors[f], 0.6F, 0.0F);
        NphPhysicsBody b[3];
        for (u32 k = 0U; k < 3U; ++k) {
            b[k] = add_moving(s.w, nph_dvec3(-1.3 + 1.1 * (f64)k, 3.0, 0.4 - 0.3 * (f64)k),
                              nph_quat_identity(), shapes[k], nph_vec3(3.0F, -150.0F, 1.0F),
                              nph_vec3_zero(), false);
        }
        for (u32 i = 0U; i < 240U; ++i) {
            step(s.w, NULL, 1U);
            if (i < 12U) {
                for (u32 k = 0U; k < 3U; ++k) {
                    const NphDVec3 p = nph_physics_body_position(s.w, b[k]);
                    NTEST_LOG("floor %u step %u body %u: (%.3f %.3f %.3f) vy %.2f", f, i, k, p.x,
                              p.y, p.z, (f64)nph_physics_body_linear_velocity(s.w, b[k]).y);
                }
            }
        }
        for (u32 k = 0U; k < 3U; ++k) {
            const NphDVec3 p = nph_physics_body_position(s.w, b[k]);
            NTEST_LOG("floor %u body %u final: (%.3f %.3f %.3f)", f, k, p.x, p.y, p.z);
            NTEST_EXPECT_TRUE(p.y > 0.0 && p.y < 0.3);
        }
        scene_end(&s);
    }
    nph_physics_mesh_free(mesh, alloc);
    nph_physics_heightfield_free(field, alloc);
    test_alloc_end(&ta);
}

NTEST(continuous, spin_does_not_carry_a_blade_through_a_wall)
{
    // A blade 2 m long spinning at 150 rad/s (a tenth of a turn a step)
    // beside a wall its tips reach, upright to start with and clear of it:
    // without continuous collision its tips cross the wall's plane between
    // two steps' contacts. With it, no point of the blade ever ends a step
    // past the wall.
    const NphPhysicsGeometry blade = nph_physics_geometry_box(nph_vec3(1.0F, 0.04F, 0.2F));
    const NphQuat upright = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.5F * k_pi);
    u32 past[2] = {0U, 0U};
    for (u32 on = 0U; on < 2U; ++on) {
        Scene s;
        scene_begin_with(&s, weightless(on != 0U));
        add_wall(s.w, nph_dvec3_zero(), 0.6);
        const NphPhysicsBody b = add_moving(s.w, nph_dvec3_zero(), upright, blade, nph_vec3_zero(),
                                            nph_vec3(0.0F, 0.0F, 150.0F), false);
        for (u32 i = 0U; i < 60U; ++i) {
            step(s.w, NULL, 1U);
            // The blade's corners past the wall's far face (x = 0.61).
            const NphDVec3 c = nph_physics_body_position(s.w, b);
            const NphQuat q = nph_physics_body_rotation(s.w, b);
            for (u32 k = 0U; k < 8U; ++k) {
                const NphVec3 corner =
                    nph_vec3((k & 1U) != 0U ? 1.0F : -1.0F, (k & 2U) != 0U ? 0.04F : -0.04F,
                             (k & 4U) != 0U ? 0.2F : -0.2F);
                const f64 x = c.x + (f64)nph_quat_rotate(q, corner).x;
                past[on] += x > 0.61 ? 1U : 0U;
            }
        }
        scene_end(&s);
    }
    NTEST_LOG("corners past the wall: %u without, %u with", past[0], past[1]);
    NTEST_EXPECT_TRUE(past[0] > 0U);
    NTEST_EXPECT_EQ_UINT(past[1], 0U);
}

NTEST(continuous, sliding_fast_across_seams_is_never_stopped)
{
    // A box and a capsule sliding at 60 m/s (25 cm a step) on a frictionless
    // floor of 1 m cells: swept every step, never stopped at a seam: they
    // cover the same ground as without continuous collision.
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 48U, 1.0F, -4.0F, -24.0F, NULL, false);
    const NphPhysicsGeometry shapes[2] = {nph_physics_geometry_box(nph_vec3(0.2F, 0.1F, 0.2F)),
                                          nph_physics_geometry_capsule(0.2F, 0.1F)};
    const NphQuat lying = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.5F * k_pi);
    f64 travel[2][2];
    u32 swept = 0U;
    u32 impacts = 0U;
    for (u32 on = 0U; on < 2U; ++on) {
        Scene s;
        NphPhysicsWorldDesc d = nph_physics_world_desc_default();
        d.enable_continuous = on != 0U ? 1 : 0;
        scene_begin_with(&s, d);
        NphPhysicsBodyDesc fd = nph_physics_body_desc_default();
        fd.type = NPH_PHYSICS_BODY_STATIC;
        NphPhysicsBody floor;
        NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &fd, &floor), NPH_RESULT_OK);
        NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(nph_physics_geometry_mesh(mesh));
        sd.friction = 0.0F;
        NphPhysicsShape fs;
        NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, floor, &sd, &fs), NPH_RESULT_OK);
        NphPhysicsBody b[2];
        for (u32 k = 0U; k < 2U; ++k) {
            NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
            bd.position = nph_dvec3(-2.0, 0.099, -18.0 + 4.0 * (f64)k);
            bd.rotation = k == 1U ? lying : nph_quat_identity();
            NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &b[k]), NPH_RESULT_OK);
            NphPhysicsShapeDesc bs = nph_physics_shape_desc_default(shapes[k]);
            bs.friction = 0.0F;
            NphPhysicsShape sh;
            NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, b[k], &bs, &sh), NPH_RESULT_OK);
        }
        step(s.w, NULL, 60U); // settle
        for (u32 k = 0U; k < 2U; ++k) {
            nph_physics_body_set_linear_velocity(s.w, b[k], nph_vec3(0.0F, 0.0F, 60.0F));
        }
        const f64 z0[2] = {nph_physics_body_position(s.w, b[0]).z,
                           nph_physics_body_position(s.w, b[1]).z};
        for (u32 i = 0U; i < 120U; ++i) {
            step(s.w, NULL, 1U);
            if (on != 0U) {
                swept += nph_physics_world_stats(s.w).swept_bodies;
                impacts += nph_physics_world_stats(s.w).impact_bodies;
            }
        }
        for (u32 k = 0U; k < 2U; ++k) {
            travel[on][k] = nph_physics_body_position(s.w, b[k]).z - z0[k];
            NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, b[k]).y > 0.05);
        }
        scene_end(&s);
    }
    NTEST_LOG("travel: box %.3f / %.3f m, capsule %.3f / %.3f m; %u swept, %u impacts",
              travel[0][0], travel[1][0], travel[0][1], travel[1][1], swept, impacts);
    NTEST_EXPECT_TRUE(swept >= 200U);
    NTEST_EXPECT_EQ_UINT(impacts, 0U);
    for (u32 k = 0U; k < 2U; ++k) {
        NTEST_EXPECT_NEAR_F64(travel[1][k], travel[0][k], 1e-6);
        NTEST_EXPECT_TRUE(travel[1][k] > 29.0); // 60 m/s for half a second
    }
    nph_physics_mesh_free(mesh, alloc);
    test_alloc_end(&ta);
}

NTEST(continuous, bullets_hit_what_other_fast_bodies_pass)
{
    // A ball at 300 m/s at a thin dynamic plate: a bullet hits it, a plain
    // fast body passes through it (only bullets sweep against dynamic
    // bodies).
    for (u32 bullet = 0U; bullet < 2U; ++bullet) {
        Scene s;
        scene_begin_with(&s, weightless(true));
        const NphPhysicsBody plate =
            add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(5.0, 0.0, 0.0), nph_quat_identity(),
                     nph_physics_geometry_box(nph_vec3(0.01F, 1.0F, 1.0F)), 0.6F, 0.0F);
        // Its steps land clear of the plate (0.3 m, then 1.25 m a step).
        const NphPhysicsBody ball = add_moving(
            s.w, nph_dvec3(0.3, 0.1, 0.0), nph_quat_identity(), nph_physics_geometry_sphere(0.05F),
            nph_vec3(300.0F, 0.0F, 0.0F), nph_vec3_zero(), bullet != 0U);
        NTEST_EXPECT_EQ_INT(nph_physics_body_is_bullet(s.w, ball), bullet != 0U);
        step(s.w, NULL, 24U);
        const f32 plate_speed = nph_vec3_length(nph_physics_body_linear_velocity(s.w, plate));
        const f64 x = x_of(s.w, ball, nph_dvec3_zero());
        NTEST_LOG("bullet %u: ball at %.2f m, plate at %.2f m/s", bullet, x, (f64)plate_speed);
        if (bullet != 0U) {
            NTEST_EXPECT_TRUE(plate_speed > 1.0F);
            NTEST_EXPECT_TRUE(x < nph_physics_body_position(s.w, plate).x);
        } else {
            NTEST_EXPECT_TRUE(plate_speed < 1e-3F);
            NTEST_EXPECT_TRUE(x > 6.0);
        }
        scene_end(&s);
    }
}

NTEST(continuous, a_body_shot_into_the_floor_it_rests_on_stays_on_it)
{
    // Resting on a floor 2 cm thick, then sent down at 200 m/s: the pair
    // starts within reach, so the box's core is swept (its own sweep says
    // nothing), and the contact it rests on holds it either way. (Even a
    // ball 2 cm across resting on a sheet 1 mm thick, sent at it at
    // 400 m/s, is held: the core is a safety net.)
    for (u32 on = 0U; on < 2U; ++on) {
        Scene s;
        NphPhysicsWorldDesc d = nph_physics_world_desc_default();
        d.enable_continuous = on != 0U ? 1 : 0;
        scene_begin_with(&s, d);
        add_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, -0.01, 0.0), nph_quat_identity(),
                 nph_physics_geometry_box(nph_vec3(5.0F, 0.01F, 5.0F)), 0.6F, 0.0F);
        const NphPhysicsBody box = add_box(s.w, nph_dvec3(0.0, 0.2, 0.0), nph_vec3_splat(0.2F));
        step(s.w, NULL, 120U);
        nph_physics_body_set_linear_velocity(s.w, box, nph_vec3(0.0F, -200.0F, 0.0F));
        step(s.w, NULL, 60U);
        const f64 y = nph_physics_body_position(s.w, box).y;
        NTEST_EXPECT_TRUE(y > 0.1 && y < 0.25);
        scene_end(&s);
    }
}

NTEST(continuous, slow_and_resting_bodies_are_never_swept)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    NphPcg32 r = test_rng(303U);
    // Forty boxes on a grid, from up to 2.5 m: some land on others.
    for (u32 i = 0U; i < 40U; ++i) {
        const u32 column = i % 5U;
        const u32 row = i / 5U;
        const f32 dx = test_range(&r, -0.3F, 0.3F);
        const f32 dz = test_range(&r, -0.2F, 0.2F);
        const NphQuat q = test_rotation(&r);
        const NphVec3 half = test_vec3(&r, 0.2F, 0.35F);
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC,
                 nph_dvec3(-3.0 + 1.5 * (f64)column + (f64)dx, 0.5 + 0.05 * (f64)i,
                           -3.0 + 0.9 * (f64)row + (f64)dz),
                 q, nph_physics_geometry_box(half), 0.6F, 0.0F);
    }
    u32 swept = 0U;
    for (u32 i = 0U; i < 480U; ++i) {
        step(s.w, NULL, 1U);
        swept += nph_physics_world_stats(s.w).swept_bodies;
    }
    // Dropped from up to 2.5 m (7 m/s, 3 cm a step) and tumbling as they
    // land, none moves half its inner radius in a step. (Boxes dropped from
    // 10 m onto a heap turn at 30 rad/s off it, their corners 7 cm a step:
    // fast, and swept.)
    NTEST_EXPECT_EQ_UINT(swept, 0U);
    scene_end(&s);
}

// Bullets and plain fast bodies fired through a room of thin walls and
// plates, `at` from the origin.
static void shooting_gallery(Scene *s, NphDVec3 at, NphPhysicsBody bodies[24])
{
    for (u32 k = 0U; k < 4U; ++k) {
        add_wall(s->w, at, 3.0 + 2.0 * (f64)k);
    }
    for (u32 k = 0U; k < 4U; ++k) {
        bodies[20U + k] = add_body(
            s->w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(at.x + 2.0, at.y - 1.5 + (f64)k, at.z),
            nph_quat_identity(), nph_physics_geometry_box(nph_vec3(0.02F, 0.4F, 0.4F)), 0.6F, 0.0F);
    }
    NphPcg32 r = test_rng(305U);
    for (u32 i = 0U; i < 20U; ++i) {
        const NphVec3 p = nph_vec3(-1.0F, test_range(&r, -1.8F, 1.8F), test_range(&r, -1.0F, 1.0F));
        const NphVec3 v = nph_vec3(test_range(&r, 100.0F, 380.0F), test_range(&r, -20.0F, 20.0F),
                                   test_range(&r, -20.0F, 20.0F));
        const NphPhysicsGeometry g = i % 2U == 0U
                                         ? nph_physics_geometry_sphere(test_range(&r, 0.03F, 0.08F))
                                         : nph_physics_geometry_box(test_vec3(&r, 0.03F, 0.08F));
        bodies[i] = add_moving(s->w, nph_dvec3_add_vec3(at, p), test_rotation(&r), g, v,
                               test_vec3(&r, -50.0F, 50.0F), i % 3U == 0U);
    }
}

NTEST(continuous, the_same_course_on_any_number_of_workers)
{
    const u32 workers[5] = {0U, 1U, 4U, 8U, 8U};
    const u32 forced[5] = {0U, 2U, 5U, 9U, 0U};
    u64 reference = 0U;
    for (u32 run = 0U; run < 5U; ++run) {
        NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
        Scene s;
        scene_begin(&s);
        s.w->forced_participants = forced[run];
        NphPhysicsBody bodies[24];
        shooting_gallery(&s, nph_dvec3_zero(), bodies);
        u32 impacts = 0U;
        for (u32 i = 0U; i < 120U; ++i) {
            step(s.w, js, 1U);
            impacts += nph_physics_world_stats(s.w).impact_bodies;
        }
        const u64 h = nph_physics_world_hash(s.w);
        NTEST_LOG("%u workers: %u impacts", workers[run], impacts);
        NTEST_EXPECT_TRUE(impacts >= 20U);
        if (run == 0U) {
            reference = h;
        } else {
            NTEST_EXPECT_EQ_UINT(h, reference);
        }
        scene_end(&s);
        nph_job_system_destroy(js);
    }
}

NTEST(continuous, the_same_impacts_10000_km_out)
{
    // The wall scene at the origin and 10 000 km out: each body is set back
    // to the same impact, relative to the wall, and stays there.
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(301U);
    NphPhysicsHull *hull = test_random_hull(alloc, &r, 24U, 0.12F, nph_vec3(1.0F, 0.7F, 0.9F));
    const NphPhysicsGeometry shapes[4] = {
        nph_physics_geometry_sphere(0.05F), nph_physics_geometry_box(nph_vec3(0.05F, 0.1F, 0.08F)),
        nph_physics_geometry_capsule(0.1F, 0.05F), nph_physics_geometry_hull(hull)};
    const NphDVec3 far_at = nph_dvec3(1.0e7, 2.0e5, -1.0e7);
    Scene a;
    Scene b;
    scene_begin_with(&a, weightless(true));
    scene_begin_with(&b, weightless(true));
    NphPhysicsBody ba[4];
    NphPhysicsBody bb[4];
    wall_scene(&a, nph_dvec3_zero(), shapes, ba);
    wall_scene(&b, far_at, shapes, bb);
    for (u32 i = 0U; i < 24U; ++i) {
        step(a.w, NULL, 1U);
        step(b.w, NULL, 1U);
        for (u32 k = 0U; k < 4U; ++k) {
            const NphDVec3 pa = nph_physics_body_position(a.w, ba[k]);
            const NphDVec3 pb = nph_dvec3_sub(nph_physics_body_position(b.w, bb[k]), far_at);
            NTEST_EXPECT_TRUE(nph_dvec3_near(pa, pb, 1e-4));
        }
    }
    for (u32 k = 0U; k < 4U; ++k) {
        NTEST_EXPECT_TRUE(x_of(b.w, bb[k], far_at) < 4.99);
    }
    scene_end(&a);
    scene_end(&b);
    nph_physics_hull_free(hull, alloc);
    test_alloc_end(&ta);
}

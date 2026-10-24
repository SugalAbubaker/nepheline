// SPDX-License-Identifier: Apache-2.0
// The character controller (character.h; docs/physics.md "The character
// controller"): standing, walking, walls and corners, slopes walkable and
// not, steps up and stairs down, ledges and ceilings, meshes and
// heightfields, a moving platform, pushing a box, overlaps pushed out, the
// same moves 10 000 km out, other shapes, and bad descriptions.

#include <nepheline/physics/character.h>

#include "test_world_support.h"

// A capsule 1.8 m tall standing on its axis.
static NphPhysicsCharacterDesc capsule_desc(void)
{
    return nph_physics_character_desc_default(nph_physics_geometry_capsule(0.6F, 0.3F));
}

// The capsule's bottom below its centre.
#define FEET 0.9

// A character driven as a game would: its own vertical speed, gravity while
// it is off the ground, `velocity` across, one move a frame.
typedef struct Walker {
    NphPhysicsCharacterDesc desc;
    NphPhysicsCharacter c;
    f32 fall; // vertical speed, m/s
} Walker;

static void walker_begin(Walker *k, const NphPhysicsWorld *w, NphPhysicsCharacterDesc desc,
                         NphDVec3 at)
{
    k->desc = desc;
    k->fall = 0.0F;
    NTEST_ASSERT_EQ_INT(nph_physics_character_init(w, &desc, at, nph_quat_identity(), &k->c),
                        NPH_RESULT_OK);
}

static void walker_frame(Walker *k, const NphPhysicsWorld *w, NphVec3 velocity)
{
    if (k->c.ground == NPH_PHYSICS_CHARACTER_WALKABLE) {
        k->fall = 0.0F;
    } else {
        k->fall -= 9.81F * DT;
    }
    const NphVec3 v =
        nph_vec3_add(nph_vec3_add(velocity, k->c.ground_velocity), nph_vec3(0.0F, k->fall, 0.0F));
    NTEST_ASSERT_EQ_INT(
        nph_physics_character_move(w, &k->desc, &k->c, nph_vec3_scale(v, DT), NULL, 0U, NULL),
        NPH_RESULT_OK);
}

static void walk(Walker *k, const NphPhysicsWorld *w, NphVec3 velocity, u32 frames)
{
    for (u32 i = 0U; i < frames; ++i) {
        walker_frame(k, w, velocity);
    }
}

static NphPhysicsBody add_static_box(NphPhysicsWorld *w, NphDVec3 p, NphQuat q, NphVec3 half)
{
    return add_body(w, NPH_PHYSICS_BODY_STATIC, p, q, nph_physics_geometry_box(half), 0.6F, 0.0F);
}

// A ramp rising along +X at `angle` from the floor at x = 0, 4 m long.
static void add_ramp(NphPhysicsWorld *w, f64 z, f32 angle)
{
    const NphQuat q = nph_quat_from_axis_angle(nph_vec3_unit_z(), angle);
    const f32 c = nph_f32_cos(angle);
    const f32 s = nph_f32_sin(angle);
    // Its top face's lower edge at the origin: the box's centre is 2 m up
    // the slope and 0.1 m under it.
    const NphDVec3 centre = nph_dvec3((f64)(2.0F * c + 0.1F * s), (f64)(2.0F * s - 0.1F * c), z);
    add_static_box(w, centre, q, nph_vec3(2.0F, 0.1F, 1.0F));
}

// ---------------------------------------------------------------------------
// Standing and walking
// ---------------------------------------------------------------------------

NTEST(character, lands_and_stands_still_a_skin_above_the_ground)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(0.0, FEET + 1.0, 0.0));
    NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_AIRBORNE);
    walk(&k, s.w, nph_vec3_zero(), 240U); // falls 1 m and lands
    NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    const f64 landed = k.c.position.y;
    NTEST_EXPECT_NEAR_F64(landed, FEET + (f64)k.desc.skin, 1e-4);
    for (u32 i = 0U; i < 480U; ++i) {
        walker_frame(&k, s.w, nph_vec3_zero());
        NTEST_EXPECT_NEAR_F64(k.c.position.y, landed, 1e-6);
        NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    }
    NTEST_EXPECT_NEAR_F64(k.c.position.x, 0.0, 1e-6);
    NTEST_EXPECT_TRUE(nph_vec3_near(k.c.ground_normal, nph_vec3(0.0F, 1.0F, 0.0F), 1e-5F));
    scene_end(&s);
}

NTEST(character, walks_across_the_ground_at_its_speed)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-4.0, FEET + 0.005, 0.0));
    walk(&k, s.w, nph_vec3_zero(), 10U);
    const NphDVec3 start = k.c.position;
    walk(&k, s.w, nph_vec3(2.0F, 0.0F, 1.0F), 480U); // 2 s
    NTEST_EXPECT_NEAR_F64(k.c.position.x - start.x, 4.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(k.c.position.z - start.z, 2.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(k.c.position.y, start.y, 1e-6);
    NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Walls and corners
// ---------------------------------------------------------------------------

NTEST(character, slides_along_a_wall_and_never_into_it)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // A wall with its face at x = 1.
    add_static_box(s.w, nph_dvec3(1.5, 1.0, 0.0), nph_quat_identity(), nph_vec3(0.5F, 1.0F, 10.0F));
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(0.0, FEET + 0.005, -4.0));
    walk(&k, s.w, nph_vec3_zero(), 10U);
    // Into it at 45 degrees: the part along it is kept.
    f64 nearest = 0.0;
    f64 z0 = 0.0;
    for (u32 i = 0U; i < 480U; ++i) {
        walker_frame(&k, s.w, nph_vec3(2.0F, 0.0F, 2.0F));
        nearest = fmax(nearest, k.c.position.x);
        if (i == 240U) {
            z0 = k.c.position.z;
        }
    }
    const f64 limit = 1.0 - 0.3 - (f64)k.desc.skin;
    NTEST_LOG("against the wall at x %.6f (the skin's limit %.6f); along it %.4f m/s", nearest,
              limit, (k.c.position.z - z0) / (239.0 * (f64)DT));
    NTEST_EXPECT_TRUE(nearest <= limit + 1e-4);
    NTEST_EXPECT_NEAR_F64(k.c.position.x, limit, 1e-3);
    // The along-wall part of the walk, 2 m/s.
    NTEST_EXPECT_NEAR_F64((k.c.position.z - z0) / (239.0 * (f64)DT), 2.0, 0.02);
    NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    scene_end(&s);
}

NTEST(character, stops_still_in_corners)
{
    // A right-angled corner and a 30 degree one, walked into.
    static const f32 angles[2] = {1.5707963F, 0.5235988F};
    for (u32 a = 0U; a < 2U; ++a) {
        Scene s;
        scene_begin(&s);
        add_ground(s.w, 0.0);
        // Two walls meeting at the origin, opening towards -X.
        const f32 half = 0.5F * angles[a];
        for (u32 side = 0U; side < 2U; ++side) {
            const f32 turn = side == 0U ? half : -half;
            const NphQuat q = nph_quat_from_axis_angle(nph_vec3_unit_y(), turn);
            // The wall's inner face through the origin, along -X rotated.
            const NphVec3 along = nph_quat_rotate(q, nph_vec3(-1.0F, 0.0F, 0.0F));
            const NphVec3 out = nph_quat_rotate(q, nph_vec3(0.0F, 0.0F, side == 0U ? -1.0F : 1.0F));
            const NphVec3 c =
                nph_vec3_add(nph_vec3_scale(along, 3.0F), nph_vec3_scale(out, -0.25F));
            add_static_box(s.w, nph_dvec3((f64)c.x, 1.0, (f64)c.z), q, nph_vec3(3.0F, 1.0F, 0.25F));
        }
        Walker k;
        walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-4.0, FEET + 0.005, 0.0));
        walk(&k, s.w, nph_vec3(3.0F, 0.0F, 0.0F), 480U);
        const NphDVec3 settled = k.c.position;
        f64 moved = 0.0;
        for (u32 i = 0U; i < 120U; ++i) {
            walker_frame(&k, s.w, nph_vec3(3.0F, 0.0F, 0.0F));
            moved = fmax(moved, dist(k.c.position, settled));
        }
        NTEST_LOG("corner %.0f deg: settled at (%.4f %.4f), moved %.2g m after",
                  (f64)angles[a] * 57.29578, settled.x, settled.z, moved);
        NTEST_EXPECT_TRUE(moved < 1e-4);
        // Never into either wall: the axis a radius and a skin off both.
        const f64 off = 0.3 + (f64)k.desc.skin;
        const f64 dz = fabs(settled.z);
        const f64 dx = -settled.x;
        NTEST_EXPECT_TRUE(dx * (f64)nph_f32_sin(half) - dz * (f64)nph_f32_cos(half) >= off - 1e-3);
        scene_end(&s);
    }
}

// ---------------------------------------------------------------------------
// Slopes
// ---------------------------------------------------------------------------

NTEST(character, climbs_walkable_slopes_and_not_steeper_ones)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    add_ramp(s.w, -3.0, 0.5235988F); // 30 degrees
    add_ramp(s.w, 3.0, 1.0471976F);  // 60 degrees
    Walker gentle;
    Walker steep;
    walker_begin(&gentle, s.w, capsule_desc(), nph_dvec3(-2.0, FEET + 0.005, -3.0));
    walker_begin(&steep, s.w, capsule_desc(), nph_dvec3(-2.0, FEET + 0.005, 3.0));
    walk(&gentle, s.w, nph_vec3(2.0F, 0.0F, 0.0F), 600U);
    walk(&steep, s.w, nph_vec3(2.0F, 0.0F, 0.0F), 600U);
    NTEST_LOG("30 deg: at (%.3f %.3f), %d; 60 deg: at (%.3f %.3f), %d", gentle.c.position.x,
              gentle.c.position.y, gentle.c.ground, steep.c.position.x, steep.c.position.y,
              steep.c.ground);
    // Up the gentle one and standing on it.
    NTEST_EXPECT_TRUE(gentle.c.position.y > FEET + 1.0);
    NTEST_EXPECT_EQ_INT(gentle.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    // At the foot of the steep one, on the floor.
    NTEST_EXPECT_TRUE(steep.c.position.y < FEET + 0.35);
    NTEST_EXPECT_TRUE(steep.c.position.x < 0.5);
    scene_end(&s);
}

NTEST(character, stands_on_a_walkable_slope_and_slides_down_a_steep_one)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, -10.0);
    add_ramp(s.w, -3.0, 0.5235988F);
    add_ramp(s.w, 3.0, 1.0471976F);
    // Dropped onto each, 2 m up its slope.
    Walker on[2];
    const f32 angles[2] = {0.5235988F, 1.0471976F};
    for (u32 i = 0U; i < 2U; ++i) {
        const f64 x = 2.0 * (f64)nph_f32_cos(angles[i]);
        const f64 y = 2.0 * (f64)nph_f32_sin(angles[i]) + FEET + 0.3;
        walker_begin(&on[i], s.w, capsule_desc(), nph_dvec3(x, y, i == 0U ? -3.0 : 3.0));
        walk(&on[i], s.w, nph_vec3_zero(), 120U);
    }
    NTEST_EXPECT_EQ_INT(on[0].c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    const NphDVec3 standing = on[0].c.position;
    const NphDVec3 sliding = on[1].c.position;
    NTEST_EXPECT_EQ_INT(on[1].c.ground, NPH_PHYSICS_CHARACTER_STEEP);
    walk(&on[0], s.w, nph_vec3_zero(), 480U);
    walk(&on[1], s.w, nph_vec3_zero(), 30U);
    NTEST_LOG("on 30 deg: drifted %.2g m in 2 s; on 60 deg: slid %.3f m down in 1/8 s",
              dist(on[0].c.position, standing), sliding.y - on[1].c.position.y);
    NTEST_EXPECT_TRUE(dist(on[0].c.position, standing) < 1e-5);
    NTEST_EXPECT_TRUE(on[1].c.position.y < sliding.y - 0.01);
    NTEST_EXPECT_TRUE(on[1].c.position.x < sliding.x);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Steps, stairs, ledges, ceilings
// ---------------------------------------------------------------------------

NTEST(character, climbs_steps_up_to_its_step_height)
{
    static const f32 heights[3] = {0.1F, 0.25F, 0.4F};
    // At a run and at a stroll: 0.5 m/s is 2 mm a frame, less than the skin.
    static const f32 speeds[2] = {1.5F, 0.5F};
    for (u32 run = 0U; run < 6U; ++run) {
        const u32 h = run % 3U;
        const f32 speed = speeds[run / 3U];
        Scene s;
        scene_begin(&s);
        add_ground(s.w, 0.0);
        // A step's riser at x = 0.
        add_static_box(s.w, nph_dvec3(2.0, 0.5 * (f64)heights[h], 0.0), nph_quat_identity(),
                       nph_vec3(2.0F, 0.5F * heights[h], 2.0F));
        Walker k;
        walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-1.5, FEET + 0.005, 0.0));
        // 3 m, to x 1.5 unhindered.
        walk(&k, s.w, nph_vec3(speed, 0.0F, 0.0F), (u32)(3.0F / (speed * DT) + 0.5F));
        NTEST_LOG("a step of %.2f m at %.1f m/s: at x %.3f, y %.3f, %d", (f64)heights[h],
                  (f64)speed, k.c.position.x, k.c.position.y, k.c.ground);
        if (heights[h] <= k.desc.step_height) {
            // On top of it, standing a skin above it.
            NTEST_EXPECT_TRUE(k.c.position.x > 1.0);
            NTEST_EXPECT_NEAR_F64(k.c.position.y, FEET + (f64)heights[h] + (f64)k.desc.skin, 1e-3);
            NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
        } else {
            // Stopped at its riser, on the floor.
            NTEST_EXPECT_TRUE(k.c.position.x < 0.0);
            NTEST_EXPECT_NEAR_F64(k.c.position.y, FEET + (f64)k.desc.skin, 1e-3);
        }
        scene_end(&s);
    }
}

NTEST(character, walks_down_stairs_on_the_ground)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // Ten steps 0.2 m high and 0.3 m deep, down along +X from a landing at
    // 2 m.
    add_static_box(s.w, nph_dvec3(-2.0, 1.0, 0.0), nph_quat_identity(), nph_vec3(2.0F, 1.0F, 1.0F));
    for (u32 i = 0U; i < 10U; ++i) {
        const f32 top = 2.0F - 0.2F * (f32)(i + 1U);
        if (top > 0.0F) {
            add_static_box(s.w, nph_dvec3(0.15 + 0.3 * (f64)i, 0.5 * (f64)top, 0.0),
                           nph_quat_identity(), nph_vec3(0.15F, 0.5F * top, 1.0F));
        }
    }
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-1.0, FEET + 2.005, 0.0));
    walk(&k, s.w, nph_vec3_zero(), 10U);
    u32 airborne = 0U;
    for (u32 i = 0U; i < 960U; ++i) {
        walker_frame(&k, s.w, nph_vec3(1.5F, 0.0F, 0.0F));
        airborne += k.c.ground == NPH_PHYSICS_CHARACTER_AIRBORNE ? 1U : 0U;
    }
    NTEST_LOG("down the stairs: at x %.3f, y %.3f; %u frames off the ground", k.c.position.x,
              k.c.position.y, airborne);
    NTEST_EXPECT_EQ_UINT(airborne, 0U);
    NTEST_EXPECT_NEAR_F64(k.c.position.y, FEET + (f64)k.desc.skin, 1e-3); // at the bottom
    scene_end(&s);
}

NTEST(character, walks_off_a_ledge_and_falls)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    add_static_box(s.w, nph_dvec3(-2.0, 0.5, 0.0), nph_quat_identity(), nph_vec3(2.0F, 0.5F, 2.0F));
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-1.0, FEET + 1.005, 0.0));
    walk(&k, s.w, nph_vec3_zero(), 10U);
    u32 airborne = 0U;
    for (u32 i = 0U; i < 360U; ++i) {
        walker_frame(&k, s.w, nph_vec3(2.0F, 0.0F, 0.0F));
        airborne += k.c.ground == NPH_PHYSICS_CHARACTER_AIRBORNE ? 1U : 0U;
    }
    // A metre is more than the snap: it falls (about 0.45 s) and lands.
    NTEST_LOG("off the ledge: %u frames in the air", airborne);
    NTEST_EXPECT_TRUE(airborne > 60U && airborne < 140U);
    NTEST_EXPECT_NEAR_F64(k.c.position.y, FEET + (f64)k.desc.skin, 1e-3);
    NTEST_EXPECT_EQ_INT(k.c.ground, NPH_PHYSICS_CHARACTER_WALKABLE);
    scene_end(&s);
}

NTEST(character, a_low_ceiling_keeps_it_off_a_step)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    add_static_box(s.w, nph_dvec3(2.0, 0.1, 0.0), nph_quat_identity(), nph_vec3(2.0F, 0.1F, 2.0F));
    // A ceiling 1.85 m up over the step's edge: room for the capsule (1.8 m)
    // on the floor, not on the step.
    add_static_box(s.w, nph_dvec3(0.0, 1.85 + 0.5, 0.0), nph_quat_identity(),
                   nph_vec3(1.0F, 0.5F, 2.0F));
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-2.0, FEET + 0.005, 0.0));
    walk(&k, s.w, nph_vec3(1.5F, 0.0F, 0.0F), 360U);
    NTEST_LOG("under the ceiling: x %.3f, y %.3f", k.c.position.x, k.c.position.y);
    NTEST_EXPECT_TRUE(k.c.position.x < 0.0);
    NTEST_EXPECT_NEAR_F64(k.c.position.y, FEET + (f64)k.desc.skin, 1e-3);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Surfaces, platforms, bodies
// ---------------------------------------------------------------------------

NTEST(character, walks_over_meshes_and_heightfields_without_catching)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(601U);
    f32 heights[33U * 33U];
    for (u32 j = 0U; j < 33U; ++j) {
        for (u32 i = 0U; i < 33U; ++i) {
            // Gentle hills (under 20 degrees) and a little roughness.
            const f32 x = 0.5F * (f32)i;
            const f32 z = 0.5F * (f32)j;
            const f32 rough = test_range(&r, -0.02F, 0.02F);
            heights[j * 33U + i] = 0.4F * nph_f32_sin(0.6F * x) * nph_f32_cos(0.5F * z) + rough;
        }
    }
    NphPhysicsMesh *mesh = test_grid_mesh(alloc, 32U, 0.5F, -8.0F, -8.0F, heights, false);
    NphPhysicsHeightfield *field = test_heightfield(alloc, 33U, 33U, 0.5F, heights, NULL);
    const NphPhysicsGeometry floors[2] = {nph_physics_geometry_mesh(mesh),
                                          nph_physics_geometry_heightfield(field)};
    const NphDVec3 at[2] = {nph_dvec3_zero(), nph_dvec3(-8.0, 0.0, -8.0)};
    for (u32 f = 0U; f < 2U; ++f) {
        Scene s;
        scene_begin(&s);
        add_body(s.w, NPH_PHYSICS_BODY_STATIC, at[f], nph_quat_identity(), floors[f], 0.6F, 0.0F);
        Walker k;
        walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-6.0, FEET + 1.0, -5.0));
        walk(&k, s.w, nph_vec3_zero(), 120U);
        u32 airborne = 0U;
        f64 slowest = 1e9;
        for (u32 i = 0U; i < 960U; ++i) {
            const NphDVec3 before = k.c.position;
            walker_frame(&k, s.w, nph_vec3(1.5F, 0.0F, 1.0F));
            airborne += k.c.ground == NPH_PHYSICS_CHARACTER_AIRBORNE ? 1U : 0U;
            const f64 across_speed =
                sqrt((k.c.position.x - before.x) * (k.c.position.x - before.x) +
                     (k.c.position.z - before.z) * (k.c.position.z - before.z)) /
                (f64)DT;
            slowest = fmin(slowest, across_speed);
        }
        NTEST_LOG("%s: %u frames off the ground, slowest %.3f m/s across (of 1.80)",
                  f == 0U ? "mesh" : "heightfield", airborne, slowest);
        NTEST_EXPECT_EQ_UINT(airborne, 0U);
        // Never caught on a seam (the hills slow it by their slope only).
        NTEST_EXPECT_TRUE(slowest > 1.3);
        scene_end(&s);
    }
    nph_physics_mesh_free(mesh, alloc);
    nph_physics_heightfield_free(field, alloc);
    test_alloc_end(&ta);
}

NTEST(character, rides_a_moving_platform)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, -5.0);
    const NphPhysicsBody platform =
        add_body(s.w, NPH_PHYSICS_BODY_KINEMATIC, nph_dvec3(0.0, -0.1, 0.0), nph_quat_identity(),
                 nph_physics_geometry_box(nph_vec3(3.0F, 0.1F, 3.0F)), 0.6F, 0.0F);
    nph_physics_body_set_linear_velocity(s.w, platform, nph_vec3(1.0F, 0.0F, 0.0F));
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(0.0, FEET + 0.005, 0.0));
    for (u32 i = 0U; i < 240U; ++i) {
        // The game steps the world, then moves the character on what it
        // stands on (its velocity added to the walk).
        step(s.w, NULL, 1U);
        walker_frame(&k, s.w, nph_vec3_zero());
    }
    const NphDVec3 deck = nph_physics_body_position(s.w, platform);
    NTEST_LOG("on the platform: character at x %.4f, the platform at %.4f", k.c.position.x, deck.x);
    NTEST_EXPECT_EQ_UINT(k.c.ground_body.value, platform.value);
    NTEST_EXPECT_TRUE(nph_vec3_near(k.c.ground_velocity, nph_vec3(1.0F, 0.0F, 0.0F), 1e-5F));
    NTEST_EXPECT_NEAR_F64(k.c.position.x, deck.x, 0.01);
    scene_end(&s);
}

NTEST(character, pushes_a_box_it_walks_into)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    const NphPhysicsBody box = add_box(s.w, nph_dvec3(1.0, 0.25, 0.0), nph_vec3_splat(0.25F));
    step(s.w, NULL, 60U); // settle
    const NphDVec3 start = nph_physics_body_position(s.w, box);
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(-1.0, FEET + 0.005, 0.0));
    u32 hits_on_box = 0U;
    for (u32 i = 0U; i < 720U; ++i) { // 3 s: 1.4 m to the box, then pushing
        k.fall = k.c.ground == NPH_PHYSICS_CHARACTER_WALKABLE ? 0.0F : k.fall - 9.81F * DT;
        const NphVec3 v = nph_vec3(1.5F, k.fall, 0.0F);
        NphPhysicsCharacterHit hits[8];
        u32 count = 0U;
        NTEST_ASSERT_EQ_INT(
            nph_physics_character_move(s.w, &k.desc, &k.c, nph_vec3_scale(v, DT), hits, 8U, &count),
            NPH_RESULT_OK);
        for (u32 h = 0U; h < count; ++h) {
            hits_on_box += hits[h].body.value == box.value ? 1U : 0U;
        }
        nph_physics_character_push(s.w, hits, count, 80.0F, v);
        step(s.w, NULL, 1U);
    }
    const NphDVec3 end = nph_physics_body_position(s.w, box);
    NTEST_LOG("the box moved %.3f m along x (%u hits)", end.x - start.x, hits_on_box);
    NTEST_EXPECT_TRUE(hits_on_box > 10U);
    NTEST_EXPECT_TRUE(end.x - start.x > 1.0);
    // Never through it: the character stays behind it.
    NTEST_EXPECT_TRUE(k.c.position.x < end.x);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Overlaps, precision, other shapes, descriptions
// ---------------------------------------------------------------------------

NTEST(character, pushes_out_of_an_overlap)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // A wall with its face at x = 1; the character's axis 0.1 m from it.
    add_static_box(s.w, nph_dvec3(1.5, 1.0, 0.0), nph_quat_identity(), nph_vec3(0.5F, 1.0F, 10.0F));
    Walker k;
    walker_begin(&k, s.w, capsule_desc(), nph_dvec3(0.9, FEET - 0.05, 0.0)); // in the floor too
    walker_frame(&k, s.w, nph_vec3_zero());
    NTEST_LOG("pushed out to (%.5f %.5f)", k.c.position.x, k.c.position.y);
    NTEST_EXPECT_NEAR_F64(k.c.position.x, 1.0 - 0.3 - (f64)k.desc.skin, 1e-4);
    NTEST_EXPECT_NEAR_F64(k.c.position.y, FEET + (f64)k.desc.skin, 1e-4);
    scene_end(&s);
}

// The steps scene's walk, at `origin`: its path relative to it.
static void course(NphDVec3 origin, NphDVec3 path[48])
{
    Scene s;
    scene_begin(&s);
    add_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(origin.x, origin.y - 1.0, origin.z),
             nph_quat_identity(), nph_physics_geometry_box(nph_vec3(20.0F, 1.0F, 20.0F)), 0.6F,
             0.0F);
    add_static_box(s.w, nph_dvec3(origin.x + 2.0, origin.y + 0.1, origin.z), nph_quat_identity(),
                   nph_vec3(2.0F, 0.1F, 2.0F));
    add_static_box(s.w, nph_dvec3(origin.x + 1.5, origin.y + 1.0, origin.z + 2.5),
                   nph_quat_identity(), nph_vec3(0.5F, 1.0F, 0.5F));
    Walker k;
    walker_begin(&k, s.w, capsule_desc(),
                 nph_dvec3(origin.x - 1.5, origin.y + FEET + 0.005, origin.z + 0.5));
    for (u32 i = 0U; i < 48U; ++i) {
        walk(&k, s.w, nph_vec3(1.5F, 0.0F, 0.6F), 10U);
        path[i] = nph_dvec3(k.c.position.x - origin.x, k.c.position.y - origin.y,
                            k.c.position.z - origin.z);
    }
    scene_end(&s);
}

NTEST(character, the_same_moves_10000_km_out)
{
    NphDVec3 near[48];
    NphDVec3 far[48];
    course(nph_dvec3_zero(), near);
    course(nph_dvec3(1e7, 2e3, -1e7), far);
    f64 worst = 0.0;
    for (u32 i = 0U; i < 48U; ++i) {
        worst = fmax(worst, dist(near[i], far[i]));
    }
    NTEST_LOG("10 000 km out: at most %.3g m from the path at the origin", worst);
    NTEST_EXPECT_TRUE(worst < 1e-4);
    // It climbed the step and slid along the post.
    NTEST_EXPECT_TRUE(near[47].y > FEET + 0.15);
}

NTEST(character, boxes_and_balls_walk_and_climb_too)
{
    const NphPhysicsGeometry shapes[3] = {nph_physics_geometry_box(nph_vec3(0.3F, 0.9F, 0.3F)),
                                          nph_physics_geometry_sphere(0.4F),
                                          nph_physics_geometry_capsule(0.6F, 0.3F)};
    const f64 feet[3] = {0.9, 0.4, 0.9};
    for (u32 k = 0U; k < 3U; ++k) {
        Scene s;
        scene_begin(&s);
        add_ground(s.w, 0.0);
        add_static_box(s.w, nph_dvec3(2.0, 0.1, 0.0), nph_quat_identity(),
                       nph_vec3(2.0F, 0.1F, 2.0F));
        Walker w;
        walker_begin(&w, s.w, nph_physics_character_desc_default(shapes[k]),
                     nph_dvec3(-1.5, feet[k] + 0.005, 0.0));
        walk(&w, s.w, nph_vec3(1.5F, 0.0F, 0.0F), 480U);
        NTEST_LOG("shape %u: at x %.3f, y %.3f", k, w.c.position.x, w.c.position.y);
        NTEST_EXPECT_TRUE(w.c.position.x > 1.0);
        NTEST_EXPECT_NEAR_F64(w.c.position.y, feet[k] + 0.2 + (f64)w.desc.skin, 1e-3);
        scene_end(&s);
    }
}

NTEST(character, bad_descriptions_are_refused)
{
    Scene s;
    scene_begin(&s);
    const NphPhysicsCharacterDesc good = capsule_desc();
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&good), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(NULL), NPH_RESULT_INVALID_ARGUMENT);
    NphPhysicsCharacterDesc d = good;
    d.up = nph_vec3(0.0F, 2.0F, 0.0F);
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&d), NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.max_slope = 1.6F;
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&d), NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.skin = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&d), NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.iterations = 0U;
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&d), NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.step_height = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&d), NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.shape = nph_physics_geometry_capsule(-1.0F, 0.3F);
    NTEST_EXPECT_EQ_INT(nph_physics_character_desc_validate(&d), NPH_RESULT_INVALID_ARGUMENT);
    NphPhysicsCharacter c;
    NTEST_EXPECT_EQ_INT(
        nph_physics_character_init(s.w, &good, nph_dvec3_zero(), nph_quat_identity(), &c),
        NPH_RESULT_OK);
    const f32 nan = nph_f32_from_bits(0x7FC00000U);
    NTEST_EXPECT_EQ_INT(
        nph_physics_character_move(s.w, &good, &c, nph_vec3(nan, 0.0F, 0.0F), NULL, 0U, NULL),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(
        nph_physics_character_move(NULL, &good, &c, nph_vec3_zero(), NULL, 0U, NULL),
        NPH_RESULT_INVALID_ARGUMENT);
    scene_end(&s);
}

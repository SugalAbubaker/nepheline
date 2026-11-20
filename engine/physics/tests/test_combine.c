// SPDX-License-Identifier: Apache-2.0
// Combine modes (shape.h NphPhysicsCombine; docs/physics.md "Bodies and shapes"; ADR-0108): each
// mode's value, the higher mode deciding in either order, the defaults as before, in contacts on
// a ramp and in a bounce, changed between steps, and refused out of range.
#include "test_world_support.h"

#include <string.h>

static const NphPhysicsCombine k_modes[NPH_PHYSICS_COMBINE_COUNT] = {
    NPH_PHYSICS_COMBINE_DEFAULT, NPH_PHYSICS_COMBINE_AVERAGE, NPH_PHYSICS_COMBINE_MIN,
    NPH_PHYSICS_COMBINE_MULTIPLY, NPH_PHYSICS_COMBINE_MAX};

// A 4 degree ramp: tan 0.070, so a box holds where its pair's friction is above that.
static const f32 k_ramp_theta = 0.0698132F;

static u32 bits(f32 x)
{
    u32 u = 0U;
    memcpy(&u, &x, sizeof u);
    return u;
}

// 0.2 against 0.8, mode by mode, both shapes of the mode.
NTEST(combine, each_mode_combines_as_documented)
{
    static const f32 friction[NPH_PHYSICS_COMBINE_COUNT] = {0.4F, 0.5F, 0.2F, 0.16F, 0.8F};
    static const f32 restitution[NPH_PHYSICS_COMBINE_COUNT] = {0.8F, 0.5F, 0.2F, 0.16F, 0.8F};
    for (u32 m = 0U; m < NPH_PHYSICS_COMBINE_COUNT; ++m) {
        NTEST_EXPECT_NEAR_F32(nph_physics_combine_friction(0.2F, k_modes[m], 0.8F, k_modes[m]),
                              friction[m], 1e-6F);
        NTEST_EXPECT_NEAR_F32(nph_physics_combine_restitution(0.2F, k_modes[m], 0.8F, k_modes[m]),
                              restitution[m], 1e-6F);
    }
    // The defaults are what contacts always used.
    NTEST_EXPECT_EQ_UINT(bits(nph_physics_combine_friction(0.3F, NPH_PHYSICS_COMBINE_DEFAULT, 0.7F,
                                                           NPH_PHYSICS_COMBINE_DEFAULT)),
                         bits(nph_f32_sqrt(0.3F * 0.7F)));
    NTEST_EXPECT_EQ_UINT(bits(nph_physics_combine_restitution(0.3F, NPH_PHYSICS_COMBINE_DEFAULT,
                                                              0.7F, NPH_PHYSICS_COMBINE_DEFAULT)),
                         bits(0.7F));
}

// Of two modes the higher decides, and the order of the shapes never changes a bit.
NTEST(combine, the_higher_mode_decides_in_either_order)
{
    static const f32 values[4] = {0.0F, 0.13F, 0.5F, 1.0F};
    for (u32 ma = 0U; ma < NPH_PHYSICS_COMBINE_COUNT; ++ma) {
        for (u32 mb = 0U; mb < NPH_PHYSICS_COMBINE_COUNT; ++mb) {
            const NphPhysicsCombine top = k_modes[ma > mb ? ma : mb];
            for (u32 i = 0U; i < 4U; ++i) {
                for (u32 j = 0U; j < 4U; ++j) {
                    const f32 a = values[i];
                    const f32 b = values[j];
                    const f32 f = nph_physics_combine_friction(a, k_modes[ma], b, k_modes[mb]);
                    const f32 f_swapped =
                        nph_physics_combine_friction(b, k_modes[mb], a, k_modes[ma]);
                    NTEST_EXPECT_EQ_UINT(bits(f), bits(f_swapped));
                    NTEST_EXPECT_EQ_UINT(bits(f),
                                         bits(nph_physics_combine_friction(a, top, b, top)));
                    const f32 r = nph_physics_combine_restitution(a, k_modes[ma], b, k_modes[mb]);
                    const f32 r_swapped =
                        nph_physics_combine_restitution(b, k_modes[mb], a, k_modes[ma]);
                    NTEST_EXPECT_EQ_UINT(bits(r), bits(r_swapped));
                    NTEST_EXPECT_EQ_UINT(bits(r),
                                         bits(nph_physics_combine_restitution(a, top, b, top)));
                    NTEST_EXPECT_TRUE(r >= 0.0F && r <= 1.0F);
                }
            }
        }
    }
}

typedef struct Ramp {
    Scene s;
    NphPhysicsShape ramp;
    NphPhysicsShape box;
    NphPhysicsBody box_body;
    NphVec3 along;
} Ramp;

// A static 4 degree ramp and a box resting on it, each with its friction and mode.
static void ramp_begin(Ramp *r, f32 ramp_mu, NphPhysicsCombine ramp_mode, f32 box_mu,
                       NphPhysicsCombine box_mode)
{
    scene_begin(&r->s);
    const NphQuat q = nph_quat_from_rotation_z(k_ramp_theta);
    NphPhysicsBodyDesc rd = nph_physics_body_desc_default();
    rd.type = NPH_PHYSICS_BODY_STATIC;
    rd.rotation = q;
    NphPhysicsBody ramp_body;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(r->s.w, &rd, &ramp_body), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3(20.0F, 0.5F, 5.0F)));
    sd.friction = ramp_mu;
    sd.friction_combine = ramp_mode;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(r->s.w, ramp_body, &sd, &r->ramp), NPH_RESULT_OK);
    const NphVec3 up = nph_quat_rotate(q, nph_vec3_unit_y());
    r->along = nph_quat_rotate(q, nph_vec3_unit_x());
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3_from_vec3(nph_vec3_scale(up, 0.5F + 0.25F - 0.001F));
    bd.rotation = q;
    bd.enable_sleep = 0;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(r->s.w, &bd, &r->box_body), NPH_RESULT_OK);
    NphPhysicsShapeDesc bs =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3(0.25F, 0.25F, 0.25F)));
    bs.friction = box_mu;
    bs.friction_combine = box_mode;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(r->s.w, r->box_body, &bs, &r->box), NPH_RESULT_OK);
    step(r->s.w, NULL, 24U); // settle onto the ramp
}

// How far the box moves along the ramp in `seconds` (negative: down), and its acceleration.
static f32 ramp_run(Ramp *r, f32 seconds, f32 *out_accel)
{
    const NphDVec3 p0 = nph_physics_body_position(r->s.w, r->box_body);
    const f32 v0 = nph_vec3_dot(nph_physics_body_linear_velocity(r->s.w, r->box_body), r->along);
    const u32 steps = (u32)(seconds * 240.0F);
    step(r->s.w, NULL, steps);
    const NphDVec3 p1 = nph_physics_body_position(r->s.w, r->box_body);
    const f32 v1 = nph_vec3_dot(nph_physics_body_linear_velocity(r->s.w, r->box_body), r->along);
    if (out_accel != NULL) {
        *out_accel = (v1 - v0) / ((f32)steps * DT);
    }
    return nph_vec3_dot(nph_dvec3_to_vec3(nph_dvec3_sub(p1, p0)), r->along);
}

static f32 ramp_slide(f32 ramp_mu, NphPhysicsCombine ramp_mode, f32 box_mu,
                      NphPhysicsCombine box_mode, f32 *out_accel)
{
    Ramp r;
    ramp_begin(&r, ramp_mu, ramp_mode, box_mu, box_mode);
    const f32 moved = ramp_run(&r, 1.0F, out_accel);
    scene_end(&r.s);
    return moved;
}

// An icy box (0.02) on a rough ramp (0.6) at 4 degrees: by default the pair's friction is their
// geometric mean, 0.11, and it holds; as MIN it is 0.02, and the box slides at g (sin - 0.02 cos);
// the ramp's MAX outranks the box's MIN, and it holds again.
NTEST(combine, ice_slides_where_the_default_holds)
{
    f32 accel = 0.0F;
    const f32 held =
        ramp_slide(0.6F, NPH_PHYSICS_COMBINE_DEFAULT, 0.02F, NPH_PHYSICS_COMBINE_DEFAULT, &accel);
    NTEST_EXPECT_TRUE(nph_f32_abs(held) < 0.005F);
    const f32 slid =
        ramp_slide(0.6F, NPH_PHYSICS_COMBINE_DEFAULT, 0.02F, NPH_PHYSICS_COMBINE_MIN, &accel);
    NTEST_EXPECT_TRUE(slid < -0.1F);
    const f32 expected = 9.81F * (nph_f32_sin(k_ramp_theta) - 0.02F * nph_f32_cos(k_ramp_theta));
    NTEST_EXPECT_NEAR_F32(-accel, expected, 0.01F * expected);
    const f32 outranked =
        ramp_slide(0.6F, NPH_PHYSICS_COMBINE_MAX, 0.02F, NPH_PHYSICS_COMBINE_MIN, &accel);
    NTEST_EXPECT_TRUE(nph_f32_abs(outranked) < 0.005F);
}

// A ball dropped 2 m onto a floor, each with its restitution and mode: the rebound's height
// above the floor, e^2 times the drop.
static f32 bounce_height(f32 floor_e, NphPhysicsCombine floor_mode, f32 ball_e,
                         NphPhysicsCombine ball_mode)
{
    Scene s;
    scene_begin(&s);
    NphPhysicsBodyDesc fd = nph_physics_body_desc_default();
    fd.type = NPH_PHYSICS_BODY_STATIC;
    fd.position = nph_dvec3(0.0, -1.0, 0.0);
    NphPhysicsBody floor_body;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &fd, &floor_body), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3(10.0F, 1.0F, 10.0F)));
    sd.restitution = floor_e;
    sd.restitution_combine = floor_mode;
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, floor_body, &sd, &shape), NPH_RESULT_OK);
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(0.0, 2.5, 0.0);
    NphPhysicsBody ball;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &ball), NPH_RESULT_OK);
    NphPhysicsShapeDesc bs = nph_physics_shape_desc_default(nph_physics_geometry_sphere(0.5F));
    bs.restitution = ball_e;
    bs.restitution_combine = ball_mode;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, ball, &bs, &shape), NPH_RESULT_OK);
    f64 peak = -1.0;
    bool bounced = false;
    for (u32 i = 0U; i < 240U * 3U; ++i) {
        step(s.w, NULL, 1U);
        const f64 y = nph_physics_body_position(s.w, ball).y;
        const f32 vy = nph_physics_body_linear_velocity(s.w, ball).y;
        bounced = bounced || vy > 0.0F;
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

// A lively ball (0.9) on a dead floor (0.1): by default the larger wins and it bounces back to
// 1.62 m; as MIN on the ball, 0.1, to 2 cm; the floor's MULTIPLY outranks it, 0.09, about as low.
NTEST(combine, a_restitution_mode_sets_the_bounce)
{
    NTEST_EXPECT_NEAR_F32(
        bounce_height(0.1F, NPH_PHYSICS_COMBINE_DEFAULT, 0.9F, NPH_PHYSICS_COMBINE_DEFAULT), 1.62F,
        0.08F);
    NTEST_EXPECT_TRUE(
        bounce_height(0.1F, NPH_PHYSICS_COMBINE_DEFAULT, 0.9F, NPH_PHYSICS_COMBINE_MIN) < 0.06F);
    NTEST_EXPECT_TRUE(
        bounce_height(0.1F, NPH_PHYSICS_COMBINE_MULTIPLY, 0.9F, NPH_PHYSICS_COMBINE_MIN) < 0.06F);
}

// A mode set between steps applies from the next; a shape's modes read back; modes out of range
// are refused at creation.
NTEST(combine, modes_change_between_steps_and_are_checked)
{
    Ramp r;
    ramp_begin(&r, 0.6F, NPH_PHYSICS_COMBINE_DEFAULT, 0.02F, NPH_PHYSICS_COMBINE_DEFAULT);
    NTEST_EXPECT_TRUE(nph_f32_abs(ramp_run(&r, 0.5F, NULL)) < 0.005F);
    NTEST_EXPECT_EQ_INT(nph_physics_shape_friction_combine(r.s.w, r.box),
                        NPH_PHYSICS_COMBINE_DEFAULT);
    nph_physics_shape_set_friction_combine(r.s.w, r.box, NPH_PHYSICS_COMBINE_MIN);
    NTEST_EXPECT_EQ_INT(nph_physics_shape_friction_combine(r.s.w, r.box), NPH_PHYSICS_COMBINE_MIN);
    NTEST_EXPECT_TRUE(ramp_run(&r, 0.5F, NULL) < -0.02F);
    nph_physics_shape_set_restitution_combine(r.s.w, r.box, NPH_PHYSICS_COMBINE_MAX);
    NTEST_EXPECT_EQ_INT(nph_physics_shape_restitution_combine(r.s.w, r.box),
                        NPH_PHYSICS_COMBINE_MAX);
    NTEST_EXPECT_EQ_INT(nph_physics_shape_restitution_combine(r.s.w, r.ramp),
                        NPH_PHYSICS_COMBINE_DEFAULT);
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(nph_physics_geometry_sphere(0.1F));
    NTEST_EXPECT_EQ_INT(sd.friction_combine, NPH_PHYSICS_COMBINE_DEFAULT);
    NTEST_EXPECT_EQ_INT(sd.restitution_combine, NPH_PHYSICS_COMBINE_DEFAULT);
    NphPhysicsShape out;
    sd.friction_combine = NPH_PHYSICS_COMBINE_COUNT;
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(r.s.w, r.box_body, &sd, &out),
                        NPH_RESULT_INVALID_ARGUMENT);
    sd.friction_combine = NPH_PHYSICS_COMBINE_DEFAULT;
    sd.restitution_combine = NPH_PHYSICS_COMBINE_COUNT;
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(r.s.w, r.box_body, &sd, &out),
                        NPH_RESULT_INVALID_ARGUMENT);
    scene_end(&r.s);
}

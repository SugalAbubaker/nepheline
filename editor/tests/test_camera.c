// SPDX-License-Identifier: Apache-2.0
// The editor camera (editor/src/camera.h): orbit, look, pan, dolly, fly,
// and focus keep their invariants.
#include <nepheline/math/scalar.h>

#include <ntest/ntest.h>

#include "camera.h"

#define EPS 1e-4

static f64 dvec3_distance(NphDVec3 a, NphDVec3 b)
{
    const NphDVec3 d = nph_dvec3_sub(a, b);
    return nph_f64_sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

static void expect_dvec3(NphDVec3 a, NphDVec3 b, f64 tol)
{
    NTEST_EXPECT_NEAR_F64(a.x, b.x, tol);
    NTEST_EXPECT_NEAR_F64(a.y, b.y, tol);
    NTEST_EXPECT_NEAR_F64(a.z, b.z, tol);
}

NTEST(EditorCamera, the_default_looks_at_the_pivot_from_its_distance)
{
    EditorCamera c;
    editor_camera_default(&c);
    const NphDVec3 eye = editor_camera_eye(&c);
    NTEST_EXPECT_NEAR_F64(dvec3_distance(eye, c.pivot), c.distance, EPS);
    // The eye is in front of the origin, above it, looking down at it.
    NTEST_EXPECT_TRUE(eye.z > 0.0);
    NTEST_EXPECT_TRUE(eye.y > 0.0);
    const NphVec3 f = editor_camera_forward(&c);
    const NphDVec3 to_pivot = nph_dvec3_scale(nph_dvec3_sub(c.pivot, eye), 1.0 / (f64)c.distance);
    NTEST_EXPECT_NEAR_F64(to_pivot.x, f.x, EPS);
    NTEST_EXPECT_NEAR_F64(to_pivot.y, f.y, EPS);
    NTEST_EXPECT_NEAR_F64(to_pivot.z, f.z, EPS);
    const NphRenderCamera rc = editor_camera_render(&c);
    expect_dvec3(rc.position, eye, EPS);
    NTEST_EXPECT_NEAR_F32(rc.yfov, c.yfov, 0.0F);
    NTEST_EXPECT_NEAR_F32(rc.znear, c.znear, 0.0F);
    // A yaw of zero looks down -Z (ADR-0008); right is +X.
    c.pitch = 0.0F;
    const NphVec3 level = editor_camera_forward(&c);
    NTEST_EXPECT_NEAR_F32(level.z, -1.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(editor_camera_right(&c).x, 1.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(editor_camera_up(&c).y, 1.0F, 1e-6F);
}

NTEST(EditorCamera, orbiting_keeps_the_pivot_and_the_distance)
{
    EditorCamera c;
    editor_camera_default(&c);
    c.pivot = nph_dvec3(3.0, 1.0, -2.0);
    const NphDVec3 eye0 = editor_camera_eye(&c);
    editor_camera_orbit(&c, 0.7F, 0.3F);
    const NphDVec3 eye1 = editor_camera_eye(&c);
    expect_dvec3(c.pivot, nph_dvec3(3.0, 1.0, -2.0), 0.0);
    NTEST_EXPECT_NEAR_F64(dvec3_distance(eye1, c.pivot), c.distance, EPS);
    NTEST_EXPECT_TRUE(dvec3_distance(eye0, eye1) > 0.5);
}

NTEST(EditorCamera, looking_keeps_the_eye)
{
    EditorCamera c;
    editor_camera_default(&c);
    const NphDVec3 eye0 = editor_camera_eye(&c);
    const NphDVec3 pivot0 = c.pivot;
    editor_camera_look(&c, -0.4F, 0.2F);
    expect_dvec3(editor_camera_eye(&c), eye0, EPS);
    NTEST_EXPECT_TRUE(dvec3_distance(pivot0, c.pivot) > 0.5);
    NTEST_EXPECT_NEAR_F64(dvec3_distance(editor_camera_eye(&c), c.pivot), c.distance, EPS);
}

NTEST(EditorCamera, pitch_stops_short_of_straight_up_and_down)
{
    EditorCamera c;
    editor_camera_default(&c);
    editor_camera_orbit(&c, 0.0F, 10.0F);
    NTEST_EXPECT_NEAR_F32(c.pitch, EDITOR_CAMERA_MAX_PITCH, 0.0F);
    editor_camera_look(&c, 0.0F, -20.0F);
    NTEST_EXPECT_NEAR_F32(c.pitch, -EDITOR_CAMERA_MAX_PITCH, 0.0F);
    // Yaw wraps instead of growing without bound.
    editor_camera_orbit(&c, 7.0F * NPH_PI, 0.0F);
    NTEST_EXPECT_TRUE(nph_f32_abs(c.yaw) <= NPH_PI);
    NTEST_EXPECT_NEAR_F32(nph_f32_abs(c.yaw), NPH_PI, 1e-5F);
}

NTEST(EditorCamera, panning_moves_the_eye_and_the_pivot_together)
{
    EditorCamera c;
    editor_camera_default(&c);
    const NphDVec3 eye0 = editor_camera_eye(&c);
    const NphDVec3 pivot0 = c.pivot;
    editor_camera_pan(&c, 2.0F, -1.0F);
    const NphVec3 r = editor_camera_right(&c);
    const NphVec3 u = editor_camera_up(&c);
    const NphDVec3 expected =
        nph_dvec3_add_vec3(pivot0, nph_vec3_add(nph_vec3_scale(r, 2.0F), nph_vec3_scale(u, -1.0F)));
    expect_dvec3(c.pivot, expected, EPS);
    const NphDVec3 moved_eye = editor_camera_eye(&c);
    NTEST_EXPECT_NEAR_F64(dvec3_distance(moved_eye, eye0), nph_f64_sqrt(5.0), EPS);
}

NTEST(EditorCamera, dolly_scales_the_distance_within_its_limits)
{
    EditorCamera c;
    editor_camera_default(&c);
    const f32 d0 = c.distance;
    editor_camera_dolly(&c, 1.0F);
    NTEST_EXPECT_NEAR_F32(c.distance, d0 * 0.85F, 1e-5F);
    editor_camera_dolly(&c, -1.0F);
    NTEST_EXPECT_NEAR_F32(c.distance, d0, 1e-5F);
    editor_camera_dolly(&c, 1000.0F);
    NTEST_EXPECT_NEAR_F32(c.distance, EDITOR_CAMERA_MIN_DISTANCE, 0.0F);
    editor_camera_dolly(&c, -1000.0F);
    NTEST_EXPECT_NEAR_F32(c.distance, EDITOR_CAMERA_MAX_DISTANCE, 0.0F);
}

NTEST(EditorCamera, flying_moves_along_the_view_and_the_world_up)
{
    EditorCamera c;
    editor_camera_default(&c);
    c.fly_speed = 4.0F;
    const NphVec3 f = editor_camera_forward(&c);
    const NphDVec3 pivot0 = c.pivot;
    editor_camera_fly(&c, nph_vec3(0.0F, 0.0F, 1.0F), 0.5F); // two metres forward
    expect_dvec3(c.pivot, nph_dvec3_add_vec3(pivot0, nph_vec3_scale(f, 2.0F)), EPS);
    const NphDVec3 pivot1 = c.pivot;
    editor_camera_fly(&c, nph_vec3(0.0F, 1.0F, 0.0F), 0.25F); // one metre up, the world's
    expect_dvec3(c.pivot, nph_dvec3_add(pivot1, nph_dvec3(0.0, 1.0, 0.0)), EPS);
}

NTEST(EditorCamera, focus_fits_a_sphere_in_the_field_of_view)
{
    EditorCamera c;
    editor_camera_default(&c);
    editor_camera_focus(&c, nph_dvec3(10.0, 2.0, -5.0), 3.0F);
    expect_dvec3(c.pivot, nph_dvec3(10.0, 2.0, -5.0), 0.0);
    NTEST_EXPECT_NEAR_F32(c.distance, 3.0F / nph_f32_sin(c.yfov * 0.5F), 1e-4F);
    // The sphere touches the edges of the vertical field of view.
    const f32 half = nph_f32_asin(3.0F / c.distance);
    NTEST_EXPECT_NEAR_F32(half, c.yfov * 0.5F, 1e-5F);
}

// The pivot is 64-bit (§8.2): far from the origin, centimetre moves stay
// centimetre moves.
NTEST(EditorCamera, far_from_the_origin_small_moves_stay_exact)
{
    EditorCamera c;
    editor_camera_default(&c);
    c.pivot = nph_dvec3(1.0e7, 0.0, -1.0e7); // ten thousand kilometres
    c.yaw = 0.0F;
    c.pitch = 0.0F;
    editor_camera_pan(&c, 0.01F, 0.0F);
    NTEST_EXPECT_NEAR_F64(c.pivot.x - 1.0e7, 0.01, 1e-6);
    NTEST_EXPECT_NEAR_F64(c.pivot.z + 1.0e7, 0.0, 1e-6);
}

// SPDX-License-Identifier: Apache-2.0
// The editor's camera. See camera.h.
#include "camera.h"

#include <nepheline/math/scalar.h>

void editor_camera_default(EditorCamera *c)
{
    c->pivot = nph_dvec3_zero();
    c->yaw = 0.0F;
    c->pitch = -0.35F;
    c->distance = 8.0F;
    c->yfov = nph_f32_radians(60.0F);
    c->znear = 0.05F;
    c->fly_speed = 6.0F;
}

NphQuat editor_camera_orientation(const EditorCamera *c)
{
    return nph_quat_mul(nph_quat_from_rotation_y(c->yaw), nph_quat_from_rotation_x(c->pitch));
}

NphVec3 editor_camera_forward(const EditorCamera *c)
{
    return nph_quat_rotate(editor_camera_orientation(c), nph_vec3(0.0F, 0.0F, -1.0F));
}

NphVec3 editor_camera_right(const EditorCamera *c)
{
    return nph_quat_rotate(editor_camera_orientation(c), nph_vec3(1.0F, 0.0F, 0.0F));
}

NphVec3 editor_camera_up(const EditorCamera *c)
{
    return nph_quat_rotate(editor_camera_orientation(c), nph_vec3(0.0F, 1.0F, 0.0F));
}

NphDVec3 editor_camera_eye(const EditorCamera *c)
{
    return nph_dvec3_add_vec3(c->pivot, nph_vec3_scale(editor_camera_forward(c), -c->distance));
}

NphRenderCamera editor_camera_render(const EditorCamera *c)
{
    NphRenderCamera rc;
    rc.position = editor_camera_eye(c);
    rc.orientation = editor_camera_orientation(c);
    rc.yfov = c->yfov;
    rc.znear = c->znear;
    return rc;
}

static void turn(EditorCamera *c, f32 dyaw, f32 dpitch)
{
    c->yaw = nph_f32_wrap_angle(c->yaw + dyaw);
    c->pitch = nph_f32_clamp(c->pitch + dpitch, -EDITOR_CAMERA_MAX_PITCH, EDITOR_CAMERA_MAX_PITCH);
}

void editor_camera_orbit(EditorCamera *c, f32 dyaw, f32 dpitch)
{
    turn(c, dyaw, dpitch);
}

void editor_camera_look(EditorCamera *c, f32 dyaw, f32 dpitch)
{
    const NphDVec3 eye = editor_camera_eye(c);
    turn(c, dyaw, dpitch);
    c->pivot = nph_dvec3_add_vec3(eye, nph_vec3_scale(editor_camera_forward(c), c->distance));
}

void editor_camera_pan(EditorCamera *c, f32 right, f32 up)
{
    const NphVec3 move = nph_vec3_add(nph_vec3_scale(editor_camera_right(c), right),
                                      nph_vec3_scale(editor_camera_up(c), up));
    c->pivot = nph_dvec3_add_vec3(c->pivot, move);
}

void editor_camera_dolly(EditorCamera *c, f32 steps)
{
    c->distance = nph_f32_clamp(c->distance * nph_f32_pow(0.85F, steps), EDITOR_CAMERA_MIN_DISTANCE,
                                EDITOR_CAMERA_MAX_DISTANCE);
}

void editor_camera_fly(EditorCamera *c, NphVec3 dir, f32 dt)
{
    const f32 step = c->fly_speed * dt;
    NphVec3 move = nph_vec3_scale(editor_camera_right(c), dir.x * step);
    move = nph_vec3_add(move, nph_vec3(0.0F, dir.y * step, 0.0F));
    move = nph_vec3_add(move, nph_vec3_scale(editor_camera_forward(c), dir.z * step));
    c->pivot = nph_dvec3_add_vec3(c->pivot, move);
}

void editor_camera_focus(EditorCamera *c, NphDVec3 center, f32 radius)
{
    c->pivot = center;
    const f32 r = radius > 0.01F ? radius : 0.01F;
    c->distance = nph_f32_clamp(r / nph_f32_sin(c->yfov * 0.5F), EDITOR_CAMERA_MIN_DISTANCE,
                                EDITOR_CAMERA_MAX_DISTANCE);
}

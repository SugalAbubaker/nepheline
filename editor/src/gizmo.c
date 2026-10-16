// SPDX-License-Identifier: Apache-2.0
// The transform gizmo's geometry. See gizmo.h.
#include "gizmo.h"

#include <nepheline/math/quat.h>
#include <nepheline/math/scalar.h>

#include <string.h>

#define PARALLEL_EPS 1e-6F

void editor_gizmo_axes(const NphWorldTransform *t, bool local, NphVec3 out[3])
{
    out[0] = nph_vec3_unit_x();
    out[1] = nph_vec3_unit_y();
    out[2] = nph_vec3_unit_z();
    if (local) {
        out[0] = nph_vec3_normalize_or(t->x_axis, out[0]);
        out[1] = nph_vec3_normalize_or(t->y_axis, out[1]);
        out[2] = nph_vec3_normalize_or(t->z_axis, out[2]);
    }
}

bool editor_gizmo_axis_param(NphVec3 ray_origin, NphVec3 ray_dir, NphVec3 axis, f32 *out)
{
    // The nearest points of the lines s * axis and origin + t * dir
    // (Ericson, Real-Time Collision Detection, 5.1.8), with w = -origin.
    const f32 b = nph_vec3_dot(axis, ray_dir);
    const f32 c = nph_vec3_dot(ray_dir, ray_dir);
    const f32 a = nph_vec3_dot(axis, axis);
    const NphVec3 w = nph_vec3_neg(ray_origin);
    const f32 d = nph_vec3_dot(axis, w);
    const f32 e = nph_vec3_dot(ray_dir, w);
    const f32 denom = a * c - b * b;
    if (denom <= PARALLEL_EPS * a * c) {
        return false;
    }
    *out = (b * e - c * d) / denom;
    return true;
}

bool editor_gizmo_plane_hit(NphVec3 ray_origin, NphVec3 ray_dir, NphVec3 normal, NphVec3 *out)
{
    const f32 denom = nph_vec3_dot(normal, ray_dir);
    if (nph_f32_abs(denom) <= PARALLEL_EPS) {
        return false;
    }
    const f32 t = -nph_vec3_dot(normal, ray_origin) / denom;
    if (t < 0.0F) {
        return false;
    }
    *out = nph_vec3_mul_add(ray_origin, ray_dir, t);
    return true;
}

f32 editor_gizmo_angle(NphVec3 a, NphVec3 b, NphVec3 axis)
{
    const NphVec3 pa = nph_vec3_sub(a, nph_vec3_scale(axis, nph_vec3_dot(a, axis)));
    const NphVec3 pb = nph_vec3_sub(b, nph_vec3_scale(axis, nph_vec3_dot(b, axis)));
    return nph_f32_atan2(nph_vec3_dot(nph_vec3_cross(pa, pb), axis), nph_vec3_dot(pa, pb));
}

f32 editor_gizmo_snap(f32 value, f32 step)
{
    return step > 0.0F ? nph_f32_round(value / step) * step : value;
}

static u32 axis_index(EditorGizmoHandle h)
{
    switch (h) {
    case EDITOR_GIZMO_Y:
    case EDITOR_GIZMO_PLANE_Y:
        return 1U;
    case EDITOR_GIZMO_Z:
    case EDITOR_GIZMO_PLANE_Z:
        return 2U;
    default:
        return 0U;
    }
}

static bool is_plane(EditorGizmoHandle h)
{
    return h == EDITOR_GIZMO_PLANE_X || h == EDITOR_GIZMO_PLANE_Y || h == EDITOR_GIZMO_PLANE_Z;
}

bool editor_gizmo_begin(EditorGizmoDrag *d, EditorGizmoMode mode, EditorGizmoHandle handle,
                        const NphWorldTransform *start, bool local, NphVec3 ray_origin,
                        NphVec3 ray_dir, f32 mouse_x)
{
    memset(d, 0, sizeof *d);
    d->mode = mode;
    d->handle = handle;
    d->start = *start;
    d->grab_x = mouse_x;
    editor_gizmo_axes(start, mode == EDITOR_GIZMO_SCALE || local, d->axes);
    if (handle == EDITOR_GIZMO_NONE) {
        return false;
    }
    if (handle == EDITOR_GIZMO_ALL) {
        return true;
    }
    const NphVec3 axis = d->axes[axis_index(handle)];
    if (mode == EDITOR_GIZMO_ROTATE || is_plane(handle)) {
        return editor_gizmo_plane_hit(ray_origin, ray_dir, axis, &d->grab);
    }
    return editor_gizmo_axis_param(ray_origin, ray_dir, axis, &d->grab_param);
}

static NphWorldTransform translate(const EditorGizmoDrag *d, NphVec3 o, NphVec3 dir, bool snap)
{
    NphWorldTransform t = d->start;
    const u32 i = axis_index(d->handle);
    if (is_plane(d->handle)) {
        NphVec3 hit;
        if (!editor_gizmo_plane_hit(o, dir, d->axes[i], &hit)) {
            return t;
        }
        NphVec3 delta = nph_vec3_sub(hit, d->grab);
        if (snap) {
            // Whole steps along each of the plane's two axes.
            const NphVec3 u = d->axes[(i + 1U) % 3U];
            const NphVec3 v = d->axes[(i + 2U) % 3U];
            delta = nph_vec3_add(nph_vec3_scale(u, editor_gizmo_snap(nph_vec3_dot(delta, u),
                                                                     EDITOR_GIZMO_SNAP_TRANSLATE)),
                                 nph_vec3_scale(v, editor_gizmo_snap(nph_vec3_dot(delta, v),
                                                                     EDITOR_GIZMO_SNAP_TRANSLATE)));
        }
        t.translation = nph_dvec3_add_vec3(t.translation, delta);
        return t;
    }
    f32 s = 0.0F;
    if (!editor_gizmo_axis_param(o, dir, d->axes[i], &s)) {
        return t;
    }
    f32 moved = s - d->grab_param;
    if (snap) {
        moved = editor_gizmo_snap(moved, EDITOR_GIZMO_SNAP_TRANSLATE);
    }
    t.translation = nph_dvec3_add_vec3(t.translation, nph_vec3_scale(d->axes[i], moved));
    return t;
}

static NphWorldTransform rotate(const EditorGizmoDrag *d, NphVec3 o, NphVec3 dir, bool snap)
{
    NphWorldTransform t = d->start;
    const NphVec3 axis = d->axes[axis_index(d->handle)];
    NphVec3 hit;
    if (!editor_gizmo_plane_hit(o, dir, axis, &hit)) {
        return t;
    }
    f32 angle = editor_gizmo_angle(d->grab, hit, axis);
    if (snap) {
        angle = editor_gizmo_snap(angle, EDITOR_GIZMO_SNAP_ROTATE);
    }
    const NphQuat q = nph_quat_from_axis_angle(axis, angle);
    t.x_axis = nph_quat_rotate(q, d->start.x_axis);
    t.y_axis = nph_quat_rotate(q, d->start.y_axis);
    t.z_axis = nph_quat_rotate(q, d->start.z_axis);
    return t;
}

static NphWorldTransform scale(const EditorGizmoDrag *d, NphVec3 o, NphVec3 dir, f32 mouse_x,
                               bool snap)
{
    NphWorldTransform t = d->start;
    f32 f = 1.0F;
    if (d->handle == EDITOR_GIZMO_ALL) {
        f = 1.0F + (mouse_x - d->grab_x) * EDITOR_GIZMO_SCALE_PER_UNIT;
    } else {
        f32 s = 0.0F;
        if (!editor_gizmo_axis_param(o, dir, d->axes[axis_index(d->handle)], &s) ||
            nph_f32_abs(d->grab_param) < 1e-6F) {
            return t;
        }
        f = s / d->grab_param;
    }
    if (snap) {
        f = editor_gizmo_snap(f, EDITOR_GIZMO_SNAP_SCALE);
    }
    f = nph_f32_max(f, 0.01F); // never flat, never mirrored by a drag
    const u32 i = axis_index(d->handle);
    if (d->handle == EDITOR_GIZMO_ALL || i == 0U) {
        t.x_axis = nph_vec3_scale(t.x_axis, f);
    }
    if (d->handle == EDITOR_GIZMO_ALL || i == 1U) {
        t.y_axis = nph_vec3_scale(t.y_axis, f);
    }
    if (d->handle == EDITOR_GIZMO_ALL || i == 2U) {
        t.z_axis = nph_vec3_scale(t.z_axis, f);
    }
    return t;
}

NphWorldTransform editor_gizmo_drag(const EditorGizmoDrag *d, NphVec3 ray_origin, NphVec3 ray_dir,
                                    f32 mouse_x, bool snap)
{
    switch (d->mode) {
    case EDITOR_GIZMO_ROTATE:
        return rotate(d, ray_origin, ray_dir, snap);
    case EDITOR_GIZMO_SCALE:
        return scale(d, ray_origin, ray_dir, mouse_x, snap);
    case EDITOR_GIZMO_TRANSLATE:
    default:
        return translate(d, ray_origin, ray_dir, snap);
    }
}

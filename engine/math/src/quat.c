// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/quat.h>

void nph_quat_to_axis_angle(NphQuat q, NphVec3 *out_axis, f32 *out_angle)
{
    // Canonicalise to w >= 0 so the angle lands in [0, pi].
    const NphQuat u = q.w < 0.0F ? nph_quat_neg(q) : q;
    const NphVec3 v = nph_quat_xyz(u);
    const f32 s = nph_vec3_length(v);
    if (s < 1e-12F) {
        *out_axis = nph_vec3_unit_x();
        *out_angle = 0.0F;
        return;
    }
    *out_axis = nph_vec3_scale(v, 1.0F / s);
    // atan2 keeps full precision for small angles where acos(w) loses it.
    *out_angle = 2.0F * nph_f32_atan2(s, u.w);
}

NphQuat nph_quat_from_euler(NphVec3 angles)
{
    const NphQuat rx = nph_quat_from_rotation_x(angles.x);
    const NphQuat ry = nph_quat_from_rotation_y(angles.y);
    const NphQuat rz = nph_quat_from_rotation_z(angles.z);
    return nph_quat_mul(rz, nph_quat_mul(ry, rx));
}

NphVec3 nph_quat_to_euler(NphQuat q)
{
    // The rotation matrix of Rz·Ry·Rx is
    //   [ cy cz   cz sx sy - cx sz   cx cz sy + sx sz ]
    //   [ cy sz   cx cz + sx sy sz   cx sy sz - cz sx ]
    //   [ -sy     cy sx              cx cy            ]
    // so cy = sqrt(m00² + m10²), y = atan2(-m20, cy), and away from the
    // poles x = atan2(m21, m22), z = atan2(m10, m00). atan2 for y (rather
    // than asin(-m20)) keeps precision near the poles, where asin is
    // ill-conditioned.
    const NphVec3 cx = nph_quat_axis_x(q);
    const NphVec3 cy = nph_quat_axis_y(q);
    const NphVec3 cz = nph_quat_axis_z(q);
    const f32 cos_y = nph_f32_sqrt(cx.x * cx.x + cx.y * cx.y); // sqrt(m00² + m10²)
    NphVec3 e;
    e.y = nph_f32_atan2(-cx.z, cos_y); // -m20
    if (cos_y > 1e-5F) {
        e.x = nph_f32_atan2(cy.z, cz.z); // m21, m22
        e.z = nph_f32_atan2(cx.y, cx.x); // m10, m00
    } else {
        // Gimbal lock: x and z rotate about the same axis, so only x -/+ z is
        // determined. Choose z = 0; then m11 = cos(x) and m12 = -sin(x) at
        // either pole.
        e.x = nph_f32_atan2(-cz.y, cy.y); // -m12, m11
        e.z = 0.0F;
    }
    return e;
}

NphQuat nph_quat_from_to(NphVec3 from, NphVec3 to)
{
    const f32 d = nph_vec3_dot(from, to);
    if (d >= 1.0F - 1e-6F) {
        return nph_quat_identity();
    }
    if (d <= -1.0F + 1e-6F) {
        // Half turn about any axis perpendicular to `from`.
        const NphVec3 axis = nph_vec3_perpendicular(from);
        return nph_quat_from_xyz_w(axis, 0.0F);
    }
    // (from x to, 1 + from·to) is the half-angle quaternion before
    // normalisation: |c| = sin(a), 1 + d = 1 + cos(a) = 2 cos²(a/2).
    const NphVec3 c = nph_vec3_cross(from, to);
    return nph_quat_normalize(nph_quat_from_xyz_w(c, 1.0F + d));
}

NphQuat nph_quat_slerp(NphQuat a, NphQuat b, f32 t)
{
    f32 cos_theta = nph_quat_dot(a, b);
    NphQuat bb = b;
    if (cos_theta < 0.0F) {
        // Take the shorter of the two arcs: q and -q are the same rotation.
        bb = nph_quat_neg(b);
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995F) {
        // Nearly parallel: sin(theta) is too small to divide by, and nlerp
        // is within rounding of slerp here.
        return nph_quat_nlerp(a, bb, t);
    }
    const f32 theta = nph_f32_acos(cos_theta);
    const f32 inv_sin = 1.0F / nph_f32_sin(theta);
    const f32 wa = nph_f32_sin((1.0F - t) * theta) * inv_sin;
    const f32 wb = nph_f32_sin(t * theta) * inv_sin;
    return nph_quat_add(nph_quat_scale(a, wa), nph_quat_scale(bb, wb));
}

void nph_quat_swing_twist(NphQuat q, NphVec3 twist_axis, NphQuat *out_swing, NphQuat *out_twist)
{
    // The twist is the projection of q's vector part onto the axis, with
    // q's scalar part, renormalised. If that projection vanishes the
    // rotation is a half turn about a perpendicular axis: pure swing.
    const NphVec3 p = nph_vec3_project(nph_quat_xyz(q), twist_axis);
    const NphQuat raw = nph_quat_from_xyz_w(p, q.w);
    NphQuat twist;
    if (nph_quat_length_sq(raw) < 1e-12F) {
        twist = nph_quat_identity();
    } else {
        twist = nph_quat_normalize(raw);
    }
    *out_twist = twist;
    *out_swing = nph_quat_mul(q, nph_quat_conjugate(twist));
}

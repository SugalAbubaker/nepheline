// SPDX-License-Identifier: Apache-2.0
#pragma once

// Quaternions (CLAUDE.md §8.2; docs/adr/0008-math-conventions.md §3).
//
// Layout {x, y, z, w} with the scalar last, as in glTF and GLSL. The
// Hamilton product nph_quat_mul(a, b) == a·b applies b first, then a, so
// nph_quat_rotate(nph_quat_mul(a, b), v) == nph_quat_rotate(a, nph_quat_rotate(b, v)).
// Functions documented as taking a rotation expect a unit quaternion and do
// not renormalise; nph_quat_normalize after long chains of multiplications.
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphQuat {
    f32 x, y, z, w;
} NphQuat;

NPH_STATIC_ASSERT(sizeof(NphQuat) == 16U, "NphQuat is four floats");

NPH_INLINE NphQuat nph_quat(f32 x, f32 y, f32 z, f32 w)
{
    return (NphQuat){x, y, z, w};
}
NPH_INLINE NphQuat nph_quat_identity(void)
{
    return (NphQuat){0.0F, 0.0F, 0.0F, 1.0F};
}
NPH_INLINE NphQuat nph_quat_from_vec4(NphVec4 v)
{
    return (NphQuat){v.x, v.y, v.z, v.w};
}
NPH_INLINE NphVec4 nph_quat_to_vec4(NphQuat q)
{
    return (NphVec4){q.x, q.y, q.z, q.w};
}
// The vector (imaginary) part.
NPH_INLINE NphVec3 nph_quat_xyz(NphQuat q)
{
    return (NphVec3){q.x, q.y, q.z};
}
NPH_INLINE NphQuat nph_quat_from_xyz_w(NphVec3 xyz, f32 w)
{
    return (NphQuat){xyz.x, xyz.y, xyz.z, w};
}

NPH_INLINE NphQuat nph_quat_add(NphQuat a, NphQuat b)
{
    return (NphQuat){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
NPH_INLINE NphQuat nph_quat_sub(NphQuat a, NphQuat b)
{
    return (NphQuat){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
NPH_INLINE NphQuat nph_quat_scale(NphQuat q, f32 s)
{
    return (NphQuat){q.x * s, q.y * s, q.z * s, q.w * s};
}
NPH_INLINE NphQuat nph_quat_neg(NphQuat q)
{
    return (NphQuat){-q.x, -q.y, -q.z, -q.w};
}

// Hamilton product a·b: the rotation that applies b, then a.
NPH_INLINE NphQuat nph_quat_mul(NphQuat a, NphQuat b)
{
    return (NphQuat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
NPH_INLINE NphQuat nph_quat_conjugate(NphQuat q)
{
    return (NphQuat){-q.x, -q.y, -q.z, q.w};
}
NPH_INLINE f32 nph_quat_dot(NphQuat a, NphQuat b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
NPH_INLINE f32 nph_quat_length_sq(NphQuat q)
{
    return nph_quat_dot(q, q);
}
NPH_INLINE f32 nph_quat_length(NphQuat q)
{
    return nph_f32_sqrt(nph_quat_length_sq(q));
}
// Multiplicative inverse of any non-zero quaternion. For a unit quaternion
// the conjugate is cheaper and equal.
NPH_INLINE NphQuat nph_quat_inverse(NphQuat q)
{
    const f32 len_sq = nph_quat_length_sq(q);
    NPH_ASSERT_MSG(len_sq > 0.0F, "nph_quat_inverse of a zero quaternion");
    return nph_quat_scale(nph_quat_conjugate(q), 1.0F / len_sq);
}
// q must not be zero (asserted).
NPH_INLINE NphQuat nph_quat_normalize(NphQuat q)
{
    const f32 len_sq = nph_quat_length_sq(q);
    NPH_ASSERT_MSG(len_sq > 0.0F, "nph_quat_normalize of a zero quaternion");
    return nph_quat_scale(q, nph_f32_rsqrt(len_sq));
}
// As nph_quat_normalize, returning the identity for a zero quaternion.
NPH_INLINE NphQuat nph_quat_normalize_or_identity(NphQuat q)
{
    const f32 len_sq = nph_quat_length_sq(q);
    return len_sq > 0.0F ? nph_quat_scale(q, nph_f32_rsqrt(len_sq)) : nph_quat_identity();
}
NPH_INLINE bool nph_quat_is_normalized(NphQuat q, f32 tol)
{
    return nph_f32_abs(nph_quat_length_sq(q) - 1.0F) <= 2.0F * tol;
}

// Rotation of `angle` radians about the unit vector `axis` (right-handed:
// positive angles turn counter-clockwise when looking down the axis).
NPH_INLINE NphQuat nph_quat_from_axis_angle(NphVec3 axis, f32 angle)
{
    f32 s;
    f32 c;
    nph_f32_sincos(0.5F * angle, &s, &c);
    return (NphQuat){axis.x * s, axis.y * s, axis.z * s, c};
}
// Inverse of nph_quat_from_axis_angle for a unit quaternion: the angle is
// in [0, pi]; the axis is unit, or +X when the angle is 0.
void nph_quat_to_axis_angle(NphQuat q, NphVec3 *out_axis, f32 *out_angle);

// Rotation of `angle` radians about the X, Y, or Z axis.
NPH_INLINE NphQuat nph_quat_from_rotation_x(f32 angle)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_x(), angle);
}
NPH_INLINE NphQuat nph_quat_from_rotation_y(f32 angle)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_y(), angle);
}
NPH_INLINE NphQuat nph_quat_from_rotation_z(f32 angle)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_z(), angle);
}

// Euler angles {x, y, z} in radians applied as extrinsic rotations about
// the world X axis first, then Y, then Z (q = Rz · Ry · Rx), the glTF /
// Blender "XYZ" order. Equivalently intrinsic Z, then Y', then X''.
NphQuat nph_quat_from_euler(NphVec3 angles);

// Inverse of nph_quat_from_euler for a unit quaternion. y is in
// [-pi/2, pi/2]; at the poles (|y| == pi/2) the x/z split is ambiguous and
// z is set to 0.
NphVec3 nph_quat_to_euler(NphQuat q);

// Rotates v by the unit quaternion q (computes q v q*).
NPH_INLINE NphVec3 nph_quat_rotate(NphQuat q, NphVec3 v)
{
    // v' = v + 2 w (u × v) + 2 u × (u × v), with u the vector part. Two cross
    // products instead of two quaternion products (Shoemake's formulation).
    const NphVec3 u = nph_quat_xyz(q);
    const NphVec3 t = nph_vec3_scale(nph_vec3_cross(u, v), 2.0F);
    return nph_vec3_add(nph_vec3_add(v, nph_vec3_scale(t, q.w)), nph_vec3_cross(u, t));
}
// Rotates v by the inverse of the unit quaternion q.
NPH_INLINE NphVec3 nph_quat_rotate_inverse(NphQuat q, NphVec3 v)
{
    return nph_quat_rotate(nph_quat_conjugate(q), v);
}
// The rotated basis vectors: the columns of the equivalent 3x3 matrix.
NPH_INLINE NphVec3 nph_quat_axis_x(NphQuat q)
{
    return (NphVec3){1.0F - 2.0F * (q.y * q.y + q.z * q.z), 2.0F * (q.x * q.y + q.w * q.z),
                     2.0F * (q.x * q.z - q.w * q.y)};
}
NPH_INLINE NphVec3 nph_quat_axis_y(NphQuat q)
{
    return (NphVec3){2.0F * (q.x * q.y - q.w * q.z), 1.0F - 2.0F * (q.x * q.x + q.z * q.z),
                     2.0F * (q.y * q.z + q.w * q.x)};
}
NPH_INLINE NphVec3 nph_quat_axis_z(NphQuat q)
{
    return (NphVec3){2.0F * (q.x * q.z + q.w * q.y), 2.0F * (q.y * q.z - q.w * q.x),
                     1.0F - 2.0F * (q.x * q.x + q.y * q.y)};
}

// The shortest rotation taking the unit vector `from` onto the unit vector
// `to` (Melax, "The Shortest Arc Quaternion", Game Programming Gems 1).
// Antiparallel inputs rotate by pi about an axis perpendicular to `from`.
NphQuat nph_quat_from_to(NphVec3 from, NphVec3 to);

// The angle of rotation of a unit quaternion, in [0, pi]. atan2 of the
// vector and scalar parts keeps full precision for small angles, where
// 2 acos(|w|) loses it (acos is ill-conditioned near 1).
NPH_INLINE f32 nph_quat_angle(NphQuat q)
{
    return 2.0F * nph_f32_atan2(nph_vec3_length(nph_quat_xyz(q)), nph_f32_abs(q.w));
}
// The angle of the rotation taking a to b (both unit), in [0, pi]. For unit
// quaternions |a - b| = 2 sin(phi/2) and |a + b| = 2 cos(phi/2), where phi is
// the 4D angle between them and the rotation angle is 2 phi. Taking the
// smaller of the two norms as the sine picks the shorter arc (q and -q are
// the same rotation) and stays accurate for nearly equal inputs.
NPH_INLINE f32 nph_quat_angle_between(NphQuat a, NphQuat b)
{
    const f32 d = nph_quat_length(nph_quat_sub(a, b));
    const f32 s = nph_quat_length(nph_quat_add(a, b));
    return 4.0F * nph_f32_atan2(nph_f32_min(d, s), nph_f32_max(d, s));
}

// Normalised linear interpolation along the shorter arc. Not constant
// speed, but commutative and cheap; the right choice when t is sampled
// densely (animation blending).
NPH_INLINE NphQuat nph_quat_nlerp(NphQuat a, NphQuat b, f32 t)
{
    const NphQuat bb = nph_quat_dot(a, b) < 0.0F ? nph_quat_neg(b) : b;
    return nph_quat_normalize(nph_quat_add(nph_quat_scale(a, 1.0F - t), nph_quat_scale(bb, t)));
}
// Spherical linear interpolation along the shorter arc at constant angular
// speed (Shoemake, "Animating Rotation with Quaternion Curves", SIGGRAPH
// 1985). Falls back to nlerp when a and b are nearly equal.
NphQuat nph_quat_slerp(NphQuat a, NphQuat b, f32 t);

// Splits the unit quaternion q into q == swing · twist, where twist is a
// rotation about the unit vector twist_axis and swing is a rotation about
// an axis perpendicular to it (Dobrowolski, "Swing-twist decomposition in
// Clifford algebra", 2015). When q is a half turn about an axis
// perpendicular to twist_axis, twist is the identity.
void nph_quat_swing_twist(NphQuat q, NphVec3 twist_axis, NphQuat *out_swing, NphQuat *out_twist);

// Advances the unit orientation q by the angular velocity omega (radians per
// second, world axes) over dt seconds: q' = normalize(q + dt/2 · (omega ⊗ q)).
// First-order; exact enough for the physics substeps it is used in.
NPH_INLINE NphQuat nph_quat_integrate(NphQuat q, NphVec3 omega, f32 dt)
{
    const NphQuat w = nph_quat_from_xyz_w(omega, 0.0F);
    const NphQuat dq = nph_quat_scale(nph_quat_mul(w, q), 0.5F * dt);
    return nph_quat_normalize_or_identity(nph_quat_add(q, dq));
}

NPH_INLINE bool nph_quat_eq(NphQuat a, NphQuat b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
NPH_INLINE bool nph_quat_near(NphQuat a, NphQuat b, f32 tol)
{
    return nph_f32_near(a.x, b.x, tol) && nph_f32_near(a.y, b.y, tol) &&
           nph_f32_near(a.z, b.z, tol) && nph_f32_near(a.w, b.w, tol);
}
// True when a and b represent the same rotation (q and -q are the same
// rotation) within tol per component.
NPH_INLINE bool nph_quat_near_rotation(NphQuat a, NphQuat b, f32 tol)
{
    return nph_quat_near(a, b, tol) || nph_quat_near(a, nph_quat_neg(b), tol);
}

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
#pragma once

// Column-major 3x3 and 4x4 matrices (CLAUDE.md §8.2; docs/adr/0008-math-conventions.md).
//
// `c[i]` is column i, so m.c[col] is the vector the i-th basis vector maps
// to and nph_mat4_at(m, row, col) == m.c[col] component `row`. Transforms
// apply as M · v (column vectors); nph_mat4_mul(a, b) == a · b applies b
// first. The layout matches GLSL's default, so these structs are shared
// with shaders unchanged.
//
// Projection matrices target Vulkan clip space (X right, Y down, Z in
// [0, 1]) with reversed depth (near plane at 1) and, by default, an
// infinite far plane; see ADR-0008 §2 and the functions at the end.
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/math/quat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphMat3 {
    NphVec3 c[3]; // columns
} NphMat3;

typedef struct NphMat4 {
    NphVec4 c[4]; // columns
} NphMat4;

NPH_STATIC_ASSERT(sizeof(NphMat3) == 36U, "NphMat3 is nine floats");
NPH_STATIC_ASSERT(sizeof(NphMat4) == 64U, "NphMat4 is sixteen floats");

// ---------------------------------------------------------------------------
// NphMat3
// ---------------------------------------------------------------------------

NPH_INLINE NphMat3 nph_mat3_from_cols(NphVec3 c0, NphVec3 c1, NphVec3 c2)
{
    NphMat3 m;
    m.c[0] = c0;
    m.c[1] = c1;
    m.c[2] = c2;
    return m;
}
NPH_INLINE NphMat3 nph_mat3_from_rows(NphVec3 r0, NphVec3 r1, NphVec3 r2)
{
    return nph_mat3_from_cols(nph_vec3(r0.x, r1.x, r2.x), nph_vec3(r0.y, r1.y, r2.y),
                              nph_vec3(r0.z, r1.z, r2.z));
}
NPH_INLINE NphMat3 nph_mat3_identity(void)
{
    return nph_mat3_from_cols(nph_vec3_unit_x(), nph_vec3_unit_y(), nph_vec3_unit_z());
}
NPH_INLINE NphMat3 nph_mat3_zero(void)
{
    return nph_mat3_from_cols(nph_vec3_zero(), nph_vec3_zero(), nph_vec3_zero());
}
NPH_INLINE NphMat3 nph_mat3_from_diagonal(NphVec3 d)
{
    return nph_mat3_from_cols(nph_vec3(d.x, 0.0F, 0.0F), nph_vec3(0.0F, d.y, 0.0F),
                              nph_vec3(0.0F, 0.0F, d.z));
}
NPH_INLINE NphMat3 nph_mat3_from_scale(NphVec3 s)
{
    return nph_mat3_from_diagonal(s);
}
// The matrix [v]x with [v]x · w == cross(v, w).
NPH_INLINE NphMat3 nph_mat3_skew(NphVec3 v)
{
    return nph_mat3_from_cols(nph_vec3(0.0F, v.z, -v.y), nph_vec3(-v.z, 0.0F, v.x),
                              nph_vec3(v.y, -v.x, 0.0F));
}
// The rotation matrix of a unit quaternion.
NPH_INLINE NphMat3 nph_mat3_from_quat(NphQuat q)
{
    return nph_mat3_from_cols(nph_quat_axis_x(q), nph_quat_axis_y(q), nph_quat_axis_z(q));
}
NPH_INLINE NphMat3 nph_mat3_from_axis_angle(NphVec3 axis, f32 angle)
{
    return nph_mat3_from_quat(nph_quat_from_axis_angle(axis, angle));
}
NPH_INLINE f32 nph_mat3_at(NphMat3 m, u32 row, u32 col)
{
    return nph_vec3_at(m.c[col], row);
}
NPH_INLINE NphVec3 nph_mat3_row(NphMat3 m, u32 row)
{
    return nph_vec3(nph_vec3_at(m.c[0], row), nph_vec3_at(m.c[1], row), nph_vec3_at(m.c[2], row));
}
NPH_INLINE NphMat3 nph_mat3_add(NphMat3 a, NphMat3 b)
{
    return nph_mat3_from_cols(nph_vec3_add(a.c[0], b.c[0]), nph_vec3_add(a.c[1], b.c[1]),
                              nph_vec3_add(a.c[2], b.c[2]));
}
NPH_INLINE NphMat3 nph_mat3_scale(NphMat3 m, f32 s)
{
    return nph_mat3_from_cols(nph_vec3_scale(m.c[0], s), nph_vec3_scale(m.c[1], s),
                              nph_vec3_scale(m.c[2], s));
}
// m · v
NPH_INLINE NphVec3 nph_mat3_mul_vec3(NphMat3 m, NphVec3 v)
{
    return nph_vec3_add(nph_vec3_add(nph_vec3_scale(m.c[0], v.x), nph_vec3_scale(m.c[1], v.y)),
                        nph_vec3_scale(m.c[2], v.z));
}
// a · b
NPH_INLINE NphMat3 nph_mat3_mul(NphMat3 a, NphMat3 b)
{
    return nph_mat3_from_cols(nph_mat3_mul_vec3(a, b.c[0]), nph_mat3_mul_vec3(a, b.c[1]),
                              nph_mat3_mul_vec3(a, b.c[2]));
}
NPH_INLINE NphMat3 nph_mat3_transpose(NphMat3 m)
{
    return nph_mat3_from_rows(m.c[0], m.c[1], m.c[2]);
}
NPH_INLINE f32 nph_mat3_determinant(NphMat3 m)
{
    return nph_vec3_dot(m.c[0], nph_vec3_cross(m.c[1], m.c[2]));
}
NPH_INLINE f32 nph_mat3_trace(NphMat3 m)
{
    return m.c[0].x + m.c[1].y + m.c[2].z;
}
// Writes the inverse to *out and returns true, or returns false (leaving
// *out untouched) when m is singular.
bool nph_mat3_inverse(NphMat3 m, NphMat3 *out);

// The unit quaternion of a rotation matrix (orthonormal columns, positive
// determinant). Uses the largest diagonal term for stability (Shepperd
// 1978; Day, "Converting a Rotation Matrix to a Quaternion", 2015).
NphQuat nph_quat_from_mat3(NphMat3 m);

// Re-orthonormalises nearly orthonormal columns (Gram–Schmidt from c[0]).
NphMat3 nph_mat3_orthonormalize(NphMat3 m);

NPH_INLINE bool nph_mat3_eq(NphMat3 a, NphMat3 b)
{
    return nph_vec3_eq(a.c[0], b.c[0]) && nph_vec3_eq(a.c[1], b.c[1]) &&
           nph_vec3_eq(a.c[2], b.c[2]);
}
NPH_INLINE bool nph_mat3_near(NphMat3 a, NphMat3 b, f32 tol)
{
    return nph_vec3_near(a.c[0], b.c[0], tol) && nph_vec3_near(a.c[1], b.c[1], tol) &&
           nph_vec3_near(a.c[2], b.c[2], tol);
}

// ---------------------------------------------------------------------------
// NphMat4
// ---------------------------------------------------------------------------

NPH_INLINE NphMat4 nph_mat4_from_cols(NphVec4 c0, NphVec4 c1, NphVec4 c2, NphVec4 c3)
{
    NphMat4 m;
    m.c[0] = c0;
    m.c[1] = c1;
    m.c[2] = c2;
    m.c[3] = c3;
    return m;
}
NPH_INLINE NphMat4 nph_mat4_from_rows(NphVec4 r0, NphVec4 r1, NphVec4 r2, NphVec4 r3)
{
    return nph_mat4_from_cols(nph_vec4(r0.x, r1.x, r2.x, r3.x), nph_vec4(r0.y, r1.y, r2.y, r3.y),
                              nph_vec4(r0.z, r1.z, r2.z, r3.z), nph_vec4(r0.w, r1.w, r2.w, r3.w));
}
NPH_INLINE NphMat4 nph_mat4_identity(void)
{
    return nph_mat4_from_cols(nph_vec4(1.0F, 0.0F, 0.0F, 0.0F), nph_vec4(0.0F, 1.0F, 0.0F, 0.0F),
                              nph_vec4(0.0F, 0.0F, 1.0F, 0.0F), nph_vec4(0.0F, 0.0F, 0.0F, 1.0F));
}
NPH_INLINE NphMat4 nph_mat4_zero(void)
{
    return nph_mat4_from_cols(nph_vec4_zero(), nph_vec4_zero(), nph_vec4_zero(), nph_vec4_zero());
}
NPH_INLINE NphMat4 nph_mat4_from_diagonal(NphVec4 d)
{
    return nph_mat4_from_cols(nph_vec4(d.x, 0.0F, 0.0F, 0.0F), nph_vec4(0.0F, d.y, 0.0F, 0.0F),
                              nph_vec4(0.0F, 0.0F, d.z, 0.0F), nph_vec4(0.0F, 0.0F, 0.0F, d.w));
}
// Embeds a 3x3 linear map with no translation.
NPH_INLINE NphMat4 nph_mat4_from_mat3(NphMat3 m)
{
    return nph_mat4_from_cols(nph_vec4_direction(m.c[0]), nph_vec4_direction(m.c[1]),
                              nph_vec4_direction(m.c[2]), nph_vec4(0.0F, 0.0F, 0.0F, 1.0F));
}
// Embeds a 3x3 linear map followed by a translation.
NPH_INLINE NphMat4 nph_mat4_from_mat3_translation(NphMat3 m, NphVec3 t)
{
    return nph_mat4_from_cols(nph_vec4_direction(m.c[0]), nph_vec4_direction(m.c[1]),
                              nph_vec4_direction(m.c[2]), nph_vec4_point(t));
}
NPH_INLINE NphMat4 nph_mat4_from_translation(NphVec3 t)
{
    return nph_mat4_from_mat3_translation(nph_mat3_identity(), t);
}
NPH_INLINE NphMat4 nph_mat4_from_scale(NphVec3 s)
{
    return nph_mat4_from_diagonal(nph_vec4(s.x, s.y, s.z, 1.0F));
}
NPH_INLINE NphMat4 nph_mat4_from_quat(NphQuat q)
{
    return nph_mat4_from_mat3(nph_mat3_from_quat(q));
}
// Translation · Rotation · Scale: scales about the origin, rotates, then
// translates. The standard object-to-world transform.
NphMat4 nph_mat4_from_trs(NphVec3 translation, NphQuat rotation, NphVec3 scale);

// Inverse of nph_mat4_from_trs for a matrix without shear or projection
// (only the upper 3x4 is read). A negative determinant is reported as a
// negative x scale. Returns false, leaving the outputs untouched, when a
// scale axis is zero.
bool nph_mat4_decompose_trs(NphMat4 m, NphVec3 *out_translation, NphQuat *out_rotation,
                            NphVec3 *out_scale);

NPH_INLINE f32 nph_mat4_at(NphMat4 m, u32 row, u32 col)
{
    return nph_vec4_at(m.c[col], row);
}
NPH_INLINE NphVec4 nph_mat4_row(NphMat4 m, u32 row)
{
    return nph_vec4(nph_vec4_at(m.c[0], row), nph_vec4_at(m.c[1], row), nph_vec4_at(m.c[2], row),
                    nph_vec4_at(m.c[3], row));
}
// The upper-left 3x3.
NPH_INLINE NphMat3 nph_mat4_linear(NphMat4 m)
{
    return nph_mat3_from_cols(nph_vec4_xyz(m.c[0]), nph_vec4_xyz(m.c[1]), nph_vec4_xyz(m.c[2]));
}
// The translation column.
NPH_INLINE NphVec3 nph_mat4_translation(NphMat4 m)
{
    return nph_vec4_xyz(m.c[3]);
}
NPH_INLINE NphMat4 nph_mat4_add(NphMat4 a, NphMat4 b)
{
    return nph_mat4_from_cols(nph_vec4_add(a.c[0], b.c[0]), nph_vec4_add(a.c[1], b.c[1]),
                              nph_vec4_add(a.c[2], b.c[2]), nph_vec4_add(a.c[3], b.c[3]));
}
NPH_INLINE NphMat4 nph_mat4_scale(NphMat4 m, f32 s)
{
    return nph_mat4_from_cols(nph_vec4_scale(m.c[0], s), nph_vec4_scale(m.c[1], s),
                              nph_vec4_scale(m.c[2], s), nph_vec4_scale(m.c[3], s));
}
// m · v
NPH_INLINE NphVec4 nph_mat4_mul_vec4(NphMat4 m, NphVec4 v)
{
    NphVec4 r = nph_vec4_scale(m.c[0], v.x);
    r = nph_vec4_mul_add(r, m.c[1], v.y);
    r = nph_vec4_mul_add(r, m.c[2], v.z);
    r = nph_vec4_mul_add(r, m.c[3], v.w);
    return r;
}
// a · b
NPH_INLINE NphMat4 nph_mat4_mul(NphMat4 a, NphMat4 b)
{
    return nph_mat4_from_cols(nph_mat4_mul_vec4(a, b.c[0]), nph_mat4_mul_vec4(a, b.c[1]),
                              nph_mat4_mul_vec4(a, b.c[2]), nph_mat4_mul_vec4(a, b.c[3]));
}
// m · (p, 1) without the perspective divide: for affine matrices.
NPH_INLINE NphVec3 nph_mat4_transform_point(NphMat4 m, NphVec3 p)
{
    return nph_vec4_xyz(nph_mat4_mul_vec4(m, nph_vec4_point(p)));
}
// m · (p, 1) followed by the divide by w: for projection matrices. w must
// be non-zero.
NPH_INLINE NphVec3 nph_mat4_transform_point_perspective(NphMat4 m, NphVec3 p)
{
    return nph_vec4_project(nph_mat4_mul_vec4(m, nph_vec4_point(p)));
}
// m · (v, 0): rotates and scales, ignores translation.
NPH_INLINE NphVec3 nph_mat4_transform_vector(NphMat4 m, NphVec3 v)
{
    return nph_vec4_xyz(nph_mat4_mul_vec4(m, nph_vec4_direction(v)));
}
NPH_INLINE NphMat4 nph_mat4_transpose(NphMat4 m)
{
    return nph_mat4_from_rows(m.c[0], m.c[1], m.c[2], m.c[3]);
}
f32 nph_mat4_determinant(NphMat4 m);

// General inverse. Writes *out and returns true, or returns false (leaving
// *out untouched) when m is singular.
bool nph_mat4_inverse(NphMat4 m, NphMat4 *out);

// Inverse of a matrix whose last row is (0, 0, 0, 1): inverts the 3x3 and
// transforms the translation, about a third of the cost of the general
// inverse. The last row of m is not read. Same contract otherwise.
bool nph_mat4_inverse_affine(NphMat4 m, NphMat4 *out);

NPH_INLINE bool nph_mat4_eq(NphMat4 a, NphMat4 b)
{
    return nph_vec4_eq(a.c[0], b.c[0]) && nph_vec4_eq(a.c[1], b.c[1]) &&
           nph_vec4_eq(a.c[2], b.c[2]) && nph_vec4_eq(a.c[3], b.c[3]);
}
NPH_INLINE bool nph_mat4_near(NphMat4 a, NphMat4 b, f32 tol)
{
    return nph_vec4_near(a.c[0], b.c[0], tol) && nph_vec4_near(a.c[1], b.c[1], tol) &&
           nph_vec4_near(a.c[2], b.c[2], tol) && nph_vec4_near(a.c[3], b.c[3], tol);
}

// ---------------------------------------------------------------------------
// View and projection (ADR-0008 §1–2)
// ---------------------------------------------------------------------------

// World-to-view matrix for a camera at `eye` looking along the unit vector
// `forward` with `up` roughly upward (not parallel to forward). View space
// is right-handed with the camera looking down -Z and +Y up.
NphMat4 nph_mat4_look_to(NphVec3 eye, NphVec3 forward, NphVec3 up);

// As nph_mat4_look_to with forward = normalize(target - eye).
NphMat4 nph_mat4_look_at(NphVec3 eye, NphVec3 target, NphVec3 up);

// Perspective projection with vertical field of view `fovy` (radians),
// `aspect` = width / height, near plane distance `near` and an infinite
// far plane, into Vulkan clip space with reversed depth: the near plane
// maps to depth 1 and infinity to depth 0 (Reed, "Depth Precision
// Visualized", 2015). The Y axis is flipped here so that +Y up in view
// space becomes -Y in clip space, as Vulkan expects.
NphMat4 nph_mat4_perspective_infinite(f32 fovy, f32 aspect, f32 near);

// As nph_mat4_perspective_infinite with a finite far plane mapping to
// depth 0. Prefer the infinite form; this exists for cases that need a
// hard far clip (shadow and reflection cameras).
NphMat4 nph_mat4_perspective(f32 fovy, f32 aspect, f32 near, f32 far);

// Orthographic projection of the view-space box [left, right] x [bottom,
// top] x [-far, -near] into Vulkan clip space with reversed depth (near at
// 1, far at 0) and the Y flip.
NphMat4 nph_mat4_orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near, f32 far);

#ifdef __cplusplus
}
#endif

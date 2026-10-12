// SPDX-License-Identifier: Apache-2.0
#pragma once

// NphAffine: a 3x4 affine transform (rotation, scale, shear, translation)
// stored as the top three ROWS of the equivalent column-vector 4x4 matrix
// (docs/adr/0008-math-conventions.md §5). 48 bytes; three vec4 rows pack
// into GPU instance buffers without padding, and a point transform is
// three dot products.
//
//   r[0] = (m00 m01 m02 tx)
//   r[1] = (m10 m11 m12 ty)       p' = (r[0]·(p,1), r[1]·(p,1), r[2]·(p,1))
//   r[2] = (m20 m21 m22 tz)
//
// The implied fourth row is (0 0 0 1). Everything else about the algebra
// is identical to NphMat4: nph_affine_mul(a, b) == a · b applies b first.
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/math/mat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAffine {
    NphVec4 r[3]; // rows
} NphAffine;

NPH_STATIC_ASSERT(sizeof(NphAffine) == 48U, "NphAffine is three vec4 rows");

NPH_INLINE NphAffine nph_affine_from_rows(NphVec4 r0, NphVec4 r1, NphVec4 r2)
{
    NphAffine a;
    a.r[0] = r0;
    a.r[1] = r1;
    a.r[2] = r2;
    return a;
}
NPH_INLINE NphAffine nph_affine_identity(void)
{
    return nph_affine_from_rows(nph_vec4(1.0F, 0.0F, 0.0F, 0.0F), nph_vec4(0.0F, 1.0F, 0.0F, 0.0F),
                                nph_vec4(0.0F, 0.0F, 1.0F, 0.0F));
}
// From a 3x3 linear map and a translation.
NPH_INLINE NphAffine nph_affine_from_mat3_translation(NphMat3 m, NphVec3 t)
{
    return nph_affine_from_rows(nph_vec4(m.c[0].x, m.c[1].x, m.c[2].x, t.x),
                                nph_vec4(m.c[0].y, m.c[1].y, m.c[2].y, t.y),
                                nph_vec4(m.c[0].z, m.c[1].z, m.c[2].z, t.z));
}
NPH_INLINE NphAffine nph_affine_from_mat3(NphMat3 m)
{
    return nph_affine_from_mat3_translation(m, nph_vec3_zero());
}
// The top three rows of m; the fourth row is dropped, not checked.
NPH_INLINE NphAffine nph_affine_from_mat4(NphMat4 m)
{
    return nph_affine_from_rows(nph_mat4_row(m, 0U), nph_mat4_row(m, 1U), nph_mat4_row(m, 2U));
}
NPH_INLINE NphMat4 nph_affine_to_mat4(NphAffine a)
{
    return nph_mat4_from_rows(a.r[0], a.r[1], a.r[2], nph_vec4(0.0F, 0.0F, 0.0F, 1.0F));
}
NPH_INLINE NphAffine nph_affine_from_translation(NphVec3 t)
{
    return nph_affine_from_mat3_translation(nph_mat3_identity(), t);
}
NPH_INLINE NphAffine nph_affine_from_scale(NphVec3 s)
{
    return nph_affine_from_mat3(nph_mat3_from_scale(s));
}
NPH_INLINE NphAffine nph_affine_from_quat(NphQuat q)
{
    return nph_affine_from_mat3(nph_mat3_from_quat(q));
}
// Translation · Rotation · Scale, as nph_mat4_from_trs.
NphAffine nph_affine_from_trs(NphVec3 translation, NphQuat rotation, NphVec3 scale);

// As nph_mat4_decompose_trs.
bool nph_affine_decompose_trs(NphAffine a, NphVec3 *out_translation, NphQuat *out_rotation,
                              NphVec3 *out_scale);

// Column i (0..2) of the linear part, or the translation for i == 3.
NPH_INLINE NphVec3 nph_affine_column(NphAffine a, u32 i)
{
    return nph_vec3(nph_vec4_at(a.r[0], i), nph_vec4_at(a.r[1], i), nph_vec4_at(a.r[2], i));
}
NPH_INLINE NphMat3 nph_affine_linear(NphAffine a)
{
    return nph_mat3_from_cols(nph_affine_column(a, 0U), nph_affine_column(a, 1U),
                              nph_affine_column(a, 2U));
}
NPH_INLINE NphVec3 nph_affine_translation(NphAffine a)
{
    return nph_vec3(a.r[0].w, a.r[1].w, a.r[2].w);
}
NPH_INLINE NphAffine nph_affine_with_translation(NphAffine a, NphVec3 t)
{
    a.r[0].w = t.x;
    a.r[1].w = t.y;
    a.r[2].w = t.z;
    return a;
}

// a · (p, 1)
NPH_INLINE NphVec3 nph_affine_transform_point(NphAffine a, NphVec3 p)
{
    const NphVec4 h = nph_vec4_point(p);
    return nph_vec3(nph_vec4_dot(a.r[0], h), nph_vec4_dot(a.r[1], h), nph_vec4_dot(a.r[2], h));
}
// a · (v, 0): no translation.
NPH_INLINE NphVec3 nph_affine_transform_vector(NphAffine a, NphVec3 v)
{
    const NphVec4 h = nph_vec4_direction(v);
    return nph_vec3(nph_vec4_dot3(a.r[0], h), nph_vec4_dot3(a.r[1], h), nph_vec4_dot3(a.r[2], h));
}
// a · b: applies b, then a.
NPH_INLINE NphAffine nph_affine_mul(NphAffine a, NphAffine b)
{
    // Row i of the product is row i of a times the columns of b, with the
    // implied (0 0 0 1) row of b supplying the translation's extra term.
    NphAffine r;
    for (u32 i = 0; i < 3U; ++i) {
        const NphVec4 ar = a.r[i];
        r.r[i] = nph_vec4(ar.x * b.r[0].x + ar.y * b.r[1].x + ar.z * b.r[2].x,
                          ar.x * b.r[0].y + ar.y * b.r[1].y + ar.z * b.r[2].y,
                          ar.x * b.r[0].z + ar.y * b.r[1].z + ar.z * b.r[2].z,
                          ar.x * b.r[0].w + ar.y * b.r[1].w + ar.z * b.r[2].w + ar.w);
    }
    return r;
}

// General inverse. Writes *out and returns true, or returns false (leaving
// *out untouched) when the linear part is singular.
bool nph_affine_inverse(NphAffine a, NphAffine *out);

// Inverse of a rigid transform (rotation + translation, no scale or
// shear): the transpose of the linear part. Undefined for other inputs.
NPH_INLINE NphAffine nph_affine_inverse_rigid(NphAffine a)
{
    const NphMat3 rt = nph_mat3_transpose(nph_affine_linear(a));
    const NphVec3 t = nph_vec3_neg(nph_mat3_mul_vec3(rt, nph_affine_translation(a)));
    return nph_affine_from_mat3_translation(rt, t);
}

NPH_INLINE bool nph_affine_eq(NphAffine a, NphAffine b)
{
    return nph_vec4_eq(a.r[0], b.r[0]) && nph_vec4_eq(a.r[1], b.r[1]) &&
           nph_vec4_eq(a.r[2], b.r[2]);
}
NPH_INLINE bool nph_affine_near(NphAffine a, NphAffine b, f32 tol)
{
    return nph_vec4_near(a.r[0], b.r[0], tol) && nph_vec4_near(a.r[1], b.r[1], tol) &&
           nph_vec4_near(a.r[2], b.r[2], tol);
}

#ifdef __cplusplus
}
#endif

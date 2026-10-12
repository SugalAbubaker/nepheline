// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/mat.h>

// Below this |determinant| a matrix is treated as singular. The matrices
// that matter (transforms, projections) have determinants far above it;
// anything smaller is a degenerate scale and inverting it would only
// produce infinities.
#define NPH_MAT_SINGULAR_EPSILON 1e-20F

// ---------------------------------------------------------------------------
// NphMat3
// ---------------------------------------------------------------------------

bool nph_mat3_inverse(NphMat3 m, NphMat3 *out)
{
    // Rows of the inverse are the cross products of the columns divided by
    // the determinant (the adjugate), which is exact for the sizes here.
    const NphVec3 r0 = nph_vec3_cross(m.c[1], m.c[2]);
    const NphVec3 r1 = nph_vec3_cross(m.c[2], m.c[0]);
    const NphVec3 r2 = nph_vec3_cross(m.c[0], m.c[1]);
    const f32 det = nph_vec3_dot(m.c[0], r0);
    if (nph_f32_abs(det) < NPH_MAT_SINGULAR_EPSILON) {
        return false;
    }
    const f32 inv_det = 1.0F / det;
    *out = nph_mat3_from_rows(nph_vec3_scale(r0, inv_det), nph_vec3_scale(r1, inv_det),
                              nph_vec3_scale(r2, inv_det));
    return true;
}

NphQuat nph_quat_from_mat3(NphMat3 m)
{
    // m_rc is row r, column c of the rotation matrix. Dividing by the
    // largest of the four candidate terms keeps every component accurate
    // (Shepperd 1978; Day 2015).
    const f32 m00 = m.c[0].x;
    const f32 m10 = m.c[0].y;
    const f32 m20 = m.c[0].z;
    const f32 m01 = m.c[1].x;
    const f32 m11 = m.c[1].y;
    const f32 m21 = m.c[1].z;
    const f32 m02 = m.c[2].x;
    const f32 m12 = m.c[2].y;
    const f32 m22 = m.c[2].z;
    const f32 trace = m00 + m11 + m22;
    NphQuat q;
    if (trace > 0.0F) {
        const f32 s = nph_f32_sqrt(trace + 1.0F) * 2.0F; // 4w
        q.w = 0.25F * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = nph_f32_sqrt(1.0F + m00 - m11 - m22) * 2.0F; // 4x
        q.w = (m21 - m12) / s;
        q.x = 0.25F * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const f32 s = nph_f32_sqrt(1.0F + m11 - m00 - m22) * 2.0F; // 4y
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25F * s;
        q.z = (m12 + m21) / s;
    } else {
        const f32 s = nph_f32_sqrt(1.0F + m22 - m00 - m11) * 2.0F; // 4z
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25F * s;
    }
    return nph_quat_normalize(q);
}

NphMat3 nph_mat3_orthonormalize(NphMat3 m)
{
    // Modified Gram–Schmidt from c[0]; c[2] keeps its own direction (minus
    // the projections) so a left-handed input stays left-handed.
    const NphVec3 c0 = nph_vec3_normalize(m.c[0]);
    const NphVec3 c1 = nph_vec3_normalize(nph_vec3_reject(m.c[1], c0));
    const NphVec3 c2 = nph_vec3_normalize(nph_vec3_reject(nph_vec3_reject(m.c[2], c0), c1));
    return nph_mat3_from_cols(c0, c1, c2);
}

// ---------------------------------------------------------------------------
// NphMat4
// ---------------------------------------------------------------------------

NphMat4 nph_mat4_from_trs(NphVec3 translation, NphQuat rotation, NphVec3 scale)
{
    return nph_mat4_from_cols(
        nph_vec4_direction(nph_vec3_scale(nph_quat_axis_x(rotation), scale.x)),
        nph_vec4_direction(nph_vec3_scale(nph_quat_axis_y(rotation), scale.y)),
        nph_vec4_direction(nph_vec3_scale(nph_quat_axis_z(rotation), scale.z)),
        nph_vec4_point(translation));
}

bool nph_mat4_decompose_trs(NphMat4 m, NphVec3 *out_translation, NphQuat *out_rotation,
                            NphVec3 *out_scale)
{
    NphMat3 l = nph_mat4_linear(m);
    NphVec3 s = nph_vec3(nph_vec3_length(l.c[0]), nph_vec3_length(l.c[1]), nph_vec3_length(l.c[2]));
    if (s.x <= 0.0F || s.y <= 0.0F || s.z <= 0.0F) {
        return false;
    }
    // A reflection cannot be a rotation; attribute it to the x axis.
    if (nph_mat3_determinant(l) < 0.0F) {
        s.x = -s.x;
    }
    l.c[0] = nph_vec3_scale(l.c[0], 1.0F / s.x);
    l.c[1] = nph_vec3_scale(l.c[1], 1.0F / s.y);
    l.c[2] = nph_vec3_scale(l.c[2], 1.0F / s.z);
    *out_translation = nph_mat4_translation(m);
    *out_rotation = nph_quat_from_mat3(l);
    *out_scale = s;
    return true;
}

// The 4x4 determinant and inverse below follow Lengyel, "Foundations of
// Game Engine Development, Volume 1", §1.7.5 (Listings 1.10 and 1.11): the
// matrix is read as four 3-vectors (the upper parts of the columns) plus
// the bottom row, and the inverse is assembled from six cross products.

f32 nph_mat4_determinant(NphMat4 m)
{
    const NphVec3 a = nph_vec4_xyz(m.c[0]);
    const NphVec3 b = nph_vec4_xyz(m.c[1]);
    const NphVec3 c = nph_vec4_xyz(m.c[2]);
    const NphVec3 d = nph_vec4_xyz(m.c[3]);
    const f32 x = m.c[0].w;
    const f32 y = m.c[1].w;
    const f32 z = m.c[2].w;
    const f32 w = m.c[3].w;
    const NphVec3 s = nph_vec3_cross(a, b);
    const NphVec3 t = nph_vec3_cross(c, d);
    const NphVec3 u = nph_vec3_sub(nph_vec3_scale(a, y), nph_vec3_scale(b, x));
    const NphVec3 v = nph_vec3_sub(nph_vec3_scale(c, w), nph_vec3_scale(d, z));
    return nph_vec3_dot(s, v) + nph_vec3_dot(t, u);
}

bool nph_mat4_inverse(NphMat4 m, NphMat4 *out)
{
    const NphVec3 a = nph_vec4_xyz(m.c[0]);
    const NphVec3 b = nph_vec4_xyz(m.c[1]);
    const NphVec3 c = nph_vec4_xyz(m.c[2]);
    const NphVec3 d = nph_vec4_xyz(m.c[3]);
    const f32 x = m.c[0].w;
    const f32 y = m.c[1].w;
    const f32 z = m.c[2].w;
    const f32 w = m.c[3].w;

    NphVec3 s = nph_vec3_cross(a, b);
    NphVec3 t = nph_vec3_cross(c, d);
    NphVec3 u = nph_vec3_sub(nph_vec3_scale(a, y), nph_vec3_scale(b, x));
    NphVec3 v = nph_vec3_sub(nph_vec3_scale(c, w), nph_vec3_scale(d, z));

    const f32 det = nph_vec3_dot(s, v) + nph_vec3_dot(t, u);
    if (nph_f32_abs(det) < NPH_MAT_SINGULAR_EPSILON) {
        return false;
    }
    const f32 inv_det = 1.0F / det;
    s = nph_vec3_scale(s, inv_det);
    t = nph_vec3_scale(t, inv_det);
    u = nph_vec3_scale(u, inv_det);
    v = nph_vec3_scale(v, inv_det);

    const NphVec3 r0 = nph_vec3_add(nph_vec3_cross(b, v), nph_vec3_scale(t, y));
    const NphVec3 r1 = nph_vec3_sub(nph_vec3_cross(v, a), nph_vec3_scale(t, x));
    const NphVec3 r2 = nph_vec3_add(nph_vec3_cross(d, u), nph_vec3_scale(s, w));
    const NphVec3 r3 = nph_vec3_sub(nph_vec3_cross(u, c), nph_vec3_scale(s, z));

    *out = nph_mat4_from_rows(
        nph_vec4_from_vec3(r0, -nph_vec3_dot(b, t)), nph_vec4_from_vec3(r1, nph_vec3_dot(a, t)),
        nph_vec4_from_vec3(r2, -nph_vec3_dot(d, s)), nph_vec4_from_vec3(r3, nph_vec3_dot(c, s)));
    return true;
}

bool nph_mat4_inverse_affine(NphMat4 m, NphMat4 *out)
{
    NphMat3 inv_l;
    if (!nph_mat3_inverse(nph_mat4_linear(m), &inv_l)) {
        return false;
    }
    const NphVec3 t = nph_vec3_neg(nph_mat3_mul_vec3(inv_l, nph_mat4_translation(m)));
    *out = nph_mat4_from_mat3_translation(inv_l, t);
    return true;
}

// ---------------------------------------------------------------------------
// View and projection
// ---------------------------------------------------------------------------

NphMat4 nph_mat4_look_to(NphVec3 eye, NphVec3 forward, NphVec3 up)
{
    // Camera basis: s right, u up, f forward. View space looks down -Z, so
    // the third row is -f. The translation column is the eye expressed in
    // that basis, negated.
    const NphVec3 f = nph_vec3_normalize(forward);
    const NphVec3 s = nph_vec3_normalize(nph_vec3_cross(f, up));
    const NphVec3 u = nph_vec3_cross(s, f);
    return nph_mat4_from_rows(nph_vec4_from_vec3(s, -nph_vec3_dot(s, eye)),
                              nph_vec4_from_vec3(u, -nph_vec3_dot(u, eye)),
                              nph_vec4_from_vec3(nph_vec3_neg(f), nph_vec3_dot(f, eye)),
                              nph_vec4(0.0F, 0.0F, 0.0F, 1.0F));
}

NphMat4 nph_mat4_look_at(NphVec3 eye, NphVec3 target, NphVec3 up)
{
    return nph_mat4_look_to(eye, nph_vec3_sub(target, eye), up);
}

NphMat4 nph_mat4_perspective_infinite(f32 fovy, f32 aspect, f32 near)
{
    NPH_ASSERT(fovy > 0.0F && fovy < NPH_PI);
    NPH_ASSERT(aspect > 0.0F);
    NPH_ASSERT(near > 0.0F);
    // Clip z = near, clip w = -z_view, so depth = near / -z_view: 1 at the
    // near plane, 0 at infinity. The -f on y produces Vulkan's Y-down.
    const f32 f = 1.0F / nph_f32_tan(0.5F * fovy);
    NphMat4 m = nph_mat4_zero();
    m.c[0].x = f / aspect;
    m.c[1].y = -f;
    m.c[2].w = -1.0F;
    m.c[3].z = near;
    return m;
}

NphMat4 nph_mat4_perspective(f32 fovy, f32 aspect, f32 near, f32 far)
{
    NPH_ASSERT(fovy > 0.0F && fovy < NPH_PI);
    NPH_ASSERT(aspect > 0.0F);
    NPH_ASSERT(near > 0.0F && far > near);
    // depth = (near z_view + far near) / (-(far - near) z_view): 1 at -near,
    // 0 at -far. As far -> infinity this tends to the matrix above.
    const f32 f = 1.0F / nph_f32_tan(0.5F * fovy);
    const f32 inv_range = 1.0F / (far - near);
    NphMat4 m = nph_mat4_zero();
    m.c[0].x = f / aspect;
    m.c[1].y = -f;
    m.c[2].z = near * inv_range;
    m.c[2].w = -1.0F;
    m.c[3].z = far * near * inv_range;
    return m;
}

NphMat4 nph_mat4_orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near, f32 far)
{
    NPH_ASSERT(right != left && top != bottom && far != near);
    const f32 inv_w = 1.0F / (right - left);
    const f32 inv_h = 1.0F / (top - bottom);
    const f32 inv_d = 1.0F / (far - near);
    NphMat4 m = nph_mat4_zero();
    m.c[0].x = 2.0F * inv_w;
    m.c[1].y = -2.0F * inv_h; // Y-down
    m.c[2].z = inv_d;         // depth = (z_view + far) / (far - near)
    m.c[3].x = -(right + left) * inv_w;
    m.c[3].y = (top + bottom) * inv_h;
    m.c[3].z = far * inv_d;
    m.c[3].w = 1.0F;
    return m;
}

// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/simd.h>

// 2x2 blocks held as (a, b, c, d) in row-major order within one register.
// The helpers compute A·B, adj(A)·B, and A·adj(B) with shuffles only
// (Zhang, "Fast 4x4 Matrix Inverse with SSE SIMD, Explained", 2016).

static NphF32x4 mat2_mul(NphF32x4 a, NphF32x4 b)
{
    return _mm_add_ps(
        _mm_mul_ps(a, NPH_F32X4_SHUFFLE(b, 0, 3, 0, 3)),
        _mm_mul_ps(NPH_F32X4_SHUFFLE(a, 1, 0, 3, 2), NPH_F32X4_SHUFFLE(b, 2, 1, 2, 1)));
}

static NphF32x4 mat2_adj_mul(NphF32x4 a, NphF32x4 b)
{
    return _mm_sub_ps(
        _mm_mul_ps(NPH_F32X4_SHUFFLE(a, 3, 3, 0, 0), b),
        _mm_mul_ps(NPH_F32X4_SHUFFLE(a, 1, 1, 2, 2), NPH_F32X4_SHUFFLE(b, 2, 3, 0, 1)));
}

static NphF32x4 mat2_mul_adj(NphF32x4 a, NphF32x4 b)
{
    return _mm_sub_ps(
        _mm_mul_ps(a, NPH_F32X4_SHUFFLE(b, 3, 0, 3, 0)),
        _mm_mul_ps(NPH_F32X4_SHUFFLE(a, 1, 0, 3, 2), NPH_F32X4_SHUFFLE(b, 2, 1, 2, 1)));
}

bool nph_simd_mat4_inverse(NphSimdMat4 m, NphSimdMat4 *out)
{
    // Zhang's derivation is for a row-major matrix held as four row
    // registers. Feeding our columns as its rows inverts the transpose, and
    // the rows it produces are then exactly the columns of our inverse, so
    // no transposes are needed on either side.
    const NphF32x4 r0 = m.c[0];
    const NphF32x4 r1 = m.c[1];
    const NphF32x4 r2 = m.c[2];
    const NphF32x4 r3 = m.c[3];

    // Sub-matrices: [A B; C D].
    const NphF32x4 a = _mm_movelh_ps(r0, r1); // r0.xy r1.xy
    const NphF32x4 b = _mm_movehl_ps(r1, r0); // r0.zw r1.zw
    const NphF32x4 c = _mm_movelh_ps(r2, r3);
    const NphF32x4 d = _mm_movehl_ps(r3, r2);

    // Determinants of the four sub-matrices, as (|A|, |B|, |C|, |D|).
    const NphF32x4 det_sub = _mm_sub_ps(
        _mm_mul_ps(NPH_F32X4_SHUFFLE2(r0, r2, 0, 2, 0, 2), NPH_F32X4_SHUFFLE2(r1, r3, 1, 3, 1, 3)),
        _mm_mul_ps(NPH_F32X4_SHUFFLE2(r0, r2, 1, 3, 1, 3), NPH_F32X4_SHUFFLE2(r1, r3, 0, 2, 0, 2)));
    const NphF32x4 det_a = nph_f32x4_splat_x(det_sub);
    const NphF32x4 det_b = nph_f32x4_splat_y(det_sub);
    const NphF32x4 det_c = nph_f32x4_splat_z(det_sub);
    const NphF32x4 det_d = nph_f32x4_splat_w(det_sub);

    const NphF32x4 d_c = mat2_adj_mul(d, c); // adj(D)·C
    const NphF32x4 a_b = mat2_adj_mul(a, b); // adj(A)·B

    NphF32x4 x = _mm_sub_ps(_mm_mul_ps(det_d, a), mat2_mul(b, d_c));
    NphF32x4 w = _mm_sub_ps(_mm_mul_ps(det_a, d), mat2_mul(c, a_b));
    NphF32x4 y = _mm_sub_ps(_mm_mul_ps(det_b, c), mat2_mul_adj(d, a_b));
    NphF32x4 z = _mm_sub_ps(_mm_mul_ps(det_c, b), mat2_mul_adj(a, d_c));

    // |M| = |A||D| + |B||C| - tr(adj(A)·B · adj(D)·C)
    NphF32x4 det_m = _mm_add_ps(_mm_mul_ps(det_a, det_d), _mm_mul_ps(det_b, det_c));
    NphF32x4 tr = _mm_mul_ps(a_b, NPH_F32X4_SHUFFLE(d_c, 0, 2, 1, 3));
    tr = _mm_hadd_ps(tr, tr);
    tr = _mm_hadd_ps(tr, tr);
    det_m = _mm_sub_ps(det_m, tr);

    const f32 det = _mm_cvtss_f32(det_m);
    if (nph_f32_abs(det) < 1e-20F) {
        return false;
    }
    const NphF32x4 adj_sign = _mm_set_ps(1.0F, -1.0F, -1.0F, 1.0F);
    const NphF32x4 inv_det = _mm_div_ps(adj_sign, det_m);
    x = _mm_mul_ps(x, inv_det);
    y = _mm_mul_ps(y, inv_det);
    z = _mm_mul_ps(z, inv_det);
    w = _mm_mul_ps(w, inv_det);

    out->c[0] = NPH_F32X4_SHUFFLE2(x, y, 3, 1, 3, 1);
    out->c[1] = NPH_F32X4_SHUFFLE2(x, y, 2, 0, 2, 0);
    out->c[2] = NPH_F32X4_SHUFFLE2(z, w, 3, 1, 3, 1);
    out->c[3] = NPH_F32X4_SHUFFLE2(z, w, 2, 0, 2, 0);
    return true;
}

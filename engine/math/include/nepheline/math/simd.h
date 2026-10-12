// SPDX-License-Identifier: Apache-2.0
#pragma once

// The SIMD register layer (CLAUDE.md §8.2; docs/adr/0008-math-conventions.md §4):
// NphF32x4 is one SSE register holding four lanes. Following DirectXMath, a
// single register type serves vectors, points, quaternions, and planes; the
// operation says which semantics apply (nph_f32x4_dot3, nph_f32x4_quat_mul).
// NphSimdMat4 and NphSimdAffine hold a matrix in registers.
//
// Values enter and leave through explicit load/store functions that take
// the storage types of vec.h / mat.h; those use unaligned loads, so storage
// types keep their natural alignment. Everything here is NPH_INLINE and
// compiles to the SSE4.2 baseline (§3); AVX2 is used only inside the
// dedicated translation units of wide.h / kernels.h.
//
// Determinism (ADR-0008 §6): every operation here is a correctly rounded
// IEEE instruction, and operations that have a scalar twin perform the same
// arithmetic in the same order, so results are bit-identical to vec.h. The
// approximate rsqrt/rcp instructions are exposed only as *_fast and differ
// between CPU vendors; never use them where results must reproduce.
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/math/affine.h>
#include <nepheline/math/geometry.h>

#if !NPH_ARCH_X64
#error "nepheline/math/simd.h: only x86-64 (SSE4.2) is implemented; ARM64/NEON is a later phase"
#endif

#include <smmintrin.h> // SSE4.1 (SSE4.2 adds nothing for floats)

#ifdef __cplusplus
extern "C" {
#endif

typedef __m128 NphF32x4;
typedef __m128i NphI32x4;

NPH_STATIC_ASSERT(sizeof(NphF32x4) == 16U, "NphF32x4 is one 128-bit register");

// Lane shuffle with compile-time indices: result = (v[x], v[y], v[z], v[w]).
#define NPH_F32X4_SHUFFLE(v, x, y, z, w) _mm_shuffle_ps((v), (v), _MM_SHUFFLE((w), (z), (y), (x)))
// Two-source shuffle: result = (a[x], a[y], b[z], b[w]).
#define NPH_F32X4_SHUFFLE2(a, b, x, y, z, w)                                                       \
    _mm_shuffle_ps((a), (b), _MM_SHUFFLE((w), (z), (y), (x)))

// ---------------------------------------------------------------------------
// Construction, load, store
// ---------------------------------------------------------------------------

NPH_INLINE NphF32x4 nph_f32x4_set(f32 x, f32 y, f32 z, f32 w)
{
    return _mm_set_ps(w, z, y, x);
}
NPH_INLINE NphF32x4 nph_f32x4_splat(f32 s)
{
    return _mm_set1_ps(s);
}
NPH_INLINE NphF32x4 nph_f32x4_zero(void)
{
    return _mm_setzero_ps();
}
// Loads four floats from any address.
NPH_INLINE NphF32x4 nph_f32x4_load(const f32 *p)
{
    return _mm_loadu_ps(p);
}
// Loads four floats from a 16-byte-aligned address.
NPH_INLINE NphF32x4 nph_f32x4_load_aligned(const f32 *p)
{
    NPH_ASSERT(nph_is_ptr_aligned(p, 16U));
    return _mm_load_ps(p);
}
NPH_INLINE void nph_f32x4_store(f32 *p, NphF32x4 v)
{
    _mm_storeu_ps(p, v);
}
NPH_INLINE void nph_f32x4_store_aligned(f32 *p, NphF32x4 v)
{
    NPH_ASSERT(nph_is_ptr_aligned(p, 16U));
    _mm_store_ps(p, v);
}
NPH_INLINE NphF32x4 nph_f32x4_load_vec4(NphVec4 v)
{
    return _mm_loadu_ps(&v.x);
}
NPH_INLINE NphVec4 nph_f32x4_store_vec4(NphF32x4 v)
{
    NphVec4 out;
    _mm_storeu_ps(&out.x, v);
    return out;
}
// (x, y, z, 0)
NPH_INLINE NphF32x4 nph_f32x4_load_vec3(NphVec3 v)
{
    return _mm_set_ps(0.0F, v.z, v.y, v.x);
}
// (x, y, z, w)
NPH_INLINE NphF32x4 nph_f32x4_load_vec3_w(NphVec3 v, f32 w)
{
    return _mm_set_ps(w, v.z, v.y, v.x);
}
// (x, y, z, 1): a point for affine and projective transforms.
NPH_INLINE NphF32x4 nph_f32x4_load_point(NphVec3 p)
{
    return _mm_set_ps(1.0F, p.z, p.y, p.x);
}
// Drops the w lane.
NPH_INLINE NphVec3 nph_f32x4_store_vec3(NphF32x4 v)
{
    NPH_ALIGNAS(16) f32 tmp[4];
    _mm_store_ps(tmp, v);
    return nph_vec3(tmp[0], tmp[1], tmp[2]);
}
NPH_INLINE NphF32x4 nph_f32x4_load_quat(NphQuat q)
{
    return _mm_loadu_ps(&q.x);
}
NPH_INLINE NphQuat nph_f32x4_store_quat(NphF32x4 v)
{
    NphQuat out;
    _mm_storeu_ps(&out.x, v);
    return out;
}
// (normal.x, normal.y, normal.z, d)
NPH_INLINE NphF32x4 nph_f32x4_load_plane(NphPlane p)
{
    return _mm_loadu_ps(&p.normal.x);
}

// Lane access. For a run-time index store to an array instead.
NPH_INLINE f32 nph_f32x4_get_x(NphF32x4 v)
{
    return _mm_cvtss_f32(v);
}
NPH_INLINE f32 nph_f32x4_get_y(NphF32x4 v)
{
    return _mm_cvtss_f32(NPH_F32X4_SHUFFLE(v, 1, 1, 1, 1));
}
NPH_INLINE f32 nph_f32x4_get_z(NphF32x4 v)
{
    return _mm_cvtss_f32(NPH_F32X4_SHUFFLE(v, 2, 2, 2, 2));
}
NPH_INLINE f32 nph_f32x4_get_w(NphF32x4 v)
{
    return _mm_cvtss_f32(NPH_F32X4_SHUFFLE(v, 3, 3, 3, 3));
}
NPH_INLINE NphF32x4 nph_f32x4_splat_x(NphF32x4 v)
{
    return NPH_F32X4_SHUFFLE(v, 0, 0, 0, 0);
}
NPH_INLINE NphF32x4 nph_f32x4_splat_y(NphF32x4 v)
{
    return NPH_F32X4_SHUFFLE(v, 1, 1, 1, 1);
}
NPH_INLINE NphF32x4 nph_f32x4_splat_z(NphF32x4 v)
{
    return NPH_F32X4_SHUFFLE(v, 2, 2, 2, 2);
}
NPH_INLINE NphF32x4 nph_f32x4_splat_w(NphF32x4 v)
{
    return NPH_F32X4_SHUFFLE(v, 3, 3, 3, 3);
}
// Replaces the w lane.
NPH_INLINE NphF32x4 nph_f32x4_set_w(NphF32x4 v, f32 w)
{
    return _mm_insert_ps(v, _mm_set_ss(w), 0x30);
}

// ---------------------------------------------------------------------------
// Arithmetic (all correctly rounded; bit-identical to the scalar operators)
// ---------------------------------------------------------------------------

NPH_INLINE NphF32x4 nph_f32x4_add(NphF32x4 a, NphF32x4 b)
{
    return _mm_add_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_sub(NphF32x4 a, NphF32x4 b)
{
    return _mm_sub_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_mul(NphF32x4 a, NphF32x4 b)
{
    return _mm_mul_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_div(NphF32x4 a, NphF32x4 b)
{
    return _mm_div_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_scale(NphF32x4 v, f32 s)
{
    return _mm_mul_ps(v, _mm_set1_ps(s));
}
// a * b + c as two rounded operations, like the scalar code (no FMA:
// ADR-0008 §6).
NPH_INLINE NphF32x4 nph_f32x4_mul_add(NphF32x4 a, NphF32x4 b, NphF32x4 c)
{
    return _mm_add_ps(_mm_mul_ps(a, b), c);
}
// c - a * b
NPH_INLINE NphF32x4 nph_f32x4_neg_mul_add(NphF32x4 a, NphF32x4 b, NphF32x4 c)
{
    return _mm_sub_ps(c, _mm_mul_ps(a, b));
}
NPH_INLINE NphF32x4 nph_f32x4_neg(NphF32x4 v)
{
    return _mm_xor_ps(v, _mm_set1_ps(-0.0F));
}
NPH_INLINE NphF32x4 nph_f32x4_abs(NphF32x4 v)
{
    return _mm_andnot_ps(_mm_set1_ps(-0.0F), v);
}
// min/max return the second operand when either is NaN, like the scalar
// `a < b ? a : b` forms.
NPH_INLINE NphF32x4 nph_f32x4_min(NphF32x4 a, NphF32x4 b)
{
    return _mm_min_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_max(NphF32x4 a, NphF32x4 b)
{
    return _mm_max_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_clamp(NphF32x4 v, NphF32x4 lo, NphF32x4 hi)
{
    return _mm_min_ps(_mm_max_ps(v, lo), hi);
}
NPH_INLINE NphF32x4 nph_f32x4_sqrt(NphF32x4 v)
{
    return _mm_sqrt_ps(v);
}
// 1 / sqrt(v), correctly rounded in two operations (same as nph_f32_rsqrt).
NPH_INLINE NphF32x4 nph_f32x4_rsqrt(NphF32x4 v)
{
    return _mm_div_ps(_mm_set1_ps(1.0F), _mm_sqrt_ps(v));
}
// Approximate 1 / sqrt(v) (about 12 bits) from the hardware table, refined
// by one Newton step to about 23 bits. Vendor-dependent: not for anything
// that must reproduce (ADR-0008 §6).
NPH_INLINE NphF32x4 nph_f32x4_rsqrt_fast(NphF32x4 v)
{
    const __m128 r = _mm_rsqrt_ps(v);
    const __m128 half_v = _mm_mul_ps(_mm_set1_ps(0.5F), v);
    return _mm_mul_ps(r, _mm_sub_ps(_mm_set1_ps(1.5F), _mm_mul_ps(half_v, _mm_mul_ps(r, r))));
}
// Approximate 1 / v, same caveats as nph_f32x4_rsqrt_fast.
NPH_INLINE NphF32x4 nph_f32x4_rcp_fast(NphF32x4 v)
{
    const __m128 r = _mm_rcp_ps(v);
    return _mm_sub_ps(_mm_add_ps(r, r), _mm_mul_ps(v, _mm_mul_ps(r, r)));
}
NPH_INLINE NphF32x4 nph_f32x4_floor(NphF32x4 v)
{
    return _mm_floor_ps(v);
}
NPH_INLINE NphF32x4 nph_f32x4_ceil(NphF32x4 v)
{
    return _mm_ceil_ps(v);
}
// Rounds half to even (nph_f32_round_even).
NPH_INLINE NphF32x4 nph_f32x4_round_even(NphF32x4 v)
{
    return _mm_round_ps(v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
}
NPH_INLINE NphF32x4 nph_f32x4_trunc(NphF32x4 v)
{
    return _mm_round_ps(v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
}
// (1 - t) a + t b per lane, like nph_f32_lerp.
NPH_INLINE NphF32x4 nph_f32x4_lerp(NphF32x4 a, NphF32x4 b, NphF32x4 t)
{
    return _mm_add_ps(_mm_mul_ps(_mm_sub_ps(_mm_set1_ps(1.0F), t), a), _mm_mul_ps(t, b));
}
// Sum of the four lanes, splatted; summed as ((x + y) + z) + w.
NPH_INLINE NphF32x4 nph_f32x4_hadd4(NphF32x4 v)
{
    const __m128 xy = _mm_add_ps(nph_f32x4_splat_x(v), nph_f32x4_splat_y(v));
    return _mm_add_ps(_mm_add_ps(xy, nph_f32x4_splat_z(v)), nph_f32x4_splat_w(v));
}
// Sum of the first three lanes, splatted; summed as (x + y) + z.
NPH_INLINE NphF32x4 nph_f32x4_hadd3(NphF32x4 v)
{
    const __m128 xy = _mm_add_ps(nph_f32x4_splat_x(v), nph_f32x4_splat_y(v));
    return _mm_add_ps(xy, nph_f32x4_splat_z(v));
}

// ---------------------------------------------------------------------------
// Comparisons and masks. A mask lane is all ones (true) or all zeros.
// ---------------------------------------------------------------------------

NPH_INLINE NphF32x4 nph_f32x4_cmp_eq(NphF32x4 a, NphF32x4 b)
{
    return _mm_cmpeq_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_cmp_ne(NphF32x4 a, NphF32x4 b)
{
    return _mm_cmpneq_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_cmp_lt(NphF32x4 a, NphF32x4 b)
{
    return _mm_cmplt_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_cmp_le(NphF32x4 a, NphF32x4 b)
{
    return _mm_cmple_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_cmp_gt(NphF32x4 a, NphF32x4 b)
{
    return _mm_cmpgt_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_cmp_ge(NphF32x4 a, NphF32x4 b)
{
    return _mm_cmpge_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_and(NphF32x4 a, NphF32x4 b)
{
    return _mm_and_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_or(NphF32x4 a, NphF32x4 b)
{
    return _mm_or_ps(a, b);
}
NPH_INLINE NphF32x4 nph_f32x4_xor(NphF32x4 a, NphF32x4 b)
{
    return _mm_xor_ps(a, b);
}
// (~a) & b
NPH_INLINE NphF32x4 nph_f32x4_andnot(NphF32x4 a, NphF32x4 b)
{
    return _mm_andnot_ps(a, b);
}
// Per lane: mask ? a : b.
NPH_INLINE NphF32x4 nph_f32x4_select(NphF32x4 mask, NphF32x4 a, NphF32x4 b)
{
    return _mm_blendv_ps(b, a, mask);
}
// Bit i of the result is the sign (mask) bit of lane i.
NPH_INLINE u32 nph_f32x4_movemask(NphF32x4 mask)
{
    return (u32)_mm_movemask_ps(mask);
}
NPH_INLINE bool nph_f32x4_any(NphF32x4 mask)
{
    return _mm_movemask_ps(mask) != 0;
}
NPH_INLINE bool nph_f32x4_all(NphF32x4 mask)
{
    return _mm_movemask_ps(mask) == 0xF;
}
// Exact lane-wise equality of all four lanes.
NPH_INLINE bool nph_f32x4_eq(NphF32x4 a, NphF32x4 b)
{
    return _mm_movemask_ps(_mm_cmpeq_ps(a, b)) == 0xF;
}

// ---------------------------------------------------------------------------
// Integer lanes (conversions and masks)
// ---------------------------------------------------------------------------

// Truncates toward zero, like a C cast.
NPH_INLINE NphI32x4 nph_f32x4_to_i32x4_trunc(NphF32x4 v)
{
    return _mm_cvttps_epi32(v);
}
NPH_INLINE NphF32x4 nph_i32x4_to_f32x4(NphI32x4 v)
{
    return _mm_cvtepi32_ps(v);
}
NPH_INLINE NphI32x4 nph_i32x4_set(i32 x, i32 y, i32 z, i32 w)
{
    return _mm_set_epi32(w, z, y, x);
}
NPH_INLINE void nph_i32x4_store(i32 *p, NphI32x4 v)
{
    _mm_storeu_si128((__m128i *)p, v);
}

// ---------------------------------------------------------------------------
// Vector semantics on a register
// ---------------------------------------------------------------------------

// Dot of the xyz lanes, splatted to all lanes. Same order as
// nph_vec3_dot: (x + y) + z.
NPH_INLINE NphF32x4 nph_f32x4_dot3(NphF32x4 a, NphF32x4 b)
{
    return nph_f32x4_hadd3(_mm_mul_ps(a, b));
}
NPH_INLINE f32 nph_f32x4_dot3_scalar(NphF32x4 a, NphF32x4 b)
{
    return _mm_cvtss_f32(nph_f32x4_dot3(a, b));
}
// Dot of all four lanes, splatted. Same order as nph_vec4_dot.
NPH_INLINE NphF32x4 nph_f32x4_dot4(NphF32x4 a, NphF32x4 b)
{
    return nph_f32x4_hadd4(_mm_mul_ps(a, b));
}
NPH_INLINE f32 nph_f32x4_dot4_scalar(NphF32x4 a, NphF32x4 b)
{
    return _mm_cvtss_f32(nph_f32x4_dot4(a, b));
}
// Cross product of the xyz lanes; the w lane becomes 0 (a.w b.w - a.w b.w),
// or NaN if w was infinite or NaN.
NPH_INLINE NphF32x4 nph_f32x4_cross3(NphF32x4 a, NphF32x4 b)
{
    const __m128 a_yzx = NPH_F32X4_SHUFFLE(a, 1, 2, 0, 3);
    const __m128 b_zxy = NPH_F32X4_SHUFFLE(b, 2, 0, 1, 3);
    const __m128 a_zxy = NPH_F32X4_SHUFFLE(a, 2, 0, 1, 3);
    const __m128 b_yzx = NPH_F32X4_SHUFFLE(b, 1, 2, 0, 3);
    return _mm_sub_ps(_mm_mul_ps(a_yzx, b_zxy), _mm_mul_ps(a_zxy, b_yzx));
}
NPH_INLINE NphF32x4 nph_f32x4_length3_sq(NphF32x4 v)
{
    return nph_f32x4_dot3(v, v);
}
NPH_INLINE NphF32x4 nph_f32x4_length3(NphF32x4 v)
{
    return _mm_sqrt_ps(nph_f32x4_dot3(v, v));
}
NPH_INLINE NphF32x4 nph_f32x4_length4_sq(NphF32x4 v)
{
    return nph_f32x4_dot4(v, v);
}
NPH_INLINE NphF32x4 nph_f32x4_length4(NphF32x4 v)
{
    return _mm_sqrt_ps(nph_f32x4_dot4(v, v));
}
// Normalises the xyz lanes (w is scaled too; load with w == 0 to keep it
// 0). Same arithmetic as nph_vec3_normalize: v * (1 / sqrt(dot)). The
// zero vector produces infinities, as the scalar version would.
NPH_INLINE NphF32x4 nph_f32x4_normalize3(NphF32x4 v)
{
    return _mm_mul_ps(v, nph_f32x4_rsqrt(nph_f32x4_dot3(v, v)));
}
NPH_INLINE NphF32x4 nph_f32x4_normalize4(NphF32x4 v)
{
    return _mm_mul_ps(v, nph_f32x4_rsqrt(nph_f32x4_dot4(v, v)));
}

// ---------------------------------------------------------------------------
// Quaternion semantics on a register (lanes x, y, z, w)
// ---------------------------------------------------------------------------

NPH_INLINE NphF32x4 nph_f32x4_quat_identity(void)
{
    return _mm_set_ps(1.0F, 0.0F, 0.0F, 0.0F);
}
NPH_INLINE NphF32x4 nph_f32x4_quat_conjugate(NphF32x4 q)
{
    return _mm_xor_ps(q, _mm_set_ps(0.0F, -0.0F, -0.0F, -0.0F));
}
// Hamilton product a·b, with the same term order as nph_quat_mul so the
// result is bit-identical.
NPH_INLINE NphF32x4 nph_f32x4_quat_mul(NphF32x4 a, NphF32x4 b)
{
    // x = aw bx + ax bw + ay bz - az by
    // y = aw by - ax bz + ay bw + az bx
    // z = aw bz + ax by - ay bx + az bw
    // w = aw bw - ax bx - ay by - az bz
    const __m128 t0 = _mm_mul_ps(nph_f32x4_splat_w(a), b);
    const __m128 t1 = _mm_mul_ps(nph_f32x4_splat_x(a), NPH_F32X4_SHUFFLE(b, 3, 2, 1, 0));
    const __m128 t2 = _mm_mul_ps(nph_f32x4_splat_y(a), NPH_F32X4_SHUFFLE(b, 2, 3, 0, 1));
    const __m128 t3 = _mm_mul_ps(nph_f32x4_splat_z(a), NPH_F32X4_SHUFFLE(b, 1, 0, 3, 2));
    const __m128 s1 = _mm_set_ps(-0.0F, 0.0F, -0.0F, 0.0F); // (+, -, +, -)
    const __m128 s2 = _mm_set_ps(-0.0F, -0.0F, 0.0F, 0.0F); // (+, +, -, -)
    const __m128 s3 = _mm_set_ps(-0.0F, 0.0F, 0.0F, -0.0F); // (-, +, +, -)
    __m128 r = _mm_add_ps(t0, _mm_xor_ps(t1, s1));
    r = _mm_add_ps(r, _mm_xor_ps(t2, s2));
    return _mm_add_ps(r, _mm_xor_ps(t3, s3));
}
// Rotates the xyz lanes of v by the unit quaternion q, with the same
// arithmetic as nph_quat_rotate. v.w must be 0 for the result's w to be 0.
NPH_INLINE NphF32x4 nph_f32x4_quat_rotate(NphF32x4 q, NphF32x4 v)
{
    const __m128 u = nph_f32x4_set_w(q, 0.0F);
    const __m128 t = _mm_mul_ps(nph_f32x4_cross3(u, v), _mm_set1_ps(2.0F));
    const __m128 vw = _mm_add_ps(v, _mm_mul_ps(t, nph_f32x4_splat_w(q)));
    return _mm_add_ps(vw, nph_f32x4_cross3(u, t));
}

// ---------------------------------------------------------------------------
// NphSimdMat4: four column registers
// ---------------------------------------------------------------------------

typedef struct NphSimdMat4 {
    NphF32x4 c[4];
} NphSimdMat4;

NPH_INLINE NphSimdMat4 nph_simd_mat4_load(const NphMat4 *m)
{
    NphSimdMat4 r;
    r.c[0] = _mm_loadu_ps(&m->c[0].x);
    r.c[1] = _mm_loadu_ps(&m->c[1].x);
    r.c[2] = _mm_loadu_ps(&m->c[2].x);
    r.c[3] = _mm_loadu_ps(&m->c[3].x);
    return r;
}
NPH_INLINE void nph_simd_mat4_store(NphMat4 *out, NphSimdMat4 m)
{
    _mm_storeu_ps(&out->c[0].x, m.c[0]);
    _mm_storeu_ps(&out->c[1].x, m.c[1]);
    _mm_storeu_ps(&out->c[2].x, m.c[2]);
    _mm_storeu_ps(&out->c[3].x, m.c[3]);
}
NPH_INLINE NphSimdMat4 nph_simd_mat4_identity(void)
{
    NphSimdMat4 r;
    r.c[0] = _mm_set_ps(0.0F, 0.0F, 0.0F, 1.0F);
    r.c[1] = _mm_set_ps(0.0F, 0.0F, 1.0F, 0.0F);
    r.c[2] = _mm_set_ps(0.0F, 1.0F, 0.0F, 0.0F);
    r.c[3] = _mm_set_ps(1.0F, 0.0F, 0.0F, 0.0F);
    return r;
}
// m · v, with the same term order as nph_mat4_mul_vec4.
NPH_INLINE NphF32x4 nph_simd_mat4_mul_vec4(NphSimdMat4 m, NphF32x4 v)
{
    __m128 r = _mm_mul_ps(m.c[0], nph_f32x4_splat_x(v));
    r = _mm_add_ps(r, _mm_mul_ps(m.c[1], nph_f32x4_splat_y(v)));
    r = _mm_add_ps(r, _mm_mul_ps(m.c[2], nph_f32x4_splat_z(v)));
    return _mm_add_ps(r, _mm_mul_ps(m.c[3], nph_f32x4_splat_w(v)));
}
// a · b, bit-identical to nph_mat4_mul.
NPH_INLINE NphSimdMat4 nph_simd_mat4_mul(NphSimdMat4 a, NphSimdMat4 b)
{
    NphSimdMat4 r;
    r.c[0] = nph_simd_mat4_mul_vec4(a, b.c[0]);
    r.c[1] = nph_simd_mat4_mul_vec4(a, b.c[1]);
    r.c[2] = nph_simd_mat4_mul_vec4(a, b.c[2]);
    r.c[3] = nph_simd_mat4_mul_vec4(a, b.c[3]);
    return r;
}
// m · (p, 1); the result's w lane is row 3 of m applied to the point.
NPH_INLINE NphF32x4 nph_simd_mat4_transform_point(NphSimdMat4 m, NphF32x4 p)
{
    __m128 r = _mm_mul_ps(m.c[0], nph_f32x4_splat_x(p));
    r = _mm_add_ps(r, _mm_mul_ps(m.c[1], nph_f32x4_splat_y(p)));
    r = _mm_add_ps(r, _mm_mul_ps(m.c[2], nph_f32x4_splat_z(p)));
    return _mm_add_ps(r, m.c[3]);
}
// m · (v, 0)
NPH_INLINE NphF32x4 nph_simd_mat4_transform_vector(NphSimdMat4 m, NphF32x4 v)
{
    __m128 r = _mm_mul_ps(m.c[0], nph_f32x4_splat_x(v));
    r = _mm_add_ps(r, _mm_mul_ps(m.c[1], nph_f32x4_splat_y(v)));
    return _mm_add_ps(r, _mm_mul_ps(m.c[2], nph_f32x4_splat_z(v)));
}
// Perspective divide of a transformed point: xyz / w, w lane becomes 1.
NPH_INLINE NphF32x4 nph_f32x4_project(NphF32x4 v)
{
    return _mm_div_ps(v, nph_f32x4_splat_w(v));
}
NPH_INLINE NphSimdMat4 nph_simd_mat4_transpose(NphSimdMat4 m)
{
    const __m128 t0 = _mm_unpacklo_ps(m.c[0], m.c[1]); // (c0.x c1.x c0.y c1.y)
    const __m128 t1 = _mm_unpackhi_ps(m.c[0], m.c[1]); // (c0.z c1.z c0.w c1.w)
    const __m128 t2 = _mm_unpacklo_ps(m.c[2], m.c[3]);
    const __m128 t3 = _mm_unpackhi_ps(m.c[2], m.c[3]);
    NphSimdMat4 r;
    r.c[0] = _mm_movelh_ps(t0, t2);
    r.c[1] = _mm_movehl_ps(t2, t0);
    r.c[2] = _mm_movelh_ps(t1, t3);
    r.c[3] = _mm_movehl_ps(t3, t1);
    return r;
}
// General inverse by the 2x2 block method (Zhang, "Fast 4x4 Matrix Inverse
// with SSE SIMD, Explained", 2016). Returns false, leaving *out untouched,
// when the matrix is singular. The operation order differs from
// nph_mat4_inverse, so results agree to a few ULPs rather than exactly.
bool nph_simd_mat4_inverse(NphSimdMat4 m, NphSimdMat4 *out);

// ---------------------------------------------------------------------------
// NphSimdAffine: the linear part as three column registers (w lane 0) plus
// the translation column (w lane 1). The storage type NphAffine keeps rows
// for the GPU (ADR-0008 §5); in registers, columns make a point transform
// three multiply-adds with no horizontal sums, so load and store transpose
// once. The per-lane operation order equals nph_affine_transform_point and
// nph_affine_mul, so results are bit-identical.
// ---------------------------------------------------------------------------

typedef struct NphSimdAffine {
    NphF32x4 c[4];
} NphSimdAffine;

// From the three rows of the storage layout.
NPH_INLINE NphSimdAffine nph_simd_affine_from_rows(NphF32x4 r0, NphF32x4 r1, NphF32x4 r2)
{
    NphSimdMat4 rows;
    rows.c[0] = r0;
    rows.c[1] = r1;
    rows.c[2] = r2;
    rows.c[3] = _mm_set_ps(1.0F, 0.0F, 0.0F, 0.0F);
    const NphSimdMat4 t = nph_simd_mat4_transpose(rows);
    NphSimdAffine a;
    a.c[0] = t.c[0];
    a.c[1] = t.c[1];
    a.c[2] = t.c[2];
    a.c[3] = t.c[3];
    return a;
}
NPH_INLINE NphSimdAffine nph_simd_affine_load(const NphAffine *a)
{
    return nph_simd_affine_from_rows(_mm_loadu_ps(&a->r[0].x), _mm_loadu_ps(&a->r[1].x),
                                     _mm_loadu_ps(&a->r[2].x));
}
NPH_INLINE void nph_simd_affine_store(NphAffine *out, NphSimdAffine a)
{
    NphSimdMat4 cols;
    cols.c[0] = a.c[0];
    cols.c[1] = a.c[1];
    cols.c[2] = a.c[2];
    cols.c[3] = a.c[3];
    const NphSimdMat4 rows = nph_simd_mat4_transpose(cols);
    _mm_storeu_ps(&out->r[0].x, rows.c[0]);
    _mm_storeu_ps(&out->r[1].x, rows.c[1]);
    _mm_storeu_ps(&out->r[2].x, rows.c[2]);
}
NPH_INLINE NphSimdAffine nph_simd_affine_identity(void)
{
    NphSimdAffine a;
    a.c[0] = _mm_set_ps(0.0F, 0.0F, 0.0F, 1.0F);
    a.c[1] = _mm_set_ps(0.0F, 0.0F, 1.0F, 0.0F);
    a.c[2] = _mm_set_ps(0.0F, 1.0F, 0.0F, 0.0F);
    a.c[3] = _mm_set_ps(1.0F, 0.0F, 0.0F, 0.0F);
    return a;
}
// a · (p, 1): the point's w lane is ignored. Result w lane is 1.
NPH_INLINE NphF32x4 nph_simd_affine_transform_point(NphSimdAffine a, NphF32x4 p)
{
    __m128 r = _mm_mul_ps(a.c[0], nph_f32x4_splat_x(p));
    r = _mm_add_ps(r, _mm_mul_ps(a.c[1], nph_f32x4_splat_y(p)));
    r = _mm_add_ps(r, _mm_mul_ps(a.c[2], nph_f32x4_splat_z(p)));
    return _mm_add_ps(r, a.c[3]);
}
// a · (v, 0). Result w lane is 0.
NPH_INLINE NphF32x4 nph_simd_affine_transform_vector(NphSimdAffine a, NphF32x4 v)
{
    __m128 r = _mm_mul_ps(a.c[0], nph_f32x4_splat_x(v));
    r = _mm_add_ps(r, _mm_mul_ps(a.c[1], nph_f32x4_splat_y(v)));
    return _mm_add_ps(r, _mm_mul_ps(a.c[2], nph_f32x4_splat_z(v)));
}
// a · b (applies b first), bit-identical to nph_affine_mul.
NPH_INLINE NphSimdAffine nph_simd_affine_mul(NphSimdAffine a, NphSimdAffine b)
{
    NphSimdAffine r;
    r.c[0] = nph_simd_affine_transform_vector(a, b.c[0]);
    r.c[1] = nph_simd_affine_transform_vector(a, b.c[1]);
    r.c[2] = nph_simd_affine_transform_vector(a, b.c[2]);
    r.c[3] = nph_simd_affine_transform_point(a, b.c[3]);
    return r;
}
NPH_INLINE NphSimdMat4 nph_simd_affine_to_mat4(NphSimdAffine a)
{
    NphSimdMat4 m;
    m.c[0] = a.c[0];
    m.c[1] = a.c[1];
    m.c[2] = a.c[2];
    m.c[3] = a.c[3];
    return m;
}

// ---------------------------------------------------------------------------
// Plane semantics
// ---------------------------------------------------------------------------

// Signed distance of the point (x, y, z, 1) from the plane (n, d), splatted.
NPH_INLINE NphF32x4 nph_f32x4_plane_distance(NphF32x4 plane, NphF32x4 point_w1)
{
    return nph_f32x4_dot4(plane, point_w1);
}

#ifdef __cplusplus
}
#endif

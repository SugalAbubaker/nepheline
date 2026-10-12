// SPDX-License-Identifier: Apache-2.0
#pragma once

// Wide SoA types for batch work (CLAUDE.md §8.2; docs/adr/0008-math-conventions.md §4):
// NphVec3x4 and NphVec3x8 hold four or eight independent vectors with the
// x, y, and z components in separate registers, so culling and physics
// loops process whole groups per instruction with no shuffles.
//
// NphF32x8 is one AVX register inside translation units compiled with AVX
// (NPH_WIDE_AVX == 1) and a pair of SSE registers elsewhere, with the same
// size and alignment. Everything here is NPH_INLINE, so each translation
// unit gets the code its ISA level allows; wide values are never passed
// between units compiled at different levels (kernels.h is the dispatch
// boundary and takes plain memory).
//
// Arithmetic matches the scalar functions term for term, so results are
// bit-identical to vec.h and geometry.h (ADR-0008 §6).
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/math/simd.h>

#if defined(__AVX__)
#include <immintrin.h>
#define NPH_WIDE_AVX 1
#else
#define NPH_WIDE_AVX 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// NphF32x8: eight lanes
// ---------------------------------------------------------------------------

#if NPH_WIDE_AVX
typedef __m256 NphF32x8;
#else
typedef struct NphF32x8 {
    NPH_ALIGNAS(32) NphF32x4 lo;
    NphF32x4 hi;
} NphF32x8;
#endif

NPH_STATIC_ASSERT(sizeof(NphF32x8) == 32U, "NphF32x8 is 32 bytes at every ISA level");
NPH_STATIC_ASSERT(NPH_ALIGNOF(NphF32x8) == 32U, "NphF32x8 is 32-byte aligned at every ISA level");

NPH_INLINE NphF32x8 nph_f32x8_from_x4(NphF32x4 lo, NphF32x4 hi)
{
#if NPH_WIDE_AVX
    return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
#else
    NphF32x8 r;
    r.lo = lo;
    r.hi = hi;
    return r;
#endif
}
NPH_INLINE NphF32x4 nph_f32x8_lo(NphF32x8 v)
{
#if NPH_WIDE_AVX
    return _mm256_castps256_ps128(v);
#else
    return v.lo;
#endif
}
NPH_INLINE NphF32x4 nph_f32x8_hi(NphF32x8 v)
{
#if NPH_WIDE_AVX
    return _mm256_extractf128_ps(v, 1);
#else
    return v.hi;
#endif
}

// Lifts a two-operand SSE function to eight lanes in the fallback.
#define NPH_F32X8_LIFT2_(name, op4, op8)                                                           \
    NPH_INLINE NphF32x8 name(NphF32x8 a, NphF32x8 b)                                               \
    {                                                                                              \
        NPH_F32X8_LIFT2_BODY_(op4, op8)                                                            \
    }
#if NPH_WIDE_AVX
#define NPH_F32X8_LIFT2_BODY_(op4, op8) return op8(a, b);
#define NPH_F32X8_LIFT1_BODY_(op4, op8) return op8(v);
#else
#define NPH_F32X8_LIFT2_BODY_(op4, op8)                                                            \
    NphF32x8 r;                                                                                    \
    r.lo = op4(a.lo, b.lo);                                                                        \
    r.hi = op4(a.hi, b.hi);                                                                        \
    return r;
#define NPH_F32X8_LIFT1_BODY_(op4, op8)                                                            \
    NphF32x8 r;                                                                                    \
    r.lo = op4(v.lo);                                                                              \
    r.hi = op4(v.hi);                                                                              \
    return r;
#endif
#define NPH_F32X8_LIFT1_(name, op4, op8)                                                           \
    NPH_INLINE NphF32x8 name(NphF32x8 v)                                                           \
    {                                                                                              \
        NPH_F32X8_LIFT1_BODY_(op4, op8)                                                            \
    }

NPH_INLINE NphF32x8 nph_f32x8_splat(f32 s)
{
#if NPH_WIDE_AVX
    return _mm256_set1_ps(s);
#else
    return nph_f32x8_from_x4(_mm_set1_ps(s), _mm_set1_ps(s));
#endif
}
NPH_INLINE NphF32x8 nph_f32x8_zero(void)
{
#if NPH_WIDE_AVX
    return _mm256_setzero_ps();
#else
    return nph_f32x8_from_x4(_mm_setzero_ps(), _mm_setzero_ps());
#endif
}
// Loads eight floats from any address.
NPH_INLINE NphF32x8 nph_f32x8_load(const f32 *p)
{
#if NPH_WIDE_AVX
    return _mm256_loadu_ps(p);
#else
    return nph_f32x8_from_x4(_mm_loadu_ps(p), _mm_loadu_ps(p + 4));
#endif
}
NPH_INLINE void nph_f32x8_store(f32 *p, NphF32x8 v)
{
#if NPH_WIDE_AVX
    _mm256_storeu_ps(p, v);
#else
    _mm_storeu_ps(p, v.lo);
    _mm_storeu_ps(p + 4, v.hi);
#endif
}

NPH_F32X8_LIFT2_(nph_f32x8_add, _mm_add_ps, _mm256_add_ps)
NPH_F32X8_LIFT2_(nph_f32x8_sub, _mm_sub_ps, _mm256_sub_ps)
NPH_F32X8_LIFT2_(nph_f32x8_mul, _mm_mul_ps, _mm256_mul_ps)
NPH_F32X8_LIFT2_(nph_f32x8_div, _mm_div_ps, _mm256_div_ps)
NPH_F32X8_LIFT2_(nph_f32x8_min, _mm_min_ps, _mm256_min_ps)
NPH_F32X8_LIFT2_(nph_f32x8_max, _mm_max_ps, _mm256_max_ps)
NPH_F32X8_LIFT2_(nph_f32x8_and, _mm_and_ps, _mm256_and_ps)
NPH_F32X8_LIFT2_(nph_f32x8_or, _mm_or_ps, _mm256_or_ps)
NPH_F32X8_LIFT2_(nph_f32x8_xor, _mm_xor_ps, _mm256_xor_ps)
NPH_F32X8_LIFT2_(nph_f32x8_andnot, _mm_andnot_ps, _mm256_andnot_ps)
NPH_F32X8_LIFT1_(nph_f32x8_sqrt, _mm_sqrt_ps, _mm256_sqrt_ps)
NPH_F32X8_LIFT1_(nph_f32x8_floor, _mm_floor_ps, _mm256_floor_ps)
NPH_F32X8_LIFT1_(nph_f32x8_ceil, _mm_ceil_ps, _mm256_ceil_ps)

// a * b + c as two rounded operations (no FMA: ADR-0008 §6).
NPH_INLINE NphF32x8 nph_f32x8_mul_add(NphF32x8 a, NphF32x8 b, NphF32x8 c)
{
    return nph_f32x8_add(nph_f32x8_mul(a, b), c);
}
NPH_INLINE NphF32x8 nph_f32x8_neg(NphF32x8 v)
{
    return nph_f32x8_xor(v, nph_f32x8_splat(-0.0F));
}
NPH_INLINE NphF32x8 nph_f32x8_abs(NphF32x8 v)
{
    return nph_f32x8_andnot(nph_f32x8_splat(-0.0F), v);
}
// 1 / sqrt(v), correctly rounded (two operations).
NPH_INLINE NphF32x8 nph_f32x8_rsqrt(NphF32x8 v)
{
    return nph_f32x8_div(nph_f32x8_splat(1.0F), nph_f32x8_sqrt(v));
}
NPH_INLINE NphF32x8 nph_f32x8_lerp(NphF32x8 a, NphF32x8 b, NphF32x8 t)
{
    return nph_f32x8_add(nph_f32x8_mul(nph_f32x8_sub(nph_f32x8_splat(1.0F), t), a),
                         nph_f32x8_mul(t, b));
}

#if NPH_WIDE_AVX
#define NPH_F32X8_CMP_(name, imm, op4)                                                             \
    NPH_INLINE NphF32x8 name(NphF32x8 a, NphF32x8 b)                                               \
    {                                                                                              \
        return _mm256_cmp_ps(a, b, imm);                                                           \
    }
#else
#define NPH_F32X8_CMP_(name, imm, op4)                                                             \
    NPH_INLINE NphF32x8 name(NphF32x8 a, NphF32x8 b)                                               \
    {                                                                                              \
        NphF32x8 r;                                                                                \
        r.lo = op4(a.lo, b.lo);                                                                    \
        r.hi = op4(a.hi, b.hi);                                                                    \
        return r;                                                                                  \
    }
#endif
NPH_F32X8_CMP_(nph_f32x8_cmp_eq, _CMP_EQ_OQ, _mm_cmpeq_ps)
NPH_F32X8_CMP_(nph_f32x8_cmp_lt, _CMP_LT_OQ, _mm_cmplt_ps)
NPH_F32X8_CMP_(nph_f32x8_cmp_le, _CMP_LE_OQ, _mm_cmple_ps)
NPH_F32X8_CMP_(nph_f32x8_cmp_gt, _CMP_GT_OQ, _mm_cmpgt_ps)
NPH_F32X8_CMP_(nph_f32x8_cmp_ge, _CMP_GE_OQ, _mm_cmpge_ps)

// Per lane: mask ? a : b.
NPH_INLINE NphF32x8 nph_f32x8_select(NphF32x8 mask, NphF32x8 a, NphF32x8 b)
{
#if NPH_WIDE_AVX
    return _mm256_blendv_ps(b, a, mask);
#else
    NphF32x8 r;
    r.lo = _mm_blendv_ps(b.lo, a.lo, mask.lo);
    r.hi = _mm_blendv_ps(b.hi, a.hi, mask.hi);
    return r;
#endif
}
// Bit i of the result is the sign (mask) bit of lane i.
NPH_INLINE u32 nph_f32x8_movemask(NphF32x8 mask)
{
#if NPH_WIDE_AVX
    return (u32)_mm256_movemask_ps(mask);
#else
    return (u32)_mm_movemask_ps(mask.lo) | ((u32)_mm_movemask_ps(mask.hi) << 4U);
#endif
}
NPH_INLINE bool nph_f32x8_any(NphF32x8 mask)
{
    return nph_f32x8_movemask(mask) != 0U;
}
NPH_INLINE bool nph_f32x8_all(NphF32x8 mask)
{
    return nph_f32x8_movemask(mask) == 0xFFU;
}

// ---------------------------------------------------------------------------
// NphVec3x4: four vectors, structure of arrays
// ---------------------------------------------------------------------------

typedef struct NphVec3x4 {
    NphF32x4 x, y, z;
} NphVec3x4;

NPH_INLINE NphVec3x4 nph_vec3x4_from_lanes(NphF32x4 x, NphF32x4 y, NphF32x4 z)
{
    NphVec3x4 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}
// The same vector in all four slots.
NPH_INLINE NphVec3x4 nph_vec3x4_splat(NphVec3 v)
{
    return nph_vec3x4_from_lanes(_mm_set1_ps(v.x), _mm_set1_ps(v.y), _mm_set1_ps(v.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_zero(void)
{
    return nph_vec3x4_from_lanes(_mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps());
}
NPH_INLINE NphVec3x4 nph_vec3x4_load_soa(const f32 *x, const f32 *y, const f32 *z)
{
    return nph_vec3x4_from_lanes(_mm_loadu_ps(x), _mm_loadu_ps(y), _mm_loadu_ps(z));
}
NPH_INLINE void nph_vec3x4_store_soa(f32 *x, f32 *y, f32 *z, NphVec3x4 v)
{
    _mm_storeu_ps(x, v.x);
    _mm_storeu_ps(y, v.y);
    _mm_storeu_ps(z, v.z);
}
// Transposes four consecutive NphVec3 (12 floats) into SoA with five
// shuffles.
NPH_INLINE NphVec3x4 nph_vec3x4_load_aos(const NphVec3 *p)
{
    const __m128 a = _mm_loadu_ps(&p[0].x);                // x0 y0 z0 x1
    const __m128 b = _mm_loadu_ps(&p[1].y);                // y1 z1 x2 y2
    const __m128 c = _mm_loadu_ps(&p[2].z);                // z2 x3 y3 z3
    const __m128 t = NPH_F32X4_SHUFFLE2(b, c, 2, 3, 1, 2); // x2 y2 x3 y3
    const __m128 u = NPH_F32X4_SHUFFLE2(a, b, 1, 2, 0, 1); // y0 z0 y1 z1
    return nph_vec3x4_from_lanes(NPH_F32X4_SHUFFLE2(a, t, 0, 3, 0, 2),
                                 NPH_F32X4_SHUFFLE2(u, t, 0, 2, 1, 3),
                                 NPH_F32X4_SHUFFLE2(u, c, 1, 3, 0, 3));
}
// Inverse of nph_vec3x4_load_aos: writes four NphVec3.
NPH_INLINE void nph_vec3x4_store_aos(NphVec3 *out, NphVec3x4 v)
{
    const __m128 xy01 = NPH_F32X4_SHUFFLE2(v.x, v.y, 0, 1, 0, 1);         // x0 x1 y0 y1
    const __m128 zx12 = NPH_F32X4_SHUFFLE2(v.z, v.x, 0, 1, 1, 2);         // z0 z1 x1 x2
    const __m128 yz12 = NPH_F32X4_SHUFFLE2(v.y, v.z, 1, 2, 1, 2);         // y1 y2 z1 z2
    const __m128 xy23 = NPH_F32X4_SHUFFLE2(v.x, v.y, 2, 3, 2, 3);         // x2 x3 y2 y3
    const __m128 zx23 = NPH_F32X4_SHUFFLE2(v.z, v.x, 2, 3, 3, 3);         // z2 z3 x3 x3
    const __m128 yz33 = NPH_F32X4_SHUFFLE2(v.y, v.z, 3, 3, 3, 3);         // y3 y3 z3 z3
    _mm_storeu_ps(&out[0].x, NPH_F32X4_SHUFFLE2(xy01, zx12, 0, 2, 0, 2)); // x0 y0 z0 x1
    _mm_storeu_ps(&out[1].y, NPH_F32X4_SHUFFLE2(yz12, xy23, 0, 2, 0, 2)); // y1 z1 x2 y2
    _mm_storeu_ps(&out[2].z, NPH_F32X4_SHUFFLE2(zx23, yz33, 0, 2, 0, 2)); // z2 x3 y3 z3
}
// Reads slot i (0..3) back as a scalar vector.
NPH_INLINE NphVec3 nph_vec3x4_get(NphVec3x4 v, u32 i)
{
    NPH_ALIGNAS(16) f32 x[4];
    NPH_ALIGNAS(16) f32 y[4];
    NPH_ALIGNAS(16) f32 z[4];
    _mm_store_ps(x, v.x);
    _mm_store_ps(y, v.y);
    _mm_store_ps(z, v.z);
    return nph_vec3(x[i], y[i], z[i]);
}

NPH_INLINE NphVec3x4 nph_vec3x4_add(NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_add_ps(a.x, b.x), _mm_add_ps(a.y, b.y), _mm_add_ps(a.z, b.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_sub(NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_sub_ps(a.x, b.x), _mm_sub_ps(a.y, b.y), _mm_sub_ps(a.z, b.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_mul(NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_mul_ps(a.x, b.x), _mm_mul_ps(a.y, b.y), _mm_mul_ps(a.z, b.z));
}
// Per-slot scale: slot i is multiplied by lane i of s.
NPH_INLINE NphVec3x4 nph_vec3x4_scale(NphVec3x4 v, NphF32x4 s)
{
    return nph_vec3x4_from_lanes(_mm_mul_ps(v.x, s), _mm_mul_ps(v.y, s), _mm_mul_ps(v.z, s));
}
// a + b * s, like nph_vec3_mul_add.
NPH_INLINE NphVec3x4 nph_vec3x4_mul_add(NphVec3x4 a, NphVec3x4 b, NphF32x4 s)
{
    return nph_vec3x4_from_lanes(_mm_add_ps(a.x, _mm_mul_ps(b.x, s)),
                                 _mm_add_ps(a.y, _mm_mul_ps(b.y, s)),
                                 _mm_add_ps(a.z, _mm_mul_ps(b.z, s)));
}
NPH_INLINE NphVec3x4 nph_vec3x4_neg(NphVec3x4 v)
{
    return nph_vec3x4_from_lanes(nph_f32x4_neg(v.x), nph_f32x4_neg(v.y), nph_f32x4_neg(v.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_min(NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_min_ps(a.x, b.x), _mm_min_ps(a.y, b.y), _mm_min_ps(a.z, b.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_max(NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_max_ps(a.x, b.x), _mm_max_ps(a.y, b.y), _mm_max_ps(a.z, b.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_select(NphF32x4 mask, NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_blendv_ps(b.x, a.x, mask), _mm_blendv_ps(b.y, a.y, mask),
                                 _mm_blendv_ps(b.z, a.z, mask));
}
// (x + y) + z per slot, like nph_vec3_dot.
NPH_INLINE NphF32x4 nph_vec3x4_dot(NphVec3x4 a, NphVec3x4 b)
{
    return _mm_add_ps(_mm_add_ps(_mm_mul_ps(a.x, b.x), _mm_mul_ps(a.y, b.y)), _mm_mul_ps(a.z, b.z));
}
NPH_INLINE NphVec3x4 nph_vec3x4_cross(NphVec3x4 a, NphVec3x4 b)
{
    return nph_vec3x4_from_lanes(_mm_sub_ps(_mm_mul_ps(a.y, b.z), _mm_mul_ps(a.z, b.y)),
                                 _mm_sub_ps(_mm_mul_ps(a.z, b.x), _mm_mul_ps(a.x, b.z)),
                                 _mm_sub_ps(_mm_mul_ps(a.x, b.y), _mm_mul_ps(a.y, b.x)));
}
NPH_INLINE NphF32x4 nph_vec3x4_length_sq(NphVec3x4 v)
{
    return nph_vec3x4_dot(v, v);
}
NPH_INLINE NphF32x4 nph_vec3x4_length(NphVec3x4 v)
{
    return _mm_sqrt_ps(nph_vec3x4_dot(v, v));
}
// v * (1 / sqrt(dot)), like nph_vec3_normalize; zero slots become inf/NaN.
NPH_INLINE NphVec3x4 nph_vec3x4_normalize(NphVec3x4 v)
{
    return nph_vec3x4_scale(v, nph_f32x4_rsqrt(nph_vec3x4_dot(v, v)));
}
NPH_INLINE NphVec3x4 nph_vec3x4_lerp(NphVec3x4 a, NphVec3x4 b, NphF32x4 t)
{
    return nph_vec3x4_from_lanes(nph_f32x4_lerp(a.x, b.x, t), nph_f32x4_lerp(a.y, b.y, t),
                                 nph_f32x4_lerp(a.z, b.z, t));
}
// m · (p, 1) for four points, term for term as nph_affine_transform_point.
NPH_INLINE NphVec3x4 nph_vec3x4_transform_point(const NphAffine *m, NphVec3x4 p)
{
    NphVec3x4 r;
    __m128 t = _mm_mul_ps(_mm_set1_ps(m->r[0].x), p.x);
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[0].y), p.y));
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[0].z), p.z));
    r.x = _mm_add_ps(t, _mm_set1_ps(m->r[0].w));
    t = _mm_mul_ps(_mm_set1_ps(m->r[1].x), p.x);
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[1].y), p.y));
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[1].z), p.z));
    r.y = _mm_add_ps(t, _mm_set1_ps(m->r[1].w));
    t = _mm_mul_ps(_mm_set1_ps(m->r[2].x), p.x);
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[2].y), p.y));
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[2].z), p.z));
    r.z = _mm_add_ps(t, _mm_set1_ps(m->r[2].w));
    return r;
}
// m · (v, 0) for four vectors.
NPH_INLINE NphVec3x4 nph_vec3x4_transform_vector(const NphAffine *m, NphVec3x4 v)
{
    NphVec3x4 r;
    __m128 t = _mm_mul_ps(_mm_set1_ps(m->r[0].x), v.x);
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[0].y), v.y));
    r.x = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[0].z), v.z));
    t = _mm_mul_ps(_mm_set1_ps(m->r[1].x), v.x);
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[1].y), v.y));
    r.y = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[1].z), v.z));
    t = _mm_mul_ps(_mm_set1_ps(m->r[2].x), v.x);
    t = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[2].y), v.y));
    r.z = _mm_add_ps(t, _mm_mul_ps(_mm_set1_ps(m->r[2].z), v.z));
    return r;
}

// ---------------------------------------------------------------------------
// NphVec3x8: eight vectors, structure of arrays
// ---------------------------------------------------------------------------

typedef struct NphVec3x8 {
    NphF32x8 x, y, z;
} NphVec3x8;

NPH_INLINE NphVec3x8 nph_vec3x8_from_lanes(NphF32x8 x, NphF32x8 y, NphF32x8 z)
{
    NphVec3x8 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}
NPH_INLINE NphVec3x8 nph_vec3x8_from_x4(NphVec3x4 lo, NphVec3x4 hi)
{
    return nph_vec3x8_from_lanes(nph_f32x8_from_x4(lo.x, hi.x), nph_f32x8_from_x4(lo.y, hi.y),
                                 nph_f32x8_from_x4(lo.z, hi.z));
}
NPH_INLINE NphVec3x4 nph_vec3x8_lo(NphVec3x8 v)
{
    return nph_vec3x4_from_lanes(nph_f32x8_lo(v.x), nph_f32x8_lo(v.y), nph_f32x8_lo(v.z));
}
NPH_INLINE NphVec3x4 nph_vec3x8_hi(NphVec3x8 v)
{
    return nph_vec3x4_from_lanes(nph_f32x8_hi(v.x), nph_f32x8_hi(v.y), nph_f32x8_hi(v.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_splat(NphVec3 v)
{
    return nph_vec3x8_from_lanes(nph_f32x8_splat(v.x), nph_f32x8_splat(v.y), nph_f32x8_splat(v.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_load_soa(const f32 *x, const f32 *y, const f32 *z)
{
    return nph_vec3x8_from_lanes(nph_f32x8_load(x), nph_f32x8_load(y), nph_f32x8_load(z));
}
NPH_INLINE void nph_vec3x8_store_soa(f32 *x, f32 *y, f32 *z, NphVec3x8 v)
{
    nph_f32x8_store(x, v.x);
    nph_f32x8_store(y, v.y);
    nph_f32x8_store(z, v.z);
}
// Eight consecutive NphVec3 to SoA.
NPH_INLINE NphVec3x8 nph_vec3x8_load_aos(const NphVec3 *p)
{
    return nph_vec3x8_from_x4(nph_vec3x4_load_aos(p), nph_vec3x4_load_aos(p + 4));
}
NPH_INLINE void nph_vec3x8_store_aos(NphVec3 *out, NphVec3x8 v)
{
    nph_vec3x4_store_aos(out, nph_vec3x8_lo(v));
    nph_vec3x4_store_aos(out + 4, nph_vec3x8_hi(v));
}
NPH_INLINE NphVec3 nph_vec3x8_get(NphVec3x8 v, u32 i)
{
    return i < 4U ? nph_vec3x4_get(nph_vec3x8_lo(v), i) : nph_vec3x4_get(nph_vec3x8_hi(v), i - 4U);
}

NPH_INLINE NphVec3x8 nph_vec3x8_add(NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_add(a.x, b.x), nph_f32x8_add(a.y, b.y),
                                 nph_f32x8_add(a.z, b.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_sub(NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_sub(a.x, b.x), nph_f32x8_sub(a.y, b.y),
                                 nph_f32x8_sub(a.z, b.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_mul(NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_mul(a.x, b.x), nph_f32x8_mul(a.y, b.y),
                                 nph_f32x8_mul(a.z, b.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_scale(NphVec3x8 v, NphF32x8 s)
{
    return nph_vec3x8_from_lanes(nph_f32x8_mul(v.x, s), nph_f32x8_mul(v.y, s),
                                 nph_f32x8_mul(v.z, s));
}
NPH_INLINE NphVec3x8 nph_vec3x8_mul_add(NphVec3x8 a, NphVec3x8 b, NphF32x8 s)
{
    return nph_vec3x8_from_lanes(nph_f32x8_add(a.x, nph_f32x8_mul(b.x, s)),
                                 nph_f32x8_add(a.y, nph_f32x8_mul(b.y, s)),
                                 nph_f32x8_add(a.z, nph_f32x8_mul(b.z, s)));
}
NPH_INLINE NphVec3x8 nph_vec3x8_neg(NphVec3x8 v)
{
    return nph_vec3x8_from_lanes(nph_f32x8_neg(v.x), nph_f32x8_neg(v.y), nph_f32x8_neg(v.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_min(NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_min(a.x, b.x), nph_f32x8_min(a.y, b.y),
                                 nph_f32x8_min(a.z, b.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_max(NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_max(a.x, b.x), nph_f32x8_max(a.y, b.y),
                                 nph_f32x8_max(a.z, b.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_select(NphF32x8 mask, NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_select(mask, a.x, b.x), nph_f32x8_select(mask, a.y, b.y),
                                 nph_f32x8_select(mask, a.z, b.z));
}
NPH_INLINE NphF32x8 nph_vec3x8_dot(NphVec3x8 a, NphVec3x8 b)
{
    return nph_f32x8_add(nph_f32x8_add(nph_f32x8_mul(a.x, b.x), nph_f32x8_mul(a.y, b.y)),
                         nph_f32x8_mul(a.z, b.z));
}
NPH_INLINE NphVec3x8 nph_vec3x8_cross(NphVec3x8 a, NphVec3x8 b)
{
    return nph_vec3x8_from_lanes(nph_f32x8_sub(nph_f32x8_mul(a.y, b.z), nph_f32x8_mul(a.z, b.y)),
                                 nph_f32x8_sub(nph_f32x8_mul(a.z, b.x), nph_f32x8_mul(a.x, b.z)),
                                 nph_f32x8_sub(nph_f32x8_mul(a.x, b.y), nph_f32x8_mul(a.y, b.x)));
}
NPH_INLINE NphF32x8 nph_vec3x8_length_sq(NphVec3x8 v)
{
    return nph_vec3x8_dot(v, v);
}
NPH_INLINE NphF32x8 nph_vec3x8_length(NphVec3x8 v)
{
    return nph_f32x8_sqrt(nph_vec3x8_dot(v, v));
}
NPH_INLINE NphVec3x8 nph_vec3x8_normalize(NphVec3x8 v)
{
    return nph_vec3x8_scale(v, nph_f32x8_rsqrt(nph_vec3x8_dot(v, v)));
}
NPH_INLINE NphVec3x8 nph_vec3x8_lerp(NphVec3x8 a, NphVec3x8 b, NphF32x8 t)
{
    return nph_vec3x8_from_lanes(nph_f32x8_lerp(a.x, b.x, t), nph_f32x8_lerp(a.y, b.y, t),
                                 nph_f32x8_lerp(a.z, b.z, t));
}
NPH_INLINE NphVec3x8 nph_vec3x8_transform_point(const NphAffine *m, NphVec3x8 p)
{
    NphVec3x8 r;
    NphF32x8 t = nph_f32x8_mul(nph_f32x8_splat(m->r[0].x), p.x);
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[0].y), p.y));
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[0].z), p.z));
    r.x = nph_f32x8_add(t, nph_f32x8_splat(m->r[0].w));
    t = nph_f32x8_mul(nph_f32x8_splat(m->r[1].x), p.x);
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[1].y), p.y));
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[1].z), p.z));
    r.y = nph_f32x8_add(t, nph_f32x8_splat(m->r[1].w));
    t = nph_f32x8_mul(nph_f32x8_splat(m->r[2].x), p.x);
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[2].y), p.y));
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[2].z), p.z));
    r.z = nph_f32x8_add(t, nph_f32x8_splat(m->r[2].w));
    return r;
}
NPH_INLINE NphVec3x8 nph_vec3x8_transform_vector(const NphAffine *m, NphVec3x8 v)
{
    NphVec3x8 r;
    NphF32x8 t = nph_f32x8_mul(nph_f32x8_splat(m->r[0].x), v.x);
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[0].y), v.y));
    r.x = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[0].z), v.z));
    t = nph_f32x8_mul(nph_f32x8_splat(m->r[1].x), v.x);
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[1].y), v.y));
    r.y = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[1].z), v.z));
    t = nph_f32x8_mul(nph_f32x8_splat(m->r[2].x), v.x);
    t = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[2].y), v.y));
    r.z = nph_f32x8_add(t, nph_f32x8_mul(nph_f32x8_splat(m->r[2].z), v.z));
    return r;
}

// ---------------------------------------------------------------------------
// Wide frustum tests. The inputs are centres and half-extents (or radii);
// bit i of the result is set when volume i is not outside the frustum.
// Arithmetic per plane, in this order so every ISA level agrees bit for bit
// with the scalar kernel in kernels.c:
//   r = n.x c.x; r += n.y c.y; r += n.z c.z; r += d;
//   a = |n.x| e.x; a += |n.y| e.y; a += |n.z| e.z;  visible &= (r + a) >= 0
// All six plane slots are used; a frustum with five planes carries an
// always-true sixth (geometry.h), so there is no branch on plane_count.
// ---------------------------------------------------------------------------

NPH_INLINE u32 nph_frustum_cull_aabb_x4(const NphFrustum *f, NphVec3x4 center, NphVec3x4 extent)
{
    __m128 visible = _mm_castsi128_ps(_mm_set1_epi32(-1));
    for (u32 i = 0; i < NPH_FRUSTUM_PLANE_COUNT; ++i) {
        const NphPlane p = f->planes[i];
        __m128 r = _mm_mul_ps(_mm_set1_ps(p.normal.x), center.x);
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(p.normal.y), center.y));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(p.normal.z), center.z));
        r = _mm_add_ps(r, _mm_set1_ps(p.d));
        __m128 a = _mm_mul_ps(_mm_set1_ps(nph_f32_abs(p.normal.x)), extent.x);
        a = _mm_add_ps(a, _mm_mul_ps(_mm_set1_ps(nph_f32_abs(p.normal.y)), extent.y));
        a = _mm_add_ps(a, _mm_mul_ps(_mm_set1_ps(nph_f32_abs(p.normal.z)), extent.z));
        visible = _mm_and_ps(visible, _mm_cmpge_ps(_mm_add_ps(r, a), _mm_setzero_ps()));
    }
    return (u32)_mm_movemask_ps(visible);
}

NPH_INLINE u32 nph_frustum_cull_sphere_x4(const NphFrustum *f, NphVec3x4 center, NphF32x4 radius)
{
    __m128 visible = _mm_castsi128_ps(_mm_set1_epi32(-1));
    const __m128 neg_radius = nph_f32x4_neg(radius);
    for (u32 i = 0; i < NPH_FRUSTUM_PLANE_COUNT; ++i) {
        const NphPlane p = f->planes[i];
        __m128 r = _mm_mul_ps(_mm_set1_ps(p.normal.x), center.x);
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(p.normal.y), center.y));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(p.normal.z), center.z));
        r = _mm_add_ps(r, _mm_set1_ps(p.d));
        visible = _mm_and_ps(visible, _mm_cmpge_ps(r, neg_radius));
    }
    return (u32)_mm_movemask_ps(visible);
}

NPH_INLINE u32 nph_frustum_cull_aabb_x8(const NphFrustum *f, NphVec3x8 center, NphVec3x8 extent)
{
    NphF32x8 visible = nph_f32x8_cmp_eq(nph_f32x8_zero(), nph_f32x8_zero());
    const NphF32x8 zero = nph_f32x8_zero();
    for (u32 i = 0; i < NPH_FRUSTUM_PLANE_COUNT; ++i) {
        const NphPlane p = f->planes[i];
        NphF32x8 r = nph_f32x8_mul(nph_f32x8_splat(p.normal.x), center.x);
        r = nph_f32x8_add(r, nph_f32x8_mul(nph_f32x8_splat(p.normal.y), center.y));
        r = nph_f32x8_add(r, nph_f32x8_mul(nph_f32x8_splat(p.normal.z), center.z));
        r = nph_f32x8_add(r, nph_f32x8_splat(p.d));
        NphF32x8 a = nph_f32x8_mul(nph_f32x8_splat(nph_f32_abs(p.normal.x)), extent.x);
        a = nph_f32x8_add(a, nph_f32x8_mul(nph_f32x8_splat(nph_f32_abs(p.normal.y)), extent.y));
        a = nph_f32x8_add(a, nph_f32x8_mul(nph_f32x8_splat(nph_f32_abs(p.normal.z)), extent.z));
        visible = nph_f32x8_and(visible, nph_f32x8_cmp_ge(nph_f32x8_add(r, a), zero));
    }
    return nph_f32x8_movemask(visible);
}

NPH_INLINE u32 nph_frustum_cull_sphere_x8(const NphFrustum *f, NphVec3x8 center, NphF32x8 radius)
{
    NphF32x8 visible = nph_f32x8_cmp_eq(nph_f32x8_zero(), nph_f32x8_zero());
    const NphF32x8 neg_radius = nph_f32x8_neg(radius);
    for (u32 i = 0; i < NPH_FRUSTUM_PLANE_COUNT; ++i) {
        const NphPlane p = f->planes[i];
        NphF32x8 r = nph_f32x8_mul(nph_f32x8_splat(p.normal.x), center.x);
        r = nph_f32x8_add(r, nph_f32x8_mul(nph_f32x8_splat(p.normal.y), center.y));
        r = nph_f32x8_add(r, nph_f32x8_mul(nph_f32x8_splat(p.normal.z), center.z));
        r = nph_f32x8_add(r, nph_f32x8_splat(p.d));
        visible = nph_f32x8_and(visible, nph_f32x8_cmp_ge(r, neg_radius));
    }
    return nph_f32x8_movemask(visible);
}

#ifdef __cplusplus
}
#endif

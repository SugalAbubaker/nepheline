// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scalar storage vectors (CLAUDE.md §8.2; docs/adr/0008-math-conventions.md):
// plain structs with natural alignment and no padding, safe to embed in
// any struct or file and to share with GLSL. Every operation is defined
// here; these are the reference implementations the SIMD layer (simd.h)
// is tested against.
//
// Naming: nph_vecN_<op>(a, b). Binary operations are component-wise unless
// the name says otherwise (dot, cross, scale). Nothing here allocates,
// asserts only on preconditions that would produce NaN (normalising a zero
// vector), and every function is pure and NPH_THREADSAFE.

#include <nepheline/math/scalar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphVec2 {
    f32 x, y;
} NphVec2;

typedef struct NphVec3 {
    f32 x, y, z;
} NphVec3;

typedef struct NphVec4 {
    f32 x, y, z, w;
} NphVec4;

typedef struct NphIVec2 {
    i32 x, y;
} NphIVec2;

typedef struct NphIVec3 {
    i32 x, y, z;
} NphIVec3;

typedef struct NphIVec4 {
    i32 x, y, z, w;
} NphIVec4;

typedef struct NphUVec2 {
    u32 x, y;
} NphUVec2;

typedef struct NphUVec3 {
    u32 x, y, z;
} NphUVec3;

typedef struct NphUVec4 {
    u32 x, y, z, w;
} NphUVec4;

// Double-precision position for the 64-bit world (ADR-0008 §8).
typedef struct NphDVec3 {
    f64 x, y, z;
} NphDVec3;

NPH_STATIC_ASSERT(sizeof(NphVec2) == 8U, "NphVec2 is two floats");
NPH_STATIC_ASSERT(sizeof(NphVec3) == 12U, "NphVec3 is three floats, unpadded");
NPH_STATIC_ASSERT(sizeof(NphVec4) == 16U, "NphVec4 is four floats");
NPH_STATIC_ASSERT(sizeof(NphIVec4) == 16U, "NphIVec4 is four ints");
NPH_STATIC_ASSERT(sizeof(NphUVec4) == 16U, "NphUVec4 is four uints");
NPH_STATIC_ASSERT(sizeof(NphDVec3) == 24U, "NphDVec3 is three doubles");
NPH_STATIC_ASSERT(NPH_ALIGNOF(NphVec4) == 4U, "storage vectors keep natural alignment");

// ---------------------------------------------------------------------------
// NphVec2
// ---------------------------------------------------------------------------

NPH_INLINE NphVec2 nph_vec2(f32 x, f32 y)
{
    return (NphVec2){x, y};
}
NPH_INLINE NphVec2 nph_vec2_splat(f32 s)
{
    return (NphVec2){s, s};
}
NPH_INLINE NphVec2 nph_vec2_zero(void)
{
    return (NphVec2){0.0F, 0.0F};
}
NPH_INLINE NphVec2 nph_vec2_one(void)
{
    return (NphVec2){1.0F, 1.0F};
}
NPH_INLINE NphVec2 nph_vec2_from_array(const f32 *p)
{
    return (NphVec2){p[0], p[1]};
}
NPH_INLINE void nph_vec2_to_array(NphVec2 v, f32 *out)
{
    out[0] = v.x;
    out[1] = v.y;
}
NPH_INLINE NphVec2 nph_vec2_add(NphVec2 a, NphVec2 b)
{
    return (NphVec2){a.x + b.x, a.y + b.y};
}
NPH_INLINE NphVec2 nph_vec2_sub(NphVec2 a, NphVec2 b)
{
    return (NphVec2){a.x - b.x, a.y - b.y};
}
NPH_INLINE NphVec2 nph_vec2_mul(NphVec2 a, NphVec2 b)
{
    return (NphVec2){a.x * b.x, a.y * b.y};
}
NPH_INLINE NphVec2 nph_vec2_div(NphVec2 a, NphVec2 b)
{
    return (NphVec2){a.x / b.x, a.y / b.y};
}
NPH_INLINE NphVec2 nph_vec2_scale(NphVec2 v, f32 s)
{
    return (NphVec2){v.x * s, v.y * s};
}
NPH_INLINE NphVec2 nph_vec2_neg(NphVec2 v)
{
    return (NphVec2){-v.x, -v.y};
}
NPH_INLINE f32 nph_vec2_dot(NphVec2 a, NphVec2 b)
{
    return a.x * b.x + a.y * b.y;
}
// The z component of the 3D cross product of (a, 0) and (b, 0): positive
// when b is counter-clockwise from a.
NPH_INLINE f32 nph_vec2_cross(NphVec2 a, NphVec2 b)
{
    return a.x * b.y - a.y * b.x;
}
// a rotated 90 degrees counter-clockwise.
NPH_INLINE NphVec2 nph_vec2_perp(NphVec2 v)
{
    return (NphVec2){-v.y, v.x};
}
NPH_INLINE f32 nph_vec2_length_sq(NphVec2 v)
{
    return nph_vec2_dot(v, v);
}
NPH_INLINE f32 nph_vec2_length(NphVec2 v)
{
    return nph_f32_sqrt(nph_vec2_length_sq(v));
}
NPH_INLINE f32 nph_vec2_distance_sq(NphVec2 a, NphVec2 b)
{
    return nph_vec2_length_sq(nph_vec2_sub(a, b));
}
NPH_INLINE f32 nph_vec2_distance(NphVec2 a, NphVec2 b)
{
    return nph_vec2_length(nph_vec2_sub(a, b));
}
// Unit vector in the direction of v. v must not be zero (asserted).
NPH_INLINE NphVec2 nph_vec2_normalize(NphVec2 v)
{
    const f32 len_sq = nph_vec2_length_sq(v);
    NPH_ASSERT_MSG(len_sq > 0.0F, "nph_vec2_normalize of a zero vector");
    return nph_vec2_scale(v, nph_f32_rsqrt(len_sq));
}
// As nph_vec2_normalize, but returns zero for a zero vector.
NPH_INLINE NphVec2 nph_vec2_normalize_or_zero(NphVec2 v)
{
    const f32 len_sq = nph_vec2_length_sq(v);
    return len_sq > 0.0F ? nph_vec2_scale(v, nph_f32_rsqrt(len_sq)) : nph_vec2_zero();
}
NPH_INLINE NphVec2 nph_vec2_lerp(NphVec2 a, NphVec2 b, f32 t)
{
    return (NphVec2){nph_f32_lerp(a.x, b.x, t), nph_f32_lerp(a.y, b.y, t)};
}
NPH_INLINE NphVec2 nph_vec2_min(NphVec2 a, NphVec2 b)
{
    return (NphVec2){nph_f32_min(a.x, b.x), nph_f32_min(a.y, b.y)};
}
NPH_INLINE NphVec2 nph_vec2_max(NphVec2 a, NphVec2 b)
{
    return (NphVec2){nph_f32_max(a.x, b.x), nph_f32_max(a.y, b.y)};
}
NPH_INLINE NphVec2 nph_vec2_clamp(NphVec2 v, NphVec2 lo, NphVec2 hi)
{
    return nph_vec2_min(nph_vec2_max(v, lo), hi);
}
NPH_INLINE NphVec2 nph_vec2_abs(NphVec2 v)
{
    return (NphVec2){nph_f32_abs(v.x), nph_f32_abs(v.y)};
}
NPH_INLINE NphVec2 nph_vec2_floor(NphVec2 v)
{
    return (NphVec2){nph_f32_floor(v.x), nph_f32_floor(v.y)};
}
NPH_INLINE f32 nph_vec2_min_component(NphVec2 v)
{
    return nph_f32_min(v.x, v.y);
}
NPH_INLINE f32 nph_vec2_max_component(NphVec2 v)
{
    return nph_f32_max(v.x, v.y);
}
NPH_INLINE bool nph_vec2_eq(NphVec2 a, NphVec2 b)
{
    return a.x == b.x && a.y == b.y;
}
NPH_INLINE bool nph_vec2_near(NphVec2 a, NphVec2 b, f32 tol)
{
    return nph_f32_near(a.x, b.x, tol) && nph_f32_near(a.y, b.y, tol);
}

// ---------------------------------------------------------------------------
// NphVec3
// ---------------------------------------------------------------------------

NPH_INLINE NphVec3 nph_vec3(f32 x, f32 y, f32 z)
{
    return (NphVec3){x, y, z};
}
NPH_INLINE NphVec3 nph_vec3_splat(f32 s)
{
    return (NphVec3){s, s, s};
}
NPH_INLINE NphVec3 nph_vec3_zero(void)
{
    return (NphVec3){0.0F, 0.0F, 0.0F};
}
NPH_INLINE NphVec3 nph_vec3_one(void)
{
    return (NphVec3){1.0F, 1.0F, 1.0F};
}
NPH_INLINE NphVec3 nph_vec3_unit_x(void)
{
    return (NphVec3){1.0F, 0.0F, 0.0F};
}
NPH_INLINE NphVec3 nph_vec3_unit_y(void)
{
    return (NphVec3){0.0F, 1.0F, 0.0F};
}
NPH_INLINE NphVec3 nph_vec3_unit_z(void)
{
    return (NphVec3){0.0F, 0.0F, 1.0F};
}
NPH_INLINE NphVec3 nph_vec3_from_vec2(NphVec2 v, f32 z)
{
    return (NphVec3){v.x, v.y, z};
}
NPH_INLINE NphVec2 nph_vec3_xy(NphVec3 v)
{
    return (NphVec2){v.x, v.y};
}
NPH_INLINE NphVec3 nph_vec3_from_array(const f32 *p)
{
    return (NphVec3){p[0], p[1], p[2]};
}
NPH_INLINE void nph_vec3_to_array(NphVec3 v, f32 *out)
{
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
// Component i (0, 1, 2) without pointer tricks.
NPH_INLINE f32 nph_vec3_at(NphVec3 v, u32 i)
{
    return i == 0U ? v.x : (i == 1U ? v.y : v.z);
}
NPH_INLINE NphVec3 nph_vec3_add(NphVec3 a, NphVec3 b)
{
    return (NphVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}
NPH_INLINE NphVec3 nph_vec3_sub(NphVec3 a, NphVec3 b)
{
    return (NphVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}
NPH_INLINE NphVec3 nph_vec3_mul(NphVec3 a, NphVec3 b)
{
    return (NphVec3){a.x * b.x, a.y * b.y, a.z * b.z};
}
NPH_INLINE NphVec3 nph_vec3_div(NphVec3 a, NphVec3 b)
{
    return (NphVec3){a.x / b.x, a.y / b.y, a.z / b.z};
}
NPH_INLINE NphVec3 nph_vec3_scale(NphVec3 v, f32 s)
{
    return (NphVec3){v.x * s, v.y * s, v.z * s};
}
// a + b * s, the fused form most code wants.
NPH_INLINE NphVec3 nph_vec3_mul_add(NphVec3 a, NphVec3 b, f32 s)
{
    return (NphVec3){a.x + b.x * s, a.y + b.y * s, a.z + b.z * s};
}
NPH_INLINE NphVec3 nph_vec3_neg(NphVec3 v)
{
    return (NphVec3){-v.x, -v.y, -v.z};
}
NPH_INLINE f32 nph_vec3_dot(NphVec3 a, NphVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
// Right-handed cross product: cross(x, y) == z.
NPH_INLINE NphVec3 nph_vec3_cross(NphVec3 a, NphVec3 b)
{
    return (NphVec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
NPH_INLINE f32 nph_vec3_length_sq(NphVec3 v)
{
    return nph_vec3_dot(v, v);
}
NPH_INLINE f32 nph_vec3_length(NphVec3 v)
{
    return nph_f32_sqrt(nph_vec3_length_sq(v));
}
NPH_INLINE f32 nph_vec3_distance_sq(NphVec3 a, NphVec3 b)
{
    return nph_vec3_length_sq(nph_vec3_sub(a, b));
}
NPH_INLINE f32 nph_vec3_distance(NphVec3 a, NphVec3 b)
{
    return nph_vec3_length(nph_vec3_sub(a, b));
}
// Unit vector in the direction of v. v must not be zero (asserted).
NPH_INLINE NphVec3 nph_vec3_normalize(NphVec3 v)
{
    const f32 len_sq = nph_vec3_length_sq(v);
    NPH_ASSERT_MSG(len_sq > 0.0F, "nph_vec3_normalize of a zero vector");
    return nph_vec3_scale(v, nph_f32_rsqrt(len_sq));
}
// As nph_vec3_normalize, but returns zero for a zero vector.
NPH_INLINE NphVec3 nph_vec3_normalize_or_zero(NphVec3 v)
{
    const f32 len_sq = nph_vec3_length_sq(v);
    return len_sq > 0.0F ? nph_vec3_scale(v, nph_f32_rsqrt(len_sq)) : nph_vec3_zero();
}
// As nph_vec3_normalize, but returns `fallback` for a zero vector.
NPH_INLINE NphVec3 nph_vec3_normalize_or(NphVec3 v, NphVec3 fallback)
{
    const f32 len_sq = nph_vec3_length_sq(v);
    return len_sq > 0.0F ? nph_vec3_scale(v, nph_f32_rsqrt(len_sq)) : fallback;
}
NPH_INLINE NphVec3 nph_vec3_lerp(NphVec3 a, NphVec3 b, f32 t)
{
    return (NphVec3){nph_f32_lerp(a.x, b.x, t), nph_f32_lerp(a.y, b.y, t),
                     nph_f32_lerp(a.z, b.z, t)};
}
NPH_INLINE NphVec3 nph_vec3_min(NphVec3 a, NphVec3 b)
{
    return (NphVec3){nph_f32_min(a.x, b.x), nph_f32_min(a.y, b.y), nph_f32_min(a.z, b.z)};
}
NPH_INLINE NphVec3 nph_vec3_max(NphVec3 a, NphVec3 b)
{
    return (NphVec3){nph_f32_max(a.x, b.x), nph_f32_max(a.y, b.y), nph_f32_max(a.z, b.z)};
}
NPH_INLINE NphVec3 nph_vec3_clamp(NphVec3 v, NphVec3 lo, NphVec3 hi)
{
    return nph_vec3_min(nph_vec3_max(v, lo), hi);
}
NPH_INLINE NphVec3 nph_vec3_abs(NphVec3 v)
{
    return (NphVec3){nph_f32_abs(v.x), nph_f32_abs(v.y), nph_f32_abs(v.z)};
}
NPH_INLINE NphVec3 nph_vec3_floor(NphVec3 v)
{
    return (NphVec3){nph_f32_floor(v.x), nph_f32_floor(v.y), nph_f32_floor(v.z)};
}
NPH_INLINE NphVec3 nph_vec3_ceil(NphVec3 v)
{
    return (NphVec3){nph_f32_ceil(v.x), nph_f32_ceil(v.y), nph_f32_ceil(v.z)};
}
NPH_INLINE NphVec3 nph_vec3_round(NphVec3 v)
{
    return (NphVec3){nph_f32_round(v.x), nph_f32_round(v.y), nph_f32_round(v.z)};
}
NPH_INLINE NphVec3 nph_vec3_sign(NphVec3 v)
{
    return (NphVec3){nph_f32_sign(v.x), nph_f32_sign(v.y), nph_f32_sign(v.z)};
}
// Component-wise reciprocal. Components must be non-zero.
NPH_INLINE NphVec3 nph_vec3_reciprocal(NphVec3 v)
{
    return (NphVec3){1.0F / v.x, 1.0F / v.y, 1.0F / v.z};
}
// Sum of the components.
NPH_INLINE f32 nph_vec3_hadd(NphVec3 v)
{
    return v.x + v.y + v.z;
}
NPH_INLINE f32 nph_vec3_min_component(NphVec3 v)
{
    return nph_f32_min3(v.x, v.y, v.z);
}
NPH_INLINE f32 nph_vec3_max_component(NphVec3 v)
{
    return nph_f32_max3(v.x, v.y, v.z);
}
// Index (0, 1, 2) of the largest component; ties go to the lowest index.
NPH_INLINE u32 nph_vec3_max_index(NphVec3 v)
{
    return v.x >= v.y ? (v.x >= v.z ? 0U : 2U) : (v.y >= v.z ? 1U : 2U);
}
// Index of the largest |component|.
NPH_INLINE u32 nph_vec3_max_abs_index(NphVec3 v)
{
    return nph_vec3_max_index(nph_vec3_abs(v));
}
// Projection of v onto the unit vector n.
NPH_INLINE NphVec3 nph_vec3_project(NphVec3 v, NphVec3 n)
{
    return nph_vec3_scale(n, nph_vec3_dot(v, n));
}
// Component of v perpendicular to the unit vector n.
NPH_INLINE NphVec3 nph_vec3_reject(NphVec3 v, NphVec3 n)
{
    return nph_vec3_sub(v, nph_vec3_project(v, n));
}
// Mirror of the incident direction v about the unit normal n.
NPH_INLINE NphVec3 nph_vec3_reflect(NphVec3 v, NphVec3 n)
{
    return nph_vec3_sub(v, nph_vec3_scale(n, 2.0F * nph_vec3_dot(v, n)));
}
// Refraction of the unit incident direction v through the unit normal n
// with relative index of refraction eta (n1 / n2), as GLSL refract().
// Returns zero on total internal reflection.
NPH_INLINE NphVec3 nph_vec3_refract(NphVec3 v, NphVec3 n, f32 eta)
{
    // cos_i = -n·v is the cosine of the incidence angle (v points toward the
    // surface); cos_t = sqrt(k) is the cosine of the transmitted angle.
    // t = eta v + (eta cos_i - cos_t) n
    const f32 cos_i = -nph_vec3_dot(n, v);
    const f32 k = 1.0F - eta * eta * (1.0F - cos_i * cos_i);
    if (k < 0.0F) {
        return nph_vec3_zero();
    }
    return nph_vec3_add(nph_vec3_scale(v, eta), nph_vec3_scale(n, eta * cos_i - nph_f32_sqrt(k)));
}
// Angle in radians between a and b (both non-zero), in [0, pi]. Uses
// atan2(|a x b|, a·b), which keeps full precision for nearly parallel and
// nearly opposite vectors where acos(a·b / |a||b|) loses it (Kahan).
NPH_INLINE f32 nph_vec3_angle(NphVec3 a, NphVec3 b)
{
    return nph_f32_atan2(nph_vec3_length(nph_vec3_cross(a, b)), nph_vec3_dot(a, b));
}
// Some unit vector perpendicular to the unit vector n. Deterministic: the
// result depends only on n.
NPH_INLINE NphVec3 nph_vec3_perpendicular(NphVec3 n)
{
    // Cross with the axis n is least aligned with (Hughes & Möller, "Building
    // an Orthonormal Basis from a Unit Vector", JGT 1999).
    const NphVec3 a = nph_vec3_abs(n);
    const NphVec3 axis = (a.x <= a.y && a.x <= a.z) ? nph_vec3_unit_x()
                         : (a.y <= a.z)             ? nph_vec3_unit_y()
                                                    : nph_vec3_unit_z();
    return nph_vec3_normalize(nph_vec3_cross(n, axis));
}
// Tangent and bitangent completing the unit normal n to a right-handed
// orthonormal basis (cross(t, b) == n), branch-free and continuous except
// at n.z == -1 (Duff et al., "Building an Orthonormal Basis, Revisited",
// JCGT 2017).
NPH_INLINE void nph_vec3_orthonormal_basis(NphVec3 n, NphVec3 *out_tangent, NphVec3 *out_bitangent)
{
    const f32 sign = nph_f32_copysign(1.0F, n.z);
    const f32 a = -1.0F / (sign + n.z);
    const f32 b = n.x * n.y * a;
    *out_tangent = nph_vec3(1.0F + sign * n.x * n.x * a, sign * b, -sign * n.x);
    *out_bitangent = nph_vec3(b, sign + n.y * n.y * a, -n.y);
}
NPH_INLINE bool nph_vec3_eq(NphVec3 a, NphVec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
NPH_INLINE bool nph_vec3_near(NphVec3 a, NphVec3 b, f32 tol)
{
    return nph_f32_near(a.x, b.x, tol) && nph_f32_near(a.y, b.y, tol) &&
           nph_f32_near(a.z, b.z, tol);
}
NPH_INLINE bool nph_vec3_is_finite(NphVec3 v)
{
    return nph_f32_is_finite(v.x) && nph_f32_is_finite(v.y) && nph_f32_is_finite(v.z);
}
// True when |length - 1| <= tol.
NPH_INLINE bool nph_vec3_is_normalized(NphVec3 v, f32 tol)
{
    return nph_f32_abs(nph_vec3_length_sq(v) - 1.0F) <= 2.0F * tol;
}

// ---------------------------------------------------------------------------
// NphVec4
// ---------------------------------------------------------------------------

NPH_INLINE NphVec4 nph_vec4(f32 x, f32 y, f32 z, f32 w)
{
    return (NphVec4){x, y, z, w};
}
NPH_INLINE NphVec4 nph_vec4_splat(f32 s)
{
    return (NphVec4){s, s, s, s};
}
NPH_INLINE NphVec4 nph_vec4_zero(void)
{
    return (NphVec4){0.0F, 0.0F, 0.0F, 0.0F};
}
NPH_INLINE NphVec4 nph_vec4_one(void)
{
    return (NphVec4){1.0F, 1.0F, 1.0F, 1.0F};
}
NPH_INLINE NphVec4 nph_vec4_from_vec3(NphVec3 v, f32 w)
{
    return (NphVec4){v.x, v.y, v.z, w};
}
// Point (w = 1) and direction (w = 0) constructors for homogeneous math.
NPH_INLINE NphVec4 nph_vec4_point(NphVec3 p)
{
    return (NphVec4){p.x, p.y, p.z, 1.0F};
}
NPH_INLINE NphVec4 nph_vec4_direction(NphVec3 d)
{
    return (NphVec4){d.x, d.y, d.z, 0.0F};
}
NPH_INLINE NphVec3 nph_vec4_xyz(NphVec4 v)
{
    return (NphVec3){v.x, v.y, v.z};
}
NPH_INLINE NphVec4 nph_vec4_from_array(const f32 *p)
{
    return (NphVec4){p[0], p[1], p[2], p[3]};
}
NPH_INLINE void nph_vec4_to_array(NphVec4 v, f32 *out)
{
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = v.w;
}
NPH_INLINE f32 nph_vec4_at(NphVec4 v, u32 i)
{
    return i == 0U ? v.x : (i == 1U ? v.y : (i == 2U ? v.z : v.w));
}
NPH_INLINE NphVec4 nph_vec4_add(NphVec4 a, NphVec4 b)
{
    return (NphVec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
NPH_INLINE NphVec4 nph_vec4_sub(NphVec4 a, NphVec4 b)
{
    return (NphVec4){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
NPH_INLINE NphVec4 nph_vec4_mul(NphVec4 a, NphVec4 b)
{
    return (NphVec4){a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
NPH_INLINE NphVec4 nph_vec4_div(NphVec4 a, NphVec4 b)
{
    return (NphVec4){a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}
NPH_INLINE NphVec4 nph_vec4_scale(NphVec4 v, f32 s)
{
    return (NphVec4){v.x * s, v.y * s, v.z * s, v.w * s};
}
NPH_INLINE NphVec4 nph_vec4_mul_add(NphVec4 a, NphVec4 b, f32 s)
{
    return (NphVec4){a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w + b.w * s};
}
NPH_INLINE NphVec4 nph_vec4_neg(NphVec4 v)
{
    return (NphVec4){-v.x, -v.y, -v.z, -v.w};
}
NPH_INLINE f32 nph_vec4_dot(NphVec4 a, NphVec4 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
// Dot of the xyz parts only (plane and homogeneous arithmetic).
NPH_INLINE f32 nph_vec4_dot3(NphVec4 a, NphVec4 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
NPH_INLINE f32 nph_vec4_length_sq(NphVec4 v)
{
    return nph_vec4_dot(v, v);
}
NPH_INLINE f32 nph_vec4_length(NphVec4 v)
{
    return nph_f32_sqrt(nph_vec4_length_sq(v));
}
// Unit vector in the direction of v. v must not be zero (asserted).
NPH_INLINE NphVec4 nph_vec4_normalize(NphVec4 v)
{
    const f32 len_sq = nph_vec4_length_sq(v);
    NPH_ASSERT_MSG(len_sq > 0.0F, "nph_vec4_normalize of a zero vector");
    return nph_vec4_scale(v, nph_f32_rsqrt(len_sq));
}
NPH_INLINE NphVec4 nph_vec4_normalize_or_zero(NphVec4 v)
{
    const f32 len_sq = nph_vec4_length_sq(v);
    return len_sq > 0.0F ? nph_vec4_scale(v, nph_f32_rsqrt(len_sq)) : nph_vec4_zero();
}
NPH_INLINE NphVec4 nph_vec4_lerp(NphVec4 a, NphVec4 b, f32 t)
{
    return (NphVec4){nph_f32_lerp(a.x, b.x, t), nph_f32_lerp(a.y, b.y, t),
                     nph_f32_lerp(a.z, b.z, t), nph_f32_lerp(a.w, b.w, t)};
}
NPH_INLINE NphVec4 nph_vec4_min(NphVec4 a, NphVec4 b)
{
    return (NphVec4){nph_f32_min(a.x, b.x), nph_f32_min(a.y, b.y), nph_f32_min(a.z, b.z),
                     nph_f32_min(a.w, b.w)};
}
NPH_INLINE NphVec4 nph_vec4_max(NphVec4 a, NphVec4 b)
{
    return (NphVec4){nph_f32_max(a.x, b.x), nph_f32_max(a.y, b.y), nph_f32_max(a.z, b.z),
                     nph_f32_max(a.w, b.w)};
}
NPH_INLINE NphVec4 nph_vec4_clamp(NphVec4 v, NphVec4 lo, NphVec4 hi)
{
    return nph_vec4_min(nph_vec4_max(v, lo), hi);
}
NPH_INLINE NphVec4 nph_vec4_abs(NphVec4 v)
{
    return (NphVec4){nph_f32_abs(v.x), nph_f32_abs(v.y), nph_f32_abs(v.z), nph_f32_abs(v.w)};
}
NPH_INLINE f32 nph_vec4_hadd(NphVec4 v)
{
    return v.x + v.y + v.z + v.w;
}
NPH_INLINE f32 nph_vec4_min_component(NphVec4 v)
{
    return nph_f32_min(nph_f32_min(v.x, v.y), nph_f32_min(v.z, v.w));
}
NPH_INLINE f32 nph_vec4_max_component(NphVec4 v)
{
    return nph_f32_max(nph_f32_max(v.x, v.y), nph_f32_max(v.z, v.w));
}
// Perspective divide: xyz / w. w must be non-zero.
NPH_INLINE NphVec3 nph_vec4_project(NphVec4 v)
{
    const f32 inv_w = 1.0F / v.w;
    return (NphVec3){v.x * inv_w, v.y * inv_w, v.z * inv_w};
}
NPH_INLINE bool nph_vec4_eq(NphVec4 a, NphVec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
NPH_INLINE bool nph_vec4_near(NphVec4 a, NphVec4 b, f32 tol)
{
    return nph_f32_near(a.x, b.x, tol) && nph_f32_near(a.y, b.y, tol) &&
           nph_f32_near(a.z, b.z, tol) && nph_f32_near(a.w, b.w, tol);
}
NPH_INLINE bool nph_vec4_is_finite(NphVec4 v)
{
    return nph_f32_is_finite(v.x) && nph_f32_is_finite(v.y) && nph_f32_is_finite(v.z) &&
           nph_f32_is_finite(v.w);
}

// ---------------------------------------------------------------------------
// Integer vectors: texture sizes, tile coordinates, cell indices
// ---------------------------------------------------------------------------

NPH_INLINE NphIVec2 nph_ivec2(i32 x, i32 y)
{
    return (NphIVec2){x, y};
}
NPH_INLINE NphIVec2 nph_ivec2_add(NphIVec2 a, NphIVec2 b)
{
    return (NphIVec2){a.x + b.x, a.y + b.y};
}
NPH_INLINE NphIVec2 nph_ivec2_sub(NphIVec2 a, NphIVec2 b)
{
    return (NphIVec2){a.x - b.x, a.y - b.y};
}
NPH_INLINE NphIVec2 nph_ivec2_mul(NphIVec2 a, NphIVec2 b)
{
    return (NphIVec2){a.x * b.x, a.y * b.y};
}
NPH_INLINE NphIVec2 nph_ivec2_scale(NphIVec2 v, i32 s)
{
    return (NphIVec2){v.x * s, v.y * s};
}
NPH_INLINE NphIVec2 nph_ivec2_neg(NphIVec2 v)
{
    return (NphIVec2){-v.x, -v.y};
}
NPH_INLINE NphIVec2 nph_ivec2_min(NphIVec2 a, NphIVec2 b)
{
    return (NphIVec2){nph_min_i32(a.x, b.x), nph_min_i32(a.y, b.y)};
}
NPH_INLINE NphIVec2 nph_ivec2_max(NphIVec2 a, NphIVec2 b)
{
    return (NphIVec2){nph_max_i32(a.x, b.x), nph_max_i32(a.y, b.y)};
}
NPH_INLINE bool nph_ivec2_eq(NphIVec2 a, NphIVec2 b)
{
    return a.x == b.x && a.y == b.y;
}
NPH_INLINE NphVec2 nph_vec2_from_ivec2(NphIVec2 v)
{
    return (NphVec2){(f32)v.x, (f32)v.y};
}
// Truncates toward zero (C cast semantics).
NPH_INLINE NphIVec2 nph_ivec2_from_vec2_trunc(NphVec2 v)
{
    return (NphIVec2){(i32)v.x, (i32)v.y};
}
NPH_INLINE NphIVec2 nph_ivec2_from_vec2_floor(NphVec2 v)
{
    return (NphIVec2){(i32)nph_f32_floor(v.x), (i32)nph_f32_floor(v.y)};
}

NPH_INLINE NphIVec3 nph_ivec3(i32 x, i32 y, i32 z)
{
    return (NphIVec3){x, y, z};
}
NPH_INLINE NphIVec3 nph_ivec3_add(NphIVec3 a, NphIVec3 b)
{
    return (NphIVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}
NPH_INLINE NphIVec3 nph_ivec3_sub(NphIVec3 a, NphIVec3 b)
{
    return (NphIVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}
NPH_INLINE NphIVec3 nph_ivec3_mul(NphIVec3 a, NphIVec3 b)
{
    return (NphIVec3){a.x * b.x, a.y * b.y, a.z * b.z};
}
NPH_INLINE NphIVec3 nph_ivec3_scale(NphIVec3 v, i32 s)
{
    return (NphIVec3){v.x * s, v.y * s, v.z * s};
}
NPH_INLINE NphIVec3 nph_ivec3_neg(NphIVec3 v)
{
    return (NphIVec3){-v.x, -v.y, -v.z};
}
NPH_INLINE NphIVec3 nph_ivec3_min(NphIVec3 a, NphIVec3 b)
{
    return (NphIVec3){nph_min_i32(a.x, b.x), nph_min_i32(a.y, b.y), nph_min_i32(a.z, b.z)};
}
NPH_INLINE NphIVec3 nph_ivec3_max(NphIVec3 a, NphIVec3 b)
{
    return (NphIVec3){nph_max_i32(a.x, b.x), nph_max_i32(a.y, b.y), nph_max_i32(a.z, b.z)};
}
NPH_INLINE i32 nph_ivec3_dot(NphIVec3 a, NphIVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
NPH_INLINE bool nph_ivec3_eq(NphIVec3 a, NphIVec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
NPH_INLINE NphVec3 nph_vec3_from_ivec3(NphIVec3 v)
{
    return (NphVec3){(f32)v.x, (f32)v.y, (f32)v.z};
}
NPH_INLINE NphIVec3 nph_ivec3_from_vec3_trunc(NphVec3 v)
{
    return (NphIVec3){(i32)v.x, (i32)v.y, (i32)v.z};
}
NPH_INLINE NphIVec3 nph_ivec3_from_vec3_floor(NphVec3 v)
{
    return (NphIVec3){(i32)nph_f32_floor(v.x), (i32)nph_f32_floor(v.y), (i32)nph_f32_floor(v.z)};
}

NPH_INLINE NphIVec4 nph_ivec4(i32 x, i32 y, i32 z, i32 w)
{
    return (NphIVec4){x, y, z, w};
}
NPH_INLINE NphIVec4 nph_ivec4_add(NphIVec4 a, NphIVec4 b)
{
    return (NphIVec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
NPH_INLINE NphIVec4 nph_ivec4_sub(NphIVec4 a, NphIVec4 b)
{
    return (NphIVec4){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
NPH_INLINE bool nph_ivec4_eq(NphIVec4 a, NphIVec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

NPH_INLINE NphUVec2 nph_uvec2(u32 x, u32 y)
{
    return (NphUVec2){x, y};
}
NPH_INLINE NphUVec2 nph_uvec2_add(NphUVec2 a, NphUVec2 b)
{
    return (NphUVec2){a.x + b.x, a.y + b.y};
}
NPH_INLINE NphUVec2 nph_uvec2_sub(NphUVec2 a, NphUVec2 b)
{
    return (NphUVec2){a.x - b.x, a.y - b.y};
}
NPH_INLINE NphUVec2 nph_uvec2_mul(NphUVec2 a, NphUVec2 b)
{
    return (NphUVec2){a.x * b.x, a.y * b.y};
}
NPH_INLINE NphUVec2 nph_uvec2_min(NphUVec2 a, NphUVec2 b)
{
    return (NphUVec2){nph_min_u32(a.x, b.x), nph_min_u32(a.y, b.y)};
}
NPH_INLINE NphUVec2 nph_uvec2_max(NphUVec2 a, NphUVec2 b)
{
    return (NphUVec2){nph_max_u32(a.x, b.x), nph_max_u32(a.y, b.y)};
}
NPH_INLINE bool nph_uvec2_eq(NphUVec2 a, NphUVec2 b)
{
    return a.x == b.x && a.y == b.y;
}
NPH_INLINE NphVec2 nph_vec2_from_uvec2(NphUVec2 v)
{
    return (NphVec2){(f32)v.x, (f32)v.y};
}

NPH_INLINE NphUVec3 nph_uvec3(u32 x, u32 y, u32 z)
{
    return (NphUVec3){x, y, z};
}
NPH_INLINE NphUVec3 nph_uvec3_add(NphUVec3 a, NphUVec3 b)
{
    return (NphUVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}
NPH_INLINE NphUVec3 nph_uvec3_sub(NphUVec3 a, NphUVec3 b)
{
    return (NphUVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}
NPH_INLINE NphUVec3 nph_uvec3_mul(NphUVec3 a, NphUVec3 b)
{
    return (NphUVec3){a.x * b.x, a.y * b.y, a.z * b.z};
}
NPH_INLINE NphUVec3 nph_uvec3_min(NphUVec3 a, NphUVec3 b)
{
    return (NphUVec3){nph_min_u32(a.x, b.x), nph_min_u32(a.y, b.y), nph_min_u32(a.z, b.z)};
}
NPH_INLINE NphUVec3 nph_uvec3_max(NphUVec3 a, NphUVec3 b)
{
    return (NphUVec3){nph_max_u32(a.x, b.x), nph_max_u32(a.y, b.y), nph_max_u32(a.z, b.z)};
}
NPH_INLINE bool nph_uvec3_eq(NphUVec3 a, NphUVec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
NPH_INLINE NphVec3 nph_vec3_from_uvec3(NphUVec3 v)
{
    return (NphVec3){(f32)v.x, (f32)v.y, (f32)v.z};
}

NPH_INLINE NphUVec4 nph_uvec4(u32 x, u32 y, u32 z, u32 w)
{
    return (NphUVec4){x, y, z, w};
}
NPH_INLINE NphUVec4 nph_uvec4_add(NphUVec4 a, NphUVec4 b)
{
    return (NphUVec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
NPH_INLINE bool nph_uvec4_eq(NphUVec4 a, NphUVec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// ---------------------------------------------------------------------------
// NphDVec3: 64-bit world positions (ADR-0008 §8)
// ---------------------------------------------------------------------------

NPH_INLINE NphDVec3 nph_dvec3(f64 x, f64 y, f64 z)
{
    return (NphDVec3){x, y, z};
}
NPH_INLINE NphDVec3 nph_dvec3_zero(void)
{
    return (NphDVec3){0.0, 0.0, 0.0};
}
NPH_INLINE NphDVec3 nph_dvec3_from_vec3(NphVec3 v)
{
    return (NphDVec3){(f64)v.x, (f64)v.y, (f64)v.z};
}
// Rounds each component to the nearest f32. Only meaningful for values
// already near the origin; use nph_dvec3_relative for world positions.
NPH_INLINE NphVec3 nph_dvec3_to_vec3(NphDVec3 v)
{
    return (NphVec3){(f32)v.x, (f32)v.y, (f32)v.z};
}
NPH_INLINE NphDVec3 nph_dvec3_add(NphDVec3 a, NphDVec3 b)
{
    return (NphDVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}
NPH_INLINE NphDVec3 nph_dvec3_sub(NphDVec3 a, NphDVec3 b)
{
    return (NphDVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}
NPH_INLINE NphDVec3 nph_dvec3_mul(NphDVec3 a, NphDVec3 b)
{
    return (NphDVec3){a.x * b.x, a.y * b.y, a.z * b.z};
}
NPH_INLINE NphDVec3 nph_dvec3_scale(NphDVec3 v, f64 s)
{
    return (NphDVec3){v.x * s, v.y * s, v.z * s};
}
// a + b * s with b in single precision: the common "world position plus
// local offset" update.
NPH_INLINE NphDVec3 nph_dvec3_add_vec3(NphDVec3 a, NphVec3 b)
{
    return (NphDVec3){a.x + (f64)b.x, a.y + (f64)b.y, a.z + (f64)b.z};
}
NPH_INLINE NphDVec3 nph_dvec3_neg(NphDVec3 v)
{
    return (NphDVec3){-v.x, -v.y, -v.z};
}
NPH_INLINE f64 nph_dvec3_dot(NphDVec3 a, NphDVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
NPH_INLINE NphDVec3 nph_dvec3_cross(NphDVec3 a, NphDVec3 b)
{
    return (NphDVec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
NPH_INLINE f64 nph_dvec3_length_sq(NphDVec3 v)
{
    return nph_dvec3_dot(v, v);
}
NPH_INLINE f64 nph_dvec3_length(NphDVec3 v)
{
    return nph_f64_sqrt(nph_dvec3_length_sq(v));
}
NPH_INLINE f64 nph_dvec3_distance(NphDVec3 a, NphDVec3 b)
{
    return nph_dvec3_length(nph_dvec3_sub(a, b));
}
NPH_INLINE NphDVec3 nph_dvec3_normalize_or_zero(NphDVec3 v)
{
    const f64 len_sq = nph_dvec3_length_sq(v);
    return len_sq > 0.0 ? nph_dvec3_scale(v, 1.0 / nph_f64_sqrt(len_sq)) : nph_dvec3_zero();
}
NPH_INLINE NphDVec3 nph_dvec3_lerp(NphDVec3 a, NphDVec3 b, f64 t)
{
    return (NphDVec3){nph_f64_lerp(a.x, b.x, t), nph_f64_lerp(a.y, b.y, t),
                      nph_f64_lerp(a.z, b.z, t)};
}
NPH_INLINE NphDVec3 nph_dvec3_min(NphDVec3 a, NphDVec3 b)
{
    return (NphDVec3){nph_f64_min(a.x, b.x), nph_f64_min(a.y, b.y), nph_f64_min(a.z, b.z)};
}
NPH_INLINE NphDVec3 nph_dvec3_max(NphDVec3 a, NphDVec3 b)
{
    return (NphDVec3){nph_f64_max(a.x, b.x), nph_f64_max(a.y, b.y), nph_f64_max(a.z, b.z)};
}
NPH_INLINE bool nph_dvec3_eq(NphDVec3 a, NphDVec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
NPH_INLINE bool nph_dvec3_near(NphDVec3 a, NphDVec3 b, f64 tol)
{
    return nph_f64_near(a.x, b.x, tol) && nph_f64_near(a.y, b.y, tol) &&
           nph_f64_near(a.z, b.z, tol);
}
// The single-precision position of `p` relative to `origin` (typically
// the camera): the subtraction happens in f64, so the result carries full
// f32 precision around `origin` no matter how far both are from the world
// origin. This is the only way world positions reach the GPU (ADR-0008 §8).
NPH_INLINE NphVec3 nph_dvec3_relative(NphDVec3 p, NphDVec3 origin)
{
    return nph_dvec3_to_vec3(nph_dvec3_sub(p, origin));
}

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scalar floating-point helpers and constants (CLAUDE.md §8.2). Thin,
// inline wrappers over <math.h> with the engine's naming, plus the
// comparisons (ULP distance, tolerances) that the property tests and the
// higher-level types are built on.
//
// Conventions (docs/adr/0008-math-conventions.md): angles are radians,
// f32 is the working precision, f64 is used only for world positions and
// for reference computations. Float literals carry the F suffix throughout
// so no expression is silently promoted to double.
//
// Thread-safety: every function here is pure and NPH_THREADSAFE.

#include <nepheline/core/assert.h>
#include <nepheline/core/base.h>

#include <float.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define NPH_PI         3.14159265358979323846F
#define NPH_TAU        6.28318530717958647692F
#define NPH_HALF_PI    1.57079632679489661923F
#define NPH_QUARTER_PI 0.78539816339744830962F
#define NPH_INV_PI     0.31830988618379067154F
#define NPH_INV_TAU    0.15915494309189533577F
#define NPH_SQRT2      1.41421356237309504880F
#define NPH_INV_SQRT2  0.70710678118654752440F
#define NPH_SQRT3      1.73205080756887729353F
#define NPH_E          2.71828182845904523536F
#define NPH_LN2        0.69314718055994530942F
#define NPH_DEG_TO_RAD (NPH_PI / 180.0F)
#define NPH_RAD_TO_DEG (180.0F / NPH_PI)

#define NPH_F64_PI         3.14159265358979323846
#define NPH_F64_TAU        6.28318530717958647692
#define NPH_F64_DEG_TO_RAD (NPH_F64_PI / 180.0)
#define NPH_F64_RAD_TO_DEG (180.0 / NPH_F64_PI)

// Smallest value with 1.0F + NPH_F32_EPSILON != 1.0F.
#define NPH_F32_EPSILON    FLT_EPSILON
#define NPH_F32_MIN_NORMAL FLT_MIN
#define NPH_F32_MAX        FLT_MAX
#define NPH_F32_INFINITY   HUGE_VALF
#define NPH_F64_EPSILON    DBL_EPSILON
#define NPH_F64_MAX        DBL_MAX
#define NPH_F64_INFINITY   HUGE_VAL

// ---------------------------------------------------------------------------
// f32
// ---------------------------------------------------------------------------

NPH_INLINE f32 nph_f32_abs(f32 x)
{
    return fabsf(x);
}
NPH_INLINE f32 nph_f32_min(f32 a, f32 b)
{
    return a < b ? a : b;
}
NPH_INLINE f32 nph_f32_max(f32 a, f32 b)
{
    return a > b ? a : b;
}
NPH_INLINE f32 nph_f32_min3(f32 a, f32 b, f32 c)
{
    return nph_f32_min(a, nph_f32_min(b, c));
}
NPH_INLINE f32 nph_f32_max3(f32 a, f32 b, f32 c)
{
    return nph_f32_max(a, nph_f32_max(b, c));
}
NPH_INLINE f32 nph_f32_clamp(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
// Clamps to [0, 1].
NPH_INLINE f32 nph_f32_saturate(f32 v)
{
    return nph_f32_clamp(v, 0.0F, 1.0F);
}
NPH_INLINE f32 nph_f32_square(f32 x)
{
    return x * x;
}
// -1, 0, or +1 (0 for both zeros and for NaN).
NPH_INLINE f32 nph_f32_sign(f32 x)
{
    return x > 0.0F ? 1.0F : (x < 0.0F ? -1.0F : 0.0F);
}
NPH_INLINE f32 nph_f32_copysign(f32 magnitude, f32 sign)
{
    return copysignf(magnitude, sign);
}

// Linear interpolation written so that t == 0 returns exactly a and t == 1
// returns exactly b (the a + (b - a) * t form does not).
NPH_INLINE f32 nph_f32_lerp(f32 a, f32 b, f32 t)
{
    return (1.0F - t) * a + t * b;
}
// The t for which lerp(a, b, t) == v. a != b.
NPH_INLINE f32 nph_f32_inverse_lerp(f32 a, f32 b, f32 v)
{
    return (v - a) / (b - a);
}
// Maps v from [in_lo, in_hi] to [out_lo, out_hi] without clamping.
NPH_INLINE f32 nph_f32_remap(f32 v, f32 in_lo, f32 in_hi, f32 out_lo, f32 out_hi)
{
    return nph_f32_lerp(out_lo, out_hi, nph_f32_inverse_lerp(in_lo, in_hi, v));
}
// 0 below e0, 1 above e1, Hermite in between.
NPH_INLINE f32 nph_f32_smoothstep(f32 e0, f32 e1, f32 x)
{
    const f32 t = nph_f32_saturate((x - e0) / (e1 - e0));
    return t * t * (3.0F - 2.0F * t);
}
NPH_INLINE f32 nph_f32_step(f32 edge, f32 x)
{
    return x < edge ? 0.0F : 1.0F;
}

NPH_INLINE f32 nph_f32_floor(f32 x)
{
    return floorf(x);
}
NPH_INLINE f32 nph_f32_ceil(f32 x)
{
    return ceilf(x);
}
// Rounds half away from zero.
NPH_INLINE f32 nph_f32_round(f32 x)
{
    return roundf(x);
}
// Rounds half to even (the default IEEE rounding, what the GPU does).
NPH_INLINE f32 nph_f32_round_even(f32 x)
{
    return rintf(x);
}
NPH_INLINE f32 nph_f32_trunc(f32 x)
{
    return truncf(x);
}
// x - floor(x), in [0, 1).
NPH_INLINE f32 nph_f32_fract(f32 x)
{
    return x - floorf(x);
}
// C remainder: sign follows x.
NPH_INLINE f32 nph_f32_mod(f32 x, f32 y)
{
    return fmodf(x, y);
}
// Wraps x into [lo, hi) periodically. hi > lo.
NPH_INLINE f32 nph_f32_wrap(f32 x, f32 lo, f32 hi)
{
    const f32 range = hi - lo;
    return x - range * floorf((x - lo) / range);
}
// Wraps an angle into [-pi, pi).
NPH_INLINE f32 nph_f32_wrap_angle(f32 radians)
{
    return nph_f32_wrap(radians, -NPH_PI, NPH_PI);
}

NPH_INLINE f32 nph_f32_sqrt(f32 x)
{
    return sqrtf(x);
}
// 1 / sqrt(x), correctly rounded in two operations. The approximate
// hardware instruction is deliberately not used (ADR-0008 §6).
NPH_INLINE f32 nph_f32_rsqrt(f32 x)
{
    return 1.0F / sqrtf(x);
}
NPH_INLINE f32 nph_f32_pow(f32 base, f32 exponent)
{
    return powf(base, exponent);
}
NPH_INLINE f32 nph_f32_exp(f32 x)
{
    return expf(x);
}
NPH_INLINE f32 nph_f32_exp2(f32 x)
{
    return exp2f(x);
}
NPH_INLINE f32 nph_f32_log(f32 x)
{
    return logf(x);
}
NPH_INLINE f32 nph_f32_log2(f32 x)
{
    return log2f(x);
}

NPH_INLINE f32 nph_f32_sin(f32 x)
{
    return sinf(x);
}
NPH_INLINE f32 nph_f32_cos(f32 x)
{
    return cosf(x);
}
NPH_INLINE f32 nph_f32_tan(f32 x)
{
    return tanf(x);
}
NPH_INLINE void nph_f32_sincos(f32 x, f32 *out_sin, f32 *out_cos)
{
    *out_sin = sinf(x);
    *out_cos = cosf(x);
}
// asin/acos with the argument clamped to [-1, 1], so rounding noise in a
// dot product never produces NaN.
NPH_INLINE f32 nph_f32_asin(f32 x)
{
    return asinf(nph_f32_clamp(x, -1.0F, 1.0F));
}
NPH_INLINE f32 nph_f32_acos(f32 x)
{
    return acosf(nph_f32_clamp(x, -1.0F, 1.0F));
}
NPH_INLINE f32 nph_f32_atan(f32 x)
{
    return atanf(x);
}
NPH_INLINE f32 nph_f32_atan2(f32 y, f32 x)
{
    return atan2f(y, x);
}
NPH_INLINE f32 nph_f32_radians(f32 degrees)
{
    return degrees * NPH_DEG_TO_RAD;
}
NPH_INLINE f32 nph_f32_degrees(f32 radians)
{
    return radians * NPH_RAD_TO_DEG;
}

NPH_INLINE bool nph_f32_is_nan(f32 x)
{
    return isnan(x) != 0;
}
NPH_INLINE bool nph_f32_is_finite(f32 x)
{
    return isfinite(x) != 0;
}
NPH_INLINE bool nph_f32_is_inf(f32 x)
{
    return isinf(x) != 0;
}

// Bit pattern access without aliasing violations.
NPH_INLINE u32 nph_f32_to_bits(f32 x)
{
    u32 bits;
    memcpy(&bits, &x, sizeof bits);
    return bits;
}
NPH_INLINE f32 nph_f32_from_bits(u32 bits)
{
    f32 x;
    memcpy(&x, &bits, sizeof x);
    return x;
}

// Distance between a and b in units in the last place: the number of
// representable floats strictly between them plus one. +0 and -0 are 0
// apart; values of opposite sign count across zero (Dawson, "Comparing
// Floating Point Numbers", 2012). NPH_U32_MAX if either is NaN.
NPH_INLINE u32 nph_f32_ulp_distance(f32 a, f32 b)
{
    if (nph_f32_is_nan(a) || nph_f32_is_nan(b)) {
        return NPH_U32_MAX;
    }
    const u32 ba = nph_f32_to_bits(a);
    const u32 bb = nph_f32_to_bits(b);
    // Map sign-magnitude onto a monotonic signed line.
    const i64 la = (ba & 0x80000000U) != 0U ? -(i64)(ba & 0x7FFFFFFFU) : (i64)ba;
    const i64 lb = (bb & 0x80000000U) != 0U ? -(i64)(bb & 0x7FFFFFFFU) : (i64)bb;
    const i64 d = la > lb ? la - lb : lb - la;
    return d > (i64)NPH_U32_MAX ? NPH_U32_MAX : (u32)d;
}

// |a - b| <= abs_tol, or |a - b| <= rel_tol * max(|a|, |b|). Equal
// infinities compare equal; NaN never does.
NPH_INLINE bool nph_f32_approx_eq(f32 a, f32 b, f32 abs_tol, f32 rel_tol)
{
    if (a == b) {
        return true;
    }
    const f32 diff = nph_f32_abs(a - b);
    if (diff <= abs_tol) {
        return true;
    }
    return diff <= rel_tol * nph_f32_max(nph_f32_abs(a), nph_f32_abs(b));
}
// |a - b| <= tol.
NPH_INLINE bool nph_f32_near(f32 a, f32 b, f32 tol)
{
    return a == b || nph_f32_abs(a - b) <= tol;
}
NPH_INLINE bool nph_f32_near_zero(f32 x, f32 tol)
{
    return nph_f32_abs(x) <= tol;
}

// ---------------------------------------------------------------------------
// f64 (world positions, reference computations)
// ---------------------------------------------------------------------------

NPH_INLINE f64 nph_f64_abs(f64 x)
{
    return fabs(x);
}
NPH_INLINE f64 nph_f64_min(f64 a, f64 b)
{
    return a < b ? a : b;
}
NPH_INLINE f64 nph_f64_max(f64 a, f64 b)
{
    return a > b ? a : b;
}
NPH_INLINE f64 nph_f64_clamp(f64 v, f64 lo, f64 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
NPH_INLINE f64 nph_f64_lerp(f64 a, f64 b, f64 t)
{
    return (1.0 - t) * a + t * b;
}
NPH_INLINE f64 nph_f64_sqrt(f64 x)
{
    return sqrt(x);
}
NPH_INLINE f64 nph_f64_floor(f64 x)
{
    return floor(x);
}
NPH_INLINE f64 nph_f64_ceil(f64 x)
{
    return ceil(x);
}
NPH_INLINE f64 nph_f64_round(f64 x)
{
    return round(x);
}
NPH_INLINE f64 nph_f64_sin(f64 x)
{
    return sin(x);
}
NPH_INLINE f64 nph_f64_cos(f64 x)
{
    return cos(x);
}
NPH_INLINE f64 nph_f64_atan2(f64 y, f64 x)
{
    return atan2(y, x);
}
NPH_INLINE bool nph_f64_is_finite(f64 x)
{
    return isfinite(x) != 0;
}
NPH_INLINE bool nph_f64_near(f64 a, f64 b, f64 tol)
{
    return a == b || fabs(a - b) <= tol;
}
NPH_INLINE u64 nph_f64_to_bits(f64 x)
{
    u64 bits;
    memcpy(&bits, &x, sizeof bits);
    return bits;
}
NPH_INLINE f64 nph_f64_from_bits(u64 bits)
{
    f64 x;
    memcpy(&x, &bits, sizeof x);
    return x;
}

// ---------------------------------------------------------------------------
// Integers (the pieces <math.h> does not cover and C's `/` gets wrong)
// ---------------------------------------------------------------------------

NPH_INLINE i32 nph_i32_abs(i32 x)
{
    return x < 0 ? -x : x;
}
NPH_INLINE i32 nph_i32_sign(i32 x)
{
    return x > 0 ? 1 : (x < 0 ? -1 : 0);
}
// Division rounding toward negative infinity. b != 0.
NPH_INLINE i32 nph_i32_div_floor(i32 a, i32 b)
{
    const i32 q = a / b;
    const i32 r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
// Remainder with the sign of the divisor, so the result is in [0, b) for
// b > 0 regardless of the sign of a. b != 0.
NPH_INLINE i32 nph_i32_mod_floor(i32 a, i32 b)
{
    const i32 r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}
// ceil(a / b) for b > 0 without overflow for a + b - 1.
NPH_INLINE u32 nph_u32_div_ceil(u32 a, u32 b)
{
    return a == 0U ? 0U : 1U + (a - 1U) / b;
}
NPH_INLINE u64 nph_u64_div_ceil(u64 a, u64 b)
{
    return a == 0U ? 0U : 1U + (a - 1U) / b;
}

#ifdef __cplusplus
}
#endif

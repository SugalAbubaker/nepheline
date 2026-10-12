// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/pack.h>

// ---------------------------------------------------------------------------
// binary16
// ---------------------------------------------------------------------------

// Rounds the magnitude bits of a binary32 (sign cleared) to an IEEE-style
// float with a 5-bit exponent (bias 15) and `mant_bits` mantissa bits, to
// nearest even, in a single rounding step: subnormals, overflow to
// infinity, infinity, and NaN (quietened, top payload bits kept) included.
// Shared by binary16 (10 bits) and the 6- and 5-bit components of
// R11G11B10F; converting through binary16 first would round twice.
static u32 f32_to_small_float(u32 f, u32 mant_bits)
{
    const u32 drop = 23U - mant_bits;
    const u32 exp = (f >> 23U) & 0xFFU;
    const u32 mant = f & 0x7FFFFFU;
    const u32 inf = 0x1FU << mant_bits;

    if (exp == 0xFFU) {
        return mant == 0U ? inf : inf | (1U << (mant_bits - 1U)) | (mant >> drop);
    }
    // Rebias from 127 to 15. e >= 31 overflows to infinity; e <= 0 is a
    // subnormal of the target or zero.
    const i32 e = (i32)exp - 112;
    if (e >= 31) {
        return inf;
    }
    if (e <= 0) {
        // The value is (mant | 2^23) * 2^(e - 38) and the target's subnormal
        // unit is 2^(-14 - mant_bits), so the mantissa is the full
        // significand shifted right by drop + 1 - e, rounded to nearest even.
        const i32 shift = (i32)drop + 1 - e;
        if (shift > 24) {
            return 0U; // below half the smallest subnormal
        }
        const u32 full = mant | 0x800000U;
        const u32 half = 1U << (shift - 1);
        const u32 rem = full & ((1U << shift) - 1U);
        u32 m = full >> shift;
        if (rem > half || (rem == half && (m & 1U) != 0U)) {
            m += 1U; // may carry into the smallest normal, which is correct
        }
        return m;
    }
    // Normal: keep the top mant_bits, round the dropped bits to nearest
    // even. A carry out of the mantissa bumps the exponent and, for e == 30,
    // produces infinity, which is the correct overflow behaviour.
    u32 r = ((u32)e << mant_bits) | (mant >> drop);
    const u32 half = 1U << (drop - 1U);
    const u32 rem = mant & ((1U << drop) - 1U);
    if (rem > half || (rem == half && (r & 1U) != 0U)) {
        r += 1U;
    }
    return r;
}

u16 nph_f32_to_f16(f32 x)
{
    const u32 f = nph_f32_to_bits(x);
    return (u16)(((f >> 16U) & 0x8000U) | f32_to_small_float(f & 0x7FFFFFFFU, 10U));
}

f32 nph_f16_to_f32(u16 h)
{
    const u32 sign = ((u32)h & 0x8000U) << 16U;
    const u32 exp = ((u32)h >> 10U) & 0x1FU;
    const u32 mant = (u32)h & 0x3FFU;
    if (exp == 0U) {
        // Zero or subnormal: mant * 2^-24, exact in binary32.
        const f32 v = (f32)mant * (1.0F / 16777216.0F);
        return nph_f32_from_bits(sign | nph_f32_to_bits(v));
    }
    if (exp == 0x1FU) {
        return nph_f32_from_bits(sign | 0x7F800000U | (mant << 13U));
    }
    return nph_f32_from_bits(sign | ((exp + 112U) << 23U) | (mant << 13U));
}

void nph_f32_to_f16_array(const f32 *in, u16 *out, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        out[i] = nph_f32_to_f16(in[i]);
    }
}

void nph_f16_to_f32_array(const u16 *in, f32 *out, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        out[i] = nph_f16_to_f32(in[i]);
    }
}

u32 nph_pack_f16x2(NphVec2 v)
{
    return (u32)nph_f32_to_f16(v.x) | ((u32)nph_f32_to_f16(v.y) << 16U);
}

NphVec2 nph_unpack_f16x2(u32 packed)
{
    return nph_vec2(nph_f16_to_f32((u16)(packed & 0xFFFFU)), nph_f16_to_f32((u16)(packed >> 16U)));
}

u64 nph_pack_f16x4(NphVec4 v)
{
    return (u64)nph_f32_to_f16(v.x) | ((u64)nph_f32_to_f16(v.y) << 16U) |
           ((u64)nph_f32_to_f16(v.z) << 32U) | ((u64)nph_f32_to_f16(v.w) << 48U);
}

NphVec4 nph_unpack_f16x4(u64 packed)
{
    return nph_vec4(
        nph_f16_to_f32((u16)(packed & 0xFFFFU)), nph_f16_to_f32((u16)((packed >> 16U) & 0xFFFFU)),
        nph_f16_to_f32((u16)((packed >> 32U) & 0xFFFFU)), nph_f16_to_f32((u16)(packed >> 48U)));
}

// ---------------------------------------------------------------------------
// Octahedral normals
// ---------------------------------------------------------------------------

static f32 sign_not_zero(f32 v)
{
    return v >= 0.0F ? 1.0F : -1.0F;
}

NphVec2 nph_oct_encode(NphVec3 n)
{
    // Project onto the octahedron (L1 normalisation), then fold the lower
    // hemisphere over the outer triangles of the square.
    const f32 inv_l1 = 1.0F / (nph_f32_abs(n.x) + nph_f32_abs(n.y) + nph_f32_abs(n.z));
    const NphVec2 p = nph_vec2(n.x * inv_l1, n.y * inv_l1);
    if (n.z < 0.0F) {
        return nph_vec2((1.0F - nph_f32_abs(p.y)) * sign_not_zero(p.x),
                        (1.0F - nph_f32_abs(p.x)) * sign_not_zero(p.y));
    }
    return p;
}

NphVec3 nph_oct_decode(NphVec2 e)
{
    NphVec3 n = nph_vec3(e.x, e.y, 1.0F - nph_f32_abs(e.x) - nph_f32_abs(e.y));
    if (n.z < 0.0F) {
        const f32 x = (1.0F - nph_f32_abs(n.y)) * sign_not_zero(n.x);
        const f32 y = (1.0F - nph_f32_abs(n.x)) * sign_not_zero(n.y);
        n.x = x;
        n.y = y;
    }
    return nph_vec3_normalize(n);
}

// ---------------------------------------------------------------------------
// R11G11B10F
// ---------------------------------------------------------------------------

// Rounds a non-negative binary16 bit pattern to an unsigned float with
// `mant_bits` mantissa bits (same 5-bit exponent) to nearest even. Adding the
// rounding term to the whole pattern lets a mantissa carry propagate into
// the exponent, so the largest finite values overflow to infinity exactly
// as IEEE rounding would.
// One unsigned component: negative values clamp to zero, NaN stays NaN.
static u32 f32_to_ufloat_component(f32 x, u32 mant_bits)
{
    const u32 f = nph_f32_to_bits(x);
    if ((f & 0x80000000U) != 0U) {
        const bool is_nan = (f & 0x7F800000U) == 0x7F800000U && (f & 0x7FFFFFU) != 0U;
        return is_nan ? (0x1FU << mant_bits) | 1U : 0U;
    }
    return f32_to_small_float(f, mant_bits);
}

u32 nph_pack_r11g11b10f(NphVec3 rgb)
{
    const u32 r = f32_to_ufloat_component(rgb.x, 6U);
    const u32 g = f32_to_ufloat_component(rgb.y, 6U);
    const u32 b = f32_to_ufloat_component(rgb.z, 5U);
    return r | (g << 11U) | (b << 22U);
}

NphVec3 nph_unpack_r11g11b10f(u32 packed)
{
    // Widening is exact: shift the mantissa back under the same exponent.
    const u16 r = (u16)((packed & 0x7FFU) << 4U);
    const u16 g = (u16)(((packed >> 11U) & 0x7FFU) << 4U);
    const u16 b = (u16)(((packed >> 22U) & 0x3FFU) << 5U);
    return nph_vec3(nph_f16_to_f32(r), nph_f16_to_f32(g), nph_f16_to_f32(b));
}

// ---------------------------------------------------------------------------
// RGB9E5 (EXT_texture_shared_exponent, section 2.1.z)
// ---------------------------------------------------------------------------

#define RGB9E5_MANTISSA_BITS 9
#define RGB9E5_EXPONENT_BIAS 15

static f32 rgb9e5_clamp(f32 c)
{
    if (nph_f32_is_nan(c)) {
        return 0.0F;
    }
    return nph_f32_clamp(c, 0.0F, NPH_RGB9E5_MAX);
}

u32 nph_pack_rgb9e5(NphVec3 rgb)
{
    const f32 rc = rgb9e5_clamp(rgb.x);
    const f32 gc = rgb9e5_clamp(rgb.y);
    const f32 bc = rgb9e5_clamp(rgb.z);
    const f32 maxc = nph_f32_max3(rc, gc, bc);

    // floor(log2(maxc)) via frexp: maxc = m * 2^e with m in [0.5, 1).
    i32 floor_log2 = -RGB9E5_EXPONENT_BIAS - 1;
    if (maxc > 0.0F) {
        int e = 0;
        (void)frexpf(maxc, &e);
        floor_log2 = nph_max_i32(e - 1, -RGB9E5_EXPONENT_BIAS - 1);
    }
    i32 exp_shared = floor_log2 + 1 + RGB9E5_EXPONENT_BIAS;
    f32 denom = ldexpf(1.0F, exp_shared - RGB9E5_EXPONENT_BIAS - RGB9E5_MANTISSA_BITS);
    const i32 max_s = (i32)nph_f32_floor(maxc / denom + 0.5F);
    if (max_s == (1 << RGB9E5_MANTISSA_BITS)) {
        // The rounded maximum needs one more bit: bump the exponent.
        exp_shared += 1;
        denom *= 2.0F;
    }
    const u32 rs = (u32)nph_f32_floor(rc / denom + 0.5F);
    const u32 gs = (u32)nph_f32_floor(gc / denom + 0.5F);
    const u32 bs = (u32)nph_f32_floor(bc / denom + 0.5F);
    return rs | (gs << 9U) | (bs << 18U) | ((u32)exp_shared << 27U);
}

NphVec3 nph_unpack_rgb9e5(u32 packed)
{
    const i32 exp = (i32)(packed >> 27U);
    const f32 scale = ldexpf(1.0F, exp - RGB9E5_EXPONENT_BIAS - RGB9E5_MANTISSA_BITS);
    return nph_vec3((f32)(packed & 0x1FFU) * scale, (f32)((packed >> 9U) & 0x1FFU) * scale,
                    (f32)((packed >> 18U) & 0x1FFU) * scale);
}

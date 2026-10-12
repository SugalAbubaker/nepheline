// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compact encodings for GPU and file data (CLAUDE.md §8.2 "half-float
// conversion, octahedral normal encoding and quantization helpers"; §10.3
// "bandwidth discipline"): IEEE binary16, normalised integers, octahedral
// unit vectors, and the packed HDR formats R11G11B10F and RGB9E5.
//
// Every conversion follows the Vulkan specification's rules so that what the
// CPU writes is what the GPU reads back: round-to-nearest-even for floats,
// round-to-nearest for unorm/snorm, and the shared-exponent algorithm of
// EXT_texture_shared_exponent for RGB9E5. The scalar functions here are the
// reference; batch versions over arrays are provided for the cooker.
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// IEEE 754 binary16
// ---------------------------------------------------------------------------

// Round-to-nearest-even conversion with correct handling of subnormals,
// overflow to infinity, infinities, and NaN (which stays NaN, quietened).
u16 nph_f32_to_f16(f32 x);
// Exact: every binary16 value is representable in binary32.
f32 nph_f16_to_f32(u16 h);

void nph_f32_to_f16_array(const f32 *in, u16 *out, u32 count);
void nph_f16_to_f32_array(const u16 *in, f32 *out, u32 count);

// Two and four halves in one word; x in the low bits.
u32 nph_pack_f16x2(NphVec2 v);
NphVec2 nph_unpack_f16x2(u32 packed);
u64 nph_pack_f16x4(NphVec4 v);
NphVec4 nph_unpack_f16x4(u64 packed);

#define NPH_F16_MAX     65504.0F
#define NPH_F16_EPSILON 0.0009765625F // 2^-10
#define NPH_F16_ONE     0x3C00U
#define NPH_F16_INF     0x7C00U
#define NPH_F16_NAN     0x7E00U

// ---------------------------------------------------------------------------
// Normalised integers (Vulkan §3.9 "Conversion from Floating-Point to
// Normalized Fixed-Point" and back): unorm maps [0, 1], snorm maps [-1, 1]
// with both -2^(n-1) and -2^(n-1)+1 decoding to -1. Inputs are clamped.
// ---------------------------------------------------------------------------

NPH_INLINE u8 nph_f32_to_unorm8(f32 x)
{
    return (u8)(nph_f32_saturate(x) * 255.0F + 0.5F);
}
NPH_INLINE f32 nph_unorm8_to_f32(u8 v)
{
    return (f32)v * (1.0F / 255.0F);
}
NPH_INLINE u16 nph_f32_to_unorm16(f32 x)
{
    return (u16)(nph_f32_saturate(x) * 65535.0F + 0.5F);
}
NPH_INLINE f32 nph_unorm16_to_f32(u16 v)
{
    return (f32)v * (1.0F / 65535.0F);
}
NPH_INLINE i8 nph_f32_to_snorm8(f32 x)
{
    return (i8)nph_f32_round(nph_f32_clamp(x, -1.0F, 1.0F) * 127.0F);
}
NPH_INLINE f32 nph_snorm8_to_f32(i8 v)
{
    return nph_f32_max((f32)v * (1.0F / 127.0F), -1.0F);
}
NPH_INLINE i16 nph_f32_to_snorm16(f32 x)
{
    return (i16)nph_f32_round(nph_f32_clamp(x, -1.0F, 1.0F) * 32767.0F);
}
NPH_INLINE f32 nph_snorm16_to_f32(i16 v)
{
    return nph_f32_max((f32)v * (1.0F / 32767.0F), -1.0F);
}

// RGBA8 unorm with x in the lowest byte (the VK_FORMAT_R8G8B8A8_UNORM
// memory order on a little-endian machine).
NPH_INLINE u32 nph_pack_unorm8x4(NphVec4 v)
{
    return (u32)nph_f32_to_unorm8(v.x) | ((u32)nph_f32_to_unorm8(v.y) << 8U) |
           ((u32)nph_f32_to_unorm8(v.z) << 16U) | ((u32)nph_f32_to_unorm8(v.w) << 24U);
}
NPH_INLINE NphVec4 nph_unpack_unorm8x4(u32 p)
{
    return nph_vec4(nph_unorm8_to_f32((u8)(p & 0xFFU)), nph_unorm8_to_f32((u8)((p >> 8U) & 0xFFU)),
                    nph_unorm8_to_f32((u8)((p >> 16U) & 0xFFU)),
                    nph_unorm8_to_f32((u8)((p >> 24U) & 0xFFU)));
}
NPH_INLINE u32 nph_pack_snorm8x4(NphVec4 v)
{
    return ((u32)(u8)nph_f32_to_snorm8(v.x)) | ((u32)(u8)nph_f32_to_snorm8(v.y) << 8U) |
           ((u32)(u8)nph_f32_to_snorm8(v.z) << 16U) | ((u32)(u8)nph_f32_to_snorm8(v.w) << 24U);
}
NPH_INLINE NphVec4 nph_unpack_snorm8x4(u32 p)
{
    return nph_vec4(nph_snorm8_to_f32((i8)(u8)(p & 0xFFU)),
                    nph_snorm8_to_f32((i8)(u8)((p >> 8U) & 0xFFU)),
                    nph_snorm8_to_f32((i8)(u8)((p >> 16U) & 0xFFU)),
                    nph_snorm8_to_f32((i8)(u8)((p >> 24U) & 0xFFU)));
}
NPH_INLINE u32 nph_pack_unorm16x2(NphVec2 v)
{
    return (u32)nph_f32_to_unorm16(v.x) | ((u32)nph_f32_to_unorm16(v.y) << 16U);
}
NPH_INLINE NphVec2 nph_unpack_unorm16x2(u32 p)
{
    return nph_vec2(nph_unorm16_to_f32((u16)(p & 0xFFFFU)), nph_unorm16_to_f32((u16)(p >> 16U)));
}
NPH_INLINE u32 nph_pack_snorm16x2(NphVec2 v)
{
    return (u32)(u16)nph_f32_to_snorm16(v.x) | ((u32)(u16)nph_f32_to_snorm16(v.y) << 16U);
}
NPH_INLINE NphVec2 nph_unpack_snorm16x2(u32 p)
{
    return nph_vec2(nph_snorm16_to_f32((i16)(u16)(p & 0xFFFFU)),
                    nph_snorm16_to_f32((i16)(u16)(p >> 16U)));
}

// ---------------------------------------------------------------------------
// Octahedral unit vectors (Meyer et al., "On Floating-Point Normal Vectors",
// EGSR 2010; Cigolle et al., "A Survey of Efficient Representations for
// Independent Unit Vectors", JCGT 2014). The sphere is projected onto the
// octahedron |x|+|y|+|z| = 1 and the lower half folded outward, giving a
// square [-1, 1]² with no singularities and nearly uniform precision.
// ---------------------------------------------------------------------------

// n must be unit.
NphVec2 nph_oct_encode(NphVec3 n);
NphVec3 nph_oct_decode(NphVec2 e);

// Two snorm16 components: 32 bits per normal, maximum error about 0.005
// degrees. The natural vertex-buffer normal format.
NPH_INLINE u32 nph_oct_encode_snorm16(NphVec3 n)
{
    return nph_pack_snorm16x2(nph_oct_encode(n));
}
NPH_INLINE NphVec3 nph_oct_decode_snorm16(u32 packed)
{
    return nph_oct_decode(nph_unpack_snorm16x2(packed));
}
// Two snorm8 components: 16 bits per normal, maximum error just under one
// degree (the survey's "oct16"; its "oct16P" halves that by searching the
// neighbouring codes, a cooker-side refinement for later).
NPH_INLINE u16 nph_oct_encode_snorm8(NphVec3 n)
{
    const NphVec2 e = nph_oct_encode(n);
    return (u16)((u16)(u8)nph_f32_to_snorm8(e.x) | ((u16)(u8)nph_f32_to_snorm8(e.y) << 8U));
}
NPH_INLINE NphVec3 nph_oct_decode_snorm8(u16 packed)
{
    return nph_oct_decode(nph_vec2(nph_snorm8_to_f32((i8)(u8)(packed & 0xFFU)),
                                   nph_snorm8_to_f32((i8)(u8)(packed >> 8U))));
}

// ---------------------------------------------------------------------------
// Packed HDR colour (CLAUDE.md §10.3: R11G11B10 / RGB9E5 render targets)
// ---------------------------------------------------------------------------

// VK_FORMAT_B10G11R11_UFLOAT_PACK32: unsigned 11-bit (5e6m) red and green,
// 10-bit (5e5m) blue, red in the low bits. Round-to-nearest-even; negative
// inputs become 0, NaN stays NaN, overflow becomes infinity.
u32 nph_pack_r11g11b10f(NphVec3 rgb);
NphVec3 nph_unpack_r11g11b10f(u32 packed);

// VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: three 9-bit mantissas sharing a 5-bit
// exponent (bias 15), red in the low bits. Exactly the algorithm of
// EXT_texture_shared_exponent: inputs are clamped to [0, 65408], NaN
// becomes 0, and the mantissas are rounded to nearest.
u32 nph_pack_rgb9e5(NphVec3 rgb);
NphVec3 nph_unpack_rgb9e5(u32 packed);

#define NPH_RGB9E5_MAX 65408.0F

#ifdef __cplusplus
}
#endif

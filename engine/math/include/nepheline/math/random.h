// SPDX-License-Identifier: Apache-2.0
#pragma once

// Random and quasi-random numbers (CLAUDE.md §8.2 "Random"): PCG32 for
// pseudo-random streams, radical-inverse (Halton) and R2 low-discrepancy
// sequences for sampling, the standard sample warps, and a blue-noise
// generator for dither and sample textures.
//
// Determinism: every generator here is a pure function of its explicit
// state or index. The same seed produces the same stream on every platform
// and compiler, which is what §2 "deterministic simulation" requires of
// gameplay randomness. Nothing here reads a clock or the OS.
//
// Thread-safety: an NphPcg32 belongs to one thread at a time; everything
// else is pure and NPH_THREADSAFE.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// PCG32 (O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically
// Good Algorithms for Random Number Generation", 2014): the 64-bit LCG with
// the XSH-RR output function. 16 bytes of state, period 2^64 per stream,
// 2^63 independent streams.
// ---------------------------------------------------------------------------

typedef struct NphPcg32 {
    u64 state;
    u64 inc; // stream selector, always odd
} NphPcg32;

// Initialises the generator for `seed` on stream `sequence` (any value; the
// stream id is derived from it). Identical to the reference pcg32_srandom_r.
void nph_pcg32_seed(NphPcg32 *rng, u64 seed, u64 sequence);

// Uniform over all 32-bit values.
NPH_INLINE u32 nph_pcg32_next_u32(NphPcg32 *rng)
{
    const u64 old = rng->state;
    rng->state = old * 6364136223846793005ULL + rng->inc;
    const u32 xorshifted = (u32)(((old >> 18U) ^ old) >> 27U);
    const u32 rot = (u32)(old >> 59U);
    return (xorshifted >> rot) | (xorshifted << ((32U - rot) & 31U));
}
NPH_INLINE u64 nph_pcg32_next_u64(NphPcg32 *rng)
{
    const u64 hi = nph_pcg32_next_u32(rng);
    const u64 lo = nph_pcg32_next_u32(rng);
    return (hi << 32U) | lo;
}
// Uniform in [0, bound) without modulo bias; bound > 0 (asserted). Lemire,
// "Fast Random Integer Generation in an Interval", ACM TOMACS 2019.
u32 nph_pcg32_next_bounded(NphPcg32 *rng, u32 bound);

// Uniform in [0, 1): 24 random mantissa bits, so every result is exactly
// representable and 1.0 never occurs.
NPH_INLINE f32 nph_pcg32_next_f32(NphPcg32 *rng)
{
    return (f32)(nph_pcg32_next_u32(rng) >> 8U) * (1.0F / 16777216.0F);
}
// Uniform in [0, 1) with 53 random bits.
NPH_INLINE f64 nph_pcg32_next_f64(NphPcg32 *rng)
{
    return (f64)(nph_pcg32_next_u64(rng) >> 11U) * (1.0 / 9007199254740992.0);
}
// Uniform in [lo, hi).
NPH_INLINE f32 nph_pcg32_next_range_f32(NphPcg32 *rng, f32 lo, f32 hi)
{
    return lo + (hi - lo) * nph_pcg32_next_f32(rng);
}
NPH_INLINE NphVec2 nph_pcg32_next_vec2(NphPcg32 *rng)
{
    return nph_vec2(nph_pcg32_next_f32(rng), nph_pcg32_next_f32(rng));
}
NPH_INLINE NphVec3 nph_pcg32_next_vec3(NphPcg32 *rng)
{
    return nph_vec3(nph_pcg32_next_f32(rng), nph_pcg32_next_f32(rng), nph_pcg32_next_f32(rng));
}
NPH_INLINE bool nph_pcg32_next_bool(NphPcg32 *rng)
{
    return (nph_pcg32_next_u32(rng) & 0x80000000U) != 0U;
}

// Advances the stream by `delta` draws in O(log delta) (Brown, "Random
// Number Generation with Arbitrary Strides", 1994). Lets N jobs take
// disjoint sub-streams of one seed: job i advances by i * draws_per_job.
void nph_pcg32_advance(NphPcg32 *rng, u64 delta);

// ---------------------------------------------------------------------------
// Low-discrepancy sequences
// ---------------------------------------------------------------------------

// Van der Corput radical inverse in base 2 (bit reversal), in [0, 1).
NPH_INLINE f32 nph_radical_inverse_base2(u32 i)
{
    i = (i << 16U) | (i >> 16U);
    i = ((i & 0x00FF00FFU) << 8U) | ((i & 0xFF00FF00U) >> 8U);
    i = ((i & 0x0F0F0F0FU) << 4U) | ((i & 0xF0F0F0F0U) >> 4U);
    i = ((i & 0x33333333U) << 2U) | ((i & 0xCCCCCCCCU) >> 2U);
    i = ((i & 0x55555555U) << 1U) | ((i & 0xAAAAAAAAU) >> 1U);
    // Keep 24 bits so the result is exact and strictly below 1.
    return (f32)(i >> 8U) * (1.0F / 16777216.0F);
}
// Radical inverse in any base >= 2, in [0, 1).
f32 nph_radical_inverse(u32 i, u32 base);

// The i-th point of the 2D Halton sequence (bases 2 and 3).
NPH_INLINE NphVec2 nph_halton_2d(u32 i)
{
    return nph_vec2(nph_radical_inverse_base2(i), nph_radical_inverse(i, 3U));
}

// Additive recurrence sequences (Roberts, "The Unreasonable Effectiveness
// of Quasirandom Sequences", 2018): fract(0.5 + i·alpha) with alpha from
// the golden ratio (1D) and the plastic constant (2D). Cheap, decorrelate
// well across pixels when offset per pixel, and extend to any count.
NPH_INLINE f32 nph_r1(u32 i)
{
    return nph_f32_fract(0.5F + (f32)i * 0.61803398874989484820F);
}
NPH_INLINE NphVec2 nph_r2(u32 i)
{
    return nph_vec2(nph_f32_fract(0.5F + (f32)i * 0.75487766624669276F),
                    nph_f32_fract(0.5F + (f32)i * 0.56984029099805327F));
}

// ---------------------------------------------------------------------------
// Sample warps: map u in [0, 1)² to the target domain, area-preserving.
// ---------------------------------------------------------------------------

// Unit disk, concentric mapping (Shirley & Chiu, "A Low Distortion Map
// Between Disk and Square", JGT 1997).
NphVec2 nph_sample_disk_concentric(NphVec2 u);
// Uniform on the unit sphere.
NphVec3 nph_sample_sphere_uniform(NphVec2 u);
// Uniform on the +Z hemisphere.
NphVec3 nph_sample_hemisphere_uniform(NphVec2 u);
// Cosine-weighted on the +Z hemisphere (Malley's method over the concentric
// disk); pdf(w) == w.z / pi.
NphVec3 nph_sample_hemisphere_cosine(NphVec2 u);
// Uniform barycentric coordinates (u, v) over a triangle; the third is
// 1 - u - v.
NphVec2 nph_sample_triangle_uniform(NphVec2 u);

#define NPH_PDF_SPHERE_UNIFORM     (1.0F / (4.0F * NPH_PI))
#define NPH_PDF_HEMISPHERE_UNIFORM (1.0F / (2.0F * NPH_PI))
NPH_INLINE f32 nph_pdf_hemisphere_cosine(f32 cos_theta)
{
    return cos_theta * NPH_INV_PI;
}

// ---------------------------------------------------------------------------
// Blue noise
// ---------------------------------------------------------------------------

// Generates a width x height blue-noise dither array with the void-and-
// cluster method (Ulichney, "The void-and-cluster method for dither array
// generation", SPIE 1993) using a toroidal Gaussian of standard deviation
// `sigma` texels (1.5 is Ulichney's choice). out_ranks receives width*height
// values forming a permutation of [0, width*height): the threshold rank of
// each texel; (rank + 0.5) / (width*height) is the usual [0, 1) dither
// value. Deterministic for a given seed.
//
// Cost is O((width*height)²) time and 14 bytes per texel of scratch from
// `alloc`, released before returning; a 128x128 array takes well under a
// second. This is a
// cooker-side tool, not a runtime one.
//
// Returns NPH_RESULT_INVALID_ARGUMENT for a zero size or a non-positive
// sigma, NPH_RESULT_OUT_OF_MEMORY when the scratch cannot be allocated.
NPH_MUST_USE NphResult nph_blue_noise_generate(NphAllocator *alloc, u32 width, u32 height,
                                               f32 sigma, u64 seed, u32 *out_ranks);

#ifdef __cplusplus
}
#endif

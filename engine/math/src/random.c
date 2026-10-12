// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/math/random.h>

#define PCG32_MULTIPLIER 6364136223846793005ULL

// ---------------------------------------------------------------------------
// PCG32
// ---------------------------------------------------------------------------

void nph_pcg32_seed(NphPcg32 *rng, u64 seed, u64 sequence)
{
    rng->state = 0U;
    rng->inc = (sequence << 1U) | 1U;
    (void)nph_pcg32_next_u32(rng);
    rng->state += seed;
    (void)nph_pcg32_next_u32(rng);
}

u32 nph_pcg32_next_bounded(NphPcg32 *rng, u32 bound)
{
    NPH_ASSERT(bound > 0U);
    // Lemire 2019: multiply into 64 bits and keep the high word; the low
    // word tells whether this draw falls in the biased remainder zone, in
    // which case redraw. The expensive modulo runs only when it might.
    u32 x = nph_pcg32_next_u32(rng);
    u64 m = (u64)x * bound;
    u32 l = (u32)m;
    if (l < bound) {
        const u32 t = (0U - bound) % bound;
        while (l < t) {
            x = nph_pcg32_next_u32(rng);
            m = (u64)x * bound;
            l = (u32)m;
        }
    }
    return (u32)(m >> 32U);
}

void nph_pcg32_advance(NphPcg32 *rng, u64 delta)
{
    // Brown 1994: compose the affine map state -> state * a + c with itself
    // by repeated squaring, as for modular exponentiation.
    u64 cur_mult = PCG32_MULTIPLIER;
    u64 cur_plus = rng->inc;
    u64 acc_mult = 1U;
    u64 acc_plus = 0U;
    while (delta > 0U) {
        if ((delta & 1U) != 0U) {
            acc_mult *= cur_mult;
            acc_plus = acc_plus * cur_mult + cur_plus;
        }
        cur_plus = (cur_mult + 1U) * cur_plus;
        cur_mult *= cur_mult;
        delta >>= 1U;
    }
    rng->state = acc_mult * rng->state + acc_plus;
}

// ---------------------------------------------------------------------------
// Low-discrepancy sequences
// ---------------------------------------------------------------------------

f32 nph_radical_inverse(u32 i, u32 base)
{
    NPH_ASSERT(base >= 2U);
    // Accumulated in double so that bases with inexact reciprocals (3, 5, 7)
    // still produce the correctly rounded float; the loop is at most 32
    // iterations.
    const f64 inv_base = 1.0 / (f64)base;
    f64 f = 1.0;
    f64 r = 0.0;
    while (i > 0U) {
        f *= inv_base;
        r += f * (f64)(i % base);
        i /= base;
    }
    return (f32)nph_f64_min(r, 0.99999994);
}

// ---------------------------------------------------------------------------
// Sample warps
// ---------------------------------------------------------------------------

NphVec2 nph_sample_disk_concentric(NphVec2 u)
{
    // Map the square to [-1, 1]² and send each of its four triangular
    // wedges to a sector of the disk, preserving area and adjacency.
    const f32 x = 2.0F * u.x - 1.0F;
    const f32 y = 2.0F * u.y - 1.0F;
    if (x == 0.0F && y == 0.0F) {
        return nph_vec2_zero();
    }
    f32 r;
    f32 theta;
    if (nph_f32_abs(x) > nph_f32_abs(y)) {
        r = x;
        theta = NPH_QUARTER_PI * (y / x);
    } else {
        r = y;
        theta = NPH_HALF_PI - NPH_QUARTER_PI * (x / y);
    }
    return nph_vec2(r * nph_f32_cos(theta), r * nph_f32_sin(theta));
}

NphVec3 nph_sample_sphere_uniform(NphVec2 u)
{
    const f32 z = 1.0F - 2.0F * u.x;
    const f32 r = nph_f32_sqrt(nph_f32_max(0.0F, 1.0F - z * z));
    const f32 phi = NPH_TAU * u.y;
    return nph_vec3(r * nph_f32_cos(phi), r * nph_f32_sin(phi), z);
}

NphVec3 nph_sample_hemisphere_uniform(NphVec2 u)
{
    const f32 z = u.x;
    const f32 r = nph_f32_sqrt(nph_f32_max(0.0F, 1.0F - z * z));
    const f32 phi = NPH_TAU * u.y;
    return nph_vec3(r * nph_f32_cos(phi), r * nph_f32_sin(phi), z);
}

NphVec3 nph_sample_hemisphere_cosine(NphVec2 u)
{
    // Malley's method: a uniform disk sample projected up onto the
    // hemisphere is cosine distributed.
    const NphVec2 d = nph_sample_disk_concentric(u);
    const f32 z = nph_f32_sqrt(nph_f32_max(0.0F, 1.0F - d.x * d.x - d.y * d.y));
    return nph_vec3(d.x, d.y, z);
}

NphVec2 nph_sample_triangle_uniform(NphVec2 u)
{
    const f32 su = nph_f32_sqrt(u.x);
    return nph_vec2(1.0F - su, u.y * su);
}

// ---------------------------------------------------------------------------
// Blue noise: void-and-cluster
// ---------------------------------------------------------------------------

typedef struct BlueNoise {
    u32 width;
    u32 height;
    u32 count;
    f32 *energy; // Gaussian energy of the current binary pattern
    f32 *kernel; // toroidal Gaussian, indexed by wrapped (dy, dx)
    u8 *on;      // current binary pattern
} BlueNoise;

// Adds (sign = +1) or removes (sign = -1) the energy contributed by the
// texel at `index` to every texel.
static void bn_toggle(BlueNoise *bn, u32 index, f32 sign)
{
    bn->on[index] = sign > 0.0F ? 1U : 0U;
    const u32 ix = index % bn->width;
    const u32 iy = index / bn->width;
    for (u32 y = 0; y < bn->height; ++y) {
        const u32 dy = y >= iy ? y - iy : y + bn->height - iy;
        const f32 *krow = bn->kernel + (u64)dy * bn->width;
        f32 *erow = bn->energy + (u64)y * bn->width;
        for (u32 x = 0; x < bn->width; ++x) {
            const u32 dx = x >= ix ? x - ix : x + bn->width - ix;
            erow[x] += sign * krow[dx];
        }
    }
}

// The set texel with the highest energy (tightest cluster), lowest index on
// ties. The pattern has at least one set texel.
static u32 bn_tightest_cluster(const BlueNoise *bn)
{
    u32 best = NPH_U32_MAX;
    f32 best_energy = -1.0F;
    for (u32 i = 0; i < bn->count; ++i) {
        if (bn->on[i] != 0U && bn->energy[i] > best_energy) {
            best_energy = bn->energy[i];
            best = i;
        }
    }
    return best;
}

// The clear texel with the lowest energy (largest void), lowest index on
// ties. The pattern has at least one clear texel.
static u32 bn_largest_void(const BlueNoise *bn)
{
    u32 best = NPH_U32_MAX;
    f32 best_energy = NPH_F32_INFINITY;
    for (u32 i = 0; i < bn->count; ++i) {
        if (bn->on[i] == 0U && bn->energy[i] < best_energy) {
            best_energy = bn->energy[i];
            best = i;
        }
    }
    return best;
}

NphResult nph_blue_noise_generate(NphAllocator *alloc, u32 width, u32 height, f32 sigma, u64 seed,
                                  u32 *out_ranks)
{
    if (alloc == NULL || out_ranks == NULL || width == 0U || height == 0U || sigma <= 0.0F ||
        (u64)width * height > 0x7FFFFFFFULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 count = width * height;
    BlueNoise bn;
    bn.width = width;
    bn.height = height;
    bn.count = count;
    bn.energy = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, count);
    bn.kernel = NPH_ALLOC_ARRAY(alloc, f32, count);
    bn.on = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, count);
    f32 *energy0 = NPH_ALLOC_ARRAY(alloc, f32, count);
    u8 *on0 = NPH_ALLOC_ARRAY(alloc, u8, count);
    NphResult result = NPH_RESULT_OK;
    if (bn.energy == NULL || bn.kernel == NULL || bn.on == NULL || energy0 == NULL || on0 == NULL) {
        result = NPH_RESULT_OUT_OF_MEMORY;
        goto cleanup;
    }

    // Toroidal Gaussian: the distance along each axis wraps around.
    const f32 inv_two_sigma_sq = 1.0F / (2.0F * sigma * sigma);
    for (u32 y = 0; y < height; ++y) {
        const u32 wy = nph_min_u32(y, height - y);
        for (u32 x = 0; x < width; ++x) {
            const u32 wx = nph_min_u32(x, width - x);
            const f32 dist_sq = (f32)(wx * wx + wy * wy);
            bn.kernel[(u64)y * width + x] = nph_f32_exp(-dist_sq * inv_two_sigma_sq);
        }
    }

    // Initial binary pattern: about a tenth of the texels, chosen at random.
    const u32 initial = nph_max_u32(1U, count / 10U);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, seed, 0x626C7565ULL); // "blue"
    for (u32 placed = 0; placed < initial;) {
        const u32 i = nph_pcg32_next_bounded(&rng, count);
        if (bn.on[i] == 0U) {
            bn_toggle(&bn, i, 1.0F);
            placed += 1U;
        }
    }
    // Relax it: move the tightest cluster into the largest void until the
    // texel just removed is itself the largest void.
    if (initial < count) {
        for (u32 iter = 0; iter < count; ++iter) {
            const u32 cluster = bn_tightest_cluster(&bn);
            bn_toggle(&bn, cluster, -1.0F);
            const u32 hole = bn_largest_void(&bn);
            if (hole == cluster) {
                bn_toggle(&bn, cluster, 1.0F);
                break;
            }
            bn_toggle(&bn, hole, 1.0F);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        energy0[i] = bn.energy[i];
        on0[i] = bn.on[i];
    }

    // Phase 1: remove the tightest cluster repeatedly; the order of removal,
    // reversed, ranks the initial texels.
    for (u32 rank = initial; rank > 0U;) {
        const u32 cluster = bn_tightest_cluster(&bn);
        rank -= 1U;
        out_ranks[cluster] = rank;
        bn_toggle(&bn, cluster, -1.0F);
    }
    // Phases 2 and 3: from the initial pattern, fill the largest void
    // repeatedly. Past half density the "tightest cluster of clear texels"
    // coincides with the largest void of set ones (the Gaussian energies
    // are complementary), so one loop covers both phases.
    for (u32 i = 0; i < count; ++i) {
        bn.energy[i] = energy0[i];
        bn.on[i] = on0[i];
    }
    for (u32 rank = initial; rank < count; ++rank) {
        const u32 hole = bn_largest_void(&bn);
        out_ranks[hole] = rank;
        bn_toggle(&bn, hole, 1.0F);
    }

cleanup:
    NPH_FREE_ARRAY(alloc, f32, bn.energy, count);
    NPH_FREE_ARRAY(alloc, f32, bn.kernel, count);
    NPH_FREE_ARRAY(alloc, u8, bn.on, count);
    NPH_FREE_ARRAY(alloc, f32, energy0, count);
    NPH_FREE_ARRAY(alloc, u8, on0, count);
    return result;
}

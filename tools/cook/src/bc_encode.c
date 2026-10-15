// SPDX-License-Identifier: Apache-2.0
// Block encoders (bc.h). Every encoder follows the same plan: fit a line
// through the block's texels (the principal axis of their covariance),
// quantise its ends to the format's endpoint precision, assign each texel
// the nearest palette entry, then refine the endpoints by least squares
// against those assignments and repeat. The decoders in bc_decode.c are
// the reference for the palettes, so an encoder's error estimate is
// exactly what the GPU will reconstruct.
#include <nepheline/core/assert.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>

#include "bc_internal.h"

#include <cook/bc.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared fitting helpers
// ---------------------------------------------------------------------------

#define BC_MAX_CHANNELS   4U
#define BC_PCA_ITERATIONS 8U

// The principal axis of `count` points with `channels` components, by
// power iteration on the covariance; `mean` receives the centroid. A
// block with no spread yields a zero axis.
static void bc_pca(const f32 *points, u32 count, u32 stride, u32 channels, f32 mean[4], f32 axis[4])
{
    for (u32 c = 0; c < BC_MAX_CHANNELS; ++c) {
        mean[c] = 0.0F;
        axis[c] = 0.0F;
    }
    if (count == 0U) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            mean[c] += points[i * stride + c];
        }
    }
    for (u32 c = 0; c < channels; ++c) {
        mean[c] /= (f32)count;
    }
    f32 cov[4][4];
    memset(cov, 0, sizeof cov);
    for (u32 i = 0; i < count; ++i) {
        f32 d[4];
        for (u32 c = 0; c < channels; ++c) {
            d[c] = points[i * stride + c] - mean[c];
        }
        for (u32 a = 0; a < channels; ++a) {
            for (u32 b = 0; b < channels; ++b) {
                cov[a][b] += d[a] * d[b];
            }
        }
    }
    // Power iteration starts from the covariance column of the channel
    // that varies most: that column is the covariance applied to the
    // channel's axis, so it leans towards the principal axis whenever the
    // spread is elongated (a fixed start vector can be orthogonal to the
    // principal axis and collapse a two-colour block to its mean).
    u32 k = 0U;
    for (u32 c = 1; c < channels; ++c) {
        if (cov[c][c] > cov[k][k]) {
            k = c;
        }
    }
    if (cov[k][k] <= 1e-12F) {
        return; // no spread at all
    }
    f32 v[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    for (u32 c = 0; c < channels; ++c) {
        v[c] = cov[c][k];
    }
    for (u32 it = 0; it < BC_PCA_ITERATIONS; ++it) {
        f32 w[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        for (u32 a = 0; a < channels; ++a) {
            for (u32 b = 0; b < channels; ++b) {
                w[a] += cov[a][b] * v[b];
            }
        }
        f32 len = 0.0F;
        for (u32 c = 0; c < channels; ++c) {
            len += w[c] * w[c];
        }
        if (len <= 1e-12F) {
            return; // no spread at all
        }
        len = nph_f32_sqrt(len);
        for (u32 c = 0; c < channels; ++c) {
            v[c] = w[c] / len;
        }
    }
    for (u32 c = 0; c < channels; ++c) {
        axis[c] = v[c];
    }
}

// The two ends of the points' extent along `axis` from `mean`.
static void bc_axis_extent(const f32 *points, u32 count, u32 stride, u32 channels,
                           const f32 mean[4], const f32 axis[4], f32 lo[4], f32 hi[4])
{
    f32 tmin = 0.0F;
    f32 tmax = 0.0F;
    for (u32 i = 0; i < count; ++i) {
        f32 t = 0.0F;
        for (u32 c = 0; c < channels; ++c) {
            t += (points[i * stride + c] - mean[c]) * axis[c];
        }
        if (i == 0U || t < tmin) {
            tmin = t;
        }
        if (i == 0U || t > tmax) {
            tmax = t;
        }
    }
    for (u32 c = 0; c < channels; ++c) {
        lo[c] = mean[c] + axis[c] * tmin;
        hi[c] = mean[c] + axis[c] * tmax;
    }
}

// Least-squares endpoints for fixed interpolation weights: minimises
// sum |(1 - w) lo + w hi - p|^2 per channel. false when every weight is
// the same (the system is singular; the caller keeps its endpoints).
static bool bc_least_squares(const f32 *points, u32 count, u32 stride, u32 channels,
                             const f32 *weights, f32 lo[4], f32 hi[4])
{
    f32 aa = 0.0F;
    f32 ab = 0.0F;
    f32 bb = 0.0F;
    f32 ap[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 bp[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    for (u32 i = 0; i < count; ++i) {
        const f32 b = weights[i];
        const f32 a = 1.0F - b;
        aa += a * a;
        ab += a * b;
        bb += b * b;
        for (u32 c = 0; c < channels; ++c) {
            ap[c] += a * points[i * stride + c];
            bp[c] += b * points[i * stride + c];
        }
    }
    const f32 det = aa * bb - ab * ab;
    if (nph_f32_abs(det) < 1e-6F) {
        return false;
    }
    const f32 inv = 1.0F / det;
    for (u32 c = 0; c < channels; ++c) {
        lo[c] = (bb * ap[c] - ab * bp[c]) * inv;
        hi[c] = (aa * bp[c] - ab * ap[c]) * inv;
    }
    return true;
}

// ---------------------------------------------------------------------------
// BC4 / BC5
// ---------------------------------------------------------------------------

typedef struct Bc4Fit {
    u8 e0;
    u8 e1;
    u8 indices[16];
    u32 error;
} Bc4Fit;

static u32 bc4_assign(const u8 values[16], u8 e0, u8 e1, u8 indices[16])
{
    u8 palette[8];
    bc4_palette(e0, e1, palette);
    u32 error = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        u32 best = 0U;
        u32 best_err = 0xFFFFFFFFU;
        for (u32 k = 0; k < 8U; ++k) {
            const i32 d = (i32)values[i] - (i32)palette[k];
            const u32 err = (u32)(d * d);
            if (err < best_err) {
                best_err = err;
                best = k;
            }
        }
        indices[i] = (u8)best;
        error += best_err;
    }
    return error;
}

static u8 bc4_clamp(f32 v)
{
    return (u8)nph_f32_clamp(v + 0.5F, 0.0F, 255.0F);
}

// Fits one endpoint mode: `eight` is the e0 > e1 mode (8 interpolated
// values), otherwise e0 <= e1 (6 values plus 0 and 255).
static void bc4_fit_mode(const u8 values[16], bool eight, Bc4Fit *out)
{
    u8 vmin = 255U;
    u8 vmax = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        vmin = values[i] < vmin ? values[i] : vmin;
        vmax = values[i] > vmax ? values[i] : vmax;
    }
    u8 e0;
    u8 e1;
    if (eight) {
        e0 = vmax;
        e1 = vmin;
        if (e0 == e1) {
            // The eight-value mode needs e0 > e1; nudge one endpoint.
            if (e1 > 0U) {
                e1 = (u8)(e1 - 1U);
            } else {
                e0 = (u8)(e0 + 1U);
            }
        }
    } else {
        // Texels at exactly 0 or 255 are served by the fixed entries, so
        // the interpolated range covers the rest.
        u8 imin = 255U;
        u8 imax = 0U;
        bool any = false;
        for (u32 i = 0; i < 16U; ++i) {
            if (values[i] != 0U && values[i] != 255U) {
                imin = values[i] < imin ? values[i] : imin;
                imax = values[i] > imax ? values[i] : imax;
                any = true;
            }
        }
        if (!any) {
            imin = vmin;
            imax = vmax;
        }
        e0 = imin;
        e1 = imax;
    }
    out->e0 = e0;
    out->e1 = e1;
    out->error = bc4_assign(values, e0, e1, out->indices);

    // Least-squares refinement on the interpolated entries; fixed entries
    // (0 and 255 in the six-value mode) take no part.
    for (u32 pass = 0; pass < 3U; ++pass) {
        f32 pts[16];
        f32 w[16];
        u32 n = 0U;
        const u32 steps = eight ? 7U : 5U;
        for (u32 i = 0; i < 16U; ++i) {
            const u32 k = out->indices[i];
            if (!eight && k >= 6U) {
                continue;
            }
            // Palette entry k lies at fraction k'/steps from e0 to e1 with
            // k' = 0 for entry 0, steps for entry 1, and k - 1 otherwise.
            const u32 kk = k == 0U ? 0U : k == 1U ? steps : k - 1U;
            pts[n] = (f32)values[i];
            w[n] = (f32)kk / (f32)steps;
            ++n;
        }
        f32 lo[4] = {(f32)out->e0, 0.0F, 0.0F, 0.0F};
        f32 hi[4] = {(f32)out->e1, 0.0F, 0.0F, 0.0F};
        if (n == 0U || !bc_least_squares(pts, n, 1U, 1U, w, lo, hi)) {
            break;
        }
        u8 ne0 = bc4_clamp(lo[0]);
        u8 ne1 = bc4_clamp(hi[0]);
        if (eight ? ne0 <= ne1 : ne0 > ne1) {
            const u8 t = ne0;
            ne0 = ne1;
            ne1 = t;
        }
        if (eight && ne0 == ne1) {
            break;
        }
        Bc4Fit trial;
        trial.e0 = ne0;
        trial.e1 = ne1;
        trial.error = bc4_assign(values, ne0, ne1, trial.indices);
        if (trial.error >= out->error) {
            break;
        }
        *out = trial;
    }
}

void cook_bc4_encode(const u8 values[16], u8 out[8])
{
    Bc4Fit eight;
    Bc4Fit six;
    bc4_fit_mode(values, true, &eight);
    bc4_fit_mode(values, false, &six);
    const Bc4Fit *best = six.error < eight.error ? &six : &eight;
    memset(out, 0, 8U);
    out[0] = best->e0;
    out[1] = best->e1;
    u32 pos = 16U;
    for (u32 i = 0; i < 16U; ++i) {
        bc_write_bits(out, &pos, 3U, best->indices[i]);
    }
}

void cook_bc5_encode(const u8 r[16], const u8 g[16], u8 out[16])
{
    cook_bc4_encode(r, out);
    cook_bc4_encode(g, out + 8);
}

// ---------------------------------------------------------------------------
// BC7
// ---------------------------------------------------------------------------

// The result of fitting one subset in one mode.
typedef struct Bc7Subset {
    u32 code[2][4]; // quantised endpoint codes [endpoint][channel]
    u32 pbit[2];
    u8 palette[2][4]; // decoded endpoints
    u8 indices[16];   // for the subset's texels, in block order (unused slots 0)
    u32 error;
} Bc7Subset;

// The channel range a subset fit works on: colour (0..2), alpha (3), or
// both (0..3).
typedef struct Bc7Channels {
    u32 first;
    u32 count;
} Bc7Channels;

// Quantises a channel value (0..255) to `bits` with p-bit `p` appended
// (`pbits` 0 for none), choosing the code whose decoded value is nearest.
static u32 bc7_quantize(f32 value, u32 bits, u32 pbits, u32 p)
{
    const u32 max_code = (1U << bits) - 1U;
    const u32 total = bits + pbits;
    const f32 scale = (f32)((1U << total) - 1U) / 255.0F;
    const f32 target = nph_f32_clamp(value, 0.0F, 255.0F) * scale;
    // The full code is (q << pbits) | p; solve for q near the target.
    const f32 q_est = pbits != 0U ? (target - (f32)p) * 0.5F : target;
    u32 best = 0U;
    u32 best_err = 0xFFFFFFFFU;
    i32 base = (i32)q_est; // towards zero: one less for the negative half-step
    if ((f32)base > q_est) {
        --base;
    }
    for (i32 cand = base - 1; cand <= base + 2; ++cand) {
        if (cand < 0 || (u32)cand > max_code) {
            continue;
        }
        const u32 full = pbits != 0U ? (((u32)cand << 1U) | p) : (u32)cand;
        const i32 decoded = bc7_expand(full, total);
        const i32 d = decoded - (i32)(nph_f32_clamp(value, 0.0F, 255.0F) + 0.5F);
        const u32 err = (u32)(d * d);
        if (err < best_err) {
            best_err = err;
            best = (u32)cand;
        }
    }
    return best;
}

static void bc7_decode_endpoint(const u32 code[4], u32 pbit, const Bc7ModeInfo *m, Bc7Channels ch,
                                u8 out[4])
{
    for (u32 c = ch.first; c < ch.first + ch.count; ++c) {
        const u32 bits = c == 3U ? m->alpha_bits : m->color_bits;
        const u32 full = m->p_bits != 0U ? ((code[c] << 1U) | pbit) : code[c];
        out[c] = bc7_expand(full, m->p_bits != 0U ? bits + 1U : bits);
    }
}

// Assigns indices against the decoded endpoints and returns the error
// over the subset's texels.
static u32 bc7_assign(const u8 rgba[16][4], const u8 *member, u32 index_bits, Bc7Channels ch,
                      const u8 lo[4], const u8 hi[4], u8 indices[16])
{
    const u8 *weights = bc7_weights(index_bits);
    const u32 levels = 1U << index_bits;
    // The palette once per call rather than once per texel: the decoder's
    // integer interpolation, so the errors and the choices are unchanged.
    i32 palette[16][4];
    for (u32 k = 0; k < levels; ++k) {
        const u32 w = weights[k];
        for (u32 c = ch.first; c < ch.first + ch.count; ++c) {
            palette[k][c] = (i32)(((64U - w) * lo[c] + w * hi[c] + 32U) >> 6U);
        }
    }
    u32 error = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        if (!member[i]) {
            indices[i] = 0U;
            continue;
        }
        u32 best = 0U;
        u32 best_err = 0xFFFFFFFFU;
        for (u32 k = 0; k < levels; ++k) {
            u32 err = 0U;
            for (u32 c = ch.first; c < ch.first + ch.count; ++c) {
                const i32 d = palette[k][c] - (i32)rgba[i][c];
                err += (u32)(d * d);
            }
            if (err < best_err) {
                best_err = err;
                best = k;
            }
        }
        indices[i] = (u8)best;
        error += best_err;
    }
    return error;
}

// Fits the subset whose texels are flagged in `member`: PCA endpoints,
// every p-bit combination, least-squares passes.
static void bc7_fit_subset(const u8 rgba[16][4], const u8 *member, const Bc7ModeInfo *m,
                           Bc7Channels ch, u32 index_bits, u32 refine_passes, Bc7Subset *out)
{
    f32 pts[16 * 4];
    u32 count = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        if (member[i]) {
            for (u32 c = 0; c < 4U; ++c) {
                pts[count * 4U + c] = (f32)rgba[i][c];
            }
            ++count;
        }
    }
    f32 mean[4];
    f32 axis[4];
    f32 lo0[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 hi0[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    bc_pca(pts + ch.first, count, 4U, ch.count, mean, axis);
    bc_axis_extent(pts + ch.first, count, 4U, ch.count, mean, axis, lo0, hi0);
    // bc_pca and bc_axis_extent index channels from 0: shift into place.
    f32 lo[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 hi[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    for (u32 c = 0; c < ch.count; ++c) {
        lo[ch.first + c] = lo0[c];
        hi[ch.first + c] = hi0[c];
    }

    out->error = 0xFFFFFFFFU;
    const u32 pcombos = m->p_bits == 1U ? 4U : m->p_bits == 2U ? 2U : 1U;
    for (u32 combo = 0; combo < pcombos; ++combo) {
        Bc7Subset trial;
        memset(&trial, 0, sizeof trial);
        trial.pbit[0] = m->p_bits == 1U ? (combo & 1U) : combo;
        trial.pbit[1] = m->p_bits == 1U ? (combo >> 1U) : combo;
        f32 cur_lo[4];
        f32 cur_hi[4];
        memcpy(cur_lo, lo, sizeof cur_lo);
        memcpy(cur_hi, hi, sizeof cur_hi);
        u32 best_err = 0xFFFFFFFFU;
        Bc7Subset best;
        memset(&best, 0, sizeof best);
        for (u32 pass = 0; pass <= refine_passes; ++pass) {
            for (u32 c = ch.first; c < ch.first + ch.count; ++c) {
                const u32 bits = c == 3U ? m->alpha_bits : m->color_bits;
                trial.code[0][c] =
                    bc7_quantize(cur_lo[c], bits, m->p_bits != 0U ? 1U : 0U, trial.pbit[0]);
                trial.code[1][c] =
                    bc7_quantize(cur_hi[c], bits, m->p_bits != 0U ? 1U : 0U, trial.pbit[1]);
            }
            bc7_decode_endpoint(trial.code[0], trial.pbit[0], m, ch, trial.palette[0]);
            bc7_decode_endpoint(trial.code[1], trial.pbit[1], m, ch, trial.palette[1]);
            trial.error = bc7_assign(rgba, member, index_bits, ch, trial.palette[0],
                                     trial.palette[1], trial.indices);
            if (trial.error < best_err) {
                best_err = trial.error;
                best = trial;
            }
            if (pass == refine_passes || trial.error == 0U) {
                break;
            }
            // Refine against the current assignment.
            f32 w[16];
            const u8 *weights = bc7_weights(index_bits);
            u32 n = 0U;
            for (u32 i = 0; i < 16U; ++i) {
                if (member[i]) {
                    w[n++] = (f32)weights[trial.indices[i]] / 64.0F;
                }
            }
            f32 nlo[4];
            f32 nhi[4];
            memcpy(nlo, cur_lo, sizeof nlo);
            memcpy(nhi, cur_hi, sizeof nhi);
            f32 slo[4];
            f32 shi[4];
            for (u32 c = 0; c < ch.count; ++c) {
                slo[c] = nlo[ch.first + c];
                shi[c] = nhi[ch.first + c];
            }
            if (!bc_least_squares(pts + ch.first, count, 4U, ch.count, w, slo, shi)) {
                break;
            }
            for (u32 c = 0; c < ch.count; ++c) {
                cur_lo[ch.first + c] = slo[c];
                cur_hi[ch.first + c] = shi[c];
            }
        }
        if (best_err < out->error) {
            *out = best;
        }
    }
}

// Flips a subset's endpoints so its anchor texel's index has a clear top
// bit, as the format requires (the anchor's index is stored one bit short).
static void bc7_fix_anchor(Bc7Subset *s, u32 anchor, u32 index_bits)
{
    const u32 levels = 1U << index_bits;
    if (s->indices[anchor] < levels / 2U) {
        return;
    }
    for (u32 c = 0; c < 4U; ++c) {
        const u32 t = s->code[0][c];
        s->code[0][c] = s->code[1][c];
        s->code[1][c] = t;
        const u8 p = s->palette[0][c];
        s->palette[0][c] = s->palette[1][c];
        s->palette[1][c] = p;
    }
    const u32 tp = s->pbit[0];
    s->pbit[0] = s->pbit[1];
    s->pbit[1] = tp;
    for (u32 i = 0; i < 16U; ++i) {
        s->indices[i] = (u8)((levels - 1U) - s->indices[i]);
    }
}

typedef struct Bc7Candidate {
    u32 mode;
    u32 partition;
    u32 rotation;
    u32 index_selector;
    Bc7Subset subsets[3];
    Bc7Subset alpha; // modes 4 and 5: the alpha subset (channel 3)
    u32 error;
} Bc7Candidate;

static void bc7_write(const Bc7Candidate *cand, u8 out[16])
{
    const Bc7ModeInfo *m = &g_bc7_modes[cand->mode];
    memset(out, 0, 16U);
    u32 pos = 0U;
    bc_write_bits(out, &pos, cand->mode + 1U, 1U << cand->mode);
    bc_write_bits(out, &pos, m->partition_bits, cand->partition);
    bc_write_bits(out, &pos, m->rotation_bits, cand->rotation);
    bc_write_bits(out, &pos, m->index_selector_bits, cand->index_selector);
    const u32 channels = m->alpha_bits != 0U ? 4U : 3U;
    const bool separate_alpha = m->index2_bits != 0U;
    for (u32 c = 0; c < channels; ++c) {
        const u32 bits = c == 3U ? m->alpha_bits : m->color_bits;
        for (u32 s = 0; s < m->subsets; ++s) {
            const Bc7Subset *sub = (c == 3U && separate_alpha) ? &cand->alpha : &cand->subsets[s];
            bc_write_bits(out, &pos, bits, sub->code[0][c]);
            bc_write_bits(out, &pos, bits, sub->code[1][c]);
        }
    }
    if (m->p_bits == 1U) {
        for (u32 s = 0; s < m->subsets; ++s) {
            bc_write_bits(out, &pos, 1U, cand->subsets[s].pbit[0]);
            bc_write_bits(out, &pos, 1U, cand->subsets[s].pbit[1]);
        }
    } else if (m->p_bits == 2U) {
        for (u32 s = 0; s < m->subsets; ++s) {
            bc_write_bits(out, &pos, 1U, cand->subsets[s].pbit[0]);
        }
    }
    if (separate_alpha) {
        // Modes 4 and 5 store an index_bits-wide set and then an
        // index2_bits-wide one, texel 0 one bit short in each. Colour owns
        // the first set unless mode 4's index selector gives it the second
        // (and alpha the first); the fit chose the widths to match.
        const bool swap = cand->index_selector != 0U;
        const Bc7Subset *first = swap ? &cand->alpha : &cand->subsets[0];
        const Bc7Subset *second = swap ? &cand->subsets[0] : &cand->alpha;
        for (u32 i = 0; i < 16U; ++i) {
            bc_write_bits(out, &pos, i == 0U ? m->index_bits - 1U : m->index_bits,
                          first->indices[i]);
        }
        for (u32 i = 0; i < 16U; ++i) {
            bc_write_bits(out, &pos, i == 0U ? m->index2_bits - 1U : m->index2_bits,
                          second->indices[i]);
        }
        NPH_ASSERT(pos == 128U);
        return;
    }
    const u8 *parts = cook_bc7_partition(m->subsets, cand->partition);
    for (u32 i = 0; i < 16U; ++i) {
        const u32 s = parts[i];
        const bool anchor = i == cook_bc7_anchor(m->subsets, cand->partition, s);
        bc_write_bits(out, &pos, anchor ? m->index_bits - 1U : m->index_bits,
                      cand->subsets[s].indices[i]);
    }
    NPH_ASSERT(pos == 128U);
}

// Fits every subset of a partitioned mode (0, 1, 2, 3, 7).
static void bc7_fit_partitioned(const u8 rgba[16][4], u32 mode, u32 partition,
                                const CookBc7Settings *settings, Bc7Candidate *out)
{
    const Bc7ModeInfo *m = &g_bc7_modes[mode];
    const u8 *parts = cook_bc7_partition(m->subsets, partition);
    const Bc7Channels ch = {0U, m->alpha_bits != 0U ? 4U : 3U};
    memset(out, 0, sizeof *out);
    out->mode = mode;
    out->partition = partition;
    for (u32 s = 0; s < m->subsets; ++s) {
        u8 member[16];
        for (u32 i = 0; i < 16U; ++i) {
            member[i] = parts[i] == s;
        }
        bc7_fit_subset(rgba, member, m, ch, m->index_bits, settings->refine_passes,
                       &out->subsets[s]);
        bc7_fix_anchor(&out->subsets[s], cook_bc7_anchor(m->subsets, partition, s), m->index_bits);
        out->error += out->subsets[s].error;
    }
}

// Per-block moments for the partition estimates, 16 integer lanes per
// texel: its four channels, the products of every channel pair, and a 1
// that counts it. A subset's lane sums give n times its covariance exactly
// (n S2 - S1 S1^T, at most 16 * 16 * 255^2 < 2^31), so ranking the 64
// partitions costs masked lane sums (which the compiler vectorises) and a
// 3x3 or 4x4 eigenvalue each, instead of a pass over gathered texels.
#define BC7_LANES           16U
#define BC7_LANE_SUM2       4U // the products 00 01 02 03 11 12 13 22 23 33 start here
#define BC7_LANE_COUNT      14U
#define BC7_RANK_ITERATIONS 4U

typedef struct Bc7Moments {
    i32 lane[16][BC7_LANES];
    i32 total[BC7_LANES]; // over the whole block
} Bc7Moments;

static void bc7_moments(const u8 rgba[16][4], Bc7Moments *out)
{
    memset(out, 0, sizeof *out);
    for (u32 i = 0; i < 16U; ++i) {
        u32 k = BC7_LANE_SUM2;
        for (u32 a = 0; a < 4U; ++a) {
            out->lane[i][a] = rgba[i][a];
            for (u32 b = a; b < 4U; ++b) {
                out->lane[i][k++] = (i32)rgba[i][a] * (i32)rgba[i][b];
            }
        }
        out->lane[i][BC7_LANE_COUNT] = 1;
        for (u32 k2 = 0; k2 < BC7_LANES; ++k2) {
            out->total[k2] += out->lane[i][k2];
        }
    }
}

// The squared distance of a subset's texels from their principal axis:
// trace(C) - axis^T C axis for the scattering matrix C (the covariance
// times n), with the axis found by power iteration as in bc_pca.
static f32 bc7_subset_residual(const i32 m[BC7_LANES], u32 channels)
{
    static const u8 pair[4][4] = {{0, 1, 2, 3}, {1, 4, 5, 6}, {2, 5, 7, 8}, {3, 6, 8, 9}};
    const i32 n = m[BC7_LANE_COUNT];
    if (n < 2 || channels == 0U || channels > 4U) {
        return 0.0F; // a single texel lies on its line exactly
    }
    f32 cov[4][4];
    f32 trace = 0.0F;
    for (u32 a = 0; a < channels; ++a) {
        for (u32 b = 0; b < channels; ++b) {
            cov[a][b] = (f32)(n * m[BC7_LANE_SUM2 + pair[a][b]] - m[a] * m[b]);
        }
        trace += cov[a][a];
    }
    u32 k = 0U;
    for (u32 c = 1; c < channels; ++c) {
        if (cov[c][c] > cov[k][k]) {
            k = c;
        }
    }
    if (cov[k][k] <= 0.0F) {
        return 0.0F; // every texel the same colour
    }
    f32 v[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    for (u32 c = 0; c < channels; ++c) {
        v[c] = cov[c][k];
    }
    for (u32 it = 0; it < BC7_RANK_ITERATIONS; ++it) {
        f32 w[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        for (u32 a = 0; a < channels; ++a) {
            for (u32 b = 0; b < channels; ++b) {
                w[a] += cov[a][b] * v[b];
            }
        }
        f32 len = 0.0F;
        for (u32 c = 0; c < channels; ++c) {
            len += w[c] * w[c];
        }
        if (len <= 1e-12F) {
            return trace / (f32)n;
        }
        len = nph_f32_sqrt(len);
        for (u32 c = 0; c < channels; ++c) {
            v[c] = w[c] / len;
        }
    }
    f32 along = 0.0F; // axis^T C axis
    for (u32 a = 0; a < channels; ++a) {
        f32 row = 0.0F;
        for (u32 b = 0; b < channels; ++b) {
            row += cov[a][b] * v[b];
        }
        along += v[a] * row;
    }
    return nph_f32_max(trace - along, 0.0F) / (f32)n;
}

// A cheap estimate of a partition's fit: the residual of each subset
// around its principal axis, with no quantisation.
static f32 bc7_estimate_partition(const Bc7Moments *mo, u32 subsets, u32 partition, u32 channels)
{
    const u8 *parts = cook_bc7_partition(subsets, partition);
    i32 acc[2][BC7_LANES]; // subsets 1 and 2; subset 0 is the rest of the block
    memset(acc, 0, sizeof acc);
    for (u32 i = 0; i < 16U; ++i) {
        // Branch-free masks keep the lane loop a straight vector add.
        const i32 in1 = -(i32)(parts[i] == 1U);
        const i32 in2 = -(i32)(parts[i] == 2U);
        for (u32 k = 0; k < BC7_LANES; ++k) {
            acc[0][k] += mo->lane[i][k] & in1;
            acc[1][k] += mo->lane[i][k] & in2;
        }
    }
    i32 first[BC7_LANES];
    for (u32 k = 0; k < BC7_LANES; ++k) {
        first[k] = mo->total[k] - acc[0][k] - acc[1][k];
    }
    f32 total = bc7_subset_residual(first, channels) + bc7_subset_residual(acc[0], channels);
    if (subsets == 3U) {
        total += bc7_subset_residual(acc[1], channels);
    }
    return total;
}

// Partitions in order of their estimated fit, best first. Modes with the
// same subset and channel counts (1 and 3; 0 and 2) share one ranking, so
// the encoder computes each at most once per block.
typedef struct Bc7Ranking {
    b32 valid;
    u32 count; // partitions ranked: 16 for mode 0's four partition bits, else 64
    u32 order[64];
} Bc7Ranking;

static void bc7_rank_partitions(const u8 rgba[16][4], u32 subsets, u32 channels, u32 count,
                                Bc7Ranking *out)
{
    Bc7Moments mo;
    bc7_moments(rgba, &mo);
    f32 estimate[64];
    for (u32 p = 0; p < count; ++p) {
        estimate[p] = bc7_estimate_partition(&mo, subsets, p, channels);
        out->order[p] = p;
    }
    for (u32 i = 1; i < count; ++i) { // insertion sort: 64 elements at most, stable
        const u32 key = out->order[i];
        u32 j = i;
        while (j > 0U && estimate[out->order[j - 1U]] > estimate[key]) {
            out->order[j] = out->order[j - 1U];
            --j;
        }
        out->order[j] = key;
    }
    out->count = count;
    out->valid = 1;
}

static void bc7_try_partitioned(const u8 rgba[16][4], u32 mode, const CookBc7Settings *settings,
                                Bc7Ranking rankings[2][2], Bc7Candidate *best)
{
    const Bc7ModeInfo *m = &g_bc7_modes[mode];
    const u32 partitions = 1U << m->partition_bits;
    const u32 channels = m->alpha_bits != 0U ? 4U : 3U;
    // Rank every partition by the cheap estimate, then encode the best few.
    // Mode 0 addresses only the first 16 three-subset partitions, so its
    // ranking is its own; the others share theirs with the sibling mode.
    Bc7Ranking local;
    Bc7Ranking *ranking = &local;
    if (partitions == 64U) {
        ranking = &rankings[m->subsets - 2U][channels - 3U];
    }
    if (ranking == &local || !ranking->valid) {
        bc7_rank_partitions(rgba, m->subsets, channels, partitions, ranking);
    }
    const u32 tries = nph_min_u32(settings->max_partitions, partitions);
    for (u32 t = 0; t < tries; ++t) {
        Bc7Candidate cand;
        bc7_fit_partitioned(rgba, mode, ranking->order[t], settings, &cand);
        if (cand.error < best->error) {
            *best = cand;
        }
        if (best->error == 0U) {
            return;
        }
    }
}

// Mode 6: one subset of RGBA with 4-bit indices.
static void bc7_try_mode6(const u8 rgba[16][4], const CookBc7Settings *settings, Bc7Candidate *best)
{
    Bc7Candidate cand;
    bc7_fit_partitioned(rgba, 6U, 0U, settings, &cand);
    if (cand.error < best->error) {
        *best = cand;
    }
}

// Modes 4 and 5: colour and alpha fitted separately, every rotation, and
// (mode 4) both index selectors.
static void bc7_try_rotated(const u8 rgba[16][4], u32 mode, const CookBc7Settings *settings,
                            Bc7Candidate *best)
{
    const Bc7ModeInfo *m = &g_bc7_modes[mode];
    static const u8 all[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    for (u32 rotation = 0; rotation < 4U; ++rotation) {
        CookBlockRgba8 rotated;
        memcpy(rotated.texels, rgba, sizeof rotated.texels);
        if (rotation != 0U) {
            for (u32 i = 0; i < 16U; ++i) {
                const u8 t = rotated.texels[i][3];
                rotated.texels[i][3] = rotated.texels[i][rotation - 1U];
                rotated.texels[i][rotation - 1U] = t;
            }
        }
        // Through a pointer to const the member array is `const u8
        // [16][4]`, which the fitting takes (see CookBlockRgba8).
        const CookBlockRgba8 *view = &rotated;
        const u32 selectors = m->index_selector_bits != 0U ? 2U : 1U;
        for (u32 sel = 0; sel < selectors; ++sel) {
            u32 color_bits = m->index_bits;
            u32 alpha_bits = m->index2_bits;
            if (sel != 0U) {
                const u32 t = color_bits;
                color_bits = alpha_bits;
                alpha_bits = t;
            }
            Bc7Candidate cand;
            memset(&cand, 0, sizeof cand);
            cand.mode = mode;
            cand.rotation = rotation;
            cand.index_selector = sel;
            const Bc7Channels color = {0U, 3U};
            const Bc7Channels alpha = {3U, 1U};
            bc7_fit_subset(view->texels, all, m, color, color_bits, settings->refine_passes,
                           &cand.subsets[0]);
            bc7_fix_anchor(&cand.subsets[0], 0U, color_bits);
            bc7_fit_subset(view->texels, all, m, alpha, alpha_bits, settings->refine_passes,
                           &cand.alpha);
            bc7_fix_anchor(&cand.alpha, 0U, alpha_bits);
            cand.error = cand.subsets[0].error + cand.alpha.error;
            if (cand.error < best->error) {
                *best = cand;
            }
        }
    }
}

void cook_bc7_encode(const CookBlockRgba8 *in, const CookBc7Settings *settings_in, u8 out[16])
{
    const u8(*rgba)[4] = in->texels;
    CookBc7Settings settings;
    if (settings_in != NULL) {
        settings = *settings_in;
    } else {
        memset(&settings, 0, sizeof settings);
    }
    if (settings.mode_mask == 0U) {
        settings.mode_mask = COOK_BC7_DEFAULT_MODES;
    }
    if (settings.max_partitions == 0U) {
        settings.max_partitions = 8U;
    }
    settings.max_partitions = nph_min_u32(settings.max_partitions, 64U);
    if (settings.refine_passes == 0U) {
        settings.refine_passes = 2U;
    }
    bool opaque = true;
    for (u32 i = 0; i < 16U; ++i) {
        if (rgba[i][3] != 255U) {
            opaque = false;
            break;
        }
    }
    Bc7Candidate best;
    memset(&best, 0, sizeof best);
    best.error = 0xFFFFFFFFU;
    Bc7Ranking rankings[2][2]; // [subsets - 2][channels - 3]
    memset(rankings, 0, sizeof rankings);
    // Opaque blocks are best served by the opaque modes; the alpha modes
    // spend bits on an alpha they would only reconstruct as 255.
    const u32 mask = settings.mode_mask & (opaque ? 0x4FU : 0xF0U);
    for (u32 mode = 0; mode < 8U && best.error != 0U; ++mode) {
        if (((mask >> mode) & 1U) == 0U) {
            continue;
        }
        if (mode == 6U) {
            bc7_try_mode6(rgba, &settings, &best);
        } else if (mode == 4U || mode == 5U) {
            bc7_try_rotated(rgba, mode, &settings, &best);
        } else {
            bc7_try_partitioned(rgba, mode, &settings, rankings, &best);
        }
    }
    if (best.error == 0xFFFFFFFFU) {
        // No mode allowed by the mask fits this block's alpha: fall back
        // to mode 6, which handles anything.
        bc7_try_mode6(rgba, &settings, &best);
    }
    bc7_write(&best, out);
}

// ---------------------------------------------------------------------------
// BC6H unsigned, mode 11
// ---------------------------------------------------------------------------

// The 16-bit "unquantised" domain of the format, from half-float bits.
static f32 bc6h_to_unq(u16 half)
{
    return (f32)half * (64.0F / 31.0F);
}

// Quantises an unquantised-domain value to 10 bits, choosing the code
// whose unquantised value is nearest.
static u32 bc6h_quantize10(f32 unq)
{
    const f32 clamped = nph_f32_clamp(unq, 0.0F, 65535.0F);
    const i32 base = (i32)nph_f32_floor(clamped / 64.0F);
    u32 best = 0U;
    f32 best_err = NPH_F32_MAX;
    for (i32 cand = base - 1; cand <= base + 2; ++cand) {
        if (cand < 0 || cand > 1023) {
            continue;
        }
        const f32 d = (f32)bc6h_unquantize_unsigned((u32)cand, 10U) - clamped;
        if (d * d < best_err) {
            best_err = d * d;
            best = (u32)cand;
        }
    }
    return best;
}

// Error in the half-float-bit domain of the texels against a palette.
static u64 bc6h_assign(const u16 rgb[16][3], const u32 lo[3], const u32 hi[3], u8 indices[16])
{
    u64 error = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        u32 best = 0U;
        u64 best_err = NPH_U64_MAX;
        for (u32 k = 0; k < 16U; ++k) {
            const u32 w = g_bc7_weights4[k];
            u64 err = 0U;
            for (u32 c = 0; c < 3U; ++c) {
                const u32 v = ((64U - w) * lo[c] + w * hi[c] + 32U) >> 6U;
                const i64 d = (i64)bc6h_finish_unsigned(v) - (i64)rgb[i][c];
                err += (u64)(d * d);
            }
            if (err < best_err) {
                best_err = err;
                best = k;
            }
        }
        indices[i] = (u8)best;
        error += best_err;
    }
    return error;
}

void cook_bc6h_encode(const CookBlockRgbHalf *in, u8 out[16])
{
    const u16(*rgb)[3] = in->texels;
    f32 pts[16 * 3];
    for (u32 i = 0; i < 16U; ++i) {
        for (u32 c = 0; c < 3U; ++c) {
            pts[i * 3U + c] = bc6h_to_unq(rgb[i][c]);
        }
    }
    f32 mean[4];
    f32 axis[4];
    f32 lo[4];
    f32 hi[4];
    bc_pca(pts, 16U, 3U, 3U, mean, axis);
    bc_axis_extent(pts, 16U, 3U, 3U, mean, axis, lo, hi);

    u32 best_code[2][3] = {{0, 0, 0}, {0, 0, 0}};
    u8 best_indices[16];
    memset(best_indices, 0, sizeof best_indices);
    u64 best_err = NPH_U64_MAX;
    for (u32 pass = 0; pass < 4U; ++pass) {
        u32 code[2][3];
        u32 unq[2][3];
        for (u32 c = 0; c < 3U; ++c) {
            code[0][c] = bc6h_quantize10(lo[c]);
            code[1][c] = bc6h_quantize10(hi[c]);
            unq[0][c] = bc6h_unquantize_unsigned(code[0][c], 10U);
            unq[1][c] = bc6h_unquantize_unsigned(code[1][c], 10U);
        }
        u8 indices[16];
        const u64 err = bc6h_assign(rgb, unq[0], unq[1], indices);
        if (err < best_err) {
            best_err = err;
            memcpy(best_code, code, sizeof best_code);
            memcpy(best_indices, indices, sizeof best_indices);
        }
        if (err == 0U) {
            break;
        }
        f32 w[16];
        for (u32 i = 0; i < 16U; ++i) {
            w[i] = (f32)g_bc7_weights4[indices[i]] / 64.0F;
        }
        if (!bc_least_squares(pts, 16U, 3U, 3U, w, lo, hi)) {
            break;
        }
    }
    // The anchor (texel 0) stores three bits: its index must be below 8.
    if (best_indices[0] >= 8U) {
        for (u32 c = 0; c < 3U; ++c) {
            const u32 t = best_code[0][c];
            best_code[0][c] = best_code[1][c];
            best_code[1][c] = t;
        }
        for (u32 i = 0; i < 16U; ++i) {
            best_indices[i] = (u8)(15U - best_indices[i]);
        }
    }
    memset(out, 0, 16U);
    u32 pos = 0U;
    bc_write_bits(out, &pos, 5U, BC6H_MODE11_BITS);
    for (u32 c = 0; c < 3U; ++c) {
        bc_write_bits(out, &pos, 10U, best_code[0][c]);
    }
    for (u32 c = 0; c < 3U; ++c) {
        bc_write_bits(out, &pos, 10U, best_code[1][c]);
    }
    for (u32 i = 0; i < 16U; ++i) {
        bc_write_bits(out, &pos, i == 0U ? 3U : 4U, best_indices[i]);
    }
}

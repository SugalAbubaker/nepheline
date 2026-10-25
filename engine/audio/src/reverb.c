// SPDX-License-Identifier: Apache-2.0
// The reverberator (reverb.h).

#include <nepheline/audio/filter.h>
#include <nepheline/audio/reverb.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

#define RV_PI        3.14159265358979323846
#define RV_N         NPH_AUDIO_REVERB_LINES
#define RV_TAPS      NPH_AUDIO_REVERB_TAPS
#define RV_BLOCK     NPH_AUDIO_BLOCK_FRAMES
#define RV_MAX_SIZE  2.0F
#define RV_MIN_SIZE  0.25F
#define RV_WANDER_MS 0.6F // the wandering lines' largest excursion
#define RV_LFO_HZ    0.5F

// The lines' lengths at size 1 (ms), spread so that no two share a short
// common period; each becomes the nearest prime number of frames above.
static const f32 k_rv_line_ms[RV_N] = {29.7F, 31.3F, 33.9F, 37.1F, 41.1F, 43.7F, 47.3F, 51.9F,
                                       53.7F, 59.3F, 61.1F, 67.9F, 71.3F, 73.9F, 79.1F, 83.7F};
// The lines that wander; the k-th is a quarter turn of the LFO behind the
// one before.
static const u32 k_rv_wander[4] = {1U, 5U, 10U, 14U};
// The diffusers: Dattorro's input all-passes (142, 107, 379, and 277 frames
// at 29761 Hz) and gains (0.75, 0.75, 0.625, 0.625, scaled by diffusion).
static const f32 k_rv_diffuser_ms[4] = {4.7713F, 3.5953F, 12.7348F, 9.3075F};
static const f32 k_rv_diffuser_gain[4] = {0.75F, 0.75F, 0.625F, 0.625F};
// The early reflections at size 1 (ms after the pre-delay); tap t falls to
// 0.9 * 0.82^t, the even ones on the left, the odd ones on the right.
static const f32 k_rv_tap_ms[RV_TAPS] = {7.1F, 11.3F, 17.9F, 23.7F, 31.1F, 37.9F, 46.3F, 57.1F};

NphAudioReverbParams nph_audio_reverb_params_default(void)
{
    NphAudioReverbParams p;
    p.size = 1.0F;
    p.decay_s = 1.6F;
    p.damping = 0.5F;
    p.crossover_hz = 3000.0F;
    p.predelay_ms = 12.0F;
    p.diffusion = 0.8F;
    p.modulation = 0.3F;
    p.early = 0.3F;
    p.late = 0.5F;
    p.width = 1.0F;
    p.dry = 0.0F;
    return p;
}

static f32 rv_clamp(f32 x, f32 lo, f32 hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static bool rv_prime(u32 n)
{
    if (n < 2U) {
        return false;
    }
    for (u32 d = 2U; d * d <= n; ++d) {
        if (n % d == 0U) {
            return false;
        }
    }
    return true;
}

// The lines' lengths for `size`: the nearest primes at or above the
// scaled lengths, each above the last.
static void rv_lengths(const NphAudioReverb *r, f32 size, u32 out[RV_N])
{
    u32 last = 0U;
    for (u32 i = 0U; i < RV_N; ++i) {
        u32 n = (u32)(k_rv_line_ms[i] * size * 0.001F * r->rate + 0.5F);
        n = n > last ? n : last + 1U;
        while (!rv_prime(n)) {
            n += 1U;
        }
        out[i] = n;
        last = n;
    }
}

static u32 rv_predelay_frames(const NphAudioReverb *r)
{
    return (u32)(r->params.predelay_ms * 0.001F * r->rate);
}

// The early taps' distances from the newest input for a size and pre-delay.
static void rv_taps(const NphAudioReverb *r, f32 size, u32 predelay, u32 out[RV_TAPS])
{
    for (u32 t = 0U; t < RV_TAPS; ++t) {
        out[t] = predelay + 1U + (u32)(k_rv_tap_ms[t] * size * 0.001F * r->rate);
    }
}

// The decay shelves: line i loses 60 dB in decay at DC and in decay *
// damping at Nyquist (Jot 1992), crossing over at the corner. The shelf is
// g_low times the bilinear one-pole low-pass plus g_high times its
// complementary high-pass, so its gains at DC and Nyquist are exact.
static void rv_shelves(NphAudioReverb *r)
{
    const f64 t60_low = (f64)r->decay_now;
    const f64 t60_high = (f64)(r->decay_now * r->damping_now);
    const f64 w = tan(RV_PI * (f64)r->crossover_now / (f64)r->rate);
    const f64 p = (1.0 - w) / (1.0 + w);
    for (u32 i = 0U; i < RV_N; ++i) {
        const f64 m = (f64)r->length[i];
        const f64 lo = pow(10.0, -3.0 * m / ((f64)r->rate * t60_low));
        const f64 hi = pow(10.0, -3.0 * m / ((f64)r->rate * t60_high));
        r->b0[i] = (f32)(lo * (1.0 - p) * 0.5 + hi * (1.0 + p) * 0.5);
        r->b1[i] = (f32)(lo * (1.0 - p) * 0.5 - hi * (1.0 + p) * 0.5);
        r->a1[i] = (f32)(-p);
    }
}

void nph_audio_reverb_set(NphAudioReverb *r, const NphAudioReverbParams *params)
{
    NPH_ASSERT(r != NULL && params != NULL);
    NphAudioReverbParams p = *params;
    p.size = rv_clamp(p.size, RV_MIN_SIZE, RV_MAX_SIZE);
    p.decay_s = rv_clamp(p.decay_s, 0.1F, 30.0F);
    p.damping = rv_clamp(p.damping, 0.05F, 1.0F);
    p.crossover_hz = rv_clamp(p.crossover_hz, 100.0F, 10000.0F);
    p.predelay_ms = rv_clamp(p.predelay_ms, 0.0F, 250.0F);
    p.diffusion = rv_clamp(p.diffusion, 0.0F, 1.0F);
    p.modulation = rv_clamp(p.modulation, 0.0F, 1.0F);
    p.width = rv_clamp(p.width, 0.0F, 1.0F);
    r->params = p;
}

NphResult nph_audio_reverb_init(NphAudioReverb *r, NphAllocator *alloc, u32 channels, f32 rate,
                                const NphAudioReverbParams *params)
{
    NPH_ASSERT(r != NULL && params != NULL);
    memset(r, 0, sizeof *r);
    if (alloc == NULL || channels < 1U || channels > NPH_AUDIO_MAX_CHANNELS ||
        !(rate >= (f32)NPH_AUDIO_MIN_RATE && rate <= (f32)NPH_AUDIO_MAX_RATE)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    r->alloc = alloc;
    r->rate = rate;
    r->channels = channels;
    nph_audio_reverb_set(r, params);
    // The lines hold the longest length at the largest size, and the
    // wandering on top.
    u32 longest[RV_N];
    rv_lengths(r, RV_MAX_SIZE, longest);
    const u32 wander = (u32)ceilf(RV_WANDER_MS * 0.001F * rate) * 2U + 4U;
    NphResult res = NPH_RESULT_OK;
    for (u32 i = 0U; i < RV_N && res == NPH_RESULT_OK; ++i) {
        res = nph_audio_delay_init(&r->lines[i], alloc, longest[i] + wander);
    }
    const u32 pre =
        (u32)ceilf((250.0F + k_rv_tap_ms[RV_TAPS - 1U] * RV_MAX_SIZE) * 0.001F * rate) + 4U;
    if (res == NPH_RESULT_OK) {
        res = nph_audio_delay_init(&r->predelay, alloc, pre);
    }
    for (u32 d = 0U; d < 4U && res == NPH_RESULT_OK; ++d) {
        r->diffuser_length[d] = (u32)(k_rv_diffuser_ms[d] * 0.001F * rate + 0.5F);
        r->diffuser_length[d] = r->diffuser_length[d] > 1U ? r->diffuser_length[d] : 1U;
        res = nph_audio_delay_init(&r->diffuser[d], alloc, r->diffuser_length[d]);
    }
    if (res == NPH_RESULT_OK) {
        r->work = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, (u64)RV_N * RV_BLOCK);
        r->input = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, RV_BLOCK);
        r->early_l = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, RV_BLOCK);
        r->early_r = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, RV_BLOCK);
        r->scratch = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, RV_BLOCK);
        if (r->work == NULL || r->input == NULL || r->early_l == NULL || r->early_r == NULL ||
            r->scratch == NULL) {
            res = NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    if (res != NPH_RESULT_OK) {
        nph_audio_reverb_free(r);
        return res;
    }
    r->size_now = r->params.size;
    r->size_from = r->size_now;
    rv_lengths(r, r->size_now, r->length);
    memcpy(r->length_from, r->length, sizeof r->length);
    r->predelay_now = rv_predelay_frames(r);
    r->predelay_from = r->predelay_now;
    r->decay_now = r->params.decay_s;
    r->damping_now = r->params.damping;
    r->crossover_now = r->params.crossover_hz;
    r->early_now = r->params.early;
    r->late_now = r->params.late;
    r->dry_now = r->params.dry;
    r->width_now = r->params.width;
    r->lfo_step = (f32)(2.0 * RV_PI * (f64)RV_LFO_HZ / (f64)rate);
    // Output c reads the lines through Hadamard row c + 1 (row 0, all ones,
    // feeds the input in): rows are orthogonal, so outputs are uncorrelated.
    for (u32 c = 0U; c < channels; ++c) {
        for (u32 i = 0U; i < RV_N; ++i) {
            u32 v = (c + 1U) & i;
            u32 parity = 0U;
            while (v != 0U) {
                parity ^= v & 1U;
                v >>= 1U;
            }
            r->out_sign[c][i] = parity != 0U ? -0.25F : 0.25F;
        }
    }
    rv_shelves(r);
    return NPH_RESULT_OK;
}

void nph_audio_reverb_free(NphAudioReverb *r)
{
    if (r == NULL || r->alloc == NULL) {
        return;
    }
    NphAllocator *a = r->alloc;
    for (u32 i = 0U; i < RV_N; ++i) {
        nph_audio_delay_free(&r->lines[i]);
    }
    nph_audio_delay_free(&r->predelay);
    for (u32 d = 0U; d < 4U; ++d) {
        nph_audio_delay_free(&r->diffuser[d]);
    }
    f32 *buffers[5] = {r->input, r->early_l, r->early_r, r->scratch, NULL};
    for (u32 b = 0U; b < 4U; ++b) {
        if (buffers[b] != NULL) {
            NPH_FREE_ARRAY(a, f32, buffers[b], RV_BLOCK);
        }
    }
    if (r->work != NULL) {
        NPH_FREE_ARRAY(a, f32, r->work, (u64)RV_N * RV_BLOCK);
    }
    memset(r, 0, sizeof *r);
}

void nph_audio_reverb_reset(NphAudioReverb *r)
{
    NPH_ASSERT(r != NULL);
    for (u32 i = 0U; i < RV_N; ++i) {
        nph_audio_delay_clear(&r->lines[i]);
    }
    nph_audio_delay_clear(&r->predelay);
    for (u32 d = 0U; d < 4U; ++d) {
        nph_audio_delay_clear(&r->diffuser[d]);
    }
    memset(r->shelf_x, 0, sizeof r->shelf_x);
    memset(r->shelf_y, 0, sizeof r->shelf_y);
    memcpy(r->length_from, r->length, sizeof r->length);
    r->size_from = r->size_now;
    r->predelay_from = r->predelay_now;
    r->fading = 0;
}

// A line's output `d` frames back (fractional; d >= 2), by Catmull-Rom.
static f32 rv_tap_frac(const NphAudioDelay *line, f32 d)
{
    const u32 k = (u32)d;
    const f32 t = d - (f32)k;
    const f32 xm = nph_audio_delay_tap(line, k - 1U);
    const f32 x0 = nph_audio_delay_tap(line, k);
    const f32 x1 = nph_audio_delay_tap(line, k + 1U);
    const f32 x2 = nph_audio_delay_tap(line, k + 2U);
    const f32 c1 = 0.5F * (x1 - xm);
    const f32 c2 = xm - 2.5F * x0 + 2.0F * x1 - 0.5F * x2;
    const f32 c3 = 0.5F * (x2 - xm) + 1.5F * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// Line i's next `n` output frames into `out`, at `length` (and wandering).
static void rv_read_line(const NphAudioReverb *r, u32 i, u32 length, f32 depth, f32 *out, u32 n)
{
    const NphAudioDelay *line = &r->lines[i];
    u32 w = 4U;
    for (u32 k = 0U; k < 4U; ++k) {
        w = k_rv_wander[k] == i ? k : w;
    }
    if (w == 4U || depth <= 0.0F) {
        nph_audio_delay_read(line, length, out, n);
        return;
    }
    // Frame k of the block is (d - k) frames before the next write.
    const f32 offset = (f32)w * (f32)(RV_PI * 0.5);
    for (u32 k = 0U; k < n; ++k) {
        const f32 phase = r->lfo_phase + offset + r->lfo_step * (f32)k;
        const f32 d = (f32)length + depth * (1.0F + sinf(phase)) - (f32)k;
        out[k] = rv_tap_frac(line, d);
    }
}

// The 16 x 16 Hadamard transform of the lines' frames, in place: four
// stages of butterflies (the caller takes the quarter that makes it
// orthonormal).
static void rv_hadamard(f32 *work, u32 n)
{
    for (u32 h = 1U; h < RV_N; h <<= 1U) {
        for (u32 i = 0U; i < RV_N; i += h << 1U) {
            for (u32 j = i; j < i + h; ++j) {
                f32 *a = work + (u64)j * RV_BLOCK;
                f32 *b = work + (u64)(j + h) * RV_BLOCK;
                for (u32 k = 0U; k < n; ++k) {
                    const f32 x = a[k];
                    const f32 y = b[k];
                    a[k] = x + y;
                    b[k] = x - y;
                }
            }
        }
    }
}

static f32 rv_smoothstep(f32 t)
{
    return t * t * (3.0F - 2.0F * t);
}

typedef struct RvBlock {
    u32 offset; // where this sub-block starts in the block
    f32 inv;    // 1 / the block's frames
    u32 taps_now[RV_TAPS];
    u32 taps_from[RV_TAPS];
} RvBlock;

static void rv_sub_block(NphAudioReverb *r, f32 *const *planes, u32 n, const RvBlock *blk)
{
    const u32 channels = r->channels;
    const f32 inv_channels = 1.0F / (f32)channels;
    const bool fading = r->fading != 0;
    f32 dg[4];
    for (u32 d = 0U; d < 4U; ++d) {
        dg[d] = k_rv_diffuser_gain[d] * r->params.diffusion;
    }
    // The input: pre-delay, early reflections, diffusion. While a new size
    // or pre-delay fades in, the old taps fade out.
    for (u32 k = 0U; k < n; ++k) {
        f32 in = 0.0F;
        for (u32 c = 0U; c < channels; ++c) {
            in += planes[c][k];
        }
        nph_audio_delay_push(&r->predelay, in * inv_channels);
        const f32 s = fading ? rv_smoothstep((f32)(blk->offset + k + 1U) * blk->inv) : 1.0F;
        f32 el = 0.0F;
        f32 er = 0.0F;
        f32 level = 0.9F;
        for (u32 t = 0U; t < RV_TAPS; ++t) {
            f32 v = nph_audio_delay_tap(&r->predelay, blk->taps_now[t]);
            if (fading) {
                const f32 old = nph_audio_delay_tap(&r->predelay, blk->taps_from[t]);
                v = old + (v - old) * s;
            }
            v *= level;
            el += (t & 1U) == 0U ? v : 0.0F;
            er += (t & 1U) == 0U ? 0.0F : v;
            level *= 0.82F;
        }
        r->early_l[k] = el;
        r->early_r[k] = er;
        f32 x = nph_audio_delay_tap(&r->predelay, r->predelay_now + 1U);
        if (fading) {
            const f32 old = nph_audio_delay_tap(&r->predelay, r->predelay_from + 1U);
            x = old + (x - old) * s;
        }
        for (u32 d = 0U; d < 4U; ++d) {
            const f32 delayed = nph_audio_delay_tap(&r->diffuser[d], r->diffuser_length[d]);
            const f32 v = x + dg[d] * delayed;
            x = delayed - dg[d] * v;
            nph_audio_delay_push(&r->diffuser[d], v);
        }
        r->input[k] = x;
    }
    // The lines' outputs (crossfading from the old lengths while a size
    // fades in), through their decay shelves.
    const f32 depth = r->params.modulation * RV_WANDER_MS * 0.001F * r->rate;
    for (u32 i = 0U; i < RV_N; ++i) {
        f32 *y = r->work + (u64)i * RV_BLOCK;
        rv_read_line(r, i, r->length[i], depth, y, n);
        if (fading && r->length_from[i] != r->length[i]) {
            rv_read_line(r, i, r->length_from[i], depth, r->scratch, n);
            for (u32 k = 0U; k < n; ++k) {
                const f32 s = rv_smoothstep((f32)(blk->offset + k + 1U) * blk->inv);
                y[k] = r->scratch[k] + (y[k] - r->scratch[k]) * s;
            }
        }
        const f32 b0 = r->b0[i];
        const f32 b1 = r->b1[i];
        const f32 a1 = r->a1[i];
        f32 x1 = r->shelf_x[i];
        f32 y1 = r->shelf_y[i];
        for (u32 k = 0U; k < n; ++k) {
            const f32 x = y[k];
            const f32 v = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = v;
            y[k] = v;
        }
        r->shelf_x[i] = x1;
        r->shelf_y[i] = y1;
    }
    // The outputs: each channel its own Hadamard row, blended towards their
    // mean by the width (the mean scaled back to the rows' power).
    const f32 w = r->width_now;
    const f32 mean_scale = inv_channels * sqrtf((f32)channels);
    for (u32 k = 0U; k < n; ++k) {
        f32 own[NPH_AUDIO_MAX_CHANNELS];
        f32 mean = 0.0F;
        for (u32 c = 0U; c < channels; ++c) {
            f32 sum = 0.0F;
            for (u32 i = 0U; i < RV_N; ++i) {
                sum += r->out_sign[c][i] * r->work[(u64)i * RV_BLOCK + k];
            }
            own[c] = sum;
            mean += sum;
        }
        mean *= mean_scale;
        const f32 t = (f32)(blk->offset + k) * blk->inv;
        const f32 dry = r->dry_now + (r->params.dry - r->dry_now) * t;
        const f32 early = r->early_now + (r->params.early - r->early_now) * t;
        const f32 late = r->late_now + (r->params.late - r->late_now) * t;
        for (u32 c = 0U; c < channels; ++c) {
            const f32 e = channels == 1U ? 0.5F * (r->early_l[k] + r->early_r[k])
                                         : ((c & 1U) == 0U ? r->early_l[k] : r->early_r[k]);
            const f32 tail = w * own[c] + (1.0F - w) * mean;
            planes[c][k] = dry * planes[c][k] + early * e + late * tail;
        }
    }
    // The feedback: mixed by the matrix, joined by the input (Hadamard row
    // 0), a quarter of each to keep the transform orthonormal.
    rv_hadamard(r->work, n);
    for (u32 i = 0U; i < RV_N; ++i) {
        f32 *v = r->work + (u64)i * RV_BLOCK;
        for (u32 k = 0U; k < n; ++k) {
            v[k] = 0.25F * (v[k] + r->input[k]);
        }
        nph_audio_delay_write(&r->lines[i], v, n);
    }
    r->lfo_phase += r->lfo_step * (f32)n;
    if (r->lfo_phase > (f32)(2.0 * RV_PI)) {
        r->lfo_phase -= (f32)(2.0 * RV_PI);
    }
}

void nph_audio_reverb_process(NphAudioReverb *r, f32 *const *planes, u32 frames)
{
    NPH_ASSERT(r != NULL && (planes != NULL || frames == 0U));
    if (frames == 0U) {
        return;
    }
    // The gliding parameters move once a block, a 50 ms time constant.
    const f32 k = 1.0F - expf(-(f32)frames / (0.05F * r->rate));
    if (r->decay_now != r->params.decay_s || r->damping_now != r->params.damping ||
        r->crossover_now != r->params.crossover_hz) {
        r->decay_now += (r->params.decay_s - r->decay_now) * k;
        r->damping_now += (r->params.damping - r->damping_now) * k;
        r->crossover_now += (r->params.crossover_hz - r->crossover_now) * k;
        if (fabsf(r->decay_now - r->params.decay_s) < 1e-4F * r->params.decay_s) {
            r->decay_now = r->params.decay_s;
        }
        if (fabsf(r->damping_now - r->params.damping) < 1e-5F) {
            r->damping_now = r->params.damping;
        }
        if (fabsf(r->crossover_now - r->params.crossover_hz) < 0.01F) {
            r->crossover_now = r->params.crossover_hz;
        }
        rv_shelves(r);
    }
    r->width_now = r->params.width;
    // A new size or pre-delay: new lengths and taps, crossfaded in across
    // this block from the old.
    const u32 predelay = rv_predelay_frames(r);
    if (r->params.size != r->size_now || predelay != r->predelay_now) {
        r->size_from = r->size_now;
        r->predelay_from = r->predelay_now;
        memcpy(r->length_from, r->length, sizeof r->length);
        r->size_now = r->params.size;
        r->predelay_now = predelay;
        rv_lengths(r, r->size_now, r->length);
        rv_shelves(r);
        r->fading = 1;
    }
    RvBlock blk;
    blk.offset = 0U;
    blk.inv = 1.0F / (f32)frames;
    rv_taps(r, r->size_now, r->predelay_now, blk.taps_now);
    rv_taps(r, r->size_from, r->predelay_from, blk.taps_from);
    // Sub-blocks short enough that every read precedes its write.
    u32 shortest = RV_BLOCK * 64U;
    for (u32 i = 0U; i < RV_N; ++i) {
        shortest = r->length[i] < shortest ? r->length[i] : shortest;
        shortest = r->length_from[i] < shortest ? r->length_from[i] : shortest;
    }
    const u32 wander = (u32)ceilf(RV_WANDER_MS * 0.001F * r->rate) * 2U + 4U;
    u32 most = shortest > wander + 1U ? shortest - wander : 1U;
    most = most < RV_BLOCK ? most : RV_BLOCK;
    f32 *sub[NPH_AUDIO_MAX_CHANNELS];
    while (blk.offset < frames) {
        const u32 n = nph_min_u32(most, frames - blk.offset);
        for (u32 c = 0U; c < r->channels; ++c) {
            sub[c] = planes[c] + blk.offset;
        }
        rv_sub_block(r, sub, n, &blk);
        blk.offset += n;
    }
    r->dry_now = r->params.dry;
    r->early_now = r->params.early;
    r->late_now = r->params.late;
    if (r->fading) {
        r->fading = 0;
        memcpy(r->length_from, r->length, sizeof r->length);
        r->size_from = r->size_now;
        r->predelay_from = r->predelay_now;
    }
}

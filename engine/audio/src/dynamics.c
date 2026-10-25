// SPDX-License-Identifier: Apache-2.0
// The compressor and the lookahead true-peak limiter (dynamics.h).

#include <nepheline/audio/dynamics.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

#define DYN_PI 3.14159265358979323846

// ---------------------------------------------------------------------------
// The compressor
// ---------------------------------------------------------------------------

NphAudioCompressorParams nph_audio_compressor_params_default(void)
{
    NphAudioCompressorParams p;
    p.threshold_db = -18.0F;
    p.ratio = 4.0F;
    p.knee_db = 6.0F;
    p.attack_ms = 10.0F;
    p.release_ms = 100.0F;
    p.makeup_db = 0.0F;
    return p;
}

static f32 dyn_time_coeff(f32 ms, f32 rate)
{
    // A one-pole's coefficient for a time constant of `ms`: exp(-1 / tau fs).
    const f64 tau = (ms > 0.01F ? (f64)ms : 0.01) * 0.001;
    return (f32)exp(-1.0 / (tau * (f64)rate));
}

void nph_audio_compressor_set(NphAudioCompressor *c, const NphAudioCompressorParams *params)
{
    NPH_ASSERT(c != NULL && params != NULL);
    c->params = *params;
    c->params.ratio = params->ratio >= 1.0F ? params->ratio : 1.0F;
    c->params.knee_db = params->knee_db > 0.0F ? params->knee_db : 0.0F;
    c->attack_coeff = dyn_time_coeff(params->attack_ms, c->rate);
    c->release_coeff = dyn_time_coeff(params->release_ms, c->rate);
}

void nph_audio_compressor_init(NphAudioCompressor *c, u32 channels, f32 rate,
                               const NphAudioCompressorParams *params)
{
    NPH_ASSERT(c != NULL && params != NULL && channels >= 1U && channels <= NPH_AUDIO_MAX_CHANNELS);
    memset(c, 0, sizeof *c);
    c->rate = rate;
    c->channels = channels;
    nph_audio_compressor_set(c, params);
    c->makeup = nph_audio_db_to_gain(c->params.makeup_db);
}

f32 nph_audio_compressor_curve(const NphAudioCompressorParams *params, f32 level_db)
{
    // Giannoulis et al. 2012, eq. 4: the soft knee's quadratic joins the
    // two straight lines with matching slopes.
    const f32 t = params->threshold_db;
    const f32 r = params->ratio >= 1.0F ? params->ratio : 1.0F;
    const f32 w = params->knee_db > 0.0F ? params->knee_db : 0.0F;
    const f32 over = level_db - t;
    f32 out;
    if (2.0F * over < -w) {
        out = level_db;
    } else if (w > 0.0F && 2.0F * fabsf(over) <= w) {
        const f32 k = over + 0.5F * w;
        out = level_db + (1.0F / r - 1.0F) * k * k / (2.0F * w);
    } else {
        out = t + over / r;
    }
    const f32 reduction = level_db - out;
    return reduction > 0.0F ? reduction : 0.0F;
}

void nph_audio_compressor_reset(NphAudioCompressor *c)
{
    NPH_ASSERT(c != NULL);
    c->reduction_db = 0.0F;
    c->deepest_db = 0.0F;
}

void nph_audio_compressor_process(NphAudioCompressor *c, f32 *const *planes, u32 frames)
{
    NPH_ASSERT(c != NULL && (planes != NULL || frames == 0U));
    const f32 makeup_to = nph_audio_db_to_gain(c->params.makeup_db);
    const f32 makeup_from = c->makeup;
    const f32 step = frames > 0U ? (makeup_to - makeup_from) / (f32)frames : 0.0F;
    // Below the knee the curve gives no reduction: a level under this
    // needs no logarithm.
    const f32 quiet = nph_audio_db_to_gain(c->params.threshold_db - 0.5F * c->params.knee_db);
    const f32 aa = c->attack_coeff;
    const f32 ar = c->release_coeff;
    f32 y = c->reduction_db;
    f32 deepest = 0.0F;
    for (u32 i = 0U; i < frames; ++i) {
        f32 level = 0.0F;
        for (u32 ch = 0U; ch < c->channels; ++ch) {
            const f32 a = fabsf(planes[ch][i]);
            level = a > level ? a : level;
        }
        const f32 target =
            level > quiet ? nph_audio_compressor_curve(&c->params, 20.0F * log10f(level)) : 0.0F;
        // The smooth branching peak detector (Giannoulis et al. 2012, eq. 17).
        const f32 k = target > y ? aa : ar;
        y = k * y + (1.0F - k) * target;
        deepest = y > deepest ? y : deepest;
        const f32 makeup = makeup_from + step * (f32)i;
        const f32 g = y > 1e-6F ? makeup * nph_audio_db_to_gain(-y) : makeup;
        for (u32 ch = 0U; ch < c->channels; ++ch) {
            planes[ch][i] *= g;
        }
    }
    c->reduction_db = y;
    c->makeup = makeup_to;
    c->deepest_db = deepest;
}

// ---------------------------------------------------------------------------
// The limiter
// ---------------------------------------------------------------------------

NphAudioLimiterParams nph_audio_limiter_params_default(void)
{
    NphAudioLimiterParams p;
    p.ceiling_db = -1.0F;
    p.release_ms = 80.0F;
    p.lookahead_ms = 1.5F;
    return p;
}

#define LIM_HALF 6U // the interpolator's reach (frames): half its taps
NPH_STATIC_ASSERT(LIM_HALF * 2U == NPH_AUDIO_LIMITER_TAPS, "the interpolator is centred");

static f64 lim_bessel_i0(f64 x)
{
    f64 sum = 1.0;
    f64 term = 1.0;
    for (u32 k = 1U; k < 64U; ++k) {
        const f64 h = x / (2.0 * (f64)k);
        term *= h * h;
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

// The interpolator: a sinc under a Kaiser window (beta 6) of half-width
// LIM_HALF frames, sampled at the quarter phases. Phase p's tap j weighs
// x[n - 11 + j] for the point a fraction p/4 past x[n - 6]; each phase is
// normalised to unit DC gain.
static void lim_design(f32 taps[3][NPH_AUDIO_LIMITER_TAPS])
{
    const f64 beta = 6.0;
    const f64 i0b = lim_bessel_i0(beta);
    for (u32 p = 1U; p <= 3U; ++p) {
        const f64 phase = (f64)p / 4.0;
        f64 sum = 0.0;
        f64 h[NPH_AUDIO_LIMITER_TAPS];
        for (u32 j = 0U; j < NPH_AUDIO_LIMITER_TAPS; ++j) {
            const f64 u = (f64)(LIM_HALF - 1U) + phase - (f64)j; // from 5 + phase to phase - 6
            const f64 r = u / (f64)LIM_HALF;
            const f64 w = fabs(r) < 1.0 ? lim_bessel_i0(beta * sqrt(1.0 - r * r)) / i0b : 0.0;
            const f64 s = u == 0.0 ? 1.0 : sin(DYN_PI * u) / (DYN_PI * u);
            h[j] = s * w;
            sum += h[j];
        }
        for (u32 j = 0U; j < NPH_AUDIO_LIMITER_TAPS; ++j) {
            taps[p - 1U][j] = (f32)(h[j] / sum);
        }
    }
}

void nph_audio_limiter_set(NphAudioLimiter *l, const NphAudioLimiterParams *params)
{
    NPH_ASSERT(l != NULL && params != NULL);
    l->params.ceiling_db = params->ceiling_db < 0.0F ? params->ceiling_db : 0.0F;
    l->params.release_ms = params->release_ms;
    l->ceiling = nph_audio_db_to_gain(l->params.ceiling_db);
    l->release_coeff = dyn_time_coeff(params->release_ms, l->rate);
}

void nph_audio_limiter_reset(NphAudioLimiter *l)
{
    NPH_ASSERT(l != NULL);
    memset(l->history, 0, sizeof(f32) * l->channels * NPH_AUDIO_LIMITER_TAPS);
    memset(l->segment, 0, sizeof l->segment);
    memset(l->delay, 0, sizeof(f32) * l->channels * l->delay_size);
    l->delay_write = 0U;
    l->recover = 1.0F;
    l->deque_head = 0U;
    l->deque_tail = 0U;
    for (u32 i = 0U; i < l->lookahead; ++i) {
        l->window[i] = 1.0F;
    }
    l->window_pos = 0U;
    l->window_sum = (f64)l->lookahead;
    l->time = 0U;
    l->lowest_gain = 1.0F;
}

NphResult nph_audio_limiter_init(NphAudioLimiter *l, NphAllocator *alloc, u32 channels, f32 rate,
                                 const NphAudioLimiterParams *params)
{
    NPH_ASSERT(l != NULL && params != NULL);
    memset(l, 0, sizeof *l);
    if (alloc == NULL || channels < 1U || channels > NPH_AUDIO_MAX_CHANNELS ||
        !(rate >= (f32)NPH_AUDIO_MIN_RATE && rate <= (f32)NPH_AUDIO_MAX_RATE) ||
        !(params->lookahead_ms >= 0.5F &&
          params->lookahead_ms <= NPH_AUDIO_LIMITER_MAX_LOOKAHEAD_MS)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    l->alloc = alloc;
    l->rate = rate;
    l->channels = channels;
    l->params = *params;
    l->lookahead = (u32)ceilf(params->lookahead_ms * 0.001F * rate);
    l->lookahead = l->lookahead > 1U ? l->lookahead : 1U;
    l->latency = l->lookahead - 1U + LIM_HALF;
    l->delay_size = nph_next_pow2_u32(l->latency + 1U);
    l->deque_size = nph_next_pow2_u32(l->lookahead + 1U);
    l->history = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, (u64)channels * NPH_AUDIO_LIMITER_TAPS);
    l->delay = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, (u64)channels * l->delay_size);
    l->deque_value = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, l->deque_size);
    l->deque_time = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, l->deque_size);
    l->window = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, l->lookahead);
    if (l->history == NULL || l->delay == NULL || l->deque_value == NULL || l->deque_time == NULL ||
        l->window == NULL) {
        nph_audio_limiter_free(l);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    lim_design(l->taps);
    nph_audio_limiter_set(l, params);
    nph_audio_limiter_reset(l);
    return NPH_RESULT_OK;
}

void nph_audio_limiter_free(NphAudioLimiter *l)
{
    if (l == NULL || l->alloc == NULL) {
        return;
    }
    NphAllocator *a = l->alloc;
    if (l->history != NULL) {
        NPH_FREE_ARRAY(a, f32, l->history, (u64)l->channels * NPH_AUDIO_LIMITER_TAPS);
    }
    if (l->delay != NULL) {
        NPH_FREE_ARRAY(a, f32, l->delay, (u64)l->channels * l->delay_size);
    }
    if (l->deque_value != NULL) {
        NPH_FREE_ARRAY(a, f32, l->deque_value, l->deque_size);
    }
    if (l->deque_time != NULL) {
        NPH_FREE_ARRAY(a, u32, l->deque_time, l->deque_size);
    }
    if (l->window != NULL) {
        NPH_FREE_ARRAY(a, f32, l->window, l->lookahead);
    }
    memset(l, 0, sizeof *l);
}

u32 nph_audio_limiter_latency(const NphAudioLimiter *l)
{
    return l->latency;
}

// The largest magnitude of the three interpolated points between h[5] and
// h[6] of a channel's history (h[11] the newest sample). `taps` is the
// three phases' rows.
static f32 lim_segment_peak(const f32 *taps, const f32 *h)
{
    f32 peak = 0.0F;
    for (u32 p = 0U; p < 3U; ++p) {
        f32 v = 0.0F;
        for (u32 j = 0U; j < NPH_AUDIO_LIMITER_TAPS; ++j) {
            v += taps[p * NPH_AUDIO_LIMITER_TAPS + j] * h[j];
        }
        const f32 a = fabsf(v);
        peak = a > peak ? a : peak;
    }
    return peak;
}

void nph_audio_limiter_process(NphAudioLimiter *l, f32 *const *planes, u32 frames)
{
    NPH_ASSERT(l != NULL && (planes != NULL || frames == 0U));
    const u32 n_taps = NPH_AUDIO_LIMITER_TAPS;
    const u32 dmask = l->delay_size - 1U;
    const u32 qmask = l->deque_size - 1U;
    const u32 window = l->lookahead;
    // The running sum, made exact again once a block.
    f64 sum = 0.0;
    for (u32 i = 0U; i < window; ++i) {
        sum += (f64)l->window[i];
    }
    f32 lowest = 1.0F;
    for (u32 i = 0U; i < frames; ++i) {
        // The detector: each channel's history moves on a sample, and the
        // peak around the sample LIM_HALF frames back counts the points
        // between it and both its neighbours (the segment before it is the
        // one the last sample found after its own).
        f32 peak = 0.0F;
        for (u32 ch = 0U; ch < l->channels; ++ch) {
            f32 *h = l->history + (u64)ch * n_taps;
            const f32 before = l->segment[ch];
            memmove(h, h + 1, (size_t)(n_taps - 1U) * sizeof(f32));
            h[n_taps - 1U] = planes[ch][i];
            const f32 after = lim_segment_peak(&l->taps[0][0], h);
            l->segment[ch] = after;
            const f32 at = fabsf(h[LIM_HALF - 1U]);
            f32 p = before > after ? before : after;
            p = at > p ? at : p;
            peak = p > peak ? p : peak;
        }
        const f32 need = peak > l->ceiling ? l->ceiling / peak : 1.0F;
        // Recover towards 1 at the release rate, never above what is needed.
        f32 r = l->recover + (1.0F - l->recover) * (1.0F - l->release_coeff);
        r = need < r ? need : r;
        l->recover = r;
        // The window's minimum: a monotonic deque of the last L values.
        const u32 now = l->time;
        while (l->deque_tail != l->deque_head &&
               l->deque_value[(l->deque_tail - 1U) & qmask] >= r) {
            l->deque_tail -= 1U;
        }
        l->deque_value[l->deque_tail & qmask] = r;
        l->deque_time[l->deque_tail & qmask] = now;
        l->deque_tail += 1U;
        while (now - l->deque_time[l->deque_head & qmask] >= window) {
            l->deque_head += 1U;
        }
        const f32 hold = l->deque_value[l->deque_head & qmask];
        // Its average over the window: the gain for the sample leaving the delay.
        sum += (f64)hold - (f64)l->window[l->window_pos];
        l->window[l->window_pos] = hold;
        l->window_pos = l->window_pos + 1U == window ? 0U : l->window_pos + 1U;
        f32 g = (f32)(sum / (f64)window);
        g = g < 1.0F ? g : 1.0F;
        lowest = g < lowest ? g : lowest;
        l->time = now + 1U;
        // The delay: in goes this sample, out comes the one `latency` ago.
        const u32 w = l->delay_write;
        for (u32 ch = 0U; ch < l->channels; ++ch) {
            f32 *d = l->delay + (u64)ch * l->delay_size;
            d[w & dmask] = planes[ch][i];
            f32 y = d[(w - l->latency) & dmask] * g;
            // A last guard against rounding: never past the ceiling.
            y = y > l->ceiling ? l->ceiling : (y < -l->ceiling ? -l->ceiling : y);
            planes[ch][i] = y;
        }
        l->delay_write = w + 1U;
    }
    l->window_sum = sum;
    l->lowest_gain = lowest;
}

f32 nph_audio_true_peak(const NphAudioLimiter *l, const f32 *x, u32 count)
{
    f32 h[NPH_AUDIO_LIMITER_TAPS] = {0.0F};
    f32 peak = 0.0F;
    for (u32 i = 0U; i < count + LIM_HALF; ++i) {
        memmove(h, h + 1, (NPH_AUDIO_LIMITER_TAPS - 1U) * sizeof(f32));
        h[NPH_AUDIO_LIMITER_TAPS - 1U] = i < count ? x[i] : 0.0F;
        const f32 seg = lim_segment_peak(&l->taps[0][0], h);
        const f32 at = fabsf(h[LIM_HALF - 1U]);
        peak = seg > peak ? seg : peak;
        peak = at > peak ? at : peak;
    }
    return peak;
}

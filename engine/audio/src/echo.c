// SPDX-License-Identifier: Apache-2.0
// The echo (echo.h).

#include <nepheline/audio/echo.h>
#include <nepheline/audio/filter.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

NphAudioEchoParams nph_audio_echo_params_default(void)
{
    NphAudioEchoParams p;
    p.delay_ms = 350.0F;
    p.feedback = 0.4F;
    p.damping_hz = 5000.0F;
    p.highpass_hz = 120.0F;
    p.wet = 0.5F;
    p.dry = 1.0F;
    p.ping_pong = 0;
    return p;
}

static f32 echo_frames(const NphAudioEcho *e, f32 ms)
{
    // In double precision: 10 ms at 48 kHz is 480 frames, not 479.99998.
    f32 d = (f32)((f64)ms * (f64)e->rate / 1000.0);
    d = d > 2.0F ? d : 2.0F; // the spline reads a frame either side
    return d < e->max_delay ? d : e->max_delay;
}

void nph_audio_echo_set(NphAudioEcho *e, const NphAudioEchoParams *params)
{
    NPH_ASSERT(e != NULL && params != NULL);
    e->params = *params;
    e->params.feedback =
        params->feedback < 0.0F ? 0.0F : (params->feedback > 0.95F ? 0.95F : params->feedback);
    e->params.ping_pong = params->ping_pong != 0 && e->channels == 2U;
    e->lowpass_coeff =
        params->damping_hz > 0.0F ? nph_audio_one_pole_coeff(params->damping_hz, e->rate) : 1.0F;
    e->highpass_coeff =
        params->highpass_hz > 0.0F ? nph_audio_one_pole_coeff(params->highpass_hz, e->rate) : 0.0F;
}

NphResult nph_audio_echo_init(NphAudioEcho *e, NphAllocator *alloc, u32 channels, f32 rate,
                              f32 max_delay_ms, const NphAudioEchoParams *params)
{
    NPH_ASSERT(e != NULL && params != NULL);
    memset(e, 0, sizeof *e);
    if (alloc == NULL || channels < 1U || channels > NPH_AUDIO_MAX_CHANNELS ||
        !(rate >= (f32)NPH_AUDIO_MIN_RATE && rate <= (f32)NPH_AUDIO_MAX_RATE) ||
        !(max_delay_ms >= 1.0F && max_delay_ms <= 10000.0F)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    e->rate = rate;
    e->channels = channels;
    e->max_delay = ceilf(max_delay_ms * 0.001F * rate) + 2.0F;
    for (u32 c = 0U; c < channels; ++c) {
        const NphResult r = nph_audio_delay_init(&e->lines[c], alloc, (u32)e->max_delay + 3U);
        if (r != NPH_RESULT_OK) {
            nph_audio_echo_free(e);
            return r;
        }
    }
    e->fade_length = (u32)(NPH_AUDIO_ECHO_FADE_MS * 0.001F * rate);
    nph_audio_echo_set(e, params);
    e->delay_now = echo_frames(e, params->delay_ms);
    e->delay_from = e->delay_now;
    e->wet_now = e->params.wet;
    e->dry_now = e->params.dry;
    return NPH_RESULT_OK;
}

void nph_audio_echo_free(NphAudioEcho *e)
{
    if (e == NULL) {
        return;
    }
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        nph_audio_delay_free(&e->lines[c]);
    }
    memset(e, 0, sizeof *e);
}

void nph_audio_echo_reset(NphAudioEcho *e)
{
    NPH_ASSERT(e != NULL);
    for (u32 c = 0U; c < e->channels; ++c) {
        nph_audio_delay_clear(&e->lines[c]);
    }
    memset(e->lowpass_state, 0, sizeof e->lowpass_state);
    memset(e->highpass_state, 0, sizeof e->highpass_state);
    e->delay_from = e->delay_now;
    e->fade_left = 0U;
}

// The line's output `d` frames back (2 <= d <= max_delay), between samples
// by the Catmull-Rom spline.
static f32 echo_tap(const NphAudioDelay *line, f32 d)
{
    const u32 k = (u32)d;
    const f32 t = d - (f32)k;
    const f32 xm = nph_audio_delay_tap(line, k - 1U); // newer
    const f32 x0 = nph_audio_delay_tap(line, k);
    const f32 x1 = nph_audio_delay_tap(line, k + 1U);
    const f32 x2 = nph_audio_delay_tap(line, k + 2U); // older
    // From x0 (t = 0) to x1 (t = 1).
    const f32 c1 = 0.5F * (x1 - xm);
    const f32 c2 = xm - 2.5F * x0 + 2.0F * x1 - 0.5F * x2;
    const f32 c3 = 0.5F * (x2 - xm) + 1.5F * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

void nph_audio_echo_process(NphAudioEcho *e, f32 *const *planes, u32 frames)
{
    NPH_ASSERT(e != NULL && (planes != NULL || frames == 0U));
    const f32 target = echo_frames(e, e->params.delay_ms);
    if (e->fade_left == 0U && target != e->delay_now) {
        e->delay_from = e->delay_now;
        e->delay_now = target;
        e->fade_left = e->fade_length > 0U ? e->fade_length : 1U;
    }
    const f32 wet_step = frames > 0U ? (e->params.wet - e->wet_now) / (f32)frames : 0.0F;
    const f32 dry_step = frames > 0U ? (e->params.dry - e->dry_now) / (f32)frames : 0.0F;
    const f32 fb = e->params.feedback;
    const f32 lp = e->lowpass_coeff;
    const f32 hp = e->highpass_coeff;
    const bool ping_pong = e->params.ping_pong != 0;
    for (u32 i = 0U; i < frames; ++i) {
        // The tap: at the new delay, or crossfading to it from the old
        // along a smoothstep (a linear crossfade's corners would kink the
        // level of a tone whose two taps differ in phase).
        f32 mix_new = 1.0F;
        if (e->fade_left > 0U) {
            e->fade_left -= 1U;
            const f32 t = 1.0F - (f32)e->fade_left / (f32)e->fade_length;
            mix_new = t * t * (3.0F - 2.0F * t);
        }
        f32 taps[NPH_AUDIO_MAX_CHANNELS] = {0.0F};
        for (u32 c = 0U; c < e->channels; ++c) {
            const f32 now = echo_tap(&e->lines[c], e->delay_now);
            taps[c] = mix_new < 1.0F
                          ? now * mix_new + echo_tap(&e->lines[c], e->delay_from) * (1.0F - mix_new)
                          : now;
        }
        // The feedback, darkened and cleared of rumble.
        f32 loop[NPH_AUDIO_MAX_CHANNELS] = {0.0F};
        for (u32 c = 0U; c < e->channels; ++c) {
            e->lowpass_state[c] += lp * (taps[c] - e->lowpass_state[c]);
            e->highpass_state[c] += hp * (e->lowpass_state[c] - e->highpass_state[c]);
            loop[c] = fb * (e->lowpass_state[c] - e->highpass_state[c]);
        }
        const f32 wet = e->wet_now + wet_step * (f32)i;
        const f32 dry = e->dry_now + dry_step * (f32)i;
        if (ping_pong) {
            const f32 in = 0.5F * (planes[0][i] + planes[1][i]);
            nph_audio_delay_push(&e->lines[0], in + loop[1]);
            nph_audio_delay_push(&e->lines[1], loop[0]);
        } else {
            for (u32 c = 0U; c < e->channels; ++c) {
                nph_audio_delay_push(&e->lines[c], planes[c][i] + loop[c]);
            }
        }
        for (u32 c = 0U; c < e->channels; ++c) {
            planes[c][i] = dry * planes[c][i] + wet * taps[c];
        }
    }
    e->wet_now = e->params.wet;
    e->dry_now = e->params.dry;
}

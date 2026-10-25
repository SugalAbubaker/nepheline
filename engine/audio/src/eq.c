// SPDX-License-Identifier: Apache-2.0
// The equaliser (eq.h).

#include <nepheline/audio/eq.h>
#include <nepheline/core/assert.h>

#include <string.h>

void nph_audio_eq_init(NphAudioEq *eq, u32 channels, f32 rate)
{
    NPH_ASSERT(eq != NULL && channels >= 1U && channels <= NPH_AUDIO_MAX_CHANNELS);
    memset(eq, 0, sizeof *eq);
    eq->rate = rate;
    eq->channels = channels;
    for (u32 b = 0U; b < NPH_AUDIO_EQ_BANDS; ++b) {
        eq->coeffs[b] = nph_audio_filter_passthrough();
    }
}

void nph_audio_eq_set_band(NphAudioEq *eq, u32 band, const NphAudioEqBand *settings)
{
    NPH_ASSERT(eq != NULL && settings != NULL);
    if (band < NPH_AUDIO_EQ_BANDS) {
        eq->target[band] = *settings;
    }
}

void nph_audio_eq_reset(NphAudioEq *eq)
{
    NPH_ASSERT(eq != NULL);
    memset(eq->state, 0, sizeof eq->state);
}

static bool eq_same(const NphAudioEqBand *a, const NphAudioEqBand *b)
{
    return a->type == b->type && a->freq == b->freq && a->q == b->q && a->gain_db == b->gain_db;
}

void nph_audio_eq_process(NphAudioEq *eq, f32 *const *planes, u32 frames, f32 *scratch)
{
    NPH_ASSERT(eq != NULL && (planes != NULL || frames == 0U) && (scratch != NULL || frames == 0U));
    for (u32 b = 0U; b < NPH_AUDIO_EQ_BANDS; ++b) {
        const NphAudioEqBand *want = &eq->target[b];
        NphAudioEqBand *was = &eq->running[b];
        const bool on = want->enabled != 0;
        const bool had = was->enabled != 0;
        if (!on && !had) {
            continue;
        }
        const NphAudioFilterCoeffs old = eq->coeffs[b];
        const NphAudioFilterCoeffs now =
            on ? nph_audio_filter_design(want->type, want->freq, want->q, want->gain_db, eq->rate)
               : old;
        for (u32 c = 0U; c < eq->channels; ++c) {
            f32 *x = planes[c];
            NphAudioFilterState *s = &eq->state[b][c];
            if (on && had && want->type == was->type) {
                if (eq_same(want, was)) {
                    nph_audio_filter_process(&now, s, x, x, frames);
                } else {
                    nph_audio_filter_process_glide(&old, &now, s, x, x, frames);
                }
            } else if (on && had) {
                // Another type: the old section's output crossfades to the
                // new one's, which starts from silence.
                memcpy(scratch, x, (size_t)frames * sizeof(f32));
                nph_audio_filter_process(&old, s, x, x, frames);
                NphAudioFilterState fresh = {0.0F, 0.0F};
                nph_audio_filter_process(&now, &fresh, scratch, scratch, frames);
                nph_audio_smooth_crossfade(scratch, x, frames, true);
                memcpy(x, scratch, (size_t)frames * sizeof(f32));
                *s = fresh;
            } else {
                // Switching on (from silence) or off (to the dry signal).
                if (on) {
                    nph_audio_filter_reset(s);
                }
                memcpy(scratch, x, (size_t)frames * sizeof(f32));
                nph_audio_filter_process(&now, s, x, x, frames);
                nph_audio_smooth_crossfade(x, scratch, frames, on);
            }
        }
        if (!on) {
            for (u32 c = 0U; c < eq->channels; ++c) {
                nph_audio_filter_reset(&eq->state[b][c]);
            }
        }
        eq->coeffs[b] = now;
        *was = *want;
    }
}

// SPDX-License-Identifier: Apache-2.0
// The mixer's insert effects (mixer.h "Effects"): the game thread's API,
// and the audio thread's chain on a bus.

#include <nepheline/core/assert.h>

#include "mixer_internal.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Both threads
// ---------------------------------------------------------------------------

void mix_effect_free(NphAudioMixer *m, MixEffect *e)
{
    if (e == NULL) {
        return;
    }
    switch (e->type) {
    case NPH_AUDIO_EFFECT_LIMITER:
        nph_audio_limiter_free(&e->dsp.limiter);
        break;
    case NPH_AUDIO_EFFECT_ECHO:
        nph_audio_echo_free(&e->dsp.echo);
        break;
    case NPH_AUDIO_EFFECT_REVERB:
        nph_audio_reverb_free(&e->dsp.reverb);
        break;
    case NPH_AUDIO_EFFECT_EQ:
    case NPH_AUDIO_EFFECT_COMPRESSOR:
    case NPH_AUDIO_EFFECT_COUNT:
        break;
    }
    NPH_FREE_TYPE(m->alloc, MixEffect, e);
}

// ---------------------------------------------------------------------------
// The audio thread
// ---------------------------------------------------------------------------

void mix_effect_set_param(MixEffect *e, u32 param, f32 value)
{
    switch (e->type) {
    case NPH_AUDIO_EFFECT_EQ: {
        const u32 band = param / (u32)NPH_AUDIO_EQ_FIELDS;
        if (band >= NPH_AUDIO_EQ_BANDS) {
            break;
        }
        NphAudioEqBand b = e->dsp.eq.target[band];
        switch ((NphAudioEqField)(param % (u32)NPH_AUDIO_EQ_FIELDS)) {
        case NPH_AUDIO_EQ_TYPE:
            if (value >= 0.0F && value < (f32)NPH_AUDIO_FILTER_COUNT) {
                b.type = (NphAudioFilterType)(u32)value;
            }
            break;
        case NPH_AUDIO_EQ_FREQ:
            b.freq = value;
            break;
        case NPH_AUDIO_EQ_Q:
            b.q = value;
            break;
        case NPH_AUDIO_EQ_GAIN:
            b.gain_db = value;
            break;
        case NPH_AUDIO_EQ_ENABLED:
            b.enabled = value != 0.0F;
            break;
        case NPH_AUDIO_EQ_FIELDS:
            break;
        }
        nph_audio_eq_set_band(&e->dsp.eq, band, &b);
        break;
    }
    case NPH_AUDIO_EFFECT_COMPRESSOR: {
        NphAudioCompressorParams p = e->dsp.compressor.params;
        f32 *fields[6] = {&p.threshold_db, &p.ratio,      &p.knee_db,
                          &p.attack_ms,    &p.release_ms, &p.makeup_db};
        if (param < 6U) {
            *fields[param] = value;
            nph_audio_compressor_set(&e->dsp.compressor, &p);
        }
        break;
    }
    case NPH_AUDIO_EFFECT_LIMITER: {
        NphAudioLimiterParams p = e->dsp.limiter.params;
        if (param == NPH_AUDIO_LIMITER_CEILING) {
            p.ceiling_db = value;
        } else if (param == NPH_AUDIO_LIMITER_RELEASE) {
            p.release_ms = value;
        } else {
            break;
        }
        nph_audio_limiter_set(&e->dsp.limiter, &p);
        break;
    }
    case NPH_AUDIO_EFFECT_ECHO: {
        NphAudioEchoParams p = e->dsp.echo.params;
        f32 *fields[6] = {&p.delay_ms, &p.feedback, &p.damping_hz, &p.highpass_hz, &p.wet, &p.dry};
        if (param < 6U) {
            *fields[param] = value;
        } else if (param == NPH_AUDIO_ECHO_PING_PONG) {
            p.ping_pong = value != 0.0F;
        } else {
            break;
        }
        nph_audio_echo_set(&e->dsp.echo, &p);
        break;
    }
    case NPH_AUDIO_EFFECT_REVERB: {
        NphAudioReverbParams p = e->dsp.reverb.params;
        f32 *fields[11] = {&p.size,        &p.decay_s,   &p.damping,    &p.crossover_hz,
                           &p.predelay_ms, &p.diffusion, &p.modulation, &p.early,
                           &p.late,        &p.width,     &p.dry};
        if (param < 11U) {
            *fields[param] = value;
            nph_audio_reverb_set(&e->dsp.reverb, &p);
        }
        break;
    }
    case NPH_AUDIO_EFFECT_COUNT:
        break;
    }
}

static void me_reset(MixEffect *e)
{
    switch (e->type) {
    case NPH_AUDIO_EFFECT_EQ:
        nph_audio_eq_reset(&e->dsp.eq);
        break;
    case NPH_AUDIO_EFFECT_COMPRESSOR:
        nph_audio_compressor_reset(&e->dsp.compressor);
        break;
    case NPH_AUDIO_EFFECT_LIMITER:
        nph_audio_limiter_reset(&e->dsp.limiter);
        break;
    case NPH_AUDIO_EFFECT_ECHO:
        nph_audio_echo_reset(&e->dsp.echo);
        break;
    case NPH_AUDIO_EFFECT_REVERB:
        nph_audio_reverb_reset(&e->dsp.reverb);
        break;
    case NPH_AUDIO_EFFECT_COUNT:
        break;
    }
}

// Runs the effect on the block; returns its gain reduction for the meter.
static f32 me_run(NphAudioMixer *m, MixEffect *e, f32 *const *planes)
{
    switch (e->type) {
    case NPH_AUDIO_EFFECT_EQ:
        // The voices' filter scratch is free while the buses run.
        nph_audio_eq_process(&e->dsp.eq, planes, MIX_BLOCK, m->filter_dry);
        return 0.0F;
    case NPH_AUDIO_EFFECT_COMPRESSOR:
        nph_audio_compressor_process(&e->dsp.compressor, planes, MIX_BLOCK);
        return e->dsp.compressor.deepest_db;
    case NPH_AUDIO_EFFECT_LIMITER: {
        nph_audio_limiter_process(&e->dsp.limiter, planes, MIX_BLOCK);
        const f32 g = e->dsp.limiter.lowest_gain;
        return g < 1.0F ? -nph_audio_gain_to_db(g) : 0.0F;
    }
    case NPH_AUDIO_EFFECT_ECHO:
        nph_audio_echo_process(&e->dsp.echo, planes, MIX_BLOCK);
        return 0.0F;
    case NPH_AUDIO_EFFECT_REVERB:
        nph_audio_reverb_process(&e->dsp.reverb, planes, MIX_BLOCK);
        return 0.0F;
    case NPH_AUDIO_EFFECT_COUNT:
        break;
    }
    return 0.0F;
}

void mix_effects_process(NphAudioMixer *m, MixBus *b)
{
    u32 kept = 0U;
    for (u32 k = 0U; k < b->effect_count; ++k) {
        const u32 slot = b->effects[k];
        MixEffect *e = m->audio_effects[slot];
        const bool wet = !e->bypass && !e->removing;
        const bool was = !e->bypassed;
        f32 reduction = 0.0F;
        if (wet && !was) {
            // Coming in: the effect's input fades in across the block as
            // the signal without it fades out, (1 - s) x + E(s x), so the
            // copies its delays and tails make start from silence too.
            for (u32 c = 0U; c < m->channels; ++c) {
                memcpy(m->effect_dry[c], b->buffer[c], sizeof(f32) * MIX_BLOCK);
                nph_audio_smooth_ramp(b->buffer[c], MIX_BLOCK, true);
            }
            reduction = me_run(m, e, b->buffer);
            for (u32 c = 0U; c < m->channels; ++c) {
                nph_audio_smooth_ramp(m->effect_dry[c], MIX_BLOCK, false);
                m->kernels->mix(b->buffer[c], m->effect_dry[c], MIX_BLOCK, 1.0F);
            }
        } else if (was) {
            // Running, or going out: then its output crossfades to the
            // signal without it (its tail ends across the block).
            if (!wet) {
                for (u32 c = 0U; c < m->channels; ++c) {
                    memcpy(m->effect_dry[c], b->buffer[c], sizeof(f32) * MIX_BLOCK);
                }
            }
            reduction = me_run(m, e, b->buffer);
            if (!wet) {
                for (u32 c = 0U; c < m->channels; ++c) {
                    nph_audio_smooth_crossfade(b->buffer[c], m->effect_dry[c], MIX_BLOCK, false);
                }
            }
        }
        if (!wet && was) {
            me_reset(e); // it comes back without the tail it had
        }
        e->bypassed = !wet;
        u32 bits;
        memcpy(&bits, &reduction, sizeof bits);
        nph_atomic_store_u32(&m->effect_meter[slot], bits, NPH_MEMORY_ORDER_RELAXED);
        if (e->removing) {
            m->audio_effects[slot] = NULL;
            mix_event(m, MIX_EVENT_EFFECT_RELEASED, slot, 0U);
            continue;
        }
        b->effects[kept++] = slot;
    }
    b->effect_count = kept;
}

// ---------------------------------------------------------------------------
// The game thread
// ---------------------------------------------------------------------------

NphAudioEffectDesc nph_audio_effect_desc_default(NphAudioEffectType type)
{
    NphAudioEffectDesc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    for (u32 b = 0U; b < NPH_AUDIO_EQ_BANDS; ++b) {
        d.eq[b].type = NPH_AUDIO_FILTER_PEAK;
        d.eq[b].freq = 1000.0F;
        d.eq[b].q = 0.7071068F;
    }
    d.compressor = nph_audio_compressor_params_default();
    d.limiter = nph_audio_limiter_params_default();
    d.echo = nph_audio_echo_params_default();
    d.echo_max_delay_ms = 2000.0F;
    d.reverb = nph_audio_reverb_params_default();
    return d;
}

static u32 me_slot(const NphAudioMixer *m, NphAudioEffect effect)
{
    const NphHandle h = {effect.value};
    if (!nph_handle_pool_is_valid(&m->effect_handles, h)) {
        return MIX_NONE;
    }
    const u32 index = nph_handle_pool_index(&m->effect_handles, h);
    return m->effect_slots[index].releasing ? MIX_NONE : index;
}

NphResult nph_audio_effect_create(NphAudioMixer *mixer, NphAudioBus bus,
                                  const NphAudioEffectDesc *desc, NphAudioEffect *out)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(desc != NULL && out != NULL);
    out->value = 0U;
    const u32 b = mix_bus_slot(m, bus);
    if (b == MIX_NONE || (u32)desc->type >= (u32)NPH_AUDIO_EFFECT_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // A destroyed effect holds its place in its chain until the audio
    // thread lets it go (an update hears it), so it still counts.
    u32 on_bus = 0U;
    for (u32 i = 0U; i < m->max_effects; ++i) {
        const MixEffectSlot *s = &m->effect_slots[i];
        on_bus += s->effect != NULL && s->bus == b ? 1U : 0U;
    }
    if (on_bus >= NPH_AUDIO_BUS_EFFECTS) {
        return NPH_RESULT_FULL;
    }
    MixEffect *e = NPH_ALLOC_TYPE(m->alloc, MixEffect);
    if (e == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(e, 0, sizeof *e);
    e->type = desc->type;
    // Coming in: its first block crossfades from the bus without it.
    e->bypassed = 1;
    const f32 rate = (f32)m->rate;
    NphResult r = NPH_RESULT_OK;
    switch (desc->type) {
    case NPH_AUDIO_EFFECT_EQ:
        nph_audio_eq_init(&e->dsp.eq, m->channels, rate);
        for (u32 i = 0U; i < NPH_AUDIO_EQ_BANDS; ++i) {
            nph_audio_eq_set_band(&e->dsp.eq, i, &desc->eq[i]);
        }
        break;
    case NPH_AUDIO_EFFECT_COMPRESSOR:
        nph_audio_compressor_init(&e->dsp.compressor, m->channels, rate, &desc->compressor);
        break;
    case NPH_AUDIO_EFFECT_LIMITER:
        r = nph_audio_limiter_init(&e->dsp.limiter, m->alloc, m->channels, rate, &desc->limiter);
        break;
    case NPH_AUDIO_EFFECT_ECHO: {
        const f32 longest = desc->echo_max_delay_ms > 0.0F ? desc->echo_max_delay_ms : 2000.0F;
        r = nph_audio_echo_init(&e->dsp.echo, m->alloc, m->channels, rate, longest, &desc->echo);
        break;
    }
    case NPH_AUDIO_EFFECT_REVERB:
        r = nph_audio_reverb_init(&e->dsp.reverb, m->alloc, m->channels, rate, &desc->reverb);
        break;
    case NPH_AUDIO_EFFECT_COUNT:
        break;
    }
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(m->alloc, MixEffect, e);
        return r;
    }
    NphHandle h;
    if (nph_handle_pool_alloc(&m->effect_handles, &h) != NPH_RESULT_OK) {
        mix_effect_free(m, e);
        return NPH_RESULT_FULL;
    }
    const u32 index = nph_handle_pool_index(&m->effect_handles, h);
    MixCommand c = mix_command(MIX_CMD_EFFECT_ADD, index);
    c.param = b;
    c.ptr = e;
    if (!mix_push(m, &c)) {
        NPH_VERIFY(nph_handle_pool_release(&m->effect_handles, h));
        mix_effect_free(m, e);
        return NPH_RESULT_FULL;
    }
    MixEffectSlot *s = &m->effect_slots[index];
    s->effect = e;
    s->handle = h;
    s->bus = b;
    s->releasing = 0;
    out->value = h.value;
    return NPH_RESULT_OK;
}

void nph_audio_effect_destroy(NphAudioMixer *mixer, NphAudioEffect effect)
{
    NphAudioMixer *m = mixer;
    const u32 index = me_slot(m, effect);
    if (index == MIX_NONE) {
        return;
    }
    m->effect_slots[index].releasing = 1;
    const MixCommand c = mix_command(MIX_CMD_EFFECT_REMOVE, index);
    if (!mix_push(m, &c)) {
        m->pending_effect_removals[m->pending_effect_removal_count++] = index;
    }
}

bool nph_audio_effect_is_valid(const NphAudioMixer *mixer, NphAudioEffect effect)
{
    return me_slot(mixer, effect) != MIX_NONE;
}

void nph_audio_effect_set_param(NphAudioMixer *mixer, NphAudioEffect effect, u32 param, f32 value)
{
    const u32 index = me_slot(mixer, effect);
    if (index == MIX_NONE || !isfinite(value)) {
        return;
    }
    MixCommand c = mix_command(MIX_CMD_EFFECT_PARAM, index);
    c.param = param;
    c.values[0] = value;
    (void)mix_push(mixer, &c);
}

void nph_audio_effect_set_bypass(NphAudioMixer *mixer, NphAudioEffect effect, bool bypass)
{
    const u32 index = me_slot(mixer, effect);
    if (index == MIX_NONE) {
        return;
    }
    MixCommand c = mix_command(MIX_CMD_EFFECT_BYPASS, index);
    c.values[0] = bypass ? 1.0F : 0.0F;
    (void)mix_push(mixer, &c);
}

f32 nph_audio_effect_meter(const NphAudioMixer *mixer, NphAudioEffect effect)
{
    const u32 index = me_slot(mixer, effect);
    if (index == MIX_NONE) {
        return 0.0F;
    }
    const u32 bits = nph_atomic_load_u32(&mixer->effect_meter[index], NPH_MEMORY_ORDER_RELAXED);
    f32 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

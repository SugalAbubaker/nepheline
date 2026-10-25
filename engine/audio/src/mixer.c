// SPDX-License-Identifier: Apache-2.0
// The mixer (mixer.h): creation, destruction, and the published statistics.
// The game thread's API is in mixer_api.c, the audio thread's block in
// mixer_render.c, and one voice's block in mixer_voice.c.

#include <nepheline/core/assert.h>

#include "mixer_internal.h"

#include <string.h>

#define MIX_MAX_BUSES 256U // bus handles keep the slot in eight bits

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

static void mix_free_clip(NphAudioMixer *m, MixClip *c)
{
    if (c == NULL) {
        return;
    }
    if (c->owned_data != NULL) {
        NPH_FREE(m->alloc, c->owned_data, c->size, 16U);
    }
    if (c->owned_offsets != NULL) {
        NPH_FREE_ARRAY(m->alloc, u32, c->owned_offsets, c->owned_offset_count);
    }
    NPH_FREE_TYPE(m->alloc, MixClip, c);
}

void nph_audio_mixer_destroy(NphAudioMixer *mixer)
{
    if (mixer == NULL) {
        return;
    }
    NphAudioMixer *m = mixer;
    NphAllocator *a = m->alloc;
    if (m->clip_slots != NULL) {
        for (u32 i = 0U; i < m->max_clips; ++i) {
            mix_free_clip(m, m->clip_slots[i].clip);
        }
    }
    nph_audio_resampler_destroy(m->resampler);
    nph_spsc_ring_free(&m->commands);
    nph_spsc_ring_free(&m->events);
    nph_handle_pool_free(&m->voice_handles);
    nph_handle_pool_free(&m->clip_handles);
    if (m->buses != NULL) {
        for (u32 i = 0U; i < m->max_buses; ++i) {
            for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
                if (m->buses[i].buffer[c] != NULL) {
                    NPH_FREE_ARRAY(a, f32, m->buses[i].buffer[c], MIX_BLOCK);
                }
            }
        }
    }
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        if (m->window[c] != NULL) {
            NPH_FREE_ARRAY(a, f32, m->window[c], MIX_WINDOW);
        }
        if (m->voice_out[c] != NULL) {
            NPH_FREE_ARRAY(a, f32, m->voice_out[c], MIX_BLOCK);
        }
    }
#define MIX_RELEASE(ptr, type, count)                                                              \
    do {                                                                                           \
        if ((ptr) != NULL) {                                                                       \
            NPH_FREE_ARRAY(a, type, (ptr), (count));                                               \
        }                                                                                          \
    } while (0)
    MIX_RELEASE(m->mailbox, MixVoiceInit, m->max_voices);
    MIX_RELEASE(m->clip_slots, MixClipSlot, m->max_clips);
    MIX_RELEASE(m->voice_clip, u32, m->max_voices);
    MIX_RELEASE(m->bus_mirror, MixBusMirror, m->max_buses);
    MIX_RELEASE(m->snapshot_mailbox, MixSnapshot, m->max_snapshots);
    MIX_RELEASE(m->voices, MixVoice, m->max_voices);
    MIX_RELEASE(m->audio_clips, MixClip *, m->max_clips);
    MIX_RELEASE(m->clip_voice_count, u32, m->max_clips);
    MIX_RELEASE(m->clip_removing, b32, m->max_clips);
    MIX_RELEASE(m->buses, MixBus, m->max_buses);
    MIX_RELEASE(m->bus_order, u32, m->max_buses);
    MIX_RELEASE(m->snapshots, MixSnapshot, m->max_snapshots);
    MIX_RELEASE(m->contexts, MixContext, m->context_count);
    MIX_RELEASE(m->ranking_keys, u64, m->max_voices);
    MIX_RELEASE(m->block_out, f32, (u64)MIX_BLOCK * m->channels);
    MIX_RELEASE(m->filter_dry, f32, MIX_BLOCK);
    MIX_RELEASE(m->bus_level, f32, m->max_buses);
    MIX_RELEASE(m->bus_target, f32, m->max_buses);
    MIX_RELEASE(m->bus_peak, NphAtomicU32, m->max_buses);
    MIX_RELEASE(m->bus_rms, NphAtomicU32, m->max_buses);
    MIX_RELEASE(m->pending_clip_removals, u32, m->max_clips);
    MIX_RELEASE(m->pending_bus_removals, u32, m->max_buses);
#undef MIX_RELEASE
    NPH_FREE_TYPE(a, NphAudioMixer, m);
}

NphResult nph_audio_mixer_create(const NphAudioMixerDesc *desc, NphAudioMixer **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 rate = desc->sample_rate != 0U ? desc->sample_rate : 48000U;
    const u32 channels = desc->channels != 0U ? desc->channels : 2U;
    const u32 max_voices = desc->max_voices != 0U ? desc->max_voices : 512U;
    const u32 max_real = desc->max_real_voices != 0U ? desc->max_real_voices : 128U;
    const u32 max_clips = desc->max_clips != 0U ? desc->max_clips : 4096U;
    const u32 max_buses = desc->max_buses != 0U ? desc->max_buses : 32U;
    const u32 max_snapshots = desc->max_snapshots != 0U ? desc->max_snapshots : 32U;
    const u32 commands = desc->command_capacity != 0U ? desc->command_capacity : 4096U;
    NphAudioLayout layout;
    if (rate < NPH_AUDIO_MIN_RATE || rate > NPH_AUDIO_MAX_RATE ||
        nph_audio_layout_for_channels(channels, &layout) != NPH_RESULT_OK ||
        max_voices > (1U << 16U) || max_real > max_voices || max_clips > (1U << 20U) ||
        max_buses > MIX_MAX_BUSES || max_snapshots > 4096U || !nph_is_pow2_u32(commands)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *a = desc->alloc;
    NphAudioMixer *m = NPH_ALLOC_TYPE(a, NphAudioMixer);
    if (m == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    m->alloc = a;
    m->rate = rate;
    m->channels = channels;
    m->layout = layout;
    m->max_voices = max_voices;
    m->max_real = max_real;
    m->max_clips = max_clips;
    m->max_buses = max_buses;
    m->max_snapshots = max_snapshots;
    m->kernels = nph_audio_kernels_best();
    // A promotion needs a free context while its demoted voice still fades
    // in another: a quarter more than the real voices covers a busy block.
    m->context_count = max_real + max_real / 4U + 4U;
    NphResult r = nph_audio_panner_init(&m->panner, layout);
    if (r == NPH_RESULT_OK) {
        r = nph_audio_resampler_create(a, &m->resampler);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_spsc_ring_init(&m->commands, a, sizeof(MixCommand), commands);
    }
    if (r == NPH_RESULT_OK) {
        // Room for every voice's end and every clip's and bus's release at
        // once: each event belongs to one slot, which is not reused until the
        // game thread has heard it, so a push can never find the ring full.
        const u32 events = nph_next_pow2_u32(max_voices + max_clips + max_buses + 64U);
        r = nph_spsc_ring_init(&m->events, a, sizeof(MixEvent), events);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_handle_pool_init(
            &m->voice_handles, a, max_voices,
            nph_log2_ceil_u32(max_voices) > 0U ? nph_log2_ceil_u32(max_voices) : 1U, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_handle_pool_init(
            &m->clip_handles, a, max_clips,
            nph_log2_ceil_u32(max_clips) > 0U ? nph_log2_ceil_u32(max_clips) : 1U, 0U);
    }
    if (r != NPH_RESULT_OK) {
        nph_audio_mixer_destroy(m);
        return r;
    }
    m->mailbox = NPH_ALLOC_ARRAY_ZEROED(a, MixVoiceInit, max_voices);
    m->clip_slots = NPH_ALLOC_ARRAY_ZEROED(a, MixClipSlot, max_clips);
    m->voice_clip = NPH_ALLOC_ARRAY_ZEROED(a, u32, max_voices);
    m->bus_mirror = NPH_ALLOC_ARRAY_ZEROED(a, MixBusMirror, max_buses);
    m->snapshot_mailbox = NPH_ALLOC_ARRAY_ZEROED(a, MixSnapshot, max_snapshots);
    m->voices = NPH_ALLOC_ARRAY_ZEROED(a, MixVoice, max_voices);
    m->audio_clips = NPH_ALLOC_ARRAY_ZEROED(a, MixClip *, max_clips);
    m->clip_voice_count = NPH_ALLOC_ARRAY_ZEROED(a, u32, max_clips);
    m->clip_removing = NPH_ALLOC_ARRAY_ZEROED(a, b32, max_clips);
    m->buses = NPH_ALLOC_ARRAY_ZEROED(a, MixBus, max_buses);
    m->bus_order = NPH_ALLOC_ARRAY_ZEROED(a, u32, max_buses);
    m->snapshots = NPH_ALLOC_ARRAY_ZEROED(a, MixSnapshot, max_snapshots);
    m->contexts = NPH_ALLOC_ARRAY_ZEROED(a, MixContext, m->context_count);
    m->ranking_keys = NPH_ALLOC_ARRAY_ZEROED(a, u64, max_voices);
    m->block_out = NPH_ALLOC_ARRAY_ZEROED(a, f32, (u64)MIX_BLOCK * channels);
    m->bus_level = NPH_ALLOC_ARRAY_ZEROED(a, f32, max_buses);
    m->bus_target = NPH_ALLOC_ARRAY_ZEROED(a, f32, max_buses);
    m->bus_peak = NPH_ALLOC_ARRAY_ZEROED(a, NphAtomicU32, max_buses);
    m->bus_rms = NPH_ALLOC_ARRAY_ZEROED(a, NphAtomicU32, max_buses);
    m->pending_clip_removals = NPH_ALLOC_ARRAY_ZEROED(a, u32, max_clips);
    m->pending_bus_removals = NPH_ALLOC_ARRAY_ZEROED(a, u32, max_buses);
    bool ok = m->pending_clip_removals != NULL && m->pending_bus_removals != NULL &&
              m->mailbox != NULL && m->clip_slots != NULL && m->voice_clip != NULL &&
              m->bus_mirror != NULL && m->snapshot_mailbox != NULL && m->voices != NULL &&
              m->audio_clips != NULL && m->clip_voice_count != NULL && m->clip_removing != NULL &&
              m->buses != NULL && m->bus_order != NULL && m->snapshots != NULL &&
              m->contexts != NULL && m->ranking_keys != NULL && m->block_out != NULL &&
              m->bus_level != NULL && m->bus_target != NULL && m->bus_peak != NULL &&
              m->bus_rms != NULL;
    // Clips may have more channels than the output: the window and the
    // resampled block are per source channel.
    for (u32 c = 0U; ok && c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        m->window[c] = NPH_ALLOC_ARRAY_ZEROED(a, f32, MIX_WINDOW);
        m->voice_out[c] = NPH_ALLOC_ARRAY_ZEROED(a, f32, MIX_BLOCK);
        ok = m->window[c] != NULL && m->voice_out[c] != NULL;
    }
    if (ok) {
        m->filter_dry = NPH_ALLOC_ARRAY_ZEROED(a, f32, MIX_BLOCK);
        ok = m->filter_dry != NULL;
    }
    for (u32 i = 0U; ok && i < max_buses; ++i) {
        for (u32 c = 0U; ok && c < channels; ++c) {
            m->buses[i].buffer[c] = NPH_ALLOC_ARRAY_ZEROED(a, f32, MIX_BLOCK);
            ok = m->buses[i].buffer[c] != NULL;
        }
    }
    if (!ok) {
        nph_audio_mixer_destroy(m);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    // The free list of contexts.
    for (u32 i = 0U; i < m->context_count; ++i) {
        m->contexts[i].next_free = i + 1U < m->context_count ? i + 1U : MIX_NONE;
    }
    m->context_free = 0U;
    for (u32 i = 0U; i < max_voices; ++i) {
        m->voices[i].context = MIX_NONE;
    }
    // The master: slot 0, generation 0, its own parent.
    MixBus *master = &m->buses[0];
    master->active = 1;
    master->parent = MIX_NONE;
    master->volume = 1.0F;
    master->gain_current = 1.0F;
    master->duck_trigger = MIX_NONE;
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        master->send_bus[s] = MIX_NONE;
    }
    m->bus_mirror[0].generation = 0U;
    m->bus_mirror[0].parent = MIX_NONE;
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        m->bus_mirror[0].send_bus[s] = MIX_NONE;
    }
    memcpy(m->bus_mirror[0].name, "master", 7U);
    m->bus_generation_next = 1U;
    m->bus_order_dirty = 1;
    *out = m;
    return NPH_RESULT_OK;
}

u32 nph_audio_mixer_sample_rate(const NphAudioMixer *mixer)
{
    return mixer->rate;
}

u32 nph_audio_mixer_channels(const NphAudioMixer *mixer)
{
    return mixer->channels;
}

void nph_audio_mixer_stats(const NphAudioMixer *mixer, NphAudioMixerStats *out)
{
    memset(out, 0, sizeof *out);
    out->blocks = nph_atomic_load_u64(&mixer->stat_blocks, NPH_MEMORY_ORDER_RELAXED);
    out->voices_playing = nph_atomic_load_u32(&mixer->stat_playing, NPH_MEMORY_ORDER_RELAXED);
    out->voices_real = nph_atomic_load_u32(&mixer->stat_real, NPH_MEMORY_ORDER_RELAXED);
    out->voices_virtual = nph_atomic_load_u32(&mixer->stat_virtual, NPH_MEMORY_ORDER_RELAXED);
    out->commands_dropped = nph_atomic_load_u32(&mixer->stat_dropped, NPH_MEMORY_ORDER_RELAXED);
    out->block_ns_last = nph_atomic_load_u64(&mixer->stat_ns_last, NPH_MEMORY_ORDER_RELAXED);
    out->block_ns_max = nph_atomic_load_u64(&mixer->stat_ns_max, NPH_MEMORY_ORDER_RELAXED);
    out->block_ns_total = nph_atomic_load_u64(&mixer->stat_ns_total, NPH_MEMORY_ORDER_RELAXED);
    out->corrupt_reads = nph_atomic_load_u32(&mixer->stat_corrupt, NPH_MEMORY_ORDER_RELAXED);
}

// SPDX-License-Identifier: Apache-2.0
// The mixer's game-thread half (mixer.h): clips, buses, snapshots, voices,
// and the update that hears back from the audio thread. Everything here
// validates and then sends a command; nothing touches the audio thread's
// state.

#include <nepheline/core/assert.h>

#include "mixer_internal.h"

#include <math.h>
#include <string.h>

#define MIX_BUS_INDEX_BITS 8U
#define MIX_BUS_INDEX_MASK ((1U << MIX_BUS_INDEX_BITS) - 1U)

static bool api_push(NphAudioMixer *m, const MixCommand *cmd)
{
    if (!nph_spsc_ring_push(&m->commands, cmd)) {
        (void)nph_atomic_fetch_add_u32(&m->stat_dropped, 1U, NPH_MEMORY_ORDER_RELAXED);
        return false;
    }
    return true;
}

// 0 .. 1, and 0 for what is not a number.
static f32 api_unit(f32 x)
{
    return x > 0.0F ? (x < 1.0F ? x : 1.0F) : 0.0F;
}

static MixCommand api_command(u32 type, u32 index)
{
    MixCommand c;
    memset(&c, 0, sizeof c);
    c.type = type;
    c.index = index;
    return c;
}

static u32 api_bus_slot(const NphAudioMixer *m, NphAudioBus bus)
{
    const u32 index = bus.value & MIX_BUS_INDEX_MASK;
    const u32 generation = bus.value >> MIX_BUS_INDEX_BITS;
    if (index >= m->max_buses) {
        return MIX_NONE;
    }
    if (index == 0U) {
        return generation == 0U ? 0U : MIX_NONE;
    }
    const MixBusMirror *b = &m->bus_mirror[index];
    return b->generation != 0U && b->generation == generation && !b->releasing ? index : MIX_NONE;
}

bool mix_push(NphAudioMixer *m, const MixCommand *cmd)
{
    return api_push(m, cmd);
}

MixCommand mix_command(u32 type, u32 index)
{
    return api_command(type, index);
}

u32 mix_bus_slot(const NphAudioMixer *m, NphAudioBus bus)
{
    return api_bus_slot(m, bus);
}

static u32 api_voice_slot(const NphAudioMixer *m, NphAudioVoice voice)
{
    const NphHandle h = {voice.value};
    return nph_handle_pool_is_valid(&m->voice_handles, h)
               ? nph_handle_pool_index(&m->voice_handles, h)
               : MIX_NONE;
}

static u32 api_clip_slot(const NphAudioMixer *m, NphAudioClip clip)
{
    const NphHandle h = {clip.value};
    if (!nph_handle_pool_is_valid(&m->clip_handles, h)) {
        return MIX_NONE;
    }
    const u32 index = nph_handle_pool_index(&m->clip_handles, h);
    return m->clip_slots[index].releasing ? MIX_NONE : index;
}

static void api_free_clip(NphAudioMixer *m, MixClip *c)
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
    if (c->stream != NULL) {
        mix_stream_close(c->streamer, c->stream); // the streamer frees it
    }
    NPH_FREE_TYPE(m->alloc, MixClip, c);
}

// ---------------------------------------------------------------------------
// The update
// ---------------------------------------------------------------------------

void nph_audio_mixer_update(NphAudioMixer *mixer)
{
    NphAudioMixer *m = mixer;
    // Removals the ring had no room for.
    u32 kept = 0U;
    for (u32 i = 0U; i < m->pending_clip_removal_count; ++i) {
        const MixCommand c = api_command(MIX_CMD_CLIP_REMOVE, m->pending_clip_removals[i]);
        if (!api_push(m, &c)) {
            m->pending_clip_removals[kept++] = m->pending_clip_removals[i];
        }
    }
    m->pending_clip_removal_count = kept;
    kept = 0U;
    for (u32 i = 0U; i < m->pending_bus_removal_count; ++i) {
        const MixCommand c = api_command(MIX_CMD_BUS_REMOVE, m->pending_bus_removals[i]);
        if (!api_push(m, &c)) {
            m->pending_bus_removals[kept++] = m->pending_bus_removals[i];
        }
    }
    m->pending_bus_removal_count = kept;
    kept = 0U;
    for (u32 i = 0U; i < m->pending_effect_removal_count; ++i) {
        const MixCommand c = api_command(MIX_CMD_EFFECT_REMOVE, m->pending_effect_removals[i]);
        if (!api_push(m, &c)) {
            m->pending_effect_removals[kept++] = m->pending_effect_removals[i];
        }
    }
    m->pending_effect_removal_count = kept;
    MixEvent e;
    while (nph_spsc_ring_pop(&m->events, &e)) {
        switch (e.type) {
        case MIX_EVENT_VOICE_END: {
            const NphHandle h = {e.generation};
            NPH_VERIFY(nph_handle_pool_release(&m->voice_handles, h));
            break;
        }
        case MIX_EVENT_CLIP_RELEASED: {
            MixClipSlot *slot = &m->clip_slots[e.index];
            api_free_clip(m, slot->clip);
            NPH_VERIFY(nph_handle_pool_release(&m->clip_handles, slot->handle));
            memset(slot, 0, sizeof *slot);
            break;
        }
        case MIX_EVENT_BUS_RELEASED: {
            MixBusMirror *b = &m->bus_mirror[e.index];
            memset(b, 0, sizeof *b);
            break;
        }
        case MIX_EVENT_EFFECT_RELEASED: {
            MixEffectSlot *slot = &m->effect_slots[e.index];
            mix_effect_free(m, slot->effect);
            NPH_VERIFY(nph_handle_pool_release(&m->effect_handles, slot->handle));
            memset(slot, 0, sizeof *slot);
            break;
        }
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Clips
// ---------------------------------------------------------------------------

NphResult nph_audio_clip_create(NphAudioMixer *mixer, const NphAudioClipDesc *desc,
                                NphAudioClip *out)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(desc != NULL && out != NULL);
    out->value = 0U;
    if ((u32)desc->codec >= (u32)NPH_AUDIO_CODEC_COUNT || desc->channels == 0U ||
        desc->channels > NPH_AUDIO_MAX_CHANNELS || desc->sample_rate < NPH_AUDIO_MIN_RATE ||
        desc->sample_rate > NPH_AUDIO_MAX_RATE || desc->frames == 0U ||
        desc->frames > NPH_AUDIO_MAX_CLIP_FRAMES || desc->data == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 loop_end = desc->loop_end != 0U ? desc->loop_end : desc->frames;
    if (desc->loop_start >= loop_end || loop_end > desc->frames) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    MixClip clip;
    memset(&clip, 0, sizeof clip);
    clip.codec = desc->codec;
    clip.format = desc->format;
    clip.channels = desc->channels;
    clip.sample_rate = desc->sample_rate;
    clip.frames = desc->frames;
    clip.loop_start = desc->loop_start;
    clip.loop_end = loop_end;
    clip.size = desc->size;
    clip.rate_ratio = (f64)desc->sample_rate / (f64)m->rate;
    // Check the data here, on the game thread, so the audio thread never
    // meets a malformed clip.
    switch (desc->codec) {
    case NPH_AUDIO_CODEC_PCM: {
        const u32 bytes = nph_audio_format_bytes(desc->format);
        if (bytes == 0U) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        clip.bytes_per_frame = bytes * desc->channels;
        if (desc->size < desc->frames * clip.bytes_per_frame) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        break;
    }
    case NPH_AUDIO_CODEC_LOSSLESS: {
        const NphLosslessStream s = {
            desc->channels,         desc->bits, desc->block_frames,  desc->frames,
            (const u8 *)desc->data, desc->size, desc->block_offsets, desc->block_count};
        NPH_TRY(nph_lossless_validate(&s));
        clip.lossless = s;
        break;
    }
    case NPH_AUDIO_CODEC_ADPCM: {
        const NphAdpcmStream s = {desc->channels, desc->frames, (const u8 *)desc->data, desc->size};
        NPH_TRY(nph_adpcm_validate(&s));
        clip.adpcm = s;
        break;
    }
    case NPH_AUDIO_CODEC_COUNT:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphHandle h;
    if (nph_handle_pool_alloc(&m->clip_handles, &h) != NPH_RESULT_OK) {
        return NPH_RESULT_FULL;
    }
    MixClip *c = NPH_ALLOC_TYPE(m->alloc, MixClip);
    if (c == NULL) {
        NPH_VERIFY(nph_handle_pool_release(&m->clip_handles, h));
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    *c = clip;
    c->data = (const u8 *)desc->data;
    if (!desc->borrow) {
        c->owned_data = (u8 *)NPH_ALLOC(m->alloc, desc->size, 16U);
        if (c->owned_data == NULL) {
            api_free_clip(m, c);
            NPH_VERIFY(nph_handle_pool_release(&m->clip_handles, h));
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        memcpy(c->owned_data, desc->data, (size_t)desc->size);
        c->data = c->owned_data;
        if (desc->codec == NPH_AUDIO_CODEC_LOSSLESS && desc->block_count > 0U) {
            c->owned_offsets = NPH_ALLOC_ARRAY(m->alloc, u32, desc->block_count);
            if (c->owned_offsets == NULL) {
                api_free_clip(m, c);
                NPH_VERIFY(nph_handle_pool_release(&m->clip_handles, h));
                return NPH_RESULT_OUT_OF_MEMORY;
            }
            memcpy(c->owned_offsets, desc->block_offsets, (size_t)desc->block_count * sizeof(u32));
            c->owned_offset_count = desc->block_count;
            c->lossless.block_offsets = c->owned_offsets;
        }
    }
    c->lossless.data = c->data;
    c->adpcm.data = c->data;
    const u32 index = nph_handle_pool_index(&m->clip_handles, h);
    MixCommand cmd = api_command(MIX_CMD_CLIP_ADD, index);
    cmd.ptr = c;
    if (!api_push(m, &cmd)) {
        api_free_clip(m, c);
        NPH_VERIFY(nph_handle_pool_release(&m->clip_handles, h));
        return NPH_RESULT_FULL;
    }
    m->clip_slots[index].clip = c;
    m->clip_slots[index].handle = h;
    m->clip_slots[index].releasing = 0;
    out->value = h.value;
    return NPH_RESULT_OK;
}

void nph_audio_clip_destroy(NphAudioMixer *mixer, NphAudioClip clip)
{
    NphAudioMixer *m = mixer;
    const u32 index = api_clip_slot(m, clip);
    if (index == MIX_NONE) {
        return;
    }
    m->clip_slots[index].releasing = 1;
    const MixCommand c = api_command(MIX_CMD_CLIP_REMOVE, index);
    if (!api_push(m, &c)) {
        m->pending_clip_removals[m->pending_clip_removal_count++] = index;
    }
}

bool nph_audio_clip_is_valid(const NphAudioMixer *mixer, NphAudioClip clip)
{
    return api_clip_slot(mixer, clip) != MIX_NONE;
}

f64 nph_audio_clip_duration(const NphAudioMixer *mixer, NphAudioClip clip)
{
    const u32 index = api_clip_slot(mixer, clip);
    if (index == MIX_NONE) {
        return 0.0;
    }
    const MixClip *c = mixer->clip_slots[index].clip;
    if (c->stream != NULL && c->stream->loop) {
        return (f64)INFINITY;
    }
    return (f64)c->frames / (f64)c->sample_rate;
}

u64 nph_audio_clip_stream_ahead(const NphAudioMixer *mixer, NphAudioClip clip)
{
    const u32 index = api_clip_slot(mixer, clip);
    if (index == MIX_NONE || mixer->clip_slots[index].clip->stream == NULL) {
        return 0U;
    }
    const MixStream *st = mixer->clip_slots[index].clip->stream;
    const u64 consumed = nph_atomic_load_u64(&st->consumed, NPH_MEMORY_ORDER_ACQUIRE);
    const u64 produced = nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE);
    return produced > consumed ? produced - consumed : 0U;
}

NphResult nph_audio_clip_create_stream(NphAudioMixer *mixer, NphAudioStreamer *streamer,
                                       const NphAudioStreamDesc *desc, NphAudioClip *out)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(desc != NULL && out != NULL);
    out->value = 0U;
    if (streamer == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    MixClip *c = NPH_ALLOC_TYPE(m->alloc, MixClip);
    if (c == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(c, 0, sizeof *c);
    NphResult r = mix_stream_open(streamer, desc, &c->stream);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(m->alloc, MixClip, c);
        return r;
    }
    c->streamer = streamer;
    c->codec = desc->codec;
    c->format = NPH_AUDIO_FORMAT_F32;
    c->channels = desc->channels;
    c->sample_rate = desc->sample_rate;
    c->frames = c->stream->total;
    c->loop_start = 0U;
    c->loop_end = c->frames;
    c->rate_ratio = (f64)desc->sample_rate / (f64)m->rate;
    NphHandle h;
    if (nph_handle_pool_alloc(&m->clip_handles, &h) != NPH_RESULT_OK) {
        api_free_clip(m, c);
        return NPH_RESULT_FULL;
    }
    const u32 index = nph_handle_pool_index(&m->clip_handles, h);
    MixCommand cmd = api_command(MIX_CMD_CLIP_ADD, index);
    cmd.ptr = c;
    if (!api_push(m, &cmd)) {
        NPH_VERIFY(nph_handle_pool_release(&m->clip_handles, h));
        api_free_clip(m, c);
        return NPH_RESULT_FULL;
    }
    MixClipSlot *slot = &m->clip_slots[index];
    slot->clip = c;
    slot->handle = h;
    slot->releasing = 0;
    slot->stream_played = 0;
    out->value = h.value;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Buses
// ---------------------------------------------------------------------------

NphAudioBusDesc nph_audio_bus_desc_default(void)
{
    NphAudioBusDesc d;
    memset(&d, 0, sizeof d);
    d.parent = NPH_AUDIO_MASTER_BUS;
    d.volume = 1.0F;
    return d;
}

NphResult nph_audio_bus_create(NphAudioMixer *mixer, const NphAudioBusDesc *desc, NphAudioBus *out)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(desc != NULL && out != NULL);
    out->value = 0U;
    const u32 parent = api_bus_slot(m, desc->parent);
    if (parent == MIX_NONE) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 index = MIX_NONE;
    for (u32 i = 1U; i < m->max_buses; ++i) {
        if (m->bus_mirror[i].generation == 0U && !m->bus_mirror[i].releasing) {
            index = i;
            break;
        }
    }
    if (index == MIX_NONE) {
        return NPH_RESULT_FULL;
    }
    const u32 generation = m->bus_generation_next;
    m->bus_generation_next = (m->bus_generation_next + 1U) & 0x00FFFFFFU;
    if (m->bus_generation_next == 0U) {
        m->bus_generation_next = 1U;
    }
    MixCommand cmd = api_command(MIX_CMD_BUS_ADD, index);
    cmd.param = parent;
    cmd.values[0] = desc->volume;
    if (!api_push(m, &cmd)) {
        return NPH_RESULT_FULL;
    }
    MixBusMirror *b = &m->bus_mirror[index];
    memset(b, 0, sizeof *b);
    b->generation = generation;
    b->parent = parent;
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        b->send_bus[s] = MIX_NONE;
    }
    if (desc->name != NULL) {
        const size_t n = strlen(desc->name);
        const size_t keep = n < sizeof b->name - 1U ? n : sizeof b->name - 1U;
        memcpy(b->name, desc->name, keep);
        b->name[keep] = '\0';
    }
    out->value = (generation << MIX_BUS_INDEX_BITS) | index;
    return NPH_RESULT_OK;
}

void nph_audio_bus_destroy(NphAudioMixer *mixer, NphAudioBus bus)
{
    NphAudioMixer *m = mixer;
    const u32 index = api_bus_slot(m, bus);
    if (index == MIX_NONE || index == 0U) {
        return;
    }
    MixBusMirror *b = &m->bus_mirror[index];
    b->releasing = 1;
    // Its effects go with it: the audio thread releases them when it
    // removes the bus.
    for (u32 i = 0U; i < m->max_effects; ++i) {
        MixEffectSlot *s = &m->effect_slots[i];
        if (s->effect != NULL && s->bus == index) {
            s->releasing = 1;
        }
    }
    // The mirror moves its children to its parent and forgets sends to it,
    // as the audio thread will.
    for (u32 i = 1U; i < m->max_buses; ++i) {
        MixBusMirror *o = &m->bus_mirror[i];
        if (o->generation == 0U || i == index) {
            continue;
        }
        if (o->parent == index) {
            o->parent = b->parent;
        }
        for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
            if (o->send_bus[s] == index) {
                o->send_bus[s] = MIX_NONE;
            }
        }
    }
    const MixCommand c = api_command(MIX_CMD_BUS_REMOVE, index);
    if (!api_push(m, &c)) {
        m->pending_bus_removals[m->pending_bus_removal_count++] = index;
    }
}

bool nph_audio_bus_is_valid(const NphAudioMixer *mixer, NphAudioBus bus)
{
    return api_bus_slot(mixer, bus) != MIX_NONE;
}

void nph_audio_bus_set_volume(NphAudioMixer *mixer, NphAudioBus bus, f32 volume)
{
    const u32 index = api_bus_slot(mixer, bus);
    if (index == MIX_NONE) {
        return;
    }
    MixCommand c = api_command(MIX_CMD_BUS_SET, index);
    c.param = MIX_BUS_VOLUME;
    c.values[0] = volume > 0.0F ? volume : 0.0F;
    (void)api_push(mixer, &c);
}

void nph_audio_bus_set_mute(NphAudioMixer *mixer, NphAudioBus bus, bool mute)
{
    const u32 index = api_bus_slot(mixer, bus);
    if (index == MIX_NONE) {
        return;
    }
    MixCommand c = api_command(MIX_CMD_BUS_SET, index);
    c.param = MIX_BUS_MUTE;
    c.values[0] = mute ? 1.0F : 0.0F;
    (void)api_push(mixer, &c);
}

// True when `from` feeds, through parents and sends, into `to`.
static bool api_feeds(const NphAudioMixer *m, u32 from, u32 to, u32 depth)
{
    if (from == to) {
        return true;
    }
    if (from == MIX_NONE || depth > m->max_buses) {
        return false;
    }
    const MixBusMirror *b = &m->bus_mirror[from];
    if (from != 0U && api_feeds(m, b->parent, to, depth + 1U)) {
        return true;
    }
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        if (b->send_bus[s] != MIX_NONE && api_feeds(m, b->send_bus[s], to, depth + 1U)) {
            return true;
        }
    }
    return false;
}

NphResult nph_audio_bus_set_send(NphAudioMixer *mixer, NphAudioBus bus, u32 slot,
                                 NphAudioBus target, f32 gain)
{
    NphAudioMixer *m = mixer;
    const u32 from = api_bus_slot(m, bus);
    if (from == MIX_NONE || slot >= NPH_AUDIO_MAX_SENDS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 to = MIX_NONE;
    if (gain > 0.0F) {
        to = api_bus_slot(m, target);
        // The target must not feed back into this bus.
        if (to == MIX_NONE || from == 0U || api_feeds(m, to, from, 0U)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    MixCommand c = api_command(MIX_CMD_BUS_SEND, from);
    c.param = slot;
    c.generation = to;
    c.values[0] = gain > 0.0F ? gain : 0.0F;
    if (!api_push(m, &c)) {
        return NPH_RESULT_FULL;
    }
    m->bus_mirror[from].send_bus[slot] = to;
    return NPH_RESULT_OK;
}

void nph_audio_bus_set_duck(NphAudioMixer *mixer, NphAudioBus target, NphAudioBus trigger,
                            f32 depth_db, f32 threshold_db, f32 attack, f32 release)
{
    const u32 t = api_bus_slot(mixer, target);
    const u32 g = api_bus_slot(mixer, trigger);
    if (t == MIX_NONE || g == MIX_NONE || t == g) {
        return;
    }
    MixCommand c = api_command(MIX_CMD_BUS_DUCK, t);
    c.param = depth_db < 0.0F ? g : MIX_NONE;
    c.values[0] = depth_db < 0.0F ? depth_db : 0.0F;
    c.values[1] = threshold_db;
    c.values[2] = attack > 0.0F ? attack : 0.0F;
    c.values[3] = release > 0.0F ? release : 0.0F;
    (void)api_push(mixer, &c);
}

void nph_audio_bus_meter(const NphAudioMixer *mixer, NphAudioBus bus, f32 *out_peak, f32 *out_rms)
{
    const u32 index = api_bus_slot(mixer, bus);
    f32 peak = 0.0F;
    f32 rms = 0.0F;
    if (index != MIX_NONE) {
        const u32 p = nph_atomic_load_u32(&mixer->bus_peak[index], NPH_MEMORY_ORDER_RELAXED);
        const u32 r = nph_atomic_load_u32(&mixer->bus_rms[index], NPH_MEMORY_ORDER_RELAXED);
        memcpy(&peak, &p, sizeof peak);
        memcpy(&rms, &r, sizeof rms);
    }
    if (out_peak != NULL) {
        *out_peak = peak;
    }
    if (out_rms != NULL) {
        *out_rms = rms;
    }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

NphResult nph_audio_snapshot_create(NphAudioMixer *mixer, const NphAudioSnapshotEntry *entries,
                                    u32 count, NphAudioSnapshot *out)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(out != NULL);
    out->value = 0U;
    if (count > NPH_AUDIO_SNAPSHOT_MAX_ENTRIES || (count > 0U && entries == NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (m->snapshot_count >= m->max_snapshots) {
        return NPH_RESULT_FULL;
    }
    const u32 index = m->snapshot_count;
    MixSnapshot *s = &m->snapshot_mailbox[index];
    memset(s, 0, sizeof *s);
    for (u32 i = 0U; i < count; ++i) {
        const u32 bus = api_bus_slot(m, entries[i].bus);
        if (bus == MIX_NONE) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        s->entries[i] = entries[i];
        s->entries[i].bus.value = bus; // the slot, for the audio thread
        s->entries[i].lowpass_hz = entries[i].lowpass_hz > 0.0F ? entries[i].lowpass_hz : 0.0F;
    }
    s->count = count;
    s->active = 1;
    const MixCommand c = api_command(MIX_CMD_SNAPSHOT_ADD, index);
    if (!api_push(m, &c)) {
        return NPH_RESULT_FULL;
    }
    m->snapshot_count += 1U;
    out->value = index + 1U;
    return NPH_RESULT_OK;
}

void nph_audio_snapshot_set_weight(NphAudioMixer *mixer, NphAudioSnapshot snapshot, f32 weight,
                                   f32 seconds)
{
    if (snapshot.value == 0U || snapshot.value > mixer->snapshot_count) {
        return;
    }
    MixCommand c = api_command(MIX_CMD_SNAPSHOT_WEIGHT, snapshot.value - 1U);
    c.values[0] = weight < 0.0F ? 0.0F : (weight > 1.0F ? 1.0F : weight);
    c.values[1] = seconds > 0.0F ? seconds : 0.0F;
    (void)api_push(mixer, &c);
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

NphAudioPlayDesc nph_audio_play_desc_default(NphAudioClip clip)
{
    NphAudioPlayDesc d;
    memset(&d, 0, sizeof d);
    d.clip = clip;
    d.bus = NPH_AUDIO_MASTER_BUS;
    d.volume = 1.0F;
    d.pitch = 1.0F;
    d.priority = 128U;
    d.attenuation = nph_audio_attenuation_default();
    d.doppler = 1.0F;
    d.air_absorption = 1.0F;
    d.send_distance = 0.5F;
    return d;
}

NphResult nph_audio_voice_play(NphAudioMixer *mixer, const NphAudioPlayDesc *desc,
                               NphAudioVoice *out)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(desc != NULL && out != NULL);
    out->value = 0U;
    const u32 clip = api_clip_slot(m, desc->clip);
    const u32 bus = api_bus_slot(m, desc->bus);
    if (clip == MIX_NONE || bus == MIX_NONE ||
        desc->start_frame >= m->clip_slots[clip].clip->frames) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // A stream plays once, from where it was opened.
    const bool stream = m->clip_slots[clip].clip->stream != NULL;
    if (stream && desc->start_frame != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (stream && m->clip_slots[clip].stream_played) {
        return NPH_RESULT_INVALID_STATE;
    }
    u32 sends[NPH_AUDIO_MAX_SENDS];
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        sends[s] = MIX_NONE;
        if (desc->send_gains[s] > 0.0F) {
            sends[s] = api_bus_slot(m, desc->sends[s]);
            if (sends[s] == MIX_NONE) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
    }
    NphHandle h;
    if (nph_handle_pool_alloc(&m->voice_handles, &h) != NPH_RESULT_OK) {
        return NPH_RESULT_FULL;
    }
    const u32 index = nph_handle_pool_index(&m->voice_handles, h);
    MixVoiceInit *init = &m->mailbox[index];
    init->clip = clip;
    init->bus = bus;
    init->volume = desc->volume > 0.0F ? desc->volume : 0.0F;
    init->pitch = desc->pitch > 1e-3F ? desc->pitch : 1e-3F;
    init->pan = desc->pan < -1.0F ? -1.0F : (desc->pan > 1.0F ? 1.0F : desc->pan);
    init->priority = desc->priority;
    init->loop = desc->loop;
    init->start_frame = desc->start_frame;
    init->delay_frames = desc->delay_frames;
    init->fade_in = desc->fade_in > 0.0F ? desc->fade_in : 0.0F;
    init->lowpass_hz = desc->lowpass_hz > 0.0F ? desc->lowpass_hz : 0.0F;
    init->highpass_hz = desc->highpass_hz > 0.0F ? desc->highpass_hz : 0.0F;
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        init->send_bus[s] = sends[s];
        init->send_gain[s] = sends[s] != MIX_NONE ? desc->send_gains[s] : 0.0F;
    }
    init->sequence = m->sequence++;
    init->spatial = desc->spatial != 0;
    init->position[0] = desc->position.x;
    init->position[1] = desc->position.y;
    init->position[2] = desc->position.z;
    init->velocity[0] = desc->velocity.x;
    init->velocity[1] = desc->velocity.y;
    init->velocity[2] = desc->velocity.z;
    init->attenuation = desc->attenuation;
    init->size = desc->size > 0.0F ? desc->size : 0.0F;
    init->doppler = desc->doppler > 0.0F ? desc->doppler : 0.0F;
    init->air = desc->air_absorption > 0.0F ? desc->air_absorption : 0.0F;
    init->send_distance = desc->send_distance < 0.0F
                              ? 0.0F
                              : (desc->send_distance > 1.0F ? 1.0F : desc->send_distance);
    init->occlusion = api_unit(desc->occlusion);
    init->obstruction = api_unit(desc->obstruction);
    MixCommand c = api_command(MIX_CMD_VOICE_PLAY, index);
    c.generation = h.value;
    if (!api_push(m, &c)) {
        NPH_VERIFY(nph_handle_pool_release(&m->voice_handles, h));
        return NPH_RESULT_FULL;
    }
    m->voice_clip[index] = clip;
    if (stream) {
        m->clip_slots[clip].stream_played = 1;
    }
    out->value = h.value;
    return NPH_RESULT_OK;
}

static void api_voice_command(NphAudioMixer *m, NphAudioVoice voice, u32 type, u32 param, f32 a,
                              f32 b)
{
    const u32 index = api_voice_slot(m, voice);
    if (index == MIX_NONE) {
        return;
    }
    MixCommand c = api_command(type, index);
    c.generation = voice.value;
    c.param = param;
    c.values[0] = a;
    c.values[1] = b;
    (void)api_push(m, &c);
}

void nph_audio_voice_stop(NphAudioMixer *mixer, NphAudioVoice voice, f32 fade)
{
    api_voice_command(mixer, voice, MIX_CMD_VOICE_STOP, 0U, fade > 0.0F ? fade : 0.0F, 0.0F);
}

bool nph_audio_voice_is_playing(const NphAudioMixer *mixer, NphAudioVoice voice)
{
    return api_voice_slot(mixer, voice) != MIX_NONE;
}

void nph_audio_voice_set_volume(NphAudioMixer *mixer, NphAudioVoice voice, f32 volume)
{
    api_voice_command(mixer, voice, MIX_CMD_VOICE_SET, MIX_VOICE_VOLUME,
                      volume > 0.0F ? volume : 0.0F, 0.0F);
}

void nph_audio_voice_set_pitch(NphAudioMixer *mixer, NphAudioVoice voice, f32 pitch)
{
    api_voice_command(mixer, voice, MIX_CMD_VOICE_SET, MIX_VOICE_PITCH,
                      pitch > 1e-3F ? pitch : 1e-3F, 0.0F);
}

void nph_audio_voice_set_pan(NphAudioMixer *mixer, NphAudioVoice voice, f32 pan)
{
    api_voice_command(mixer, voice, MIX_CMD_VOICE_SET, MIX_VOICE_PAN,
                      pan < -1.0F ? -1.0F : (pan > 1.0F ? 1.0F : pan), 0.0F);
}

void nph_audio_voice_set_paused(NphAudioMixer *mixer, NphAudioVoice voice, bool paused)
{
    api_voice_command(mixer, voice, MIX_CMD_VOICE_SET, MIX_VOICE_PAUSED, paused ? 1.0F : 0.0F,
                      0.0F);
}

void nph_audio_voice_set_filters(NphAudioMixer *mixer, NphAudioVoice voice, f32 lowpass_hz,
                                 f32 highpass_hz)
{
    api_voice_command(mixer, voice, MIX_CMD_VOICE_SET, MIX_VOICE_FILTERS,
                      lowpass_hz > 0.0F ? lowpass_hz : 0.0F,
                      highpass_hz > 0.0F ? highpass_hz : 0.0F);
}

static bool api_finite3(NphVec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

void nph_audio_voice_set_position(NphAudioMixer *mixer, NphAudioVoice voice, NphVec3 position,
                                  NphVec3 velocity)
{
    const u32 index = api_voice_slot(mixer, voice);
    if (index == MIX_NONE || !api_finite3(position) || !api_finite3(velocity)) {
        return;
    }
    MixCommand c = api_command(MIX_CMD_VOICE_SET, index);
    c.generation = voice.value;
    c.param = MIX_VOICE_POSITION;
    c.values[0] = position.x;
    c.values[1] = position.y;
    c.values[2] = position.z;
    c.values[3] = velocity.x;
    c.values[4] = velocity.y;
    c.values[5] = velocity.z;
    (void)api_push(mixer, &c);
}

void nph_audio_voice_set_occlusion(NphAudioMixer *mixer, NphAudioVoice voice, f32 occlusion,
                                   f32 obstruction)
{
    const u32 index = api_voice_slot(mixer, voice);
    if (index == MIX_NONE) {
        return;
    }
    MixCommand c = api_command(MIX_CMD_VOICE_SET, index);
    c.generation = voice.value;
    c.param = MIX_VOICE_OCCLUSION;
    c.values[0] = api_unit(occlusion);
    c.values[1] = api_unit(obstruction);
    (void)api_push(mixer, &c);
}

void nph_audio_voice_set_send(NphAudioMixer *mixer, NphAudioVoice voice, u32 slot, f32 gain)
{
    if (slot >= NPH_AUDIO_MAX_SENDS) {
        return;
    }
    api_voice_command(mixer, voice, MIX_CMD_VOICE_SET, MIX_VOICE_SEND, (f32)slot,
                      gain > 0.0F ? gain : 0.0F);
}

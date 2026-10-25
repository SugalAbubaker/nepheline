// SPDX-License-Identifier: Apache-2.0
// The mixer's audio-thread half (mixer.h; docs/audio.md "The block"): each
// block applies the game thread's commands, moves the snapshots and the
// ducking, ranks the voices and settles which are real, renders the real
// ones and advances the virtual ones, then runs the buses children first
// into the master. Nothing here allocates, locks, or waits: the whole
// render runs in a real-time scope (core/realtime.h).

#include <nepheline/core/assert.h>
#include <nepheline/core/realtime.h>
#include <nepheline/platform/time.h>

#include "mixer_internal.h"

#include <math.h>
#include <string.h>
#include <xmmintrin.h>

#define MR_REAL_BONUS 2.0F // hysteresis: a real voice's score counts double (6 dB)
// The MXCSR bits that flush denormals to zero (FTZ) and read them as zero
// (DAZ): a decaying filter or reverb tail would otherwise run at a fraction
// of its speed in the subnormal range.
#define MR_FTZ_DAZ 0x8040U

static u32 mr_bits(f32 v)
{
    u32 u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static void mr_event(NphAudioMixer *m, u32 type, u32 index, u32 generation)
{
    const MixEvent e = {type, index, generation, 0U};
    // The ring holds an event for every slot at once (mixer.c): never full.
    NPH_VERIFY(nph_spsc_ring_push(&m->events, &e));
}

// ---------------------------------------------------------------------------
// Contexts and voices
// ---------------------------------------------------------------------------

static u32 mr_context_acquire(NphAudioMixer *m)
{
    const u32 index = m->context_free;
    if (index == MIX_NONE) {
        return MIX_NONE;
    }
    MixContext *ctx = &m->contexts[index];
    m->context_free = ctx->next_free;
    ctx->history_len = 0U;
    ctx->history_first = 0;
    ctx->decoder_block = MIX_NONE;
    ctx->decoder_frame = 0U;
    memset(ctx->lowpass, 0, sizeof ctx->lowpass);
    memset(ctx->highpass, 0, sizeof ctx->highpass);
    return index;
}

static void mr_context_release(NphAudioMixer *m, MixVoice *v)
{
    if (v->context == MIX_NONE) {
        return;
    }
    m->contexts[v->context].next_free = m->context_free;
    m->context_free = v->context;
    v->context = MIX_NONE;
}

static void mr_voice_end(NphAudioMixer *m, u32 index)
{
    MixVoice *v = &m->voices[index];
    mr_context_release(m, v);
    v->state = MIX_VOICE_FREE;
    v->real = 0;
    v->demoting = 0;
    const u32 clip = v->clip;
    NPH_ASSERT(m->clip_voice_count[clip] > 0U);
    m->clip_voice_count[clip] -= 1U;
    mr_event(m, MIX_EVENT_VOICE_END, index, v->generation);
    if (m->clip_removing[clip] && m->clip_voice_count[clip] == 0U) {
        m->audio_clips[clip] = NULL;
        m->clip_removing[clip] = 0;
        mr_event(m, MIX_EVENT_CLIP_RELEASED, clip, 0U);
    }
}

// A voice's live one (state PLAYING and the handle's generation).
static MixVoice *mr_voice(NphAudioMixer *m, const MixCommand *c)
{
    if (c->index >= m->max_voices) {
        return NULL;
    }
    MixVoice *v = &m->voices[c->index];
    return v->state == MIX_VOICE_PLAYING && v->generation == c->generation ? v : NULL;
}

// Starts fading the voice out over `frames` (at least a block).
static void mr_voice_fade_out(MixVoice *v, f32 frames)
{
    const f32 n = frames > (f32)MIX_BLOCK ? frames : (f32)MIX_BLOCK;
    v->stopping = 1;
    v->fade_rate = -(v->fade > 0.0F ? v->fade : 1.0F) / n;
}

static void mr_play(NphAudioMixer *m, const MixCommand *c)
{
    const u32 index = c->index;
    const MixVoiceInit *in = &m->mailbox[index];
    MixVoice *v = &m->voices[index];
    NPH_ASSERT(v->state == MIX_VOICE_FREE && m->audio_clips[in->clip] != NULL);
    memset(v, 0, sizeof *v);
    v->state = MIX_VOICE_PLAYING;
    v->generation = c->generation;
    v->clip = in->clip;
    v->bus = m->buses[in->bus].active ? in->bus : 0U;
    v->pos = in->start_frame << 32U;
    v->volume = in->volume;
    v->pitch = in->pitch;
    v->pan = in->pan;
    v->priority = in->priority;
    v->loop = in->loop;
    v->delay = in->delay_frames;
    v->context = MIX_NONE;
    v->sequence = in->sequence;
    v->lowpass_hz = in->lowpass_hz;
    v->highpass_hz = in->highpass_hz;
    v->lowpass_now = in->lowpass_hz > 0.0F ? in->lowpass_hz : MIX_FULL_BAND;
    v->highpass_now = in->highpass_hz > 0.0F ? in->highpass_hz : MIX_HIGHPASS_OFF;
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        const u32 bus = in->send_bus[s];
        v->send_bus[s] = bus != MIX_NONE && m->buses[bus].active ? bus : MIX_NONE;
        v->send_gain[s] = v->send_bus[s] != MIX_NONE ? in->send_gain[s] : 0.0F;
    }
    if (in->fade_in > 0.0F) {
        v->fade = 0.0F;
        v->fade_rate = 1.0F / (in->fade_in * (f32)m->rate);
    } else {
        v->fade = 1.0F;
        // From the clip's start the voice begins at full gain, its own
        // attack intact; anywhere else it fades in over its first block.
        v->fresh = in->start_frame == 0U ? 1 : 0;
        v->entering = in->start_frame == 0U ? 0 : 1;
    }
    m->clip_voice_count[in->clip] += 1U;
}

static void mr_clip_remove(NphAudioMixer *m, u32 clip)
{
    m->clip_removing[clip] = 1;
    for (u32 i = 0U; i < m->max_voices; ++i) {
        MixVoice *v = &m->voices[i];
        if (v->state != MIX_VOICE_PLAYING || v->clip != clip) {
            continue;
        }
        if (v->real) {
            mr_voice_fade_out(v, (f32)MIX_BLOCK); // heard to the end of a block, then gone
        } else {
            mr_voice_end(m, i);
        }
    }
    if (m->clip_voice_count[clip] == 0U && m->audio_clips[clip] != NULL) {
        m->audio_clips[clip] = NULL;
        m->clip_removing[clip] = 0;
        mr_event(m, MIX_EVENT_CLIP_RELEASED, clip, 0U);
    }
}

// ---------------------------------------------------------------------------
// Buses
// ---------------------------------------------------------------------------

static void mr_bus_add(NphAudioMixer *m, const MixCommand *c)
{
    MixBus *b = &m->buses[c->index];
    // The buffers outlive the slot's buses: keep them across the reset.
    f32 *buffers[NPH_AUDIO_MAX_CHANNELS];
    for (u32 ch = 0U; ch < NPH_AUDIO_MAX_CHANNELS; ++ch) {
        buffers[ch] = b->buffer[ch];
    }
    memset(b, 0, sizeof *b);
    for (u32 ch = 0U; ch < NPH_AUDIO_MAX_CHANNELS; ++ch) {
        b->buffer[ch] = buffers[ch];
    }
    b->active = 1;
    b->parent = m->buses[c->param].active ? c->param : 0U;
    b->volume = c->values[0];
    b->gain_current = c->values[0];
    b->duck_trigger = MIX_NONE;
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        b->send_bus[s] = MIX_NONE;
    }
    m->bus_order_dirty = 1;
}

static void mr_bus_remove(NphAudioMixer *m, u32 index)
{
    MixBus *gone = &m->buses[index];
    const u32 parent = gone->parent;
    for (u32 i = 0U; i < m->max_buses; ++i) {
        MixBus *b = &m->buses[i];
        if (!b->active || i == index) {
            continue;
        }
        if (b->parent == index) {
            b->parent = parent;
        }
        for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
            if (b->send_bus[s] == index) {
                b->send_bus[s] = MIX_NONE;
                b->send_gain[s] = 0.0F;
            }
        }
        if (b->duck_trigger == index) {
            b->duck_trigger = MIX_NONE;
        }
    }
    for (u32 i = 0U; i < m->max_voices; ++i) {
        MixVoice *v = &m->voices[i];
        if (v->state != MIX_VOICE_PLAYING) {
            continue;
        }
        if (v->bus == index) {
            v->bus = parent;
        }
        for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
            if (v->send_bus[s] == index) {
                v->send_bus[s] = MIX_NONE;
                v->send_gain[s] = 0.0F;
            }
        }
    }
    gone->active = 0;
    m->bus_order_dirty = 1;
    mr_event(m, MIX_EVENT_BUS_RELEASED, index, 0U);
}

// Kahn's topological order over "feeds" (a bus into its parent and its
// send targets): every bus after all that feed it, so the master is last.
static void mr_bus_order(NphAudioMixer *m)
{
    u32 in_degree[256];
    u32 queue[256];
    const u32 n = m->max_buses;
    memset(in_degree, 0, sizeof(u32) * n);
    for (u32 i = 0U; i < n; ++i) {
        const MixBus *b = &m->buses[i];
        if (!b->active || i == 0U) {
            continue;
        }
        in_degree[b->parent] += 1U;
        for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
            if (b->send_bus[s] != MIX_NONE) {
                in_degree[b->send_bus[s]] += 1U;
            }
        }
    }
    u32 head = 0U;
    u32 tail = 0U;
    for (u32 i = 0U; i < n; ++i) {
        if (m->buses[i].active && in_degree[i] == 0U) {
            queue[tail++] = i;
        }
    }
    u32 count = 0U;
    while (head < tail) {
        const u32 x = queue[head++];
        m->bus_order[count++] = x;
        const MixBus *b = &m->buses[x];
        if (x == 0U) {
            continue;
        }
        if (--in_degree[b->parent] == 0U) {
            queue[tail++] = b->parent;
        }
        for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
            const u32 t = b->send_bus[s];
            if (t != MIX_NONE && --in_degree[t] == 0U) {
                queue[tail++] = t;
            }
        }
    }
    m->bus_order_count = count;
    m->bus_order_dirty = 0;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static f32 mr_per_block(const NphAudioMixer *m, f32 seconds)
{
    // A one-pole's coefficient per block for a time constant of `seconds`.
    if (!(seconds > 0.0F)) {
        return 1.0F;
    }
    return 1.0F - expf(-(f32)MIX_BLOCK / (seconds * (f32)m->rate));
}

static void mr_apply(NphAudioMixer *m, const MixCommand *c)
{
    switch ((MixCommandType)c->type) {
    case MIX_CMD_CLIP_ADD:
        m->audio_clips[c->index] = (MixClip *)c->ptr;
        m->clip_voice_count[c->index] = 0U;
        m->clip_removing[c->index] = 0;
        break;
    case MIX_CMD_CLIP_REMOVE:
        mr_clip_remove(m, c->index);
        break;
    case MIX_CMD_VOICE_PLAY:
        mr_play(m, c);
        break;
    case MIX_CMD_VOICE_STOP: {
        MixVoice *v = mr_voice(m, c);
        if (v != NULL) {
            if (v->real) {
                mr_voice_fade_out(v, c->values[0] * (f32)m->rate);
            } else {
                mr_voice_end(m, c->index); // a virtual voice is not heard: it just ends
            }
        }
        break;
    }
    case MIX_CMD_VOICE_SET: {
        MixVoice *v = mr_voice(m, c);
        if (v == NULL) {
            break;
        }
        switch ((MixVoiceParam)c->param) {
        case MIX_VOICE_VOLUME:
            v->volume = c->values[0];
            break;
        case MIX_VOICE_PITCH:
            v->pitch = c->values[0];
            break;
        case MIX_VOICE_PAN:
            v->pan = c->values[0];
            break;
        case MIX_VOICE_PAUSED:
            v->paused = c->values[0] != 0.0F;
            break;
        case MIX_VOICE_FILTERS:
            v->lowpass_hz = c->values[0];
            v->highpass_hz = c->values[1];
            break;
        case MIX_VOICE_SEND: {
            const u32 slot = (u32)c->values[0];
            if (slot < NPH_AUDIO_MAX_SENDS && v->send_bus[slot] != MIX_NONE) {
                v->send_gain[slot] = c->values[1];
            }
            break;
        }
        }
        break;
    }
    case MIX_CMD_BUS_ADD:
        mr_bus_add(m, c);
        break;
    case MIX_CMD_BUS_REMOVE:
        mr_bus_remove(m, c->index);
        break;
    case MIX_CMD_BUS_SET: {
        MixBus *b = &m->buses[c->index];
        if (c->param == MIX_BUS_VOLUME) {
            b->volume = c->values[0];
        } else {
            b->mute = c->values[0] != 0.0F;
        }
        break;
    }
    case MIX_CMD_BUS_SEND: {
        MixBus *b = &m->buses[c->index];
        const u32 target = c->generation;
        b->send_bus[c->param] = target != MIX_NONE && m->buses[target].active ? target : MIX_NONE;
        b->send_gain[c->param] = b->send_bus[c->param] != MIX_NONE ? c->values[0] : 0.0F;
        m->bus_order_dirty = 1;
        break;
    }
    case MIX_CMD_BUS_DUCK: {
        MixBus *b = &m->buses[c->index];
        b->duck_trigger = c->param != MIX_NONE && m->buses[c->param].active ? c->param : MIX_NONE;
        b->duck_depth_db = c->values[0];
        b->duck_threshold = nph_audio_db_to_gain(c->values[1]);
        b->duck_attack = mr_per_block(m, c->values[2]);
        b->duck_release = mr_per_block(m, c->values[3]);
        break;
    }
    case MIX_CMD_SNAPSHOT_ADD:
        m->snapshots[c->index] = m->snapshot_mailbox[c->index];
        m->snapshots[c->index].weight = 0.0F;
        m->snapshots[c->index].target = 0.0F;
        m->snapshots[c->index].rate = 1.0F;
        break;
    case MIX_CMD_SNAPSHOT_WEIGHT: {
        MixSnapshot *s = &m->snapshots[c->index];
        s->target = c->values[0];
        const f32 frames = c->values[1] * (f32)m->rate;
        s->rate = frames > (f32)MIX_BLOCK ? (f32)MIX_BLOCK / frames : 1.0F;
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// The block
// ---------------------------------------------------------------------------

// Snapshots' weights, ducking, and every bus's gain for this block, and its
// level counting its ancestors' (the ranking's estimate).
static void mr_controls(NphAudioMixer *m)
{
    for (u32 i = 0U; i < m->max_buses; ++i) {
        m->buses[i].snapshot_db = 0.0F;
        m->buses[i].snapshot_lowpass = 0.0F;
    }
    for (u32 i = 0U; i < m->snapshot_count; ++i) {
        MixSnapshot *s = &m->snapshots[i];
        if (!s->active) {
            continue;
        }
        const f32 delta = s->target - s->weight;
        s->weight =
            fabsf(delta) <= s->rate ? s->target : s->weight + (delta > 0.0F ? s->rate : -s->rate);
        if (s->weight <= 0.0F) {
            continue;
        }
        for (u32 e = 0U; e < s->count; ++e) {
            const NphAudioSnapshotEntry *entry = &s->entries[e];
            MixBus *b = &m->buses[entry->bus.value];
            if (!b->active) {
                continue;
            }
            b->snapshot_db += s->weight * entry->volume_db;
            if (entry->lowpass_hz > 0.0F) {
                // Glide the corner in log frequency with the weight.
                const f32 f = expf(logf(MIX_FULL_BAND) +
                                   s->weight * (logf(entry->lowpass_hz) - logf(MIX_FULL_BAND)));
                if (b->snapshot_lowpass == 0.0F || f < b->snapshot_lowpass) {
                    b->snapshot_lowpass = f;
                }
            }
        }
    }
    for (u32 i = 0U; i < m->max_buses; ++i) {
        MixBus *b = &m->buses[i];
        if (!b->active) {
            continue;
        }
        if (b->duck_trigger != MIX_NONE) {
            // The trigger's level from the last block: one block late, so
            // the order of buses does not matter.
            const f32 target =
                m->buses[b->duck_trigger].rms > b->duck_threshold ? b->duck_depth_db : 0.0F;
            const f32 k = target < b->duck_db ? b->duck_attack : b->duck_release;
            b->duck_db += (target - b->duck_db) * k;
        } else {
            b->duck_db = 0.0F;
        }
        const f32 db = b->duck_db + b->snapshot_db;
        m->bus_target[i] = b->mute ? 0.0F : b->volume * nph_audio_db_to_gain(db);
    }
    // Levels, parents before children: the order reversed.
    for (u32 k = m->bus_order_count; k > 0U; --k) {
        const u32 i = m->bus_order[k - 1U];
        const MixBus *b = &m->buses[i];
        m->bus_level[i] = m->bus_target[i] * (i == 0U ? 1.0F : m->bus_level[b->parent]);
    }
}

// Sorts keys descending (heapsort: no allocation, no recursion).
static void mr_sort_descending(u64 *keys, u32 n)
{
    // A min-heap, popped to the end, leaves the array descending.
    for (u32 start = n / 2U; start-- > 0U;) {
        u32 root = start;
        for (;;) {
            u32 child = 2U * root + 1U;
            if (child >= n) {
                break;
            }
            if (child + 1U < n && keys[child + 1U] < keys[child]) {
                child += 1U;
            }
            if (keys[root] <= keys[child]) {
                break;
            }
            const u64 t = keys[root];
            keys[root] = keys[child];
            keys[child] = t;
            root = child;
        }
    }
    for (u32 end = n; end-- > 1U;) {
        const u64 t = keys[0];
        keys[0] = keys[end];
        keys[end] = t;
        u32 root = 0U;
        for (;;) {
            u32 child = 2U * root + 1U;
            if (child >= end) {
                break;
            }
            if (child + 1U < end && keys[child + 1U] < keys[child]) {
                child += 1U;
            }
            if (keys[root] <= keys[child]) {
                break;
            }
            const u64 s = keys[root];
            keys[root] = keys[child];
            keys[child] = s;
            root = child;
        }
    }
}

// Ranks the voices that could sound this block and settles which are real.
static void mr_rank(NphAudioMixer *m)
{
    u32 n = 0U;
    for (u32 i = 0U; i < m->max_voices; ++i) {
        MixVoice *v = &m->voices[i];
        v->demoting = 0;
        if (v->state != MIX_VOICE_PLAYING) {
            continue;
        }
        if (v->paused || v->delay >= MIX_BLOCK) {
            if (v->real) {
                v->demoting = 1; // a paused real voice fades out, then waits
            }
            continue;
        }
        f32 score = v->volume * (v->fade_rate < 0.0F ? v->fade : 1.0F) * m->bus_level[v->bus];
        if (v->real) {
            score *= MR_REAL_BONUS;
        }
        score = score > 0.0F ? score : 0.0F;
        m->ranking_keys[n++] = ((u64)v->priority << 48U) | ((u64)mr_bits(score) << 16U) | i;
    }
    mr_sort_descending(m->ranking_keys, n);
    const u32 want = nph_min_u32(n, m->max_real);
    for (u32 k = 0U; k < n; ++k) {
        MixVoice *v = &m->voices[m->ranking_keys[k] & 0xFFFFU];
        if (k < want) {
            if (!v->real) {
                const u32 ctx = mr_context_acquire(m);
                if (ctx != MIX_NONE) {
                    v->context = ctx;
                    v->real = 1;
                    // Back from silence (or out of the delay mid-clip): fade in.
                    v->entering = v->fresh ? 0 : 1;
                }
            }
        } else if (v->real) {
            v->demoting = 1;
        }
    }
}

static void mr_bus_lowpass(NphAudioMixer *m, MixBus *b)
{
    // The snapshots' corner. Fully open is no filter, whose state starts
    // over the next time; the blocks it switches on and off in crossfade.
    const f32 f = b->snapshot_lowpass;
    const bool on = f > 0.0F && f < MIX_FULL_BAND;
    const bool was = b->lowpass_hz != 0.0F;
    if (on || was) {
        const NphAudioFilterCoeffs k = nph_audio_filter_design(
            NPH_AUDIO_FILTER_LOWPASS, on ? f : MIX_FULL_BAND, 0.7071068F, 0.0F, (f32)m->rate);
        const MixFilterChange change =
            on == was ? MIX_FILTER_STEADY : (on ? MIX_FILTER_SWITCH_ON : MIX_FILTER_SWITCH_OFF);
        // A corner the snapshots moved glides there from the last block's,
        // out to fully open when the filter switches off.
        const bool glides = was && (on ? f : MIX_FULL_BAND) != b->lowpass_hz;
        for (u32 c = 0U; c < m->channels; ++c) {
            mix_filter_block(m, glides ? &b->lowpass : NULL, &k, &b->lowpass_state[c], b->buffer[c],
                             MIX_BLOCK, change);
        }
        b->lowpass = k;
    }
    if (!on) {
        memset(b->lowpass_state, 0, sizeof b->lowpass_state);
    }
    b->lowpass_hz = on ? f : 0.0F;
}

static void mr_buses(NphAudioMixer *m)
{
    for (u32 k = 0U; k < m->bus_order_count; ++k) {
        const u32 i = m->bus_order[k];
        MixBus *b = &m->buses[i];
        mr_bus_lowpass(m, b);
        const f32 g0 = b->gain_current;
        const f32 g1 = m->bus_target[i];
        // The meters read the bus after its gain (approximated by the
        // block's final gain: the ramp is at most a block long).
        f32 peak = 0.0F;
        f32 energy = 0.0F;
        for (u32 c = 0U; c < m->channels; ++c) {
            const f32 p = m->kernels->peak(b->buffer[c], MIX_BLOCK);
            peak = p > peak ? p : peak;
            energy += m->kernels->sum_squares(b->buffer[c], MIX_BLOCK);
        }
        b->peak = peak * g1;
        b->rms = sqrtf(energy / (f32)(MIX_BLOCK * m->channels)) * g1;
        nph_atomic_store_u32(&m->bus_peak[i], mr_bits(b->peak), NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_store_u32(&m->bus_rms[i], mr_bits(b->rms), NPH_MEMORY_ORDER_RELAXED);
        if (i == 0U) {
            for (u32 c = 0U; c < m->channels; ++c) {
                m->kernels->scale_ramp(b->buffer[c], MIX_BLOCK, g0, g1);
            }
        } else {
            MixBus *parent = &m->buses[b->parent];
            for (u32 c = 0U; c < m->channels; ++c) {
                m->kernels->mix_ramp(parent->buffer[c], b->buffer[c], MIX_BLOCK, g0, g1);
            }
            for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
                if (b->send_bus[s] == MIX_NONE) {
                    b->send_current[s] = 0.0F;
                    continue;
                }
                MixBus *target = &m->buses[b->send_bus[s]];
                const f32 s0 = g0 * b->send_current[s];
                const f32 s1 = g1 * b->send_gain[s];
                if (s0 != 0.0F || s1 != 0.0F) {
                    for (u32 c = 0U; c < m->channels; ++c) {
                        m->kernels->mix_ramp(target->buffer[c], b->buffer[c], MIX_BLOCK, s0, s1);
                    }
                }
                b->send_current[s] = b->send_gain[s];
            }
        }
        b->gain_current = g1;
    }
}

static void mr_voices(NphAudioMixer *m, u32 *out_real, u32 *out_virtual)
{
    u32 real = 0U;
    u32 virtual_count = 0U;
    for (u32 i = 0U; i < m->max_voices; ++i) {
        MixVoice *v = &m->voices[i];
        if (v->state != MIX_VOICE_PLAYING) {
            continue;
        }
        if (v->delay >= MIX_BLOCK) {
            v->delay -= MIX_BLOCK; // not started: never real, nothing to fade
            virtual_count += 1U;
            continue;
        }
        u32 offset = 0U;
        if (v->delay > 0U) {
            offset = v->delay;
            v->delay = 0U;
        }
        const u32 frames = MIX_BLOCK - offset;
        if (v->real) {
            mix_voice_render(m, v, offset, frames);
            real += 1U;
        } else {
            if (!v->paused) {
                mix_voice_advance(m, v, frames);
                v->fade += v->fade_rate * (f32)frames;
                v->fade = v->fade < 0.0F ? 0.0F : (v->fade > 1.0F ? 1.0F : v->fade);
            }
            v->fresh = 0;
            virtual_count += 1U;
        }
        if (v->fade_rate > 0.0F && v->fade >= 1.0F) {
            v->fade_rate = 0.0F; // fully faded in
        }
        if (v->demoting) {
            mr_context_release(m, v);
            v->real = 0;
            v->demoting = 0;
        }
        const bool faded_out = v->stopping && (v->fade <= 0.0F || !v->real);
        if (faded_out || mix_voice_finished(m, v)) {
            mr_voice_end(m, i);
        }
    }
    *out_real = real;
    *out_virtual = virtual_count;
}

static void mr_block(NphAudioMixer *m)
{
    const u64 t0 = nph_time_now_ns();
    MixCommand c;
    while (nph_spsc_ring_pop(&m->commands, &c)) {
        mr_apply(m, &c);
    }
    if (m->bus_order_dirty) {
        mr_bus_order(m);
    }
    mr_controls(m);
    mr_rank(m);
    for (u32 k = 0U; k < m->bus_order_count; ++k) {
        MixBus *b = &m->buses[m->bus_order[k]];
        for (u32 ch = 0U; ch < m->channels; ++ch) {
            memset(b->buffer[ch], 0, sizeof(f32) * MIX_BLOCK);
        }
    }
    u32 real = 0U;
    u32 virtual_count = 0U;
    mr_voices(m, &real, &virtual_count);
    mr_buses(m);
    m->kernels->store_f32((const f32 *const *)m->buses[0].buffer, m->channels, m->block_out,
                          MIX_BLOCK);
    m->blocks += 1U;
    const u64 ns = nph_time_now_ns() - t0;
    m->block_ns_total += ns;
    m->block_ns_max = ns > m->block_ns_max ? ns : m->block_ns_max;
    nph_atomic_store_u64(&m->stat_blocks, m->blocks, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&m->stat_playing, real + virtual_count, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&m->stat_real, real, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&m->stat_virtual, virtual_count, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&m->stat_ns_last, ns, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&m->stat_ns_max, m->block_ns_max, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&m->stat_ns_total, m->block_ns_total, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&m->stat_corrupt, m->corrupt_reads, NPH_MEMORY_ORDER_RELAXED);
}

void nph_audio_mixer_render(NphAudioMixer *mixer, f32 *out, u32 frames)
{
    NphAudioMixer *m = mixer;
    NPH_ASSERT(m != NULL && (out != NULL || frames == 0U));
    nph_realtime_enter();
    const u32 csr = _mm_getcsr();
    _mm_setcsr(csr | MR_FTZ_DAZ);
    u32 done = 0U;
    while (done < frames) {
        if (m->block_left == 0U) {
            mr_block(m);
            m->block_left = MIX_BLOCK;
        }
        const u32 take = nph_min_u32(frames - done, m->block_left);
        const u32 from = MIX_BLOCK - m->block_left;
        memcpy(out + (u64)done * m->channels, m->block_out + (u64)from * m->channels,
               (size_t)take * m->channels * sizeof(f32));
        m->block_left -= take;
        done += take;
    }
    _mm_setcsr(csr);
    nph_realtime_exit();
}

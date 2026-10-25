// SPDX-License-Identifier: Apache-2.0
// Streaming (stream.h): the streamer's thread, opening and closing
// streams, filling their rings, and the audio thread's read.

#include <nepheline/audio/kernels.h>
#include <nepheline/core/assert.h>
#include <nepheline/platform/thread.h>

#include "stream_internal.h"

#include <string.h>

enum {
    ST_FREE = 0,
    ST_ACTIVE = 1,
    ST_CLOSING = 2,
};

struct NphAudioStreamer {
    NphAllocator *alloc;
    u32 max_streams;
    u64 period_ns;
    NphThread thread;
    NphSemaphore wake;
    NphAtomicU32 quit;
    MixStream *
        *slots; // written by the game thread before ACTIVE, cleared by the streamer before FREE
    NphAtomicU32 *state; // per slot
    NphAtomicU32 stat_streams;
    NphAtomicU64 stat_frames;
    NphAtomicU64 stat_bytes;
    NphAtomicU64 stat_seeks;
    NphAtomicU32 stat_errors;
    NphAtomicU32 stat_corrupt;
    const NphAudioKernels *kernels;
};

// ---------------------------------------------------------------------------
// The streamer's thread: decoding
// ---------------------------------------------------------------------------

// The clip frame behind stream frame `i`: from the start, round the loop.
static u64 st_clip_frame(const MixStream *st, u64 i)
{
    const u64 c = st->start_frame + i;
    if (st->loop && c >= st->loop_end) {
        return st->loop_start + (c - st->loop_start) % (st->loop_end - st->loop_start);
    }
    return c;
}

// Clip frames [clip, clip + count) of a PCM stream into chunk + at.
// NPH_RESULT_IO_ERROR when the source fails.
static NphResult st_decode_pcm(NphAudioStreamer *s, MixStream *st, u64 clip, u32 count, u32 at)
{
    const u64 bytes = (u64)count * st->bytes_per_frame;
    if (st->source.read(st->source.user, clip * st->bytes_per_frame, st->scratch, bytes) !=
        NPH_RESULT_OK) {
        return NPH_RESULT_IO_ERROR;
    }
    (void)nph_atomic_fetch_add_u64(&s->stat_bytes, bytes, NPH_MEMORY_ORDER_RELAXED);
    const u32 sample = nph_audio_format_bytes(st->format);
    for (u32 ch = 0U; ch < st->channels; ++ch) {
        const void *src = st->scratch + (u64)ch * sample;
        f32 *dst = st->chunk[ch] + at;
        switch (st->format) {
        case NPH_AUDIO_FORMAT_S16:
            s->kernels->load_s16(src, st->channels, dst, count);
            break;
        case NPH_AUDIO_FORMAT_S24:
            s->kernels->load_s24(src, st->channels, dst, count);
            break;
        case NPH_AUDIO_FORMAT_S32:
            s->kernels->load_s32(src, st->channels, dst, count);
            break;
        case NPH_AUDIO_FORMAT_F32:
            s->kernels->load_f32(src, st->channels, dst, count);
            break;
        case NPH_AUDIO_FORMAT_UNKNOWN:
        case NPH_AUDIO_FORMAT_COUNT:
            return NPH_RESULT_CORRUPT;
        }
    }
    return NPH_RESULT_OK;
}

// Clip frames [clip, clip + count) of a lossless stream into chunk + at:
// the decoder carries on where it is, or reads the block the frames start
// in, checks it (its header and CRC: lossless.h), and skips to them.
// NPH_RESULT_IO_ERROR when the source fails, NPH_RESULT_CORRUPT for a
// block that fails its checks or its decoding.
static NphResult st_decode_lossless(NphAudioStreamer *s, MixStream *st, u64 clip, u32 count, u32 at)
{
    u32 done = 0U;
    while (done < count) {
        const u64 c = clip + done;
        if (st->decoder_frame != c || nph_lossless_decoder_remaining(&st->decoder) == 0U) {
            const u32 block = (u32)(c / st->block_frames);
            const u64 begin = st->block_offsets[block];
            const u64 end = block + 1U < st->block_count ? st->block_offsets[block + 1U] : st->size;
            if (st->source.read(st->source.user, begin, st->scratch, end - begin) !=
                NPH_RESULT_OK) {
                return NPH_RESULT_IO_ERROR;
            }
            (void)nph_atomic_fetch_add_u64(&s->stat_bytes, end - begin, NPH_MEMORY_ORDER_RELAXED);
            const u64 first = (u64)block * st->block_frames;
            const u32 frames = (u32)nph_min_u64(st->block_frames, st->frames - first);
            if (nph_lossless_check_block(st->scratch, end - begin, st->channels, frames) !=
                    NPH_RESULT_OK ||
                nph_lossless_decoder_begin(&st->decoder, st->scratch, end - begin, st->channels,
                                           st->bits) != NPH_RESULT_OK ||
                nph_lossless_decoder_skip(&st->decoder, (u32)(c - first)) != NPH_RESULT_OK) {
                return NPH_RESULT_CORRUPT;
            }
            st->decoder_frame = c;
        }
        const u32 n = nph_min_u32(count - done, nph_lossless_decoder_remaining(&st->decoder));
        f32 *planes[NPH_AUDIO_MAX_CHANNELS];
        for (u32 ch = 0U; ch < st->channels; ++ch) {
            planes[ch] = st->chunk[ch] + at + done;
        }
        if (nph_lossless_decoder_read(&st->decoder, planes, n, st->ints) != NPH_RESULT_OK) {
            return NPH_RESULT_CORRUPT;
        }
        done += n;
        st->decoder_frame = c + n;
    }
    return NPH_RESULT_OK;
}

// Stream frames [first, first + count) into the chunk, run by run (a run
// ends at the loop's end or the clip's).
static NphResult st_decode(NphAudioStreamer *s, MixStream *st, u64 first, u32 count)
{
    u32 done = 0U;
    while (done < count) {
        const u64 clip = st_clip_frame(st, first + done);
        const u64 limit = st->loop ? st->loop_end : st->frames;
        const u32 run = (u32)nph_min_u64(count - done, limit - clip);
        NPH_TRY(st->codec == NPH_AUDIO_CODEC_PCM ? st_decode_pcm(s, st, clip, run, done)
                                                 : st_decode_lossless(s, st, clip, run, done));
        done += run;
    }
    return NPH_RESULT_OK;
}

// Fills the ring as far as it has room, starting again where the voice is
// when it has skipped ahead of what was decoded.
static void st_fill(NphAudioStreamer *s, MixStream *st)
{
    const u32 mask = st->capacity - 1U;
    for (;;) {
        // A failed stream reads and seeks no more: its voice plays silence,
        // and `produced` stays where the stream ended.
        if (nph_atomic_load_u32(&st->failed, NPH_MEMORY_ORDER_RELAXED) != 0U) {
            return;
        }
        u64 produced = nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_RELAXED);
        const u64 consumed = nph_atomic_load_u64(&st->consumed, NPH_MEMORY_ORDER_ACQUIRE);
        if (consumed > produced) {
            produced = consumed;
            st->decoder_frame = MIX_STREAM_NOWHERE;
            nph_atomic_store_u64(&st->produced, produced, NPH_MEMORY_ORDER_RELEASE);
            (void)nph_atomic_fetch_add_u64(&s->stat_seeks, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
        if (produced >= st->total) {
            return;
        }
        const u64 room = st->capacity - (produced - consumed);
        const u64 left = st->total - produced;
        if (room < MIX_STREAM_CHUNK && room < left) {
            return; // the voice is far enough behind
        }
        const u32 n = (u32)nph_min_u64(nph_min_u64(room, MIX_STREAM_CHUNK), left);
        const NphResult r = st_decode(s, st, produced, n);
        if (r != NPH_RESULT_OK) {
            nph_atomic_store_u32(&st->failed, (u32)r, NPH_MEMORY_ORDER_RELEASE);
            (void)nph_atomic_fetch_add_u32(r == NPH_RESULT_CORRUPT ? &s->stat_corrupt
                                                                   : &s->stat_errors,
                                           1U, NPH_MEMORY_ORDER_RELAXED);
            return;
        }
        const u32 at = (u32)(produced & mask);
        const u32 first = nph_min_u32(n, st->capacity - at);
        for (u32 ch = 0U; ch < st->channels; ++ch) {
            memcpy(st->ring[ch] + at, st->chunk[ch], (size_t)first * sizeof(f32));
            memcpy(st->ring[ch], st->chunk[ch] + first, (size_t)(n - first) * sizeof(f32));
        }
        nph_atomic_store_u64(&st->produced, produced + n, NPH_MEMORY_ORDER_RELEASE);
        (void)nph_atomic_fetch_add_u64(&s->stat_frames, n, NPH_MEMORY_ORDER_RELAXED);
    }
}

static void st_free(MixStream *st)
{
    NphAllocator *a = st->alloc;
    for (u32 ch = 0U; ch < NPH_AUDIO_MAX_CHANNELS; ++ch) {
        if (st->ring[ch] != NULL) {
            NPH_FREE_ARRAY(a, f32, st->ring[ch], st->capacity);
        }
        if (st->chunk[ch] != NULL) {
            NPH_FREE_ARRAY(a, f32, st->chunk[ch], MIX_STREAM_CHUNK);
        }
    }
    if (st->ints != NULL) {
        NPH_FREE_ARRAY(a, i32, st->ints, (u64)st->channels * MIX_STREAM_CHUNK);
    }
    if (st->scratch != NULL) {
        NPH_FREE(a, st->scratch, st->scratch_size, 16U);
    }
    if (st->block_offsets != NULL) {
        NPH_FREE_ARRAY(a, u32, st->block_offsets, st->block_count);
    }
    NPH_FREE_TYPE(a, MixStream, st);
}

static void st_main(void *user)
{
    NphAudioStreamer *s = (NphAudioStreamer *)user;
    while (nph_atomic_load_u32(&s->quit, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
        for (u32 i = 0U; i < s->max_streams; ++i) {
            const u32 state = nph_atomic_load_u32(&s->state[i], NPH_MEMORY_ORDER_ACQUIRE);
            if (state == ST_ACTIVE) {
                st_fill(s, s->slots[i]);
            } else if (state == ST_CLOSING) {
                st_free(s->slots[i]);
                s->slots[i] = NULL;
                (void)nph_atomic_fetch_sub_u32(&s->stat_streams, 1U, NPH_MEMORY_ORDER_RELAXED);
                nph_atomic_store_u32(&s->state[i], ST_FREE, NPH_MEMORY_ORDER_RELEASE);
            }
        }
        (void)nph_semaphore_wait_timeout(&s->wake, s->period_ns);
    }
}

// ---------------------------------------------------------------------------
// The streamer
// ---------------------------------------------------------------------------

NphResult nph_audio_streamer_create(const NphAudioStreamerDesc *desc, NphAudioStreamer **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *a = desc->alloc;
    NphAudioStreamer *s = NPH_ALLOC_TYPE(a, NphAudioStreamer);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->max_streams = desc->max_streams != 0U ? desc->max_streams : 16U;
    s->period_ns = (u64)(desc->period_ms != 0U ? desc->period_ms : 10U) * 1000000U;
    s->kernels = nph_audio_kernels_best();
    s->slots = NPH_ALLOC_ARRAY_ZEROED(a, MixStream *, s->max_streams);
    s->state = NPH_ALLOC_ARRAY_ZEROED(a, NphAtomicU32, s->max_streams);
    if (s->slots == NULL || s->state == NULL) {
        nph_audio_streamer_destroy(s);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphResult r = nph_semaphore_init(&s->wake, 0U);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(a, MixStream *, s->slots, s->max_streams);
        NPH_FREE_ARRAY(a, NphAtomicU32, s->state, s->max_streams);
        NPH_FREE_TYPE(a, NphAudioStreamer, s);
        return r;
    }
    const NphThreadDesc td = {st_main, s, "audio-streamer", 0U};
    r = nph_thread_create(&s->thread, &td);
    if (r != NPH_RESULT_OK) {
        nph_semaphore_destroy(&s->wake);
        NPH_FREE_ARRAY(a, MixStream *, s->slots, s->max_streams);
        NPH_FREE_ARRAY(a, NphAtomicU32, s->state, s->max_streams);
        NPH_FREE_TYPE(a, NphAudioStreamer, s);
        return r;
    }
    *out = s;
    return NPH_RESULT_OK;
}

void nph_audio_streamer_destroy(NphAudioStreamer *streamer)
{
    if (streamer == NULL) {
        return;
    }
    NphAudioStreamer *s = streamer;
    NphAllocator *a = s->alloc;
    if (s->thread.joinable) {
        nph_atomic_store_u32(&s->quit, 1U, NPH_MEMORY_ORDER_RELEASE);
        nph_semaphore_post(&s->wake);
        nph_thread_join(&s->thread);
        nph_semaphore_destroy(&s->wake);
    }
    if (s->slots != NULL) {
        for (u32 i = 0U; i < s->max_streams; ++i) {
            if (s->slots[i] != NULL) {
                st_free(s->slots[i]);
            }
        }
        NPH_FREE_ARRAY(a, MixStream *, s->slots, s->max_streams);
    }
    if (s->state != NULL) {
        NPH_FREE_ARRAY(a, NphAtomicU32, s->state, s->max_streams);
    }
    NPH_FREE_TYPE(a, NphAudioStreamer, s);
}

void nph_audio_streamer_stats(const NphAudioStreamer *streamer, NphAudioStreamerStats *out)
{
    NPH_ASSERT(streamer != NULL && out != NULL);
    out->streams = nph_atomic_load_u32(&streamer->stat_streams, NPH_MEMORY_ORDER_RELAXED);
    out->frames_decoded = nph_atomic_load_u64(&streamer->stat_frames, NPH_MEMORY_ORDER_RELAXED);
    out->bytes_read = nph_atomic_load_u64(&streamer->stat_bytes, NPH_MEMORY_ORDER_RELAXED);
    out->seeks = nph_atomic_load_u64(&streamer->stat_seeks, NPH_MEMORY_ORDER_RELAXED);
    out->read_errors = nph_atomic_load_u32(&streamer->stat_errors, NPH_MEMORY_ORDER_RELAXED);
    out->corrupt_blocks = nph_atomic_load_u32(&streamer->stat_corrupt, NPH_MEMORY_ORDER_RELAXED);
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

static NphResult st_validate(const NphAudioStreamDesc *d)
{
    if (d->source.read == NULL || d->channels < 1U || d->channels > NPH_AUDIO_MAX_CHANNELS ||
        d->sample_rate < NPH_AUDIO_MIN_RATE || d->sample_rate > NPH_AUDIO_MAX_RATE ||
        d->frames == 0U || d->frames >= MIX_STREAM_ENDLESS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 loop_end = d->loop_end != 0U ? d->loop_end : d->frames;
    if (loop_end > d->frames || d->loop_start >= loop_end ||
        d->start_frame >= (d->loop ? loop_end : d->frames)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (d->codec == NPH_AUDIO_CODEC_PCM) {
        const u32 sample = nph_audio_format_bytes(d->format);
        if (sample == 0U || d->size < d->frames * sample * d->channels) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        return NPH_RESULT_OK;
    }
    if (d->codec != NPH_AUDIO_CODEC_LOSSLESS || d->block_frames == 0U || d->block_offsets == NULL ||
        d->bits < 4U || d->bits > 24U ||
        d->block_count != nph_lossless_block_count(d->frames, d->block_frames)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 b = 0U; b < d->block_count; ++b) {
        const u64 next = b + 1U < d->block_count ? d->block_offsets[b + 1U] : d->size;
        if (d->block_offsets[b] >= next || next > d->size) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    return NPH_RESULT_OK;
}

NphResult mix_stream_open(NphAudioStreamer *streamer, const NphAudioStreamDesc *desc,
                          MixStream **out)
{
    NPH_ASSERT(streamer != NULL && desc != NULL && out != NULL);
    *out = NULL;
    NphResult r = st_validate(desc);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    // A free slot (the streamer frees slots on its own thread).
    u32 slot = NPH_U32_MAX;
    for (u32 i = 0U; i < streamer->max_streams; ++i) {
        if (nph_atomic_load_u32(&streamer->state[i], NPH_MEMORY_ORDER_ACQUIRE) == ST_FREE) {
            slot = i;
            break;
        }
    }
    if (slot == NPH_U32_MAX) {
        return NPH_RESULT_FULL;
    }
    NphAllocator *a = streamer->alloc;
    MixStream *st = NPH_ALLOC_TYPE(a, MixStream);
    if (st == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(st, 0, sizeof *st);
    st->alloc = a;
    st->source = desc->source;
    st->codec = desc->codec;
    st->format = desc->format;
    st->channels = desc->channels;
    st->sample_rate = desc->sample_rate;
    st->bytes_per_frame = desc->codec == NPH_AUDIO_CODEC_PCM
                              ? nph_audio_format_bytes(desc->format) * desc->channels
                              : 0U;
    st->bits = desc->bits;
    st->block_frames = desc->block_frames;
    st->block_count = desc->codec == NPH_AUDIO_CODEC_LOSSLESS ? desc->block_count : 0U;
    st->frames = desc->frames;
    st->size = desc->size;
    st->loop_end = desc->loop_end != 0U ? desc->loop_end : desc->frames;
    st->loop_start = desc->loop_start;
    st->loop = desc->loop != 0;
    st->start_frame = desc->start_frame;
    st->total = st->loop ? MIX_STREAM_ENDLESS : desc->frames - desc->start_frame;
    st->decoder_frame = MIX_STREAM_NOWHERE;
    const f32 seconds = desc->buffer_seconds > 0.0F ? desc->buffer_seconds : 1.0F;
    const u64 want = (u64)(seconds * (f32)desc->sample_rate);
    st->capacity = nph_next_pow2_u32(
        (u32)nph_min_u64(nph_max_u64(want, 4ULL * MIX_STREAM_CHUNK), 1ULL << 24U));
    // The scratch: a PCM chunk's bytes, or the largest lossless block's.
    u64 scratch = (u64)MIX_STREAM_CHUNK * st->bytes_per_frame;
    if (st->codec == NPH_AUDIO_CODEC_LOSSLESS) {
        for (u32 b = 0U; b < st->block_count; ++b) {
            const u64 next = b + 1U < st->block_count ? desc->block_offsets[b + 1U] : desc->size;
            scratch = nph_max_u64(scratch, next - desc->block_offsets[b]);
        }
    }
    st->scratch_size = scratch;
    st->scratch = (u8 *)NPH_ALLOC(a, scratch, 16U);
    bool ok = st->scratch != NULL;
    if (ok && st->codec == NPH_AUDIO_CODEC_LOSSLESS) {
        st->block_offsets = NPH_ALLOC_ARRAY(a, u32, st->block_count);
        st->ints = NPH_ALLOC_ARRAY(a, i32, (u64)st->channels * MIX_STREAM_CHUNK);
        ok = st->block_offsets != NULL && st->ints != NULL;
        if (ok) {
            memcpy(st->block_offsets, desc->block_offsets, sizeof(u32) * st->block_count);
        }
    }
    for (u32 ch = 0U; ok && ch < st->channels; ++ch) {
        st->ring[ch] = NPH_ALLOC_ARRAY_ZEROED(a, f32, st->capacity);
        st->chunk[ch] = NPH_ALLOC_ARRAY_ZEROED(a, f32, MIX_STREAM_CHUNK);
        ok = st->ring[ch] != NULL && st->chunk[ch] != NULL;
    }
    if (!ok) {
        st_free(st);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    // The ring filled here, before the streamer has the stream, so a voice
    // played at once starts on sound.
    st_fill(streamer, st);
    const u32 failed = nph_atomic_load_u32(&st->failed, NPH_MEMORY_ORDER_RELAXED);
    if (failed != 0U) {
        st_free(st);
        return (NphResult)failed;
    }
    streamer->slots[slot] = st;
    nph_atomic_store_u32(&streamer->state[slot], ST_ACTIVE, NPH_MEMORY_ORDER_RELEASE);
    (void)nph_atomic_fetch_add_u32(&streamer->stat_streams, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_semaphore_post(&streamer->wake);
    *out = st;
    return NPH_RESULT_OK;
}

void mix_stream_close(NphAudioStreamer *streamer, MixStream *stream)
{
    for (u32 i = 0U; i < streamer->max_streams; ++i) {
        // Only this thread sets a slot ACTIVE or CLOSING, and the streamer
        // touches its pointer only in those states: reading it is safe.
        if (nph_atomic_load_u32(&streamer->state[i], NPH_MEMORY_ORDER_ACQUIRE) == ST_ACTIVE &&
            streamer->slots[i] == stream) {
            nph_atomic_store_u32(&streamer->state[i], ST_CLOSING, NPH_MEMORY_ORDER_RELEASE);
            nph_semaphore_post(&streamer->wake);
            return;
        }
    }
    NPH_ASSERT(false && "a stream not open on this streamer");
}

// ---------------------------------------------------------------------------
// The audio thread
// ---------------------------------------------------------------------------

u32 mix_stream_read(MixStream *st, u64 frame, u32 count, f32 *const *dst, u32 at)
{
    const u32 mask = st->capacity - 1U;
    u64 consumed = nph_atomic_load_u64(&st->consumed, NPH_MEMORY_ORDER_RELAXED);
    const u64 produced = nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE);
    u32 done = 0U;
    // Frames already taken (a voice reaching back as it comes back from
    // virtual): gone, silence.
    if (frame < consumed) {
        done = (u32)nph_min_u64(count, consumed - frame);
        for (u32 ch = 0U; ch < st->channels; ++ch) {
            memset(dst[ch] + at, 0, (size_t)done * sizeof(f32));
        }
    } else {
        consumed = frame; // skipped ahead: what lies between is dropped
    }
    // What has arrived.
    const u64 have = produced > consumed ? produced - consumed : 0U;
    const u32 take = (u32)nph_min_u64(count - done, have);
    if (take > 0U) {
        const u32 from = (u32)(consumed & mask);
        const u32 first = nph_min_u32(take, st->capacity - from);
        for (u32 ch = 0U; ch < st->channels; ++ch) {
            memcpy(dst[ch] + at + done, st->ring[ch] + from, (size_t)first * sizeof(f32));
            memcpy(dst[ch] + at + done + first, st->ring[ch], (size_t)(take - first) * sizeof(f32));
        }
        done += take;
        consumed += take;
    }
    // What has not: silence, and the read index moves on regardless, so the
    // streamer seeks past it and the stream stays in time.
    const u32 missing = count - done;
    if (missing > 0U) {
        for (u32 ch = 0U; ch < st->channels; ++ch) {
            memset(dst[ch] + at + done, 0, (size_t)missing * sizeof(f32));
        }
        consumed += missing;
    }
    nph_atomic_store_u64(&st->consumed, consumed, NPH_MEMORY_ORDER_RELEASE);
    return missing;
}

void mix_stream_skip(MixStream *st, u64 frame)
{
    // Only this thread writes the read index.
    if (frame > nph_atomic_load_u64(&st->consumed, NPH_MEMORY_ORDER_RELAXED)) {
        nph_atomic_store_u64(&st->consumed, frame, NPH_MEMORY_ORDER_RELEASE);
    }
}

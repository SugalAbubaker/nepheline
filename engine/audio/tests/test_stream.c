// SPDX-License-Identifier: Apache-2.0
// Streaming (stream.h, mixer.h): a stream plays what its clip would, to the
// bit, looping and starting part-way; it starves to silence, counted;
// a voice that goes virtual skips it ahead; a failed read or a corrupt
// block ends it in silence, counted apart; it says when its voice can read;
// closing frees it on the streamer; refusals.

#include <nepheline/core/atomic.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "mixer_internal.h"
#include "test_mixer_support.h"

// A source in memory, slow if asked, failing if asked (the test thread
// asks while the streamer reads: atomics).
typedef struct TsSource {
    const u8 *data;
    u64 size;
    NphAtomicU64 delay_ns; // per read
    NphAtomicU32 fail;
} TsSource;

static NphResult ts_read(void *user, u64 offset, void *dst, u64 size)
{
    TsSource *s = (TsSource *)user;
    const u64 delay = nph_atomic_load_u64(&s->delay_ns, NPH_MEMORY_ORDER_RELAXED);
    if (delay > 0U) {
        nph_thread_sleep_ns(delay);
    }
    if (nph_atomic_load_u32(&s->fail, NPH_MEMORY_ORDER_RELAXED) != 0U || offset > s->size ||
        size > s->size - offset) {
        return NPH_RESULT_IO_ERROR;
    }
    memcpy(dst, s->data + offset, (size_t)size);
    return NPH_RESULT_OK;
}

static NphAudioStreamer *ts_streamer(void)
{
    NphAudioStreamerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.max_streams = 4U;
    d.period_ms = 2U;
    NphAudioStreamer *s = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_streamer_create(&d, &s), NPH_RESULT_OK);
    return s;
}

// A 3 s stereo tone pair as 16-bit samples.
enum {
    TS_FRAMES = 144000
};

static i16 *ts_samples(void)
{
    i16 *x = (i16 *)NPH_ALLOC(nph_allocator_system(), (u64)TS_FRAMES * 2U * sizeof(i16), 16U);
    NTEST_ASSERT_NOT_NULL(x);
    for (u32 i = 0U; i < (u32)TS_FRAMES; ++i) {
        x[(u64)i * 2U] = (i16)(12000.0 * sin(2.0 * TEST_PI * 330.0 * (f64)i / 48000.0));
        x[(u64)i * 2U + 1U] = (i16)(9000.0 * sin(2.0 * TEST_PI * 495.0 * (f64)i / 48000.0));
    }
    return x;
}

static void ts_samples_free(i16 *x)
{
    NPH_FREE(nph_allocator_system(), x, (u64)TS_FRAMES * 2U * sizeof(i16), 16U);
}

// Renders `blocks` blocks, waiting before each until the stream's ring
// holds what the block needs (offline rendering outruns real time; the
// device would not).
static void ts_render(NphAudioMixer *m, const MixStream *st, f32 *out, u32 blocks)
{
    for (u32 b = 0U; b < blocks; ++b) {
        if (st != NULL) {
            const u64 need = (u64)(b + 1U) * NPH_AUDIO_BLOCK_FRAMES + 64U;
            while (nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE) <
                       nph_min_u64(need, st->total) &&
                   nph_atomic_load_u32(&st->failed, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
                nph_thread_sleep_ns(nph_time_us_to_ns(200U));
            }
        }
        nph_audio_mixer_render(m, out + (u64)b * NPH_AUDIO_BLOCK_FRAMES * 2U,
                               NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_update(m);
    }
}

// The stream's clip and its MixStream (white-box: the tests wait on it).
static NphAudioClip ts_stream_clip(NphAudioMixer *m, NphAudioStreamer *s,
                                   const NphAudioStreamDesc *d, MixStream **out)
{
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create_stream(m, s, d, &clip), NPH_RESULT_OK);
    // The clip's stream: the newest the streamer holds.
    *out = NULL;
    for (u32 i = 0U; i < 64U; ++i) {
        const MixClipSlot *slot = &m->clip_slots[i];
        if (slot->clip != NULL && slot->clip->stream != NULL && slot->handle.value == clip.value) {
            *out = slot->clip->stream;
        }
    }
    NTEST_ASSERT_NOT_NULL(*out);
    return clip;
}

// Plays `clip` (looping or not) on a fresh mixer for `blocks` blocks into
// `out` (interleaved stereo); `st` the stream behind it, if any.
static void ts_play(NphAudioMixer *m, NphAudioClip clip, bool loop, const MixStream *st, f32 *out,
                    u32 blocks)
{
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.pitch = 0.93F; // through the resampler, not the copy path
    d.loop = loop;
    (void)tm_play(m, &d);
    ts_render(m, st, out, blocks);
}

NTEST(audio_stream, a_stream_plays_what_its_clip_would)
{
    // The same 16-bit material as PCM and lossless, played from memory and
    // from a stream: the same frames, so the same mix to the bit.
    enum {
        BLOCKS = 400
    };
    i16 *pcm = ts_samples();
    i32 *ints = (i32 *)NPH_ALLOC(nph_allocator_system(), (u64)TS_FRAMES * 2U * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(ints);
    for (u64 i = 0U; i < (u64)TS_FRAMES * 2U; ++i) {
        ints[i] = pcm[i];
    }
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), ints, 2U, 16U, TS_FRAMES, NULL, &enc),
        NPH_RESULT_OK);
    for (u32 codec = 0U; codec < 2U; ++codec) {
        const bool lossless = codec == 1U;
        TsSource src;
        memset(&src, 0, sizeof src);
        src.data = lossless ? enc.stream.data : (const u8 *)pcm;
        src.size = lossless ? enc.stream.size : (u64)TS_FRAMES * 4U;
        NphAudioClipDesc cd;
        memset(&cd, 0, sizeof cd);
        cd.codec = lossless ? NPH_AUDIO_CODEC_LOSSLESS : NPH_AUDIO_CODEC_PCM;
        cd.format = NPH_AUDIO_FORMAT_S16;
        cd.channels = 2U;
        cd.sample_rate = 48000U;
        cd.frames = TS_FRAMES;
        cd.data = src.data;
        cd.size = src.size;
        cd.bits = 16U;
        cd.block_frames = enc.stream.block_frames;
        cd.block_offsets = enc.stream.block_offsets;
        cd.block_count = enc.stream.block_count;
        NphAudioStreamDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.source = (NphAudioStreamSource){ts_read, &src};
        sd.codec = cd.codec;
        sd.format = cd.format;
        sd.channels = 2U;
        sd.sample_rate = 48000U;
        sd.frames = TS_FRAMES;
        sd.size = src.size;
        sd.bits = 16U;
        sd.block_frames = cd.block_frames;
        sd.block_offsets = cd.block_offsets;
        sd.block_count = cd.block_count;
        sd.buffer_seconds = 0.25F; // the ring wraps many times
        f32 *from_clip = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
        f32 *from_stream = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
        NphAudioMixer *a = tm_mixer(2U, 4U, 4U);
        NphAudioClip clip;
        NTEST_ASSERT_EQ_INT(nph_audio_clip_create(a, &cd, &clip), NPH_RESULT_OK);
        ts_play(a, clip, false, NULL, from_clip, BLOCKS);
        nph_audio_mixer_destroy(a);
        NphAudioStreamer *s = ts_streamer();
        NphAudioMixer *b = tm_mixer(2U, 4U, 4U);
        MixStream *st = NULL;
        const NphAudioClip sclip = ts_stream_clip(b, s, &sd, &st);
        ts_play(b, sclip, false, st, from_stream, BLOCKS);
        NphAudioMixerStats stats;
        nph_audio_mixer_stats(b, &stats);
        NTEST_EXPECT_EQ_UINT(stats.stream_starved, 0U);
        NTEST_EXPECT_TRUE(
            test_same_bits(from_clip, from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U));
        nph_audio_mixer_destroy(b);
        nph_audio_streamer_destroy(s);
        test_buffer_free(from_clip, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
        test_buffer_free(from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    }
    nph_lossless_encoded_free(&enc);
    NPH_FREE(nph_allocator_system(), ints, (u64)TS_FRAMES * 2U * sizeof(i32), 16U);
    ts_samples_free(pcm);
}

NTEST(audio_stream, a_looping_stream_loops_as_a_clip_does)
{
    // A loop from 0.5 s to 1.25 s, played for 4 s: the stream unrolls it as
    // a looping clip's voice wraps it.
    enum {
        BLOCKS = 750
    };
    i16 *pcm = ts_samples();
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)pcm;
    src.size = (u64)TS_FRAMES * 4U;
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_PCM;
    cd.format = NPH_AUDIO_FORMAT_S16;
    cd.channels = 2U;
    cd.sample_rate = 48000U;
    cd.frames = TS_FRAMES;
    cd.data = pcm;
    cd.size = src.size;
    cd.loop_start = 24000U;
    cd.loop_end = 60000U;
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_S16;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    sd.loop_start = 24000U;
    sd.loop_end = 60000U;
    sd.loop = 1;
    f32 *from_clip = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    f32 *from_stream = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    NphAudioMixer *a = tm_mixer(2U, 4U, 4U);
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(a, &cd, &clip), NPH_RESULT_OK);
    ts_play(a, clip, true, NULL, from_clip, BLOCKS);
    nph_audio_mixer_destroy(a);
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *b = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip sclip = ts_stream_clip(b, s, &sd, &st);
    NTEST_EXPECT_TRUE(nph_audio_clip_duration(b, sclip) == (f64)INFINITY);
    ts_play(b, sclip, false, st, from_stream, BLOCKS);
    NTEST_EXPECT_TRUE(test_same_bits(from_clip, from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U));
    nph_audio_mixer_destroy(b);
    nph_audio_streamer_destroy(s);
    test_buffer_free(from_clip, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    test_buffer_free(from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    ts_samples_free(pcm);
}

NTEST(audio_stream, a_starving_stream_plays_silence_and_counts_it)
{
    // A source that takes 40 ms a read, rendered as fast as the machine
    // goes: the ring runs dry, the gaps are silence, the voice keeps time.
    enum {
        BLOCKS = 200
    };
    i16 *pcm = ts_samples();
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)pcm;
    src.size = (u64)TS_FRAMES * 4U;
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_S16;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    sd.buffer_seconds = 0.1F;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip clip = ts_stream_clip(m, s, &sd, &st);
    nph_atomic_store_u64(&src.delay_ns, nph_time_ms_to_ns(40U), NPH_MEMORY_ORDER_RELAXED);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    (void)tm_play(m, &d);
    f32 *out = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    ts_render(m, NULL, out, BLOCKS);
    NphAudioMixerStats stats;
    nph_audio_mixer_stats(m, &stats);
    // The voice is far ahead of the streamer, which, its slow read done,
    // goes on from where the voice is.
    NphAudioStreamerStats ss;
    const u64 t0 = nph_time_now_ns();
    do {
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
        nph_audio_streamer_stats(s, &ss);
    } while (ss.seeks == 0U && nph_time_now_ns() - t0 < nph_time_ms_to_ns(2000U));
    NTEST_LOG("starved %llu of %u frames; %llu seeks", (unsigned long long)stats.stream_starved,
              BLOCKS * NPH_AUDIO_BLOCK_FRAMES, (unsigned long long)ss.seeks);
    NTEST_EXPECT_TRUE(stats.stream_starved > 0U);
    NTEST_EXPECT_TRUE(ss.seeks > 0U);
    // The prefilled start played.
    NTEST_EXPECT_TRUE(test_rms(out, 2048U) > 0.05);
    nph_audio_mixer_destroy(m);
    nph_atomic_store_u64(&src.delay_ns, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_audio_streamer_destroy(s);
    test_buffer_free(out, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    ts_samples_free(pcm);
}

NTEST(audio_stream, a_virtual_voice_keeps_its_stream_moving)
{
    // One real voice: the stream's voice goes virtual under a higher
    // priority for a second, its stream keeps time with it, and it comes
    // back on sound, where its timeline is: nothing starves, nothing seeks.
    enum {
        BLOCKS = 600
    };
    f32 *ramp = test_buffer(TS_FRAMES);
    for (u32 i = 0U; i < (u32)TS_FRAMES; ++i) {
        ramp[i] = (f32)i / (f32)TS_FRAMES; // a sample's value is its time
    }
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)ramp;
    src.size = (u64)TS_FRAMES * sizeof(f32);
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_F32;
    sd.channels = 1U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    sd.buffer_seconds = 0.25F;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *m = tm_mixer(1U, 4U, 1U);
    MixStream *st = NULL;
    const NphAudioClip clip = ts_stream_clip(m, s, &sd, &st);
    const NphAudioClip loud = tm_tone(m, 200.0, 96000U, TM_RATE, 0.1F, NULL);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.priority = 10U;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
    NphAudioVoice other = {0U};
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        if (b == 100U) {
            NphAudioPlayDesc o = nph_audio_play_desc_default(loud);
            o.priority = 200U;
            other = tm_play(m, &o);
        }
        if (b == 290U) {
            nph_audio_voice_stop(m, other, 0.0F);
        }
        // Offline, the streamer is waited for (a device's real time would
        // give it the time): the most a block can read past the read index.
        const u64 need = nph_min_u64(nph_atomic_load_u64(&st->consumed, NPH_MEMORY_ORDER_ACQUIRE) +
                                         2ULL * NPH_AUDIO_BLOCK_FRAMES,
                                     st->total);
        while (nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE) < need) {
            nph_thread_sleep_ns(nph_time_us_to_ns(200U));
        }
        nph_audio_mixer_render(m, out + (u64)b * NPH_AUDIO_BLOCK_FRAMES, NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_update(m);
        if (b == 200U) {
            NphAudioMixerStats mid;
            nph_audio_mixer_stats(m, &mid);
            NTEST_EXPECT_EQ_UINT(mid.voices_virtual, 1U); // the stream's, under the loud one
        }
    }
    NphAudioStreamerStats ss;
    nph_audio_streamer_stats(s, &ss);
    NphAudioMixerStats stats;
    nph_audio_mixer_stats(m, &stats);
    // Back from block 291 or so, faded in over its first block: from a few
    // blocks on, the ramp's value is its time, to the bit.
    f32 worst = 0.0F;
    for (u32 i = 296U * NPH_AUDIO_BLOCK_FRAMES; i < (u32)TS_FRAMES; ++i) {
        const f32 e = fabsf(out[i] - (f32)i / (f32)TS_FRAMES);
        worst = e > worst ? e : worst;
    }
    NTEST_LOG("after the return the ramp is off its time by %.3g at worst; %llu seeks, %llu "
              "frames starved",
              (f64)worst, (unsigned long long)ss.seeks, (unsigned long long)stats.stream_starved);
    NTEST_EXPECT_TRUE(worst == 0.0F);
    NTEST_EXPECT_EQ_UINT(ss.seeks, 0U);
    NTEST_EXPECT_EQ_UINT(stats.stream_starved, 0U);
    nph_audio_mixer_destroy(m);
    nph_audio_streamer_destroy(s);
    test_buffer_free(out, BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
    test_buffer_free(ramp, TS_FRAMES);
}

NTEST(audio_stream, a_stream_voice_outlives_its_position)
{
    // A looping stream's voice never wraps (the stream unrolls the loop).
    // Moved to just short of where its 32.32 position would run out, it
    // moves the position back and its base on, and plays on in time.
    enum {
        BLOCKS = 8
    };
    f32 *ramp = test_buffer(TS_FRAMES);
    for (u32 i = 0U; i < (u32)TS_FRAMES; ++i) {
        ramp[i] = (f32)i / (f32)TS_FRAMES;
    }
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)ramp;
    src.size = (u64)TS_FRAMES * sizeof(f32);
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_F32;
    sd.channels = 1U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    sd.loop = 1;
    sd.buffer_seconds = 0.5F;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *m = tm_mixer(1U, 4U, 1U);
    MixStream *st = NULL;
    const NphAudioClip clip = ts_stream_clip(m, s, &sd, &st);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    (void)tm_play(m, &d);
    f32 *out = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
    nph_audio_mixer_render(m, out, NPH_AUDIO_BLOCK_FRAMES); // it starts
    nph_audio_mixer_update(m);
    // White-box (this thread is the audio thread): the voice 300 frames
    // short of the rebase, reading its window afresh, and its stream sent
    // there and filled.
    MixVoice *v = NULL;
    for (u32 i = 0U; i < m->max_voices; ++i) {
        if (m->voices[i].state == MIX_VOICE_PLAYING) {
            v = &m->voices[i];
        }
    }
    // (Checks that fail let the test go on to its end, so the streamer
    // never outlives the source it reads.)
    NTEST_EXPECT_TRUE(v != NULL && v->real != 0);
    if (v != NULL && v->real != 0) {
        const u64 from = MIX_STREAM_REBASE_AT - 300U;
        v->pos = from << 32U;
        m->contexts[v->context].history_len = 0U;
        mix_stream_skip(st, from - 1024U);
        while (nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE) < from + 4096U) {
            nph_thread_sleep_ns(nph_time_us_to_ns(200U));
        }
        for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
            nph_audio_mixer_render(m, out + (u64)b * NPH_AUDIO_BLOCK_FRAMES,
                                   NPH_AUDIO_BLOCK_FRAMES);
            nph_audio_mixer_update(m);
        }
        // Stream frame from + i is clip frame (from + i) mod its length.
        f32 worst = 0.0F;
        for (u32 i = 0U; i < (u32)BLOCKS * NPH_AUDIO_BLOCK_FRAMES; ++i) {
            const f32 want = (f32)((from + i) % TS_FRAMES) / (f32)TS_FRAMES;
            const f32 e = fabsf(out[i] - want);
            worst = e > worst ? e : worst;
        }
        NTEST_LOG("across the rebase the ramp is off by %.3g at worst; base %llu, position %llu",
                  (f64)worst, (unsigned long long)v->stream_base,
                  (unsigned long long)(v->pos >> 32U));
        NTEST_EXPECT_TRUE(worst == 0.0F);
        NTEST_EXPECT_EQ_UINT(v->stream_base, MIX_STREAM_REBASE_BY);
        NTEST_EXPECT_TRUE((v->pos >> 32U) < MIX_STREAM_REBASE_AT);
        NphAudioMixerStats stats;
        nph_audio_mixer_stats(m, &stats);
        NTEST_EXPECT_EQ_UINT(stats.stream_starved, 0U);
    }
    nph_audio_mixer_destroy(m);
    nph_audio_streamer_destroy(s);
    test_buffer_free(out, BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
    test_buffer_free(ramp, TS_FRAMES);
}

NTEST(audio_stream, a_failing_source_ends_its_stream_in_silence)
{
    // The source fails after the first fill: the voice plays what the ring
    // holds, then silence, counted; the streamer counts the error.
    enum {
        BLOCKS = 300
    };
    i16 *pcm = ts_samples();
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)pcm;
    src.size = (u64)TS_FRAMES * 4U;
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_S16;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip clip = ts_stream_clip(m, s, &sd, &st);
    nph_atomic_store_u32(&src.fail, 1U, NPH_MEMORY_ORDER_RELAXED);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    (void)tm_play(m, &d);
    f32 *out = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    ts_render(m, NULL, out, 8U); // room in the ring: the streamer reads, and fails
    const u64 t0 = nph_time_now_ns();
    while (nph_atomic_load_u32(&st->failed, NPH_MEMORY_ORDER_ACQUIRE) == 0U &&
           nph_time_now_ns() - t0 < nph_time_ms_to_ns(2000U)) {
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
    }
    NTEST_EXPECT_TRUE(nph_atomic_load_u32(&st->failed, NPH_MEMORY_ORDER_ACQUIRE) != 0U);
    const u64 held = nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE);
    ts_render(m, NULL, out + 8ULL * NPH_AUDIO_BLOCK_FRAMES * 2U, BLOCKS - 8U);
    NphAudioMixerStats stats;
    nph_audio_mixer_stats(m, &stats);
    NphAudioStreamerStats ss;
    nph_audio_streamer_stats(s, &ss);
    NTEST_LOG("the ring held %llu frames; %llu starved; %u read errors", (unsigned long long)held,
              (unsigned long long)stats.stream_starved, ss.read_errors);
    NTEST_EXPECT_EQ_UINT(ss.read_errors, 1U);
    NTEST_EXPECT_EQ_UINT(ss.corrupt_blocks, 0U);
    NTEST_EXPECT_EQ_UINT(ss.seeks, 0U); // a failed stream does not follow its voice
    // What the ring held played; past it, silence.
    NTEST_EXPECT_TRUE(test_rms(out, 2048U) > 0.05);
    const u64 dry = (held + 2ULL * NPH_AUDIO_BLOCK_FRAMES) * 2U;
    NTEST_EXPECT_TRUE(dry < (u64)BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    NTEST_EXPECT_TRUE(test_rms(out + dry, (u32)((u64)BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U - dry)) ==
                      0.0);
    NTEST_EXPECT_TRUE(stats.stream_starved > 0U);
    // Destroyed while it plays: the voice fades, the clip goes, the
    // streamer frees the stream.
    nph_audio_clip_destroy(m, clip);
    for (u32 b = 0U; b < 4U; ++b) {
        nph_audio_mixer_render(m, out, NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_update(m);
    }
    const u64 t1 = nph_time_now_ns();
    do {
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
        nph_audio_streamer_stats(s, &ss);
    } while (ss.streams != 0U && nph_time_now_ns() - t1 < nph_time_ms_to_ns(2000U));
    NTEST_EXPECT_EQ_UINT(ss.streams, 0U);
    nph_audio_mixer_destroy(m);
    nph_audio_streamer_destroy(s);
    test_buffer_free(out, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    ts_samples_free(pcm);
}

NTEST(audio_stream, a_corrupt_block_ends_its_stream_in_silence)
{
    // A byte flipped in the samples of a lossless block past the first
    // fill, where only the block's CRC can tell: the voice plays the stream
    // as its clip would up to that block, then silence; the streamer counts
    // a corrupt block, not a failed read.
    enum {
        BLOCKS = 600
    };
    i16 *pcm = ts_samples();
    i32 *ints = (i32 *)NPH_ALLOC(nph_allocator_system(), (u64)TS_FRAMES * 2U * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(ints);
    for (u64 i = 0U; i < (u64)TS_FRAMES * 2U; ++i) {
        ints[i] = pcm[i];
    }
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), ints, 2U, 16U, TS_FRAMES, NULL, &enc),
        NPH_RESULT_OK);
    // The block holding 2 s: a second past what the one-second ring holds
    // when the clip is made.
    const u32 bad = (u32)(96000U / enc.stream.block_frames);
    NTEST_ASSERT_TRUE(bad + 1U < enc.stream.block_count);
    u8 *data = (u8 *)NPH_ALLOC(nph_allocator_system(), enc.stream.size, 16U);
    NTEST_ASSERT_NOT_NULL(data);
    memcpy(data, enc.stream.data, (size_t)enc.stream.size);
    const u64 begin = enc.stream.block_offsets[bad];
    const u64 end = enc.stream.block_offsets[bad + 1U];
    data[begin + (end - begin) / 2U] ^= 0x10U;
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_LOSSLESS;
    cd.channels = 2U;
    cd.sample_rate = 48000U;
    cd.frames = TS_FRAMES;
    cd.data = enc.stream.data;
    cd.size = enc.stream.size;
    cd.bits = 16U;
    cd.block_frames = enc.stream.block_frames;
    cd.block_offsets = enc.stream.block_offsets;
    cd.block_count = enc.stream.block_count;
    f32 *from_clip = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    f32 *from_stream = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    NphAudioMixer *a = tm_mixer(2U, 4U, 4U);
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(a, &cd, &clip), NPH_RESULT_OK);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    (void)tm_play(a, &d);
    ts_render(a, NULL, from_clip, BLOCKS);
    nph_audio_mixer_destroy(a);
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = data;
    src.size = enc.stream.size;
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_LOSSLESS;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    sd.bits = 16U;
    sd.block_frames = enc.stream.block_frames;
    sd.block_offsets = enc.stream.block_offsets;
    sd.block_count = enc.stream.block_count;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *b = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip sclip = ts_stream_clip(b, s, &sd, &st);
    NphAudioPlayDesc e = nph_audio_play_desc_default(sclip);
    (void)tm_play(b, &e);
    ts_render(b, st, from_stream, BLOCKS);
    NTEST_EXPECT_TRUE(nph_atomic_load_u32(&st->failed, NPH_MEMORY_ORDER_ACQUIRE) != 0U);
    const u64 held = nph_atomic_load_u64(&st->produced, NPH_MEMORY_ORDER_ACQUIRE);
    const u64 at = (u64)bad * enc.stream.block_frames;
    NphAudioStreamerStats ss;
    nph_audio_streamer_stats(s, &ss);
    NTEST_LOG("block %u starts at frame %llu; the stream ended at %llu", bad,
              (unsigned long long)at, (unsigned long long)held);
    NTEST_EXPECT_EQ_UINT(ss.corrupt_blocks, 1U);
    NTEST_EXPECT_EQ_UINT(ss.read_errors, 0U);
    NTEST_EXPECT_EQ_UINT(ss.seeks, 0U);
    // Nothing of the block played, and everything before the chunk it
    // starts in did, as the clip plays it.
    NTEST_EXPECT_TRUE(held <= at && held + MIX_STREAM_CHUNK > at);
    NTEST_EXPECT_TRUE(held > 64U && test_same_bits(from_clip, from_stream, (u32)(held - 64U) * 2U));
    const u64 dry = (held + 2ULL * NPH_AUDIO_BLOCK_FRAMES) * 2U;
    NTEST_EXPECT_TRUE(
        test_rms(from_stream + dry, (u32)((u64)BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U - dry)) == 0.0);
    // A corrupt block in the first fill: the clip is refused for what it
    // is (the failed stream reads no more, so the bytes are the test's).
    data[(enc.stream.block_offsets[0] + enc.stream.block_offsets[1]) / 2U] ^= 0x10U;
    NphAudioClip refused;
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(b, s, &sd, &refused), NPH_RESULT_CORRUPT);
    nph_audio_streamer_stats(s, &ss);
    NTEST_EXPECT_EQ_UINT(ss.corrupt_blocks, 2U);
    NTEST_EXPECT_EQ_UINT(ss.read_errors, 0U);
    nph_audio_mixer_destroy(b);
    nph_audio_streamer_destroy(s);
    test_buffer_free(from_clip, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    test_buffer_free(from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    NPH_FREE(nph_allocator_system(), data, enc.stream.size, 16U);
    nph_lossless_encoded_free(&enc);
    NPH_FREE(nph_allocator_system(), ints, (u64)TS_FRAMES * 2U * sizeof(i32), 16U);
    ts_samples_free(pcm);
}

// Counts the reads made on one thread (the one that made the clip).
typedef struct TsWatchedSource {
    TsSource src;
    u64 thread;
    NphAtomicU32 reads_there;
} TsWatchedSource;

static NphResult ts_watched_read(void *user, u64 offset, void *dst, u64 size)
{
    TsWatchedSource *w = (TsWatchedSource *)user;
    if (nph_thread_current_id() == w->thread) {
        (void)nph_atomic_fetch_add_u32(&w->reads_there, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    return ts_read(&w->src, offset, dst, size);
}

NTEST(audio_stream, a_deferred_stream_is_filled_by_the_streamer)
{
    // With defer_fill, making the clip reads nothing on the calling thread;
    // the streamer fills the ring, and a voice started once it has plays
    // what the clip in memory plays, to the bit.
    enum {
        BLOCKS = 300
    };
    i16 *pcm = ts_samples();
    TsWatchedSource w;
    memset(&w, 0, sizeof w);
    w.src.data = (const u8 *)pcm;
    w.src.size = (u64)TS_FRAMES * 4U;
    w.thread = nph_thread_current_id();
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_PCM;
    cd.format = NPH_AUDIO_FORMAT_S16;
    cd.channels = 2U;
    cd.sample_rate = 48000U;
    cd.frames = TS_FRAMES;
    cd.data = pcm;
    cd.size = w.src.size;
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_watched_read, &w};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_S16;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = w.src.size;
    sd.buffer_seconds = 0.25F;
    sd.defer_fill = 1;
    f32 *from_clip = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    f32 *from_stream = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    NphAudioMixer *a = tm_mixer(2U, 4U, 4U);
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(a, &cd, &clip), NPH_RESULT_OK);
    ts_play(a, clip, false, NULL, from_clip, BLOCKS);
    nph_audio_mixer_destroy(a);
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *b = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip sclip = ts_stream_clip(b, s, &sd, &st);
    NTEST_EXPECT_EQ_UINT(nph_atomic_load_u32(&w.reads_there, NPH_MEMORY_ORDER_RELAXED), 0U);
    // The streamer fills the whole ring (a quarter second, a power of two).
    const u64 t0 = nph_time_now_ns();
    while (nph_audio_clip_stream_ahead(b, sclip) < (u64)st->capacity &&
           nph_time_now_ns() - t0 < nph_time_ms_to_ns(2000U)) {
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
    }
    NTEST_EXPECT_EQ_UINT(nph_audio_clip_stream_ahead(b, sclip), (u64)st->capacity);
    ts_play(b, sclip, false, st, from_stream, BLOCKS);
    NphAudioMixerStats stats;
    nph_audio_mixer_stats(b, &stats);
    NTEST_EXPECT_EQ_UINT(stats.stream_starved, 0U);
    NTEST_EXPECT_TRUE(test_same_bits(from_clip, from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U));
    NTEST_EXPECT_EQ_UINT(nph_atomic_load_u32(&w.reads_there, NPH_MEMORY_ORDER_RELAXED), 0U);
    nph_audio_mixer_destroy(b);
    nph_audio_streamer_destroy(s);
    test_buffer_free(from_clip, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    test_buffer_free(from_stream, BLOCKS * NPH_AUDIO_BLOCK_FRAMES * 2U);
    ts_samples_free(pcm);
}

// Waits up to two seconds for nph_audio_clip_stream_ready.
static bool ts_wait_ready(const NphAudioMixer *m, NphAudioClip clip, u64 frames)
{
    const u64 t0 = nph_time_now_ns();
    while (!nph_audio_clip_stream_ready(m, clip, frames)) {
        if (nph_time_now_ns() - t0 > nph_time_ms_to_ns(2000U)) {
            return false;
        }
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
    }
    return true;
}

NTEST(audio_stream, a_stream_says_when_its_voice_can_read)
{
    // A deferred stream whose reads are slow: not ready before the streamer
    // has decoded what was asked, then ready; asked for more than half its
    // ring, ready at half. A short stream is ready for more than it holds
    // once its end is decoded, a failed one at once; a clip in memory and a
    // stale handle always.
    i16 *pcm = ts_samples();
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)pcm;
    src.size = (u64)TS_FRAMES * 4U;
    nph_atomic_store_u64(&src.delay_ns, nph_time_ms_to_ns(50U), NPH_MEMORY_ORDER_RELAXED);
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_S16;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    sd.buffer_seconds = 0.25F;
    sd.defer_fill = 1;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip clip = ts_stream_clip(m, s, &sd, &st);
    NTEST_EXPECT_FALSE(nph_audio_clip_stream_ready(m, clip, 256U)); // its first read sleeps
    NTEST_EXPECT_TRUE(ts_wait_ready(m, clip, 256U));
    NTEST_EXPECT_TRUE(nph_audio_clip_stream_ahead(m, clip) >= 256U);
    NTEST_EXPECT_TRUE(ts_wait_ready(m, clip, NPH_U64_MAX));
    NTEST_EXPECT_TRUE(nph_audio_clip_stream_ahead(m, clip) >= (u64)st->capacity / 2U);
    // A stream of 1000 frames, all of them decoded: ready for more.
    nph_atomic_store_u64(&src.delay_ns, 0U, NPH_MEMORY_ORDER_RELAXED);
    NphAudioStreamDesc shorter = sd;
    shorter.frames = 1000U;
    shorter.size = 4000U;
    MixStream *short_st = NULL;
    const NphAudioClip short_clip = ts_stream_clip(m, s, &shorter, &short_st);
    NTEST_EXPECT_TRUE(ts_wait_ready(m, short_clip, 48000U));
    NTEST_EXPECT_EQ_UINT(nph_audio_clip_stream_ahead(m, short_clip), 1000U);
    // A stream whose source fails: ready, with nothing ahead.
    TsSource failing = src;
    nph_atomic_store_u32(&failing.fail, 1U, NPH_MEMORY_ORDER_RELAXED);
    NphAudioStreamDesc broken = sd;
    broken.source = (NphAudioStreamSource){ts_read, &failing};
    MixStream *broken_st = NULL;
    const NphAudioClip broken_clip = ts_stream_clip(m, s, &broken, &broken_st);
    NTEST_EXPECT_TRUE(ts_wait_ready(m, broken_clip, 256U));
    NTEST_EXPECT_EQ_UINT(nph_audio_clip_stream_ahead(m, broken_clip), 0U);
    // Nothing to wait for.
    const NphAudioClip tone = tm_tone(m, 440.0, 4800U, 48000U, 0.5F, NULL);
    NTEST_EXPECT_TRUE(nph_audio_clip_stream_ready(m, tone, NPH_U64_MAX));
    NTEST_EXPECT_TRUE(nph_audio_clip_stream_ready(m, (NphAudioClip){0U}, NPH_U64_MAX));
    nph_audio_mixer_destroy(m);
    nph_audio_streamer_destroy(s);
    ts_samples_free(pcm);
}

NTEST(audio_stream, closing_refusals_and_errors)
{
    i16 *pcm = ts_samples();
    TsSource src;
    memset(&src, 0, sizeof src);
    src.data = (const u8 *)pcm;
    src.size = (u64)TS_FRAMES * 4U;
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){ts_read, &src};
    sd.codec = NPH_AUDIO_CODEC_PCM;
    sd.format = NPH_AUDIO_FORMAT_S16;
    sd.channels = 2U;
    sd.sample_rate = 48000U;
    sd.frames = TS_FRAMES;
    sd.size = src.size;
    NphAudioStreamer *s = ts_streamer();
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    MixStream *st = NULL;
    const NphAudioClip clip = ts_stream_clip(m, s, &sd, &st);
    // Filled when made: the ring's whole length (a second, to a power of
    // two); nothing for a clip in memory or a stale handle.
    NTEST_EXPECT_EQ_UINT(nph_audio_clip_stream_ahead(m, clip), 65536U);
    const NphAudioClip tone = tm_tone(m, 440.0, 4800U, TM_RATE, 0.5F, NULL);
    NTEST_EXPECT_EQ_UINT(nph_audio_clip_stream_ahead(m, tone), 0U);
    const NphAudioClip stale = {0U};
    NTEST_EXPECT_EQ_UINT(nph_audio_clip_stream_ahead(m, stale), 0U);
    nph_audio_clip_destroy(m, tone);
    // Once, from the start: not part-way, not twice (while it plays, nor
    // after it ends).
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    NphAudioVoice again;
    d.start_frame = 100U;
    NTEST_EXPECT_EQ_INT(nph_audio_voice_play(m, &d, &again), NPH_RESULT_INVALID_ARGUMENT);
    d.start_frame = 0U;
    const NphAudioVoice v = tm_play(m, &d);
    NTEST_EXPECT_EQ_INT(nph_audio_voice_play(m, &d, &again), NPH_RESULT_INVALID_STATE);
    f32 out[2U * NPH_AUDIO_BLOCK_FRAMES];
    nph_audio_mixer_render(m, out, NPH_AUDIO_BLOCK_FRAMES);
    nph_audio_voice_stop(m, v, 0.0F);
    for (u32 b = 0U; b < 4U; ++b) {
        nph_audio_mixer_render(m, out, NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_update(m);
    }
    NTEST_EXPECT_FALSE(nph_audio_voice_is_playing(m, v));
    NTEST_EXPECT_EQ_INT(nph_audio_voice_play(m, &d, &again), NPH_RESULT_INVALID_STATE);
    // Destroyed: the clip goes, the streamer frees the stream.
    nph_audio_clip_destroy(m, clip);
    for (u32 b = 0U; b < 4U; ++b) {
        nph_audio_mixer_render(m, out, NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_update(m);
    }
    NphAudioStreamerStats ss;
    const u64 t0 = nph_time_now_ns();
    do {
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
        nph_audio_streamer_stats(s, &ss);
    } while (ss.streams != 0U && nph_time_now_ns() - t0 < nph_time_ms_to_ns(2000U));
    NTEST_EXPECT_EQ_UINT(ss.streams, 0U);
    // Refusals: ADPCM, a broken description, a source that fails at once,
    // more streams than the streamer holds.
    NphAudioClip c;
    NphAudioStreamDesc bad = sd;
    bad.codec = NPH_AUDIO_CODEC_ADPCM;
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, s, &bad, &c), NPH_RESULT_INVALID_ARGUMENT);
    bad = sd;
    bad.size = 16U;
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, s, &bad, &c), NPH_RESULT_INVALID_ARGUMENT);
    bad = sd;
    bad.start_frame = TS_FRAMES;
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, s, &bad, &c), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, NULL, &sd, &c),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_atomic_store_u32(&src.fail, 1U, NPH_MEMORY_ORDER_RELAXED);
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, s, &sd, &c), NPH_RESULT_IO_ERROR);
    nph_atomic_store_u32(&src.fail, 0U, NPH_MEMORY_ORDER_RELAXED);
    NphAudioClip four[4];
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, s, &sd, &four[i]), NPH_RESULT_OK);
    }
    NTEST_EXPECT_EQ_INT(nph_audio_clip_create_stream(m, s, &sd, &c), NPH_RESULT_FULL);
    nph_audio_mixer_destroy(m); // closes the four
    nph_audio_streamer_destroy(s);
    ts_samples_free(pcm);
}

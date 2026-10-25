// SPDX-License-Identifier: Apache-2.0
// Sound cooking (cook/sound.h): the per-source settings, the plan a WAV
// file gets, every codec's asset viewed and decoded back to the source's
// samples (to the bit for PCM and lossless), the file's loop, refusals.
#include <nepheline/asset/sound.h>
#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/core/alloc_debug.h>

#include "test_cook_support.h"

#include <cook/sound.h>

// ---------------------------------------------------------------------------
// WAV images of every encoding
// ---------------------------------------------------------------------------

typedef f64 (*TsSampleFn)(u64 frame, u32 channel);

#define TS_PI 3.14159265358979323846

static void put_u16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8U) & 0xFFU);
}

static void put_u32(u8 *p, u32 v)
{
    put_u16(p, v & 0xFFFFU);
    put_u16(p + 2, v >> 16U);
}

// A chunk's four-character tag (not a string: no terminator).
static void put_tag(u8 *p, const char *tag)
{
    for (u32 i = 0U; i < 4U; ++i) {
        p[i] = (u8)tag[i];
    }
}

static u32 encoding_bytes(NphWavEncoding e)
{
    static const u32 bytes[] = {1U, 2U, 3U, 4U, 4U, 8U};
    return bytes[e];
}

// One sample of `e` from v in [-1, 1]: integers rounded to nearest and
// clamped (8-bit offset by 128), floats as they are.
static void put_sample(u8 *p, NphWavEncoding e, f64 v)
{
    static const f64 scale[] = {128.0, 32768.0, 8388608.0, 2147483648.0};
    if (e == NPH_WAV_FLOAT32) {
        const f32 x = (f32)v;
        memcpy(p, &x, sizeof x);
        return;
    }
    if (e == NPH_WAV_FLOAT64) {
        memcpy(p, &v, sizeof v);
        return;
    }
    const f64 s = scale[e];
    f64 q = floor(v * s + 0.5);
    q = q > s - 1.0 ? s - 1.0 : (q < -s ? -s : q);
    const i64 i = (i64)q;
    if (e == NPH_WAV_PCM_U8) {
        p[0] = (u8)(i + 128);
        return;
    }
    const u64 u = (u64)i;
    for (u32 b = 0U; b < encoding_bytes(e); ++b) {
        p[b] = (u8)((u >> (8U * b)) & 0xFFU);
    }
}

// A WAV image of `frames` frames from `fn`, with a sampler loop
// [loop_start, loop_end) when loop_end > 0 (written inclusive, as the
// format has it). Free with ts_wav_free.
static u8 *ts_wav(NphWavEncoding e, u32 channels, u32 rate, u64 frames, TsSampleFn fn,
                  u64 loop_start, u64 loop_end, u64 *out_size)
{
    const u32 sample = encoding_bytes(e);
    const u64 data = frames * channels * sample;
    const u64 smpl = loop_end > 0U ? 8U + 36U + 24U : 0U;
    const u64 size = 12U + 8U + 16U + smpl + 8U + data;
    u8 *w = (u8 *)NPH_ALLOC(nph_allocator_system(), size, 16U);
    NTEST_ASSERT_NOT_NULL(w);
    memset(w, 0, (size_t)size);
    put_tag(w, "RIFF");
    put_u32(w + 4, (u32)(size - 8U));
    put_tag(w + 8, "WAVE");
    u8 *p = w + 12;
    put_tag(p, "fmt ");
    put_u32(p + 4, 16U);
    put_u16(p + 8, e == NPH_WAV_FLOAT32 || e == NPH_WAV_FLOAT64 ? 3U : 1U);
    put_u16(p + 10, channels);
    put_u32(p + 12, rate);
    put_u32(p + 16, rate * channels * sample);
    put_u16(p + 20, channels * sample);
    put_u16(p + 22, sample * 8U);
    p += 24;
    if (loop_end > 0U) {
        put_tag(p, "smpl");
        put_u32(p + 4, 36U + 24U);
        put_u32(p + 8 + 28, 1U);                        // one loop
        put_u32(p + 8 + 36 + 8, (u32)loop_start);       // its start
        put_u32(p + 8 + 36 + 12, (u32)(loop_end - 1U)); // its last frame
        p += smpl;
    }
    put_tag(p, "data");
    put_u32(p + 4, (u32)data);
    p += 8;
    for (u64 f = 0U; f < frames; ++f) {
        for (u32 ch = 0U; ch < channels; ++ch) {
            put_sample(p, e, fn(f, ch));
            p += sample;
        }
    }
    *out_size = size;
    return w;
}

static void ts_wav_free(u8 *w, u64 size)
{
    NPH_FREE(nph_allocator_system(), w, size, 16U);
}

// A chord with a little noise, below full scale: every bit of the samples
// moves.
static f64 ts_music(u64 frame, u32 channel)
{
    const f64 t = (f64)frame / 8000.0;
    u64 h = frame * 0x9E3779B97F4A7C15ULL + channel;
    h ^= h >> 29U;
    const f64 noise = (f64)(h & 0xFFFFU) / 65536.0 - 0.5;
    return 0.4 * sin(2.0 * TS_PI * (220.0 + 110.0 * (f64)channel) * t) +
           0.2 * sin(2.0 * TS_PI * 331.0 * t) + 0.01 * noise;
}

// The same, as exact 24-bit integers over 2^23: floats and 32-bit
// integers the lossless codec holds exactly.
static f64 ts_music24(u64 frame, u32 channel)
{
    return floor(ts_music(frame, channel) * 8388608.0) / 8388608.0;
}

typedef struct TsCooked {
    TestAsset asset;
    NphSoundView view;
} TsCooked;

// Cooks the WAV image under `settings` into `out`: the asset, viewed.
static void ts_cook(const u8 *wav, u64 size, const CookSoundSettings *settings, TsCooked *out)
{
    memset(out, 0, sizeof *out);
    NphWavInfo info;
    NTEST_ASSERT_EQ_INT(nph_wav_parse(wav, size, &info), NPH_RESULT_OK);
    NphAssetWriter w;
    nph_asset_writer_init(&w, nph_allocator_system(), NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION,
                          nph_guid_from_cstr("sound"), 1U);
    NTEST_ASSERT_EQ_INT(cook_sound(nph_allocator_system(), &info, settings, &w), NPH_RESULT_OK);
    test_asset_from_writer(nph_allocator_system(), &w, &out->asset);
    nph_asset_writer_free(&w);
    NTEST_ASSERT_EQ_INT(nph_sound_view(&out->asset.asset, &out->view), NPH_RESULT_OK);
}

static CookSoundSettings ts_settings(CookSoundCodec codec, CookSoundStream stream)
{
    CookSoundSettings s;
    cook_sound_settings_default(&s);
    s.codec = codec;
    s.stream = stream;
    return s;
}

// The lossless stream of a cooked sound.
static NphLosslessStream ts_lossless(const NphSoundView *v)
{
    NphLosslessStream s;
    memset(&s, 0, sizeof s);
    s.channels = v->header->channels;
    s.bits = v->header->bits;
    s.block_frames = v->header->block_frames;
    s.frames = v->header->frames;
    s.data = v->data;
    s.size = v->size;
    s.block_offsets = v->block_offsets;
    s.block_count = v->header->block_count;
    return s;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// Through a debug allocator: whatever the parse allocates is freed, on every
// path (a refusal too).
static NphResult ts_parse(const char *json, CookSoundSettings *s)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    const NphResult r =
        cook_sound_settings_parse(nph_debug_allocator_allocator(&da), json, strlen(json), s);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
    return r;
}

NTEST(cook_sound, settings_default_and_parse)
{
    CookSoundSettings s;
    cook_sound_settings_default(&s);
    NTEST_EXPECT_EQ_INT(s.codec, COOK_SOUND_CODEC_AUTO);
    NTEST_EXPECT_EQ_INT(s.stream, COOK_SOUND_STREAM_AUTO);
    NTEST_EXPECT_TRUE(s.stream_seconds == COOK_SOUND_STREAM_SECONDS);
    NTEST_EXPECT_FALSE(s.exhaustive);
    NTEST_ASSERT_EQ_INT(ts_parse("{\"codec\": \"adpcm\", \"stream\": \"never\", "
                                 "\"stream_seconds\": 2.5, \"exhaustive\": true}",
                                 &s),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(s.codec, COOK_SOUND_CODEC_ADPCM);
    NTEST_EXPECT_EQ_INT(s.stream, COOK_SOUND_STREAM_NEVER);
    NTEST_EXPECT_TRUE(s.stream_seconds == 2.5F);
    NTEST_EXPECT_TRUE(s.exhaustive);
    // Each key on its own, over what is there.
    NTEST_ASSERT_EQ_INT(ts_parse("{\"codec\": \"lossless\"}", &s), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(s.codec, COOK_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_INT(s.stream, COOK_SOUND_STREAM_NEVER);
    NTEST_ASSERT_EQ_INT(ts_parse("{\"stream\": \"always\", \"codec\": \"pcm\"}", &s),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(s.codec, COOK_SOUND_CODEC_PCM);
    NTEST_EXPECT_EQ_INT(s.stream, COOK_SOUND_STREAM_ALWAYS);
    NTEST_ASSERT_EQ_INT(ts_parse("{}", &s), NPH_RESULT_OK);
    // Refusals leave the settings as they were.
    const CookSoundSettings before = s;
    static const char *const bad[] = {
        "{\"codec\": \"mp3\"}",
        "{\"codec\": 3}",
        "{\"stream\": true}",
        "{\"stream\": \"sometimes\"}",
        "{\"stream_seconds\": 0}",
        "{\"stream_seconds\": -1}",
        "{\"stream_seconds\": \"8\"}",
        "{\"exhaustive\": \"yes\"}",
        "{\"loop\": true}",
        "{\"codec\": \"pcm\", \"volume\": 1}",
    };
    for (u32 i = 0U; i < NPH_ARRAY_COUNT(bad); ++i) {
        NTEST_EXPECT_EQ_INT(ts_parse(bad[i], &s), NPH_RESULT_INVALID_ARGUMENT);
    }
    NTEST_EXPECT_EQ_INT(ts_parse("[1, 2]", &s), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_INT(ts_parse("{\"codec\": ", &s), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_INT(ts_parse("", &s), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_INT(s.codec, before.codec);
    NTEST_EXPECT_EQ_INT(s.stream, before.stream);
    NTEST_EXPECT_TRUE(s.stream_seconds == before.stream_seconds);
    NTEST_EXPECT_EQ_INT(s.exhaustive, before.exhaustive);
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

static CookSoundPlan ts_plan(NphWavEncoding e, f64 seconds, const CookSoundSettings *settings,
                             TsSampleFn fn, NphResult expect)
{
    u64 size = 0U;
    u8 *wav = ts_wav(e, 1U, 8000U, (u64)(seconds * 8000.0), fn, 0U, 0U, &size);
    NphWavInfo info;
    NTEST_ASSERT_EQ_INT(nph_wav_parse(wav, size, &info), NPH_RESULT_OK);
    CookSoundPlan plan;
    NTEST_EXPECT_EQ_INT(cook_sound_plan(&info, settings, &plan), expect);
    ts_wav_free(wav, size);
    return plan;
}

NTEST(cook_sound, the_plan_follows_the_length_and_the_settings)
{
    const CookSoundSettings dflt = ts_settings(COOK_SOUND_CODEC_AUTO, COOK_SOUND_STREAM_AUTO);
    // AUTO: short sounds stay PCM in memory, long ones stream lossless.
    CookSoundPlan p = ts_plan(NPH_WAV_PCM_S16, 1.0, &dflt, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(p.stream);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_PCM);
    NTEST_EXPECT_EQ_INT(p.format, NPH_SOUND_FORMAT_S16);
    NTEST_EXPECT_EQ_UINT(p.bits, 16U);
    p = ts_plan(NPH_WAV_PCM_S24, 9.0, &dflt, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(p.stream);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_INT(p.format, NPH_SOUND_FORMAT_NONE);
    NTEST_EXPECT_EQ_UINT(p.bits, 24U);
    // A threshold of 0 is the default one.
    CookSoundSettings zero;
    memset(&zero, 0, sizeof zero);
    p = ts_plan(NPH_WAV_PCM_S16, 7.5, &zero, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(p.stream);
    p = ts_plan(NPH_WAV_PCM_S16, 8.0, &zero, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(p.stream);
    // Streaming outright, or never.
    CookSoundSettings s = ts_settings(COOK_SOUND_CODEC_AUTO, COOK_SOUND_STREAM_ALWAYS);
    p = ts_plan(NPH_WAV_PCM_S16, 0.5, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(p.stream);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_LOSSLESS);
    s = ts_settings(COOK_SOUND_CODEC_AUTO, COOK_SOUND_STREAM_NEVER);
    p = ts_plan(NPH_WAV_PCM_S16, 9.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(p.stream);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_PCM);
    // Codecs asked for, streamed or not; ADPCM never streams.
    s = ts_settings(COOK_SOUND_CODEC_LOSSLESS, COOK_SOUND_STREAM_NEVER);
    p = ts_plan(NPH_WAV_PCM_U8, 1.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_UINT(p.bits, 8U);
    s = ts_settings(COOK_SOUND_CODEC_PCM, COOK_SOUND_STREAM_ALWAYS);
    p = ts_plan(NPH_WAV_PCM_U8, 1.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(p.stream);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_PCM);
    NTEST_EXPECT_EQ_INT(p.format, NPH_SOUND_FORMAT_S16); // 8-bit samples move to 16
    s = ts_settings(COOK_SOUND_CODEC_ADPCM, COOK_SOUND_STREAM_AUTO);
    p = ts_plan(NPH_WAV_FLOAT32, 1.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_ADPCM);
    NTEST_EXPECT_EQ_UINT(p.bits, 16U);
    s.stream = COOK_SOUND_STREAM_ALWAYS;
    (void)ts_plan(NPH_WAV_PCM_S16, 1.0, &s, ts_music, NPH_RESULT_INVALID_ARGUMENT);
    // What the lossless codec holds exactly: floats and 32-bit integers
    // that are 24-bit ones; otherwise they stay PCM at their own depth.
    s = ts_settings(COOK_SOUND_CODEC_LOSSLESS, COOK_SOUND_STREAM_NEVER);
    p = ts_plan(NPH_WAV_FLOAT32, 1.0, &s, ts_music24, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_UINT(p.bits, 24U);
    p = ts_plan(NPH_WAV_FLOAT32, 1.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_PCM);
    NTEST_EXPECT_EQ_INT(p.format, NPH_SOUND_FORMAT_F32);
    NTEST_EXPECT_EQ_UINT(p.bits, 32U);
    p = ts_plan(NPH_WAV_FLOAT64, 1.0, &s, ts_music24, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_LOSSLESS);
    p = ts_plan(NPH_WAV_FLOAT64, 1.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.format, NPH_SOUND_FORMAT_F32); // 64-bit floats become 32-bit
    p = ts_plan(NPH_WAV_PCM_S32, 1.0, &s, ts_music24, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_UINT(p.bits, 24U);
    p = ts_plan(NPH_WAV_PCM_S32, 1.0, &s, ts_music, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(p.codec, NPH_SOUND_CODEC_PCM);
    NTEST_EXPECT_EQ_INT(p.format, NPH_SOUND_FORMAT_S32);
}

static f64 ts_not_a_number(u64 frame, u32 channel)
{
    NPH_UNUSED(channel);
    return frame == 100U ? (f64)NAN : 0.25;
}

NTEST(cook_sound, refusals)
{
    const CookSoundSettings dflt = ts_settings(COOK_SOUND_CODEC_AUTO, COOK_SOUND_STREAM_AUTO);
    // A sample that is not a number would reach every bus its voice feeds.
    (void)ts_plan(NPH_WAV_FLOAT32, 1.0, &dflt, ts_not_a_number, NPH_RESULT_INVALID_ARGUMENT);
    (void)ts_plan(NPH_WAV_FLOAT64, 1.0, &dflt, ts_not_a_number, NPH_RESULT_INVALID_ARGUMENT);
    // Too long for memory: AUTO streams it whatever its threshold, NEVER
    // cannot cook it. (The samples are not read for integers.)
    NphWavInfo info;
    memset(&info, 0, sizeof info);
    info.encoding = NPH_WAV_PCM_S16;
    info.channels = 1U;
    info.sample_rate = 48000U;
    info.bytes_per_frame = 2U;
    info.frames = NPH_SOUND_MAX_FRAMES + 1U;
    CookSoundSettings s = dflt;
    s.stream_seconds = 1.0e9F;
    CookSoundPlan p;
    NTEST_EXPECT_EQ_INT(cook_sound_plan(&info, &s, &p), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(p.stream);
    s.stream = COOK_SOUND_STREAM_NEVER;
    NTEST_EXPECT_EQ_INT(cook_sound_plan(&info, &s, &p), NPH_RESULT_INVALID_ARGUMENT);
    // Rates and channel counts the runtime does not take.
    info.frames = 100U;
    info.sample_rate = NPH_SOUND_MAX_RATE + 1U;
    NTEST_EXPECT_EQ_INT(cook_sound_plan(&info, &dflt, &p), NPH_RESULT_INVALID_ARGUMENT);
    info.sample_rate = 48000U;
    info.channels = 0U;
    NTEST_EXPECT_EQ_INT(cook_sound_plan(&info, &dflt, &p), NPH_RESULT_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// The cooked sounds
// ---------------------------------------------------------------------------

NTEST(cook_sound, pcm_keeps_every_sample)
{
    // Every encoding, stereo, in memory: DATA holds the file's samples in
    // its format, 8-bit ones moved to 16 bits and 64-bit floats to 32.
    const CookSoundSettings s = ts_settings(COOK_SOUND_CODEC_PCM, COOK_SOUND_STREAM_NEVER);
    for (u32 e = 0U; e <= (u32)NPH_WAV_FLOAT64; ++e) {
        u64 size = 0U;
        u8 *wav = ts_wav((NphWavEncoding)e, 2U, 22050U, 3000U, ts_music, 0U, 0U, &size);
        NphWavInfo info;
        NTEST_ASSERT_EQ_INT(nph_wav_parse(wav, size, &info), NPH_RESULT_OK);
        TsCooked c;
        ts_cook(wav, size, &s, &c);
        const NphSoundHeader *h = c.view.header;
        NTEST_EXPECT_EQ_UINT(h->codec, (u32)NPH_SOUND_CODEC_PCM);
        NTEST_EXPECT_EQ_UINT(h->channels, 2U);
        NTEST_EXPECT_EQ_UINT(h->sample_rate, 22050U);
        NTEST_EXPECT_EQ_UINT(h->frames, 3000U);
        NTEST_EXPECT_EQ_UINT(h->flags, 0U);
        const u32 bytes = nph_sound_format_bytes(h->format);
        NTEST_ASSERT_EQ_UINT(c.view.size, 3000ULL * 2U * bytes);
        u32 wrong = 0U;
        for (u64 i = 0U; i < 3000ULL * 2U; ++i) {
            const u8 *p = c.view.data + i * bytes;
            const u64 f = i / 2U;
            const u32 ch = (u32)(i % 2U);
            if (e == (u32)NPH_WAV_PCM_U8) {
                i16 x;
                memcpy(&x, p, sizeof x);
                wrong += x != (i16)(nph_wav_sample_int(&info, f, ch) * 256) ? 1U : 0U;
            } else if (e == (u32)NPH_WAV_FLOAT64) {
                f32 x;
                memcpy(&x, p, sizeof x);
                wrong += x != (f32)nph_wav_sample_float(&info, f, ch) ? 1U : 0U;
            } else {
                wrong += memcmp(p, (const u8 *)info.data + i * bytes, bytes) != 0 ? 1U : 0U;
            }
        }
        NTEST_EXPECT_EQ_UINT(wrong, 0U);
        test_asset_free(&c.asset);
        ts_wav_free(wav, size);
    }
}

NTEST(cook_sound, lossless_decodes_to_the_bit)
{
    // Streamed lossless sounds of each depth the codec takes exactly decode
    // to the source's own integers.
    static const NphWavEncoding encodings[] = {NPH_WAV_PCM_U8,  NPH_WAV_PCM_S16, NPH_WAV_PCM_S24,
                                               NPH_WAV_PCM_S32, NPH_WAV_FLOAT32, NPH_WAV_FLOAT64};
    const CookSoundSettings s = ts_settings(COOK_SOUND_CODEC_LOSSLESS, COOK_SOUND_STREAM_ALWAYS);
    for (u32 k = 0U; k < NPH_ARRAY_COUNT(encodings); ++k) {
        const NphWavEncoding e = encodings[k];
        const bool deep = e == NPH_WAV_PCM_S32 || e == NPH_WAV_FLOAT32 || e == NPH_WAV_FLOAT64;
        u64 size = 0U;
        u8 *wav = ts_wav(e, 2U, 44100U, 20000U, deep ? ts_music24 : ts_music, 0U, 0U, &size);
        NphWavInfo info;
        NTEST_ASSERT_EQ_INT(nph_wav_parse(wav, size, &info), NPH_RESULT_OK);
        TsCooked c;
        ts_cook(wav, size, &s, &c);
        const NphSoundHeader *h = c.view.header;
        NTEST_EXPECT_EQ_UINT(h->codec, (u32)NPH_SOUND_CODEC_LOSSLESS);
        NTEST_EXPECT_EQ_UINT(h->flags, (u32)NPH_SOUND_FLAG_STREAM);
        const NphLosslessStream ls = ts_lossless(&c.view);
        NTEST_ASSERT_EQ_INT(nph_lossless_validate(&ls), NPH_RESULT_OK);
        i32 *x = (i32 *)NPH_ALLOC(nph_allocator_system(), 20000ULL * 2U * sizeof(i32), 16U);
        NTEST_ASSERT_NOT_NULL(x);
        NTEST_ASSERT_EQ_INT(nph_lossless_decode_all(&ls, x), NPH_RESULT_OK);
        u32 wrong = 0U;
        for (u64 i = 0U; i < 20000ULL * 2U; ++i) {
            const u64 f = i / 2U;
            const u32 ch = (u32)(i % 2U);
            i32 want;
            if (e == NPH_WAV_FLOAT32 || e == NPH_WAV_FLOAT64) {
                want = (i32)(nph_wav_sample_float(&info, f, ch) * 8388608.0);
            } else if (e == NPH_WAV_PCM_S32) {
                want = nph_wav_sample_int(&info, f, ch) / 256;
            } else {
                want = nph_wav_sample_int(&info, f, ch);
            }
            wrong += x[i] != want ? 1U : 0U;
        }
        NTEST_EXPECT_EQ_UINT(wrong, 0U);
        // Half the bytes of 16-bit PCM or so, for the 16-bit source.
        if (e == NPH_WAV_PCM_S16) {
            NTEST_LOG("lossless: %llu bytes for %llu of PCM", (unsigned long long)c.view.size,
                      (unsigned long long)info.data_size);
            NTEST_EXPECT_TRUE(c.view.size < info.data_size);
        }
        NPH_FREE(nph_allocator_system(), x, 20000ULL * 2U * sizeof(i32), 16U);
        test_asset_free(&c.asset);
        ts_wav_free(wav, size);
    }
}

NTEST(cook_sound, adpcm_codes_16_bit_samples)
{
    // ADPCM in memory from a 24-bit source: a quarter the bytes, the
    // samples rounded to 16 bits and then coded.
    const CookSoundSettings s = ts_settings(COOK_SOUND_CODEC_ADPCM, COOK_SOUND_STREAM_NEVER);
    u64 size = 0U;
    u8 *wav = ts_wav(NPH_WAV_PCM_S24, 1U, 16000U, 16000U, ts_music, 0U, 0U, &size);
    NphWavInfo info;
    NTEST_ASSERT_EQ_INT(nph_wav_parse(wav, size, &info), NPH_RESULT_OK);
    TsCooked c;
    ts_cook(wav, size, &s, &c);
    NTEST_EXPECT_EQ_UINT(c.view.header->codec, (u32)NPH_SOUND_CODEC_ADPCM);
    NTEST_EXPECT_EQ_UINT(c.view.header->bits, 16U);
    NTEST_EXPECT_EQ_UINT(c.view.size, nph_adpcm_size(1U, 16000U));
    NphAdpcmStream as;
    memset(&as, 0, sizeof as);
    as.channels = 1U;
    as.frames = 16000U;
    as.data = c.view.data;
    as.size = c.view.size;
    NTEST_ASSERT_EQ_INT(nph_adpcm_validate(&as), NPH_RESULT_OK);
    f32 *y = (f32 *)NPH_ALLOC(nph_allocator_system(), 16000U * sizeof(f32), 16U);
    NTEST_ASSERT_NOT_NULL(y);
    f32 *planes[1] = {y};
    nph_adpcm_decode(&as, 0U, 16000U, planes);
    f64 signal = 0.0;
    f64 noise = 0.0;
    for (u64 i = 0U; i < 16000U; ++i) {
        const f64 want = nph_wav_sample_float(&info, i, 0U);
        signal += want * want;
        noise += ((f64)y[i] - want) * ((f64)y[i] - want);
    }
    const f64 snr = 10.0 * log10(signal / noise);
    NTEST_LOG("ADPCM: %llu bytes for %llu of 24-bit PCM, SNR %.1f dB",
              (unsigned long long)c.view.size, (unsigned long long)info.data_size, snr);
    NTEST_EXPECT_TRUE(snr > 30.0);
    NPH_FREE(nph_allocator_system(), y, 16000U * sizeof(f32), 16U);
    test_asset_free(&c.asset);
    ts_wav_free(wav, size);
}

NTEST(cook_sound, the_file_loop_or_the_whole_sound)
{
    const CookSoundSettings s = ts_settings(COOK_SOUND_CODEC_AUTO, COOK_SOUND_STREAM_AUTO);
    u64 size = 0U;
    u8 *wav = ts_wav(NPH_WAV_PCM_S16, 1U, 8000U, 6000U, ts_music, 1000U, 5000U, &size);
    TsCooked c;
    ts_cook(wav, size, &s, &c);
    NTEST_EXPECT_EQ_UINT(c.view.header->loop_start, 1000U);
    NTEST_EXPECT_EQ_UINT(c.view.header->loop_end, 5000U);
    test_asset_free(&c.asset);
    ts_wav_free(wav, size);
    wav = ts_wav(NPH_WAV_PCM_S16, 1U, 8000U, 6000U, ts_music, 0U, 0U, &size);
    ts_cook(wav, size, &s, &c);
    NTEST_EXPECT_EQ_UINT(c.view.header->loop_start, 0U);
    NTEST_EXPECT_EQ_UINT(c.view.header->loop_end, 6000U);
    test_asset_free(&c.asset);
    ts_wav_free(wav, size);
}

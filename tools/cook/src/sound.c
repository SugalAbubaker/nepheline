// SPDX-License-Identifier: Apache-2.0
// Sound cooking (sound.h): the settings, the plan a source gets, the
// samples' exact conversions, and each codec's blobs.
#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>

#include <cook/cook.h>
#include <cook/sound.h>
#include <float.h>
#include <math.h>
#include <string.h>

void cook_sound_settings_default(CookSoundSettings *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->codec = COOK_SOUND_CODEC_AUTO;
    out->stream = COOK_SOUND_STREAM_AUTO;
    out->stream_seconds = COOK_SOUND_STREAM_SECONDS;
}

// ---------------------------------------------------------------------------
// Per-source settings
// ---------------------------------------------------------------------------

// The index of `s` among `names`, or -1.
static i32 name_index(NphStr s, const char *const *names, u32 count)
{
    for (u32 i = 0U; i < count; ++i) {
        if (nph_str_equals(s, nph_str_from_cstr(names[i]))) {
            return (i32)i;
        }
    }
    return -1;
}

static NphResult apply_setting(const NphJsonValue *v, CookSoundSettings *s)
{
    static const char *const codecs[] = {"auto", "pcm", "lossless", "adpcm"};
    static const char *const streams[] = {"auto", "always", "never"};
    NphStr text;
    if (nph_str_equals(v->key, NPH_STR("codec"))) {
        NPH_TRY(nph_json_as_str(v, &text) == NPH_RESULT_OK ? NPH_RESULT_OK
                                                           : NPH_RESULT_INVALID_ARGUMENT);
        const i32 i = name_index(text, codecs, NPH_ARRAY_COUNT(codecs));
        if (i < 0) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        s->codec = (CookSoundCodec)i;
        return NPH_RESULT_OK;
    }
    if (nph_str_equals(v->key, NPH_STR("stream"))) {
        NPH_TRY(nph_json_as_str(v, &text) == NPH_RESULT_OK ? NPH_RESULT_OK
                                                           : NPH_RESULT_INVALID_ARGUMENT);
        const i32 i = name_index(text, streams, NPH_ARRAY_COUNT(streams));
        if (i < 0) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        s->stream = (CookSoundStream)i;
        return NPH_RESULT_OK;
    }
    if (nph_str_equals(v->key, NPH_STR("stream_seconds"))) {
        f64 seconds = 0.0;
        if (nph_json_as_f64(v, &seconds) != NPH_RESULT_OK || !(seconds > 0.0) || seconds > 1.0e9) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        s->stream_seconds = (f32)seconds;
        return NPH_RESULT_OK;
    }
    if (nph_str_equals(v->key, NPH_STR("exhaustive"))) {
        bool b = false;
        NPH_TRY(nph_json_as_bool(v, &b) == NPH_RESULT_OK ? NPH_RESULT_OK
                                                         : NPH_RESULT_INVALID_ARGUMENT);
        s->exhaustive = b;
        return NPH_RESULT_OK;
    }
    return NPH_RESULT_INVALID_ARGUMENT;
}

NphResult cook_sound_settings_parse(NphAllocator *alloc, const void *json, u64 size,
                                    CookSoundSettings *inout)
{
    NPH_ASSERT(alloc != NULL && inout != NULL && (json != NULL || size == 0U));
    NphJsonDoc doc;
    NphResult r = nph_json_parse(nph_str((const char *)json, size), alloc, &doc);
    if (r != NPH_RESULT_OK) {
        return r == NPH_RESULT_OUT_OF_MEMORY ? r : NPH_RESULT_PARSE_ERROR;
    }
    CookSoundSettings s = *inout;
    r = doc.root != NULL && doc.root->type == NPH_JSON_OBJECT ? NPH_RESULT_OK
                                                              : NPH_RESULT_PARSE_ERROR;
    if (r == NPH_RESULT_OK) {
        NPH_JSON_FOREACH(v, doc.root)
        {
            r = apply_setting(v, &s);
            if (r != NPH_RESULT_OK) {
                break;
            }
        }
    }
    nph_json_doc_free(&doc);
    if (r == NPH_RESULT_OK) {
        *inout = s;
    }
    return r;
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

static bool is_float(const NphWavInfo *wav)
{
    return wav->encoding == NPH_WAV_FLOAT32 || wav->encoding == NPH_WAV_FLOAT64;
}

static u64 sample_count(const NphWavInfo *wav)
{
    return wav->frames * wav->channels;
}

// The sample `i` of the interleaved run (frame i / channels).
static f64 sample_float(const NphWavInfo *wav, u64 i)
{
    return nph_wav_sample_float(wav, i / wav->channels, (u32)(i % wav->channels));
}

static i32 sample_int(const NphWavInfo *wav, u64 i)
{
    return nph_wav_sample_int(wav, i / wav->channels, (u32)(i % wav->channels));
}

// The depth at which the lossless codec holds every sample exactly (4 ..
// 24 bits), or 0 when it cannot: 32-bit integers whose low byte is ever
// set, floats that are not all 24-bit integers over 2^23.
static u32 lossless_bits(const NphWavInfo *wav)
{
    const u64 n = sample_count(wav);
    switch (wav->encoding) {
    case NPH_WAV_PCM_U8:
        return 8U;
    case NPH_WAV_PCM_S16:
        return 16U;
    case NPH_WAV_PCM_S24:
        return 24U;
    case NPH_WAV_PCM_S32:
        for (u64 i = 0U; i < n; ++i) {
            if (((u32)sample_int(wav, i) & 0xFFU) != 0U) {
                return 0U;
            }
        }
        return 24U;
    case NPH_WAV_FLOAT32:
    case NPH_WAV_FLOAT64:
        for (u64 i = 0U; i < n; ++i) {
            const f64 y = sample_float(wav, i) * 8388608.0;
            if (y != floor(y) || y < -8388608.0 || y > 8388607.0) {
                return 0U;
            }
        }
        return 24U;
    }
    return 0U;
}

// PCM's format for the source: its own, but 16-bit for 8-bit samples and
// 32-bit floats for 64-bit ones.
static NphSoundFormat pcm_format(const NphWavInfo *wav)
{
    switch (wav->encoding) {
    case NPH_WAV_PCM_U8:
    case NPH_WAV_PCM_S16:
        return NPH_SOUND_FORMAT_S16;
    case NPH_WAV_PCM_S24:
        return NPH_SOUND_FORMAT_S24;
    case NPH_WAV_PCM_S32:
        return NPH_SOUND_FORMAT_S32;
    case NPH_WAV_FLOAT32:
    case NPH_WAV_FLOAT64:
        return NPH_SOUND_FORMAT_F32;
    }
    return NPH_SOUND_FORMAT_NONE;
}

NphResult cook_sound_plan(const NphWavInfo *wav, const CookSoundSettings *settings,
                          CookSoundPlan *out)
{
    NPH_ASSERT(wav != NULL && settings != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (wav->frames == 0U || wav->channels == 0U || wav->channels > NPH_SOUND_MAX_CHANNELS ||
        wav->sample_rate < NPH_SOUND_MIN_RATE || wav->sample_rate > NPH_SOUND_MAX_RATE ||
        wav->frames > NPH_SOUND_MAX_STREAM_FRAMES) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Not-a-number and infinite samples are refused: in a mix they would
    // reach every bus the voice feeds. (A NaN fails both comparisons.)
    if (is_float(wav)) {
        for (u64 i = 0U; i < sample_count(wav); ++i) {
            const f64 x = sample_float(wav, i);
            if (!(x >= -DBL_MAX && x <= DBL_MAX)) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
    }
    const f64 seconds = (f64)wav->frames / (f64)wav->sample_rate;
    const f32 threshold =
        settings->stream_seconds > 0.0F ? settings->stream_seconds : COOK_SOUND_STREAM_SECONDS;
    bool stream = settings->stream == COOK_SOUND_STREAM_ALWAYS ||
                  (settings->stream == COOK_SOUND_STREAM_AUTO && seconds >= (f64)threshold);
    if (!stream && wav->frames > NPH_SOUND_MAX_FRAMES) {
        // Too long for memory: AUTO streams it whatever the threshold.
        if (settings->stream == COOK_SOUND_STREAM_NEVER) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        stream = true;
    }
    out->stream = stream;
    if (settings->codec == COOK_SOUND_CODEC_ADPCM) {
        if (stream) {
            return NPH_RESULT_INVALID_ARGUMENT; // ADPCM plays from memory only
        }
        out->codec = NPH_SOUND_CODEC_ADPCM;
        out->bits = 16U;
        return NPH_RESULT_OK;
    }
    const bool lossless = settings->codec == COOK_SOUND_CODEC_LOSSLESS ||
                          (settings->codec == COOK_SOUND_CODEC_AUTO && stream);
    const u32 bits = lossless ? lossless_bits(wav) : 0U;
    if (bits != 0U) {
        out->codec = NPH_SOUND_CODEC_LOSSLESS;
        out->bits = bits;
        return NPH_RESULT_OK;
    }
    out->codec = NPH_SOUND_CODEC_PCM;
    out->format = pcm_format(wav);
    out->bits = nph_sound_format_bytes((u32)out->format) * 8U;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The samples, as each codec takes them
// ---------------------------------------------------------------------------

// PCM in the plan's format: the file's bytes where they are that already,
// else 8-bit samples moved to 16 bits (exactly) or 64-bit floats rounded to
// 32. `*out_owned` is the converted copy (`*out_size` bytes, 16-aligned) to
// free, or NULL.
static NphResult pcm_data(NphAllocator *alloc, const NphWavInfo *wav, const void **out_data,
                          u64 *out_size, void **out_owned)
{
    *out_owned = NULL;
    const u64 n = sample_count(wav);
    if (wav->encoding == NPH_WAV_PCM_U8) {
        i16 *x = (i16 *)NPH_ALLOC(alloc, n * sizeof(i16), 16U);
        if (x == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        for (u64 i = 0U; i < n; ++i) {
            x[i] = (i16)(sample_int(wav, i) * 256);
        }
        *out_data = x;
        *out_size = n * sizeof(i16);
        *out_owned = x;
        return NPH_RESULT_OK;
    }
    if (wav->encoding == NPH_WAV_FLOAT64) {
        f32 *x = (f32 *)NPH_ALLOC(alloc, n * sizeof(f32), 16U);
        if (x == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        for (u64 i = 0U; i < n; ++i) {
            x[i] = (f32)sample_float(wav, i);
        }
        *out_data = x;
        *out_size = n * sizeof(f32);
        *out_owned = x;
        return NPH_RESULT_OK;
    }
    *out_data = wav->data;
    *out_size = wav->data_size;
    return NPH_RESULT_OK;
}

// The samples as `bits`-bit integers for the lossless codec (exact: the
// plan checked that they are).
static i32 *lossless_samples(NphAllocator *alloc, const NphWavInfo *wav)
{
    const u64 n = sample_count(wav);
    i32 *x = NPH_ALLOC_ARRAY(alloc, i32, n);
    if (x == NULL) {
        return NULL;
    }
    for (u64 i = 0U; i < n; ++i) {
        if (is_float(wav)) {
            x[i] = (i32)(sample_float(wav, i) * 8388608.0);
        } else if (wav->encoding == NPH_WAV_PCM_S32) {
            x[i] = sample_int(wav, i) / 256; // the low byte is zero: exact
        } else {
            x[i] = sample_int(wav, i);
        }
    }
    return x;
}

// The samples rounded to 16 bits for ADPCM (to nearest, clamped).
static i16 *adpcm_samples(NphAllocator *alloc, const NphWavInfo *wav)
{
    const u64 n = sample_count(wav);
    i16 *x = NPH_ALLOC_ARRAY(alloc, i16, n);
    if (x == NULL) {
        return NULL;
    }
    for (u64 i = 0U; i < n; ++i) {
        const f64 y = floor(sample_float(wav, i) * 32768.0 + 0.5);
        x[i] = (i16)(y > 32767.0 ? 32767.0 : (y < -32768.0 ? -32768.0 : y));
    }
    return x;
}

// ---------------------------------------------------------------------------
// The asset
// ---------------------------------------------------------------------------

NphResult cook_sound(NphAllocator *alloc, const NphWavInfo *wav, const CookSoundSettings *settings,
                     NphAssetWriter *writer)
{
    NPH_ASSERT(alloc != NULL && wav != NULL && settings != NULL && writer != NULL);
    CookSoundPlan plan;
    NPH_TRY(cook_sound_plan(wav, settings, &plan));
    NphSoundHeader h;
    memset(&h, 0, sizeof h);
    h.codec = (u32)plan.codec;
    h.format = (u32)plan.format;
    h.channels = wav->channels;
    h.sample_rate = wav->sample_rate;
    h.frames = wav->frames;
    // The file's loop when it names one inside the sound, else all of it.
    const bool loop =
        wav->has_loop && wav->loop_start < wav->loop_end && wav->loop_end <= wav->frames;
    h.loop_start = loop ? wav->loop_start : 0U;
    h.loop_end = loop ? wav->loop_end : wav->frames;
    h.bits = plan.bits;
    h.flags = plan.stream ? (u32)NPH_SOUND_FLAG_STREAM : 0U;
    NphResult r = NPH_RESULT_OK;
    if (plan.codec == NPH_SOUND_CODEC_PCM) {
        const void *data = NULL;
        u64 size = 0U;
        void *owned = NULL;
        r = pcm_data(alloc, wav, &data, &size, &owned);
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_HEADER, 0U, &h, sizeof h,
                                          NPH_ASSET_CODEC_NONE, 0U);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_DATA, 0U, data, size,
                                          NPH_ASSET_CODEC_NONE, 0U);
        }
        if (owned != NULL) {
            NPH_FREE(alloc, owned, size, 16U);
        }
        return r;
    }
    if (plan.codec == NPH_SOUND_CODEC_ADPCM) {
        i16 *x = adpcm_samples(alloc, wav);
        if (x == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        NphAdpcmEncoded enc;
        r = nph_adpcm_encode(alloc, x, wav->channels, wav->frames, &enc);
        NPH_FREE_ARRAY(alloc, i16, x, sample_count(wav));
        NPH_TRY(r);
        r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_HEADER, 0U, &h, sizeof h,
                                      NPH_ASSET_CODEC_NONE, 0U);
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_DATA, 0U, enc.stream.data,
                                          enc.stream.size, NPH_ASSET_CODEC_NONE, 0U);
        }
        nph_adpcm_encoded_free(&enc);
        return r;
    }
    i32 *x = lossless_samples(alloc, wav);
    if (x == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphLosslessParams params;
    memset(&params, 0, sizeof params);
    params.exhaustive = settings->exhaustive;
    NphLosslessEncoded enc;
    r = nph_lossless_encode(alloc, x, wav->channels, plan.bits, wav->frames, &params, &enc);
    NPH_FREE_ARRAY(alloc, i32, x, sample_count(wav));
    NPH_TRY(r);
    h.block_frames = enc.stream.block_frames;
    h.block_count = enc.stream.block_count;
    r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_HEADER, 0U, &h, sizeof h,
                                  NPH_ASSET_CODEC_NONE, 0U);
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_DATA, 0U, enc.stream.data,
                                      enc.stream.size, NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_SOUND_TAG_BLOCKS, 0U, enc.stream.block_offsets,
                                      (u64)enc.stream.block_count * sizeof(u32),
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    nph_lossless_encoded_free(&enc);
    return r;
}

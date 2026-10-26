// SPDX-License-Identifier: Apache-2.0
// The WAV reader and writer (wav.h).

#include <nepheline/audio/wav.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

#define WAV_FORMAT_PCM        0x0001U
#define WAV_FORMAT_IEEE_FLOAT 0x0003U
#define WAV_FORMAT_EXTENSIBLE 0xFFFEU

// The tail every WAVE_FORMAT_EXTENSIBLE subformat GUID shares
// (KSDATAFORMAT_SUBTYPE_*: {0000xxxx-0000-0010-8000-00aa00389b71}).
static const u8 g_wav_guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                       0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

static u16 wav_u16(const u8 *p)
{
    return (u16)((u16)p[0] | (u16)((u16)p[1] << 8U));
}

static u32 wav_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8U) | ((u32)p[2] << 16U) | ((u32)p[3] << 24U);
}

static bool wav_tag(const u8 *p, const char *tag)
{
    return memcmp(p, tag, 4U) == 0;
}

static u32 wav_encoding_bytes(NphWavEncoding e)
{
    switch (e) {
    case NPH_WAV_PCM_U8:
        return 1U;
    case NPH_WAV_PCM_S16:
        return 2U;
    case NPH_WAV_PCM_S24:
        return 3U;
    case NPH_WAV_PCM_S32:
    case NPH_WAV_FLOAT32:
        return 4U;
    case NPH_WAV_FLOAT64:
        return 8U;
    }
    return 0U;
}

// The encoding of a format tag and container size, or false.
static bool wav_encoding_of(u32 tag, u32 bits, NphWavEncoding *out)
{
    if (tag == WAV_FORMAT_PCM) {
        switch (bits) {
        case 8U:
            *out = NPH_WAV_PCM_U8;
            return true;
        case 16U:
            *out = NPH_WAV_PCM_S16;
            return true;
        case 24U:
            *out = NPH_WAV_PCM_S24;
            return true;
        case 32U:
            *out = NPH_WAV_PCM_S32;
            return true;
        default:
            return false;
        }
    }
    if (tag == WAV_FORMAT_IEEE_FLOAT) {
        if (bits == 32U) {
            *out = NPH_WAV_FLOAT32;
            return true;
        }
        if (bits == 64U) {
            *out = NPH_WAV_FLOAT64;
            return true;
        }
    }
    return false;
}

static NphResult wav_parse_fmt(const u8 *p, u64 size, NphWavInfo *out)
{
    if (size < 16U) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u32 tag = wav_u16(p);
    const u32 channels = wav_u16(p + 2);
    const u32 rate = wav_u32(p + 4);
    const u32 block_align = wav_u16(p + 12);
    const u32 bits = wav_u16(p + 14);
    if (tag == WAV_FORMAT_EXTENSIBLE) {
        // cbSize (2), valid bits (2), channel mask (4), subformat GUID (16).
        if (size < 40U || wav_u16(p + 16) < 22U) {
            return NPH_RESULT_PARSE_ERROR;
        }
        const u8 *guid = p + 24;
        if (memcmp(guid + 2, g_wav_guid_tail, sizeof g_wav_guid_tail) != 0) {
            return NPH_RESULT_UNSUPPORTED;
        }
        tag = wav_u16(guid);
        const u32 valid = wav_u16(p + 18);
        if (valid > bits) {
            return NPH_RESULT_PARSE_ERROR;
        }
    }
    NphWavEncoding encoding;
    if (!wav_encoding_of(tag, bits, &encoding)) {
        return NPH_RESULT_UNSUPPORTED;
    }
    if (channels == 0U || channels > NPH_AUDIO_MAX_CHANNELS) {
        return channels == 0U ? NPH_RESULT_PARSE_ERROR : NPH_RESULT_UNSUPPORTED;
    }
    if (rate < NPH_AUDIO_MIN_RATE || rate > NPH_AUDIO_MAX_RATE) {
        return NPH_RESULT_UNSUPPORTED;
    }
    const u32 frame_bytes = channels * wav_encoding_bytes(encoding);
    if (block_align != frame_bytes) {
        return NPH_RESULT_PARSE_ERROR;
    }
    out->encoding = encoding;
    out->channels = channels;
    out->sample_rate = rate;
    out->bytes_per_frame = frame_bytes;
    return NPH_RESULT_OK;
}

static void wav_parse_smpl(const u8 *p, u64 size, NphWavInfo *out)
{
    // 36 bytes of header, then 24 per loop; only the first loop is kept.
    if (size < 36U + 24U || wav_u32(p + 28) == 0U) {
        return;
    }
    const u8 *loop = p + 36;
    const u64 start = wav_u32(loop + 8);
    const u64 end = (u64)wav_u32(loop + 12) + 1U; // the file's end is inclusive
    out->has_loop = 1;
    out->loop_start = start;
    out->loop_end = end;
}

NphResult nph_wav_parse(const void *data, u64 size, NphWavInfo *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    const u8 *p = (const u8 *)data;
    if (data == NULL || size < 12U || !wav_tag(p, "RIFF") || !wav_tag(p + 8, "WAVE")) {
        return NPH_RESULT_PARSE_ERROR;
    }
    // The RIFF size bounds the walk, but writers get it wrong (0, or the
    // size before a later append): the image's own end is the limit.
    u64 at = 12U;
    bool have_fmt = false;
    bool have_data = false;
    while (at + 8U <= size) {
        const u8 *chunk = p + at;
        const u64 chunk_size = wav_u32(chunk + 4);
        const u64 body = at + 8U;
        const u64 available = size - body;
        if (wav_tag(chunk, "fmt ")) {
            // One format, one data chunk: a second format would describe
            // the data already measured in another (its frames and size
            // from the first, its reads at the second's width, past it).
            if (chunk_size > available || have_fmt) {
                return NPH_RESULT_PARSE_ERROR;
            }
            NPH_TRY(wav_parse_fmt(p + body, chunk_size, out));
            have_fmt = true;
        } else if (wav_tag(chunk, "data")) {
            if (!have_fmt || have_data) {
                return NPH_RESULT_PARSE_ERROR; // the format first, the data once
            }
            const u64 bytes = chunk_size > available ? available : chunk_size;
            out->frames = bytes / out->bytes_per_frame;
            out->data = p + body;
            out->data_size = out->frames * out->bytes_per_frame;
            have_data = true;
            if (chunk_size > available) {
                break; // truncated: nothing can follow
            }
        } else if (wav_tag(chunk, "smpl")) {
            if (chunk_size <= available) {
                wav_parse_smpl(p + body, chunk_size, out);
            }
        }
        if (chunk_size > available) {
            break; // a trailing chunk cut short; what came before stands
        }
        at = body + chunk_size + (chunk_size & 1U); // chunks are padded to even sizes
    }
    if (!have_fmt || !have_data) {
        return NPH_RESULT_PARSE_ERROR;
    }
    // A loop must lie inside the data, and be non-empty.
    if (out->has_loop && !(out->loop_start < out->loop_end && out->loop_end <= out->frames)) {
        out->has_loop = 0;
        out->loop_start = 0U;
        out->loop_end = 0U;
    }
    return NPH_RESULT_OK;
}

i32 nph_wav_sample_int(const NphWavInfo *info, u64 frame, u32 channel)
{
    NPH_ASSERT(info != NULL && frame < info->frames && channel < info->channels);
    const u32 bytes = wav_encoding_bytes(info->encoding);
    const u8 *s = (const u8 *)info->data + frame * info->bytes_per_frame + (u64)channel * bytes;
    switch (info->encoding) {
    case NPH_WAV_PCM_U8:
        return (i32)s[0] - 128;
    case NPH_WAV_PCM_S16:
        return (i32)(i16)wav_u16(s);
    case NPH_WAV_PCM_S24: {
        const u32 raw = (u32)s[0] | ((u32)s[1] << 8U) | ((u32)s[2] << 16U);
        return (i32)(raw ^ 0x800000U) - 0x800000;
    }
    case NPH_WAV_PCM_S32: {
        i32 v;
        memcpy(&v, s, sizeof v);
        return v;
    }
    case NPH_WAV_FLOAT32:
    case NPH_WAV_FLOAT64:
        break;
    }
    return 0;
}

f64 nph_wav_sample_float(const NphWavInfo *info, u64 frame, u32 channel)
{
    NPH_ASSERT(info != NULL && frame < info->frames && channel < info->channels);
    const u32 bytes = wav_encoding_bytes(info->encoding);
    const u8 *s = (const u8 *)info->data + frame * info->bytes_per_frame + (u64)channel * bytes;
    switch (info->encoding) {
    case NPH_WAV_PCM_U8:
        return (f64)nph_wav_sample_int(info, frame, channel) / 128.0;
    case NPH_WAV_PCM_S16:
        return (f64)nph_wav_sample_int(info, frame, channel) / 32768.0;
    case NPH_WAV_PCM_S24:
        return (f64)nph_wav_sample_int(info, frame, channel) / 8388608.0;
    case NPH_WAV_PCM_S32:
        return (f64)nph_wav_sample_int(info, frame, channel) / 2147483648.0;
    case NPH_WAV_FLOAT32: {
        f32 v;
        memcpy(&v, s, sizeof v);
        return (f64)v;
    }
    case NPH_WAV_FLOAT64: {
        f64 v;
        memcpy(&v, s, sizeof v);
        return v;
    }
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static bool wav_writable(NphWavEncoding e)
{
    return e == NPH_WAV_PCM_S16 || e == NPH_WAV_PCM_S24 || e == NPH_WAV_FLOAT32;
}

// RIFF + fmt (16) + data headers, plus a 'fact' chunk for floats, which the
// specification asks of every non-PCM file.
static u64 wav_header_bytes(NphWavEncoding e)
{
    return 12U + 8U + 16U + (e == NPH_WAV_FLOAT32 ? 12U : 0U) + 8U;
}

u64 nph_wav_size(NphWavEncoding encoding, u32 channels, u64 frames)
{
    if (!wav_writable(encoding) || channels == 0U || channels > NPH_AUDIO_MAX_CHANNELS) {
        return 0U;
    }
    const u64 data = frames * channels * wav_encoding_bytes(encoding);
    return wav_header_bytes(encoding) + data + (data & 1U);
}

static void wav_put_u16(u8 **p, u32 v)
{
    (*p)[0] = (u8)(v & 0xFFU);
    (*p)[1] = (u8)((v >> 8U) & 0xFFU);
    *p += 2;
}

static void wav_put_u32(u8 **p, u32 v)
{
    for (u32 i = 0U; i < 4U; ++i) {
        (*p)[i] = (u8)((v >> (8U * i)) & 0xFFU);
    }
    *p += 4;
}

static void wav_put_tag(u8 **p, const char *tag)
{
    memcpy(*p, tag, 4U);
    *p += 4;
}

static f32 wav_clamp(f32 x)
{
    if (x != x) {
        return 0.0F;
    }
    return x < -1.0F ? -1.0F : (x > 1.0F ? 1.0F : x);
}

NphResult nph_wav_write(NphWavEncoding encoding, u32 channels, u32 sample_rate,
                        const f32 *const *planar, u64 frames, void *out)
{
    const u64 total = nph_wav_size(encoding, channels, frames);
    if (total == 0U || total > (u64)NPH_U32_MAX || sample_rate < NPH_AUDIO_MIN_RATE ||
        sample_rate > NPH_AUDIO_MAX_RATE) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_ASSERT(out != NULL && (planar != NULL || frames == 0U));
    const u32 bytes = wav_encoding_bytes(encoding);
    const u32 frame_bytes = channels * bytes;
    const u64 data = frames * frame_bytes;
    u8 *p = (u8 *)out;
    wav_put_tag(&p, "RIFF");
    wav_put_u32(&p, (u32)(total - 8U));
    wav_put_tag(&p, "WAVE");
    wav_put_tag(&p, "fmt ");
    wav_put_u32(&p, 16U);
    wav_put_u16(&p, encoding == NPH_WAV_FLOAT32 ? WAV_FORMAT_IEEE_FLOAT : WAV_FORMAT_PCM);
    wav_put_u16(&p, channels);
    wav_put_u32(&p, sample_rate);
    wav_put_u32(&p, sample_rate * frame_bytes);
    wav_put_u16(&p, frame_bytes);
    wav_put_u16(&p, bytes * 8U);
    if (encoding == NPH_WAV_FLOAT32) {
        wav_put_tag(&p, "fact");
        wav_put_u32(&p, 4U);
        wav_put_u32(&p, (u32)frames);
    }
    wav_put_tag(&p, "data");
    wav_put_u32(&p, (u32)data);
    for (u64 i = 0U; i < frames; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            const f32 x = wav_clamp(planar[c][i]);
            switch (encoding) {
            case NPH_WAV_PCM_S16: {
                const i32 v = (i32)lrintf(x * 32767.0F);
                wav_put_u16(&p, (u32)v & 0xFFFFU);
                break;
            }
            case NPH_WAV_PCM_S24: {
                const i32 v = (i32)lrint((f64)x * 8388607.0);
                const u32 u = (u32)v;
                p[0] = (u8)(u & 0xFFU);
                p[1] = (u8)((u >> 8U) & 0xFFU);
                p[2] = (u8)((u >> 16U) & 0xFFU);
                p += 3;
                break;
            }
            case NPH_WAV_FLOAT32:
                memcpy(p, &x, sizeof x);
                p += 4;
                break;
            case NPH_WAV_PCM_U8:
            case NPH_WAV_PCM_S32:
            case NPH_WAV_FLOAT64:
                NPH_UNREACHABLE();
                break;
            }
        }
    }
    if ((data & 1U) != 0U) {
        *p++ = 0U; // pad to an even chunk size
    }
    NPH_ASSERT((u64)(p - (u8 *)out) == total);
    return NPH_RESULT_OK;
}

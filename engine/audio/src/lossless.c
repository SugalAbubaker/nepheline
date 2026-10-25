// SPDX-License-Identifier: Apache-2.0
// The lossless codec (lossless.h): the encoder's analysis and coding, the
// decoder that runs a mixer block at a time, and the block checks.

#include <nepheline/audio/lossless.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>

#include <math.h>
#include <string.h>

#define LL_KIND_CONSTANT 0U
#define LL_KIND_VERBATIM 1U
#define LL_KIND_FIXED    2U  // + order 0 .. 4
#define LL_KIND_LPC      31U // + order 1 .. 32
#define LL_RICE_ESCAPE   31U
#define LL_MAX_RICE      30U
#define LL_MAX_PARTITION 8U
#define LL_PI            3.14159265358979323846

// The fixed predictors as integer coefficients (shift 0), newest sample
// first: the decoder runs them as linear prediction.
static const i32 g_ll_fixed[5][4] = {
    {0, 0, 0, 0}, {1, 0, 0, 0}, {2, -1, 0, 0}, {3, -3, 1, 0}, {4, -6, 4, -1},
};

static u32 ll_get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8U) | ((u32)p[2] << 16U) | ((u32)p[3] << 24U);
}

static u16 ll_get_u16(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8U));
}

static void ll_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8U) & 0xFFU);
    p[2] = (u8)((v >> 16U) & 0xFFU);
    p[3] = (u8)((v >> 24U) & 0xFFU);
}

static u32 ll_zigzag(i32 v)
{
    return ((u32)v << 1U) ^ (u32)(v >> 31);
}

static i32 ll_unzigzag(u32 u)
{
    return (i32)(u >> 1U) ^ -(i32)(u & 1U);
}

// ---------------------------------------------------------------------------
// Block checks
// ---------------------------------------------------------------------------

NphResult nph_lossless_check_block(const u8 *block, u64 size, u32 channels, u32 expected_frames)
{
    const u64 header = NPH_LOSSLESS_BLOCK_HEADER_SIZE + 4ULL * channels;
    if (block == NULL || channels == 0U || channels > NPH_AUDIO_MAX_CHANNELS || size < header) {
        return NPH_RESULT_CORRUPT;
    }
    if (ll_get_u32(block + 4) != size || ll_get_u16(block + 8) != expected_frames ||
        block[11] != channels || block[10] >= NPH_LOSSLESS_STEREO_COUNT ||
        (channels != 2U && block[10] != 0U)) {
        return NPH_RESULT_CORRUPT;
    }
    u64 previous = header;
    for (u32 c = 0U; c < channels; ++c) {
        const u64 offset = ll_get_u32(block + 12U + (u64)c * 4U);
        // Each subframe holds at least its two-byte head.
        if (offset < previous || offset + 2U > size) {
            return NPH_RESULT_CORRUPT;
        }
        previous = offset + 2U;
    }
    if (nph_crc32c(block + 4, size - 4U, 0U) != ll_get_u32(block)) {
        return NPH_RESULT_CORRUPT;
    }
    return NPH_RESULT_OK;
}

static bool ll_params_valid(u32 channels, u32 bits, u32 block_frames)
{
    return channels >= 1U && channels <= NPH_AUDIO_MAX_CHANNELS && bits >= NPH_LOSSLESS_MIN_BITS &&
           bits <= NPH_LOSSLESS_MAX_BITS && block_frames >= 1U &&
           block_frames <= NPH_LOSSLESS_MAX_BLOCK;
}

NphResult nph_lossless_validate(const NphLosslessStream *stream)
{
    NPH_ASSERT(stream != NULL);
    if (!ll_params_valid(stream->channels, stream->bits, stream->block_frames)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 blocks = nph_lossless_block_count(stream->frames, stream->block_frames);
    if (blocks != stream->block_count || (u64)blocks * stream->block_frames < stream->frames) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (blocks > 0U && (stream->data == NULL || stream->block_offsets == NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 b = 0U; b < blocks; ++b) {
        const u64 start = stream->block_offsets[b];
        const u64 end = b + 1U < blocks ? stream->block_offsets[b + 1U] : stream->size;
        if (start >= end || end > stream->size || (b == 0U && start != 0U)) {
            return NPH_RESULT_CORRUPT;
        }
        const u64 first = (u64)b * stream->block_frames;
        const u32 frames = (u32)nph_min_u64(stream->block_frames, stream->frames - first);
        NPH_TRY(
            nph_lossless_check_block(stream->data + start, end - start, stream->channels, frames));
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Bit reading: least significant first, refilled a word at a time
// ---------------------------------------------------------------------------

static void ll_refill(NphLosslessChannelState *s)
{
    if (s->end - s->cursor >= 8) {
        u64 word;
        memcpy(&word, s->cursor, sizeof word);
        s->bits |= word << s->bit_count;
        const u32 take = (63U - s->bit_count) >> 3U;
        s->cursor += take;
        s->bit_count += take * 8U;
        // The load brought in part of a byte beyond the ones counted: clear
        // it, so the buffer holds exactly bit_count bits (the unary reader
        // searches the whole word).
        s->bits &= (1ULL << s->bit_count) - 1ULL;
        return;
    }
    while (s->bit_count <= 56U && s->cursor < s->end) {
        s->bits |= (u64)*s->cursor++ << s->bit_count;
        s->bit_count += 8U;
    }
}

// `n` bits (0 .. 32); false past the subframe's end.
static bool ll_read(NphLosslessChannelState *s, u32 n, u32 *out)
{
    NPH_ASSERT(n <= 32U);
    n &= 63U; // (never more than 32: keeps every shift below in range)
    if (s->bit_count < n) {
        ll_refill(s);
        if (s->bit_count < n) {
            return false;
        }
    }
    *out = (u32)(s->bits & ((1ULL << n) - 1ULL));
    s->bits >>= n;
    s->bit_count -= n;
    return true;
}

static bool ll_read_signed(NphLosslessChannelState *s, u32 n, i32 *out)
{
    u32 v = 0U;
    if (!ll_read(s, n, &v)) {
        return false;
    }
    if (n == 0U) {
        *out = 0;
        return true;
    }
    // Sign-extend from bit n - 1 (n <= 32), in 64 bits.
    const u64 sign = 1ULL << ((n - 1U) & 31U);
    *out = (i32)(((u64)v ^ sign) - sign);
    return true;
}

// The number of zero bits before the next one bit, which is consumed.
static bool ll_read_unary(NphLosslessChannelState *s, u32 *out)
{
    u32 q = 0U;
    for (;;) {
        if (s->bits != 0U) {
            const u32 tz = nph_ctz_u64(s->bits);
            q += tz;
            s->bits >>= tz;
            s->bits >>= 1U;
            s->bit_count -= tz + 1U;
            *out = q;
            return true;
        }
        q += s->bit_count;
        s->bit_count = 0U;
        if (q > (1U << 30U)) {
            return false; // no valid code is this long
        }
        ll_refill(s);
        if (s->bit_count == 0U) {
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

static NphResult ll_begin_channel(NphLosslessChannelState *s, const u8 *sub, const u8 *end,
                                  u32 sample_bits, u32 frames)
{
    memset(s, 0, sizeof *s);
    s->kind = sub[0];
    s->wasted = sub[1];
    if (s->wasted >= sample_bits) {
        return NPH_RESULT_CORRUPT;
    }
    s->sample_bits = sample_bits - s->wasted;
    s->cursor = sub + 2;
    s->end = end;
    if (s->kind == LL_KIND_CONSTANT) {
        if (!ll_read_signed(s, s->sample_bits, &s->constant)) {
            return NPH_RESULT_CORRUPT;
        }
        return NPH_RESULT_OK;
    }
    if (s->kind == LL_KIND_VERBATIM) {
        return NPH_RESULT_OK;
    }
    u32 order = 0U;
    if (s->kind >= LL_KIND_FIXED && s->kind <= LL_KIND_FIXED + 4U) {
        order = s->kind - LL_KIND_FIXED;
        for (u32 j = 0U; j < order; ++j) {
            // Stored oldest first for a forward dot product over the window.
            s->coeffs[j] = g_ll_fixed[order][order - 1U - j];
        }
        s->shift = 0U;
    } else if (s->kind > LL_KIND_LPC && s->kind <= LL_KIND_LPC + NPH_LOSSLESS_MAX_ORDER) {
        order = s->kind - LL_KIND_LPC;
    } else {
        return NPH_RESULT_CORRUPT;
    }
    if (order > frames) {
        return NPH_RESULT_CORRUPT;
    }
    s->order = order;
    // Warm-up samples go straight into the history.
    for (u32 j = 0U; j < order; ++j) {
        i32 v = 0;
        if (!ll_read_signed(s, s->sample_bits, &v)) {
            return NPH_RESULT_CORRUPT;
        }
        s->history[s->history_at] = v;
        s->history[s->history_at + NPH_LOSSLESS_MAX_ORDER] = v;
        s->history_at = (s->history_at + 1U) & (NPH_LOSSLESS_MAX_ORDER - 1U);
    }
    if (s->kind > LL_KIND_LPC) {
        u32 precision = 0U;
        u32 shift = 0U;
        if (!ll_read(s, 4U, &precision) || !ll_read(s, 5U, &shift)) {
            return NPH_RESULT_CORRUPT;
        }
        precision += 1U;
        s->shift = shift;
        // Coefficients are stored newest-sample first; keep them oldest first.
        for (u32 j = 0U; j < order; ++j) {
            i32 c = 0;
            if (!ll_read_signed(s, precision, &c)) {
                return NPH_RESULT_CORRUPT;
            }
            s->coeffs[order - 1U - j] = c;
        }
    }
    u32 partition_order = 0U;
    if (!ll_read(s, 4U, &partition_order) || partition_order > LL_MAX_PARTITION ||
        (frames & ((1U << partition_order) - 1U)) != 0U || (frames >> partition_order) < order) {
        return NPH_RESULT_CORRUPT;
    }
    s->partition_order = partition_order;
    s->partition_index = 0U;
    s->partition_left = 0U;
    return NPH_RESULT_OK;
}

NphResult nph_lossless_decoder_begin(NphLosslessDecoder *dec, const u8 *block, u64 size,
                                     u32 channels, u32 bits)
{
    NPH_ASSERT(dec != NULL);
    dec->failed = 1;
    dec->frames = 0U;
    dec->produced = 0U;
    if (!ll_params_valid(channels, bits, 1U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 header = NPH_LOSSLESS_BLOCK_HEADER_SIZE + 4ULL * channels;
    if (block == NULL || size < header || ll_get_u32(block + 4) != size || block[11] != channels) {
        return NPH_RESULT_CORRUPT;
    }
    const u32 frames = ll_get_u16(block + 8);
    const u32 stereo = block[10];
    if (frames == 0U || stereo >= NPH_LOSSLESS_STEREO_COUNT || (channels != 2U && stereo != 0U)) {
        return NPH_RESULT_CORRUPT;
    }
    dec->channels = channels;
    dec->bits = bits;
    dec->stereo = stereo;
    for (u32 c = 0U; c < channels; ++c) {
        const u64 start = ll_get_u32(block + 12U + (u64)c * 4U);
        const u64 end = c + 1U < channels ? ll_get_u32(block + 12U + (u64)(c + 1U) * 4U) : size;
        if (start < header || start + 2U > end || end > size) {
            return NPH_RESULT_CORRUPT;
        }
        // A side channel carries one bit more than the depth.
        const bool side = (stereo == NPH_LOSSLESS_STEREO_LEFT_SIDE && c == 1U) ||
                          (stereo == NPH_LOSSLESS_STEREO_SIDE_RIGHT && c == 0U) ||
                          (stereo == NPH_LOSSLESS_STEREO_MID_SIDE && c == 1U);
        NPH_TRY(ll_begin_channel(&dec->ch[c], block + start, block + end, bits + (side ? 1U : 0U),
                                 frames));
    }
    dec->frames = frames;
    dec->failed = 0;
    return NPH_RESULT_OK;
}

static bool ll_next_partition(NphLosslessChannelState *s, u32 frames)
{
    u32 rice = 0U;
    if (!ll_read(s, 5U, &rice)) {
        return false;
    }
    if (rice == LL_RICE_ESCAPE) {
        u32 width = 0U;
        if (!ll_read(s, 5U, &width)) {
            return false;
        }
        s->rice = NPH_U32_MAX;
        s->raw_bits = width;
    } else {
        s->rice = rice;
    }
    u32 size = frames >> s->partition_order;
    if (s->partition_index == 0U) {
        size -= s->order; // the warm-up stands in for the first samples
    }
    s->partition_left = size;
    s->partition_index += 1U;
    return true;
}

// One channel's next `count` samples (count <= what is left of the block).
static bool ll_decode_channel(NphLosslessChannelState *s, u32 frames, i32 *out, u32 count)
{
    const i64 lo = -(1LL << (s->sample_bits - 1U));
    const i64 hi = (1LL << (s->sample_bits - 1U)) - 1;
    u32 i = 0U;
    if (s->kind == LL_KIND_CONSTANT) {
        for (; i < count; ++i) {
            out[i] = s->constant;
        }
    } else if (s->kind == LL_KIND_VERBATIM) {
        for (; i < count; ++i) {
            if (!ll_read_signed(s, s->sample_bits, &out[i])) {
                return false;
            }
        }
    } else {
        const u32 order = s->order;
        // The warm-up samples come out of the history first.
        while (i < count && s->produced + i < order) {
            const u32 back = order - (s->produced + i); // how far back from the newest
            out[i] = s->history[s->history_at + NPH_LOSSLESS_MAX_ORDER - back];
            ++i;
        }
        while (i < count) {
            if (s->partition_left == 0U) {
                if (s->partition_index >= (1U << s->partition_order) ||
                    !ll_next_partition(s, frames)) {
                    return false;
                }
                continue;
            }
            const u32 run = nph_min_u32(count - i, s->partition_left);
            for (u32 k = 0U; k < run; ++k) {
                i32 residual = 0;
                if (s->rice == NPH_U32_MAX) {
                    if (!ll_read_signed(s, s->raw_bits, &residual)) {
                        return false;
                    }
                } else {
                    u32 q = 0U;
                    u32 r = 0U;
                    if (s->rice > LL_MAX_RICE || !ll_read_unary(s, &q) ||
                        !ll_read(s, s->rice, &r)) {
                        return false;
                    }
                    const u64 u = ((u64)q << s->rice) | r;
                    if (u > NPH_U32_MAX) {
                        return false;
                    }
                    residual = ll_unzigzag((u32)u);
                }
                // The window holds the last 32 samples, oldest first.
                const i32 *window = s->history + s->history_at + NPH_LOSSLESS_MAX_ORDER - order;
                i64 prediction = 0;
                for (u32 j = 0U; j < order; ++j) {
                    prediction += (i64)s->coeffs[j] * (i64)window[j];
                }
                const i64 v = (i64)residual + (prediction >> s->shift);
                if (v < lo || v > hi) {
                    return false;
                }
                const i32 sample = (i32)v;
                s->history[s->history_at] = sample;
                s->history[s->history_at + NPH_LOSSLESS_MAX_ORDER] = sample;
                s->history_at = (s->history_at + 1U) & (NPH_LOSSLESS_MAX_ORDER - 1U);
                out[i + k] = sample;
            }
            s->partition_left -= run;
            i += run;
        }
    }
    s->produced += count;
    return true;
}

NphResult nph_lossless_decoder_read_int(NphLosslessDecoder *dec, i32 *const *out, u32 count)
{
    NPH_ASSERT(dec != NULL && out != NULL);
    if (dec->failed) {
        return NPH_RESULT_CORRUPT;
    }
    NPH_ASSERT_MSG(count <= dec->frames - dec->produced, "read past the block's %u frames",
                   dec->frames);
    if (count > dec->frames - dec->produced) {
        count = dec->frames - dec->produced;
    }
    for (u32 c = 0U; c < dec->channels; ++c) {
        if (!ll_decode_channel(&dec->ch[c], dec->frames, out[c], count)) {
            dec->failed = 1;
            return NPH_RESULT_CORRUPT;
        }
        const u32 wasted = dec->ch[c].wasted;
        if (wasted != 0U) {
            for (u32 i = 0U; i < count; ++i) {
                out[c][i] = (i32)((u32)out[c][i] << wasted);
            }
        }
    }
    // Undo the stereo decorrelation.
    if (dec->channels == 2U && dec->stereo != NPH_LOSSLESS_STEREO_INDEPENDENT) {
        i32 *a = out[0];
        i32 *b = out[1];
        for (u32 i = 0U; i < count; ++i) {
            switch (dec->stereo) {
            case NPH_LOSSLESS_STEREO_LEFT_SIDE:
                b[i] = a[i] - b[i]; // right = left - side
                break;
            case NPH_LOSSLESS_STEREO_SIDE_RIGHT:
                a[i] = a[i] + b[i]; // left = side + right
                break;
            case NPH_LOSSLESS_STEREO_MID_SIDE: {
                const i32 side = b[i];
                const i32 mid = (i32)(((u32)a[i] << 1U) | ((u32)side & 1U));
                a[i] = (mid + side) >> 1;
                b[i] = (mid - side) >> 1;
                break;
            }
            default:
                break;
            }
        }
    }
    dec->produced += count;
    return NPH_RESULT_OK;
}

NphResult nph_lossless_decoder_read(NphLosslessDecoder *dec, f32 *const *out, u32 count,
                                    i32 *scratch)
{
    NPH_ASSERT(scratch != NULL);
    if (dec->failed) {
        return NPH_RESULT_CORRUPT;
    }
    i32 *planes[NPH_AUDIO_MAX_CHANNELS];
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        planes[c] = scratch + (u64)nph_min_u32(c, dec->channels - 1U) * count;
    }
    const NphResult r = nph_lossless_decoder_read_int(dec, planes, count);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    const f32 scale = 1.0F / (f32)(1U << (dec->bits - 1U));
    for (u32 c = 0U; c < dec->channels; ++c) {
        for (u32 i = 0U; i < count; ++i) {
            out[c][i] = (f32)planes[c][i] * scale;
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_lossless_decoder_skip(NphLosslessDecoder *dec, u32 count)
{
    i32 buffer[NPH_AUDIO_MAX_CHANNELS][256];
    i32 *planes[NPH_AUDIO_MAX_CHANNELS];
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        planes[c] = buffer[c];
    }
    while (count > 0U) {
        const u32 n = nph_min_u32(count, 256U);
        NPH_TRY(nph_lossless_decoder_read_int(dec, planes, n));
        count -= n;
    }
    return NPH_RESULT_OK;
}

NphResult nph_lossless_decode_all(const NphLosslessStream *stream, i32 *out)
{
    NPH_TRY(nph_lossless_validate(stream));
    NphLosslessDecoder dec;
    i32 buffer[NPH_AUDIO_MAX_CHANNELS][256];
    i32 *planes[NPH_AUDIO_MAX_CHANNELS];
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        planes[c] = buffer[c];
    }
    u64 frame = 0U;
    for (u32 b = 0U; b < stream->block_count; ++b) {
        const u64 start = stream->block_offsets[b];
        const u64 end = b + 1U < stream->block_count ? stream->block_offsets[b + 1U] : stream->size;
        NPH_TRY(nph_lossless_decoder_begin(&dec, stream->data + start, end - start,
                                           stream->channels, stream->bits));
        while (nph_lossless_decoder_remaining(&dec) > 0U) {
            const u32 n = nph_min_u32(nph_lossless_decoder_remaining(&dec), 256U);
            NPH_TRY(nph_lossless_decoder_read_int(&dec, planes, n));
            for (u32 i = 0U; i < n; ++i) {
                for (u32 c = 0U; c < stream->channels; ++c) {
                    out[(frame + i) * stream->channels + c] = planes[c][i];
                }
            }
            frame += n;
        }
    }
    return frame == stream->frames ? NPH_RESULT_OK : NPH_RESULT_CORRUPT;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// A growable byte buffer with an LSB-first bit writer on its end.
typedef struct LlWriter {
    NphAllocator *alloc;
    u8 *data;
    u64 size;
    u64 capacity;
    u64 acc;
    u32 acc_bits;
    bool failed;
} LlWriter;

static bool ll_reserve(LlWriter *w, u64 extra)
{
    if (w->failed) {
        return false;
    }
    if (w->size + extra <= w->capacity) {
        return true;
    }
    u64 cap = w->capacity < 4096U ? 4096U : w->capacity;
    while (cap < w->size + extra) {
        cap *= 2U;
    }
    u8 *grown = (u8 *)NPH_REALLOC(w->alloc, w->data, w->capacity, cap, 16U);
    if (grown == NULL) {
        w->failed = true;
        return false;
    }
    w->data = grown;
    w->capacity = cap;
    return true;
}

static void ll_put_byte(LlWriter *w, u8 b)
{
    if (ll_reserve(w, 1U)) {
        w->data[w->size++] = b;
    }
}

static void ll_put_bits(LlWriter *w, u32 value, u32 n)
{
    NPH_ASSERT(n <= 32U);
    if (n == 0U) {
        return;
    }
    const u64 mask = (1ULL << n) - 1ULL;
    w->acc |= ((u64)value & mask) << w->acc_bits;
    w->acc_bits += n;
    while (w->acc_bits >= 8U) {
        ll_put_byte(w, (u8)(w->acc & 0xFFU));
        w->acc >>= 8U;
        w->acc_bits -= 8U;
    }
}

static void ll_put_signed(LlWriter *w, i32 value, u32 n)
{
    ll_put_bits(w, (u32)value, n);
}

static void ll_put_unary(LlWriter *w, u32 q)
{
    while (q >= 32U) {
        ll_put_bits(w, 0U, 32U);
        q -= 32U;
    }
    ll_put_bits(w, 1U << q, q + 1U);
}

static void ll_align(LlWriter *w)
{
    if (w->acc_bits > 0U) {
        ll_put_byte(w, (u8)(w->acc & 0xFFU));
        w->acc = 0U;
        w->acc_bits = 0U;
    }
}

// The bits a signed value needs (two's complement, sign included).
static u32 ll_signed_bits(i64 v)
{
    const u64 m = v < 0 ? (u64)(-(v + 1)) : (u64)v;
    return m == 0U ? 1U : 2U + nph_log2_floor_u64(m);
}

// The cost in bits of Rice-coding `count` values `u` with parameter k.
static u64 ll_rice_cost(const u32 *u, u32 count, u32 k)
{
    u64 bits = (u64)count * (k + 1U);
    for (u32 i = 0U; i < count; ++i) {
        bits += u[i] >> k;
    }
    return bits;
}

typedef struct LlPartition {
    u32 order;                        // partition order
    u32 rice[1U << LL_MAX_PARTITION]; // per partition, LL_RICE_ESCAPE for raw
    u32 raw_bits[1U << LL_MAX_PARTITION];
    u64 bits; // the residual's total, headers included
} LlPartition;

// The best partitioning of residuals u[0 .. n - order) of a block of `n`
// frames (the warm-up takes the first `order` places of partition 0).
static void ll_best_partition(const u32 *u, const i32 *residual, u32 n, u32 order, LlPartition *out)
{
    out->bits = NPH_U64_MAX;
    for (u32 po = 0U; po <= LL_MAX_PARTITION; ++po) {
        if ((n & ((1U << po) - 1U)) != 0U || (n >> po) < order) {
            break;
        }
        const u32 parts = 1U << po;
        const u32 size = n >> po;
        LlPartition cand;
        cand.order = po;
        cand.bits = 4U;
        u32 at = 0U;
        for (u32 p = 0U; p < parts; ++p) {
            const u32 count = p == 0U ? size - order : size;
            const u32 *pu = u + at;
            u64 sum = 0U;
            // The escape's width: the most bits any residual needs as a
            // signed value (-1 needs one, as 1 does not), 0 only when all
            // are zero.
            u32 raw = 0U;
            for (u32 i = 0U; i < count; ++i) {
                sum += pu[i];
                const i32 r = residual[at + i];
                if (r != 0) {
                    raw = nph_max_u32(raw, ll_signed_bits(r));
                }
            }
            // The parameter near log2 of the mean, refined by exact costs.
            u32 k0 = 0U;
            if (count > 0U && sum > count) {
                k0 = nph_log2_floor_u64(sum / count);
            }
            u64 best = NPH_U64_MAX;
            u32 best_k = 0U;
            const u32 k_lo = k0 > 0U ? k0 - 1U : 0U;
            const u32 k_hi = nph_min_u32(k0 + 1U, LL_MAX_RICE);
            for (u32 k = k_lo; k <= k_hi; ++k) {
                const u64 c = ll_rice_cost(pu, count, k);
                if (c < best) {
                    best = c;
                    best_k = k;
                }
            }
            // Widths past 31 do not fit the escape's 5 bits.
            const u64 raw_cost = raw > 31U ? NPH_U64_MAX : 5U + (u64)count * raw;
            if (raw_cost < best) {
                cand.rice[p] = LL_RICE_ESCAPE;
                cand.raw_bits[p] = raw;
                cand.bits += 5U + raw_cost;
            } else {
                cand.rice[p] = best_k;
                cand.raw_bits[p] = 0U;
                cand.bits += 5U + best;
            }
            at += count;
        }
        if (cand.bits < out->bits) {
            *out = cand;
        }
    }
}

typedef struct LlSubframe {
    u32 kind;
    u32 order;
    u32 precision;
    u32 shift;
    i32 coeffs[NPH_LOSSLESS_MAX_ORDER]; // newest sample first
    LlPartition partition;
    u64 bits; // the whole subframe after its two-byte head
} LlSubframe;

// One way to code a channel of a block: its samples (wasted bits shifted
// out), the cheapest subframe for them, and that subframe's residual.
typedef struct LlCandidate {
    i32 *samples;
    i32 *residual;
    u32 wasted;
    u32 bits; // per sample after the shift
    LlSubframe sub;
} LlCandidate;

// Stereo blocks code four candidates (left, right, side, mid); others one
// per channel.
#define LL_CANDIDATES (NPH_AUDIO_MAX_CHANNELS > 4U ? NPH_AUDIO_MAX_CHANNELS : 4U)

typedef struct LlScratch {
    NphAllocator *alloc;
    u32 capacity;
    f64 *windowed;
    i32 *residual;
    u32 *zigzag;
    i32 *channel[NPH_AUDIO_MAX_CHANNELS + 2U]; // the block's channels, plus side and mid
    LlCandidate cand[LL_CANDIDATES];
} LlScratch;

// The residual of `s` (n samples, `order` warm-up) under integer
// coefficients `q` (newest first) and `shift`; false when one overflows 32
// bits (the predictor is then not usable).
static bool ll_residual(const i32 *s, u32 n, const i32 *q, u32 order, u32 shift, i32 *out)
{
    for (u32 i = order; i < n; ++i) {
        i64 p = 0;
        for (u32 j = 0U; j < order; ++j) {
            p += (i64)q[j] * (i64)s[i - 1U - j];
        }
        const i64 e = (i64)s[i] - (p >> shift);
        if (e < (i64)NPH_I32_MIN || e > (i64)NPH_I32_MAX) {
            return false;
        }
        out[i - order] = (i32)e;
    }
    return true;
}

static u64 ll_cost_residual(LlScratch *sc, u32 n, u32 order, LlPartition *part)
{
    for (u32 i = 0U; i + order < n; ++i) {
        sc->zigzag[i] = ll_zigzag(sc->residual[i]);
    }
    ll_best_partition(sc->zigzag, sc->residual, n, order, part);
    return part->bits;
}

// Levinson-Durbin on autocorrelation r[0 .. max]: lpc[o - 1][0 .. o) are the
// coefficients of order o (predicting x[n] from x[n - 1 - j] with weight
// lpc[o-1][j]), err[o - 1] the prediction error energy. Returns the orders
// computed (fewer when the error reaches zero).
static u32 ll_levinson(const f64 *r, u32 max,
                       f64 lpc[NPH_LOSSLESS_MAX_ORDER][NPH_LOSSLESS_MAX_ORDER], f64 *err)
{
    f64 a[NPH_LOSSLESS_MAX_ORDER] = {0.0};
    f64 e = r[0];
    u32 computed = 0U;
    for (u32 o = 0U; o < max; ++o) {
        if (!(e > 0.0)) {
            break;
        }
        f64 acc = r[o + 1U];
        for (u32 j = 0U; j < o; ++j) {
            acc -= a[j] * r[o - j];
        }
        const f64 k = acc / e;
        f64 next[NPH_LOSSLESS_MAX_ORDER];
        for (u32 j = 0U; j < o; ++j) {
            next[j] = a[j] - k * a[o - 1U - j];
        }
        next[o] = k;
        memcpy(a, next, sizeof(f64) * (o + 1U));
        e *= 1.0 - k * k;
        memcpy(lpc[o], a, sizeof(f64) * (o + 1U));
        err[o] = e;
        computed = o + 1U;
    }
    return computed;
}

// Quantises lpc[0 .. order) to `precision`-bit integers and a shift, with
// the rounding error carried forward (Coalson's method in libFLAC).
static bool ll_quantize(const f64 *lpc, u32 order, u32 precision, i32 *q, u32 *out_shift)
{
    f64 cmax = 0.0;
    for (u32 j = 0U; j < order; ++j) {
        cmax = fabs(lpc[j]) > cmax ? fabs(lpc[j]) : cmax;
    }
    if (!(cmax > 0.0)) {
        return false;
    }
    int exponent = 0;
    (void)frexp(cmax, &exponent);
    i32 shift = (i32)precision - 1 - exponent;
    if (shift > 31) {
        shift = 31;
    }
    if (shift < 0) {
        return false; // coefficients too large for the precision
    }
    const i32 qmax = (1 << (precision - 1U)) - 1;
    const i32 qmin = -(1 << (precision - 1U));
    f64 error = 0.0;
    for (u32 j = 0U; j < order; ++j) {
        error += lpc[j] * (f64)(1U << (u32)shift);
        f64 rounded = floor(error + 0.5);
        if (rounded > (f64)qmax) {
            rounded = (f64)qmax;
        }
        if (rounded < (f64)qmin) {
            rounded = (f64)qmin;
        }
        q[j] = (i32)rounded;
        error -= rounded;
    }
    *out_shift = (u32)shift;
    return true;
}

// Chooses the cheapest subframe for one channel's n samples (wasted bits
// already shifted out) of `sample_bits` bits.
static void ll_choose(LlScratch *sc, const i32 *s, u32 n, u32 sample_bits, u32 max_order,
                      bool exhaustive, LlSubframe *out, i32 *best_residual)
{
    // Constant.
    bool constant = true;
    for (u32 i = 1U; i < n; ++i) {
        if (s[i] != s[0]) {
            constant = false;
            break;
        }
    }
    if (constant) {
        out->kind = LL_KIND_CONSTANT;
        out->order = 0U;
        out->bits = sample_bits;
        return;
    }
    // Verbatim is the fallback.
    out->kind = LL_KIND_VERBATIM;
    out->order = 0U;
    out->bits = (u64)n * sample_bits;
    // Fixed predictors.
    for (u32 order = 0U; order <= 4U && order < n; ++order) {
        if (!ll_residual(s, n, g_ll_fixed[order], order, 0U, sc->residual)) {
            continue;
        }
        LlPartition part;
        const u64 bits = (u64)order * sample_bits + ll_cost_residual(sc, n, order, &part);
        if (bits < out->bits) {
            out->kind = LL_KIND_FIXED + order;
            out->order = order;
            out->shift = 0U;
            out->partition = part;
            out->bits = bits;
            memcpy(best_residual, sc->residual, sizeof(i32) * (n - order));
        }
    }
    // Linear prediction: a Tukey(0.5) window (as libFLAC's default
    // apodization), the autocorrelation, Levinson-Durbin.
    const u32 orders = nph_min_u32(max_order, n > 1U ? n - 1U : 0U);
    if (orders == 0U) {
        return;
    }
    for (u32 i = 0U; i < n; ++i) {
        const f64 t = (f64)i / (f64)(n > 1U ? n - 1U : 1U);
        f64 w = 1.0;
        if (t < 0.25) {
            w = 0.5 - 0.5 * cos(LL_PI * t / 0.25);
        } else if (t > 0.75) {
            w = 0.5 - 0.5 * cos(LL_PI * (1.0 - t) / 0.25);
        }
        sc->windowed[i] = (f64)s[i] * w;
    }
    f64 r[NPH_LOSSLESS_MAX_ORDER + 1U];
    for (u32 lag = 0U; lag <= orders; ++lag) {
        f64 acc = 0.0;
        for (u32 i = lag; i < n; ++i) {
            acc += sc->windowed[i] * sc->windowed[i - lag];
        }
        r[lag] = acc;
    }
    if (!(r[0] > 0.0)) {
        return;
    }
    r[0] *= 1.0 + 1e-10; // a little white noise keeps the recursion stable
    f64 lpc[NPH_LOSSLESS_MAX_ORDER][NPH_LOSSLESS_MAX_ORDER];
    f64 err[NPH_LOSSLESS_MAX_ORDER];
    const u32 computed = ll_levinson(r, orders, lpc, err);
    if (computed == 0U) {
        return;
    }
    // Rank the orders by the bits their error predicts (libFLAC's
    // estimate: half the log of the error energy per sample, plus the
    // warm-up and coefficients), then code the best few exactly.
    f64 estimate[NPH_LOSSLESS_MAX_ORDER];
    for (u32 o = 0U; o < computed; ++o) {
        const f64 per_sample = err[o] > 0.0 ? 0.5 * log2(err[o] * 0.5 / (f64)n + 1e-30) : 0.0;
        const f64 residual_bits = per_sample > 0.0 ? per_sample * (f64)(n - (o + 1U)) : 0.0;
        estimate[o] = residual_bits + (f64)(o + 1U) * (f64)(sample_bits + 15U);
    }
    u32 candidates[NPH_LOSSLESS_MAX_ORDER];
    u32 candidate_count = 0U;
    if (exhaustive) {
        for (u32 o = 0U; o < computed; ++o) {
            candidates[candidate_count++] = o + 1U;
        }
    } else {
        // The four best estimates.
        bool taken[NPH_LOSSLESS_MAX_ORDER] = {false};
        for (u32 pick = 0U; pick < 4U && pick < computed; ++pick) {
            u32 best = NPH_U32_MAX;
            for (u32 o = 0U; o < computed; ++o) {
                if (!taken[o] && (best == NPH_U32_MAX || estimate[o] < estimate[best])) {
                    best = o;
                }
            }
            taken[best] = true;
            candidates[candidate_count++] = best + 1U;
        }
    }
    // The precision: 15 bits, fewer for short blocks (as libFLAC chooses).
    const u32 precision =
        n <= 192U
            ? 7U
            : (n <= 384U ? 8U : (n <= 576U ? 9U : (n <= 1152U ? 10U : (n <= 2304U ? 11U : 15U))));
    for (u32 ci = 0U; ci < candidate_count; ++ci) {
        const u32 order = candidates[ci];
        if (order >= n) {
            continue;
        }
        i32 q[NPH_LOSSLESS_MAX_ORDER];
        u32 shift = 0U;
        if (!ll_quantize(lpc[order - 1U], order, precision, q, &shift)) {
            continue;
        }
        if (!ll_residual(s, n, q, order, shift, sc->residual)) {
            continue;
        }
        LlPartition part;
        const u64 bits = (u64)order * sample_bits + 4U + 5U + (u64)order * precision +
                         ll_cost_residual(sc, n, order, &part);
        if (bits < out->bits) {
            out->kind = LL_KIND_LPC + order;
            out->order = order;
            out->precision = precision;
            out->shift = shift;
            memcpy(out->coeffs, q, sizeof(i32) * order);
            out->partition = part;
            out->bits = bits;
            memcpy(best_residual, sc->residual, sizeof(i32) * (n - order));
        }
    }
}

// The common low zero bits of n samples.
static u32 ll_wasted_bits(const i32 *s, u32 n, u32 bits)
{
    u32 all = 0U;
    for (u32 i = 0U; i < n; ++i) {
        all |= (u32)s[i];
    }
    if (all == 0U) {
        return 0U;
    }
    const u32 tz = nph_ctz_u32(all);
    return tz < bits ? tz : 0U;
}

// Prepares candidate `c` from `src` (n samples of `sample_bits` bits): its
// wasted bits shifted out and its cheapest subframe chosen. Returns the
// subframe's size in bytes, its two-byte head included.
static u64 ll_prepare(LlScratch *sc, LlCandidate *c, const i32 *src, u32 n, u32 sample_bits,
                      u32 max_order, bool exhaustive)
{
    memcpy(c->samples, src, (size_t)n * sizeof(i32));
    c->wasted = ll_wasted_bits(c->samples, n, sample_bits);
    if (c->wasted > 0U) {
        for (u32 i = 0U; i < n; ++i) {
            c->samples[i] = c->samples[i] >> c->wasted;
        }
    }
    c->bits = sample_bits - c->wasted;
    memset(&c->sub, 0, sizeof c->sub);
    ll_choose(sc, c->samples, n, c->bits, max_order, exhaustive, &c->sub, c->residual);
    return 2U + (c->sub.bits + 7U) / 8U;
}

// Writes candidate `c`'s subframe (n samples) at the writer's end.
static void ll_emit(LlWriter *w, const LlCandidate *c, u32 n)
{
    const LlSubframe *sub = &c->sub;
    const u32 bits = c->bits;
    ll_put_byte(w, (u8)sub->kind);
    ll_put_byte(w, (u8)c->wasted);
    if (sub->kind == LL_KIND_CONSTANT) {
        ll_put_signed(w, c->samples[0], bits);
    } else if (sub->kind == LL_KIND_VERBATIM) {
        for (u32 i = 0U; i < n; ++i) {
            ll_put_signed(w, c->samples[i], bits);
        }
    } else {
        for (u32 i = 0U; i < sub->order; ++i) {
            ll_put_signed(w, c->samples[i], bits);
        }
        if (sub->kind > LL_KIND_LPC) {
            ll_put_bits(w, sub->precision - 1U, 4U);
            ll_put_bits(w, sub->shift, 5U);
            for (u32 j = 0U; j < sub->order; ++j) {
                ll_put_signed(w, sub->coeffs[j], sub->precision);
            }
        }
        const LlPartition *part = &sub->partition;
        ll_put_bits(w, part->order, 4U);
        const u32 parts = 1U << part->order;
        const u32 size = n >> part->order;
        u32 at = 0U;
        for (u32 p = 0U; p < parts; ++p) {
            const u32 count = p == 0U ? size - sub->order : size;
            const u32 rice = part->rice[p];
            ll_put_bits(w, rice, 5U);
            if (rice == LL_RICE_ESCAPE) {
                ll_put_bits(w, part->raw_bits[p], 5U);
                for (u32 i = 0U; i < count; ++i) {
                    ll_put_signed(w, c->residual[at + i], part->raw_bits[p]);
                }
            } else {
                for (u32 i = 0U; i < count; ++i) {
                    const u32 u = ll_zigzag(c->residual[at + i]);
                    ll_put_unary(w, u >> rice);
                    ll_put_bits(w, u & ((1U << rice) - 1U), rice);
                }
            }
            at += count;
        }
    }
    ll_align(w);
}

static void ll_scratch_free(LlScratch *sc)
{
    if (sc->capacity == 0U) {
        return;
    }
    NPH_FREE(sc->alloc, sc->windowed, (u64)sc->capacity * sizeof(f64), 16U);
    NPH_FREE(sc->alloc, sc->residual, (u64)sc->capacity * sizeof(i32), 16U);
    NPH_FREE(sc->alloc, sc->zigzag, (u64)sc->capacity * sizeof(u32), 16U);
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS + 2U; ++c) {
        NPH_FREE(sc->alloc, sc->channel[c], (u64)sc->capacity * sizeof(i32), 16U);
    }
    for (u32 c = 0U; c < LL_CANDIDATES; ++c) {
        NPH_FREE(sc->alloc, sc->cand[c].samples, (u64)sc->capacity * sizeof(i32), 16U);
        NPH_FREE(sc->alloc, sc->cand[c].residual, (u64)sc->capacity * sizeof(i32), 16U);
    }
    sc->capacity = 0U;
}

static NphResult ll_scratch_init(LlScratch *sc, NphAllocator *alloc, u32 capacity)
{
    memset(sc, 0, sizeof *sc);
    sc->alloc = alloc;
    sc->capacity = capacity;
    sc->windowed = (f64 *)NPH_ALLOC(alloc, (u64)capacity * sizeof(f64), 16U);
    sc->residual = (i32 *)NPH_ALLOC(alloc, (u64)capacity * sizeof(i32), 16U);
    sc->zigzag = (u32 *)NPH_ALLOC(alloc, (u64)capacity * sizeof(u32), 16U);
    bool ok = sc->windowed != NULL && sc->residual != NULL && sc->zigzag != NULL;
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS + 2U; ++c) {
        sc->channel[c] = (i32 *)NPH_ALLOC(alloc, (u64)capacity * sizeof(i32), 16U);
        ok = ok && sc->channel[c] != NULL;
    }
    for (u32 c = 0U; c < LL_CANDIDATES; ++c) {
        sc->cand[c].samples = (i32 *)NPH_ALLOC(alloc, (u64)capacity * sizeof(i32), 16U);
        sc->cand[c].residual = (i32 *)NPH_ALLOC(alloc, (u64)capacity * sizeof(i32), 16U);
        ok = ok && sc->cand[c].samples != NULL && sc->cand[c].residual != NULL;
    }
    if (!ok) {
        ll_scratch_free(sc);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return NPH_RESULT_OK;
}

void nph_lossless_encoded_free(NphLosslessEncoded *encoded)
{
    if (encoded == NULL || encoded->alloc == NULL) {
        return;
    }
    if (encoded->data != NULL) {
        NPH_FREE(encoded->alloc, encoded->data, encoded->data_capacity, 16U);
    }
    if (encoded->block_offsets != NULL) {
        NPH_FREE_ARRAY(encoded->alloc, u32, encoded->block_offsets, encoded->stream.block_count);
    }
    memset(encoded, 0, sizeof *encoded);
}

NphResult nph_lossless_encode(NphAllocator *alloc, const i32 *samples, u32 channels, u32 bits,
                              u64 frames, const NphLosslessParams *params, NphLosslessEncoded *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    const u32 block_frames = params != NULL && params->block_frames != 0U
                                 ? params->block_frames
                                 : NPH_LOSSLESS_DEFAULT_BLOCK;
    const u32 max_order = params != NULL && params->max_order != 0U
                              ? nph_min_u32(params->max_order, NPH_LOSSLESS_MAX_ORDER)
                              : NPH_LOSSLESS_MAX_ORDER;
    const bool exhaustive = params != NULL && params->exhaustive != 0;
    if (!ll_params_valid(channels, bits, block_frames) || (samples == NULL && frames > 0U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Every sample must fit its depth.
    const i32 lo = -(1 << (bits - 1U));
    const i32 hi = (1 << (bits - 1U)) - 1;
    for (u64 i = 0U; i < frames * channels; ++i) {
        if (samples[i] < lo || samples[i] > hi) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    const u32 blocks = nph_lossless_block_count(frames, block_frames);
    out->alloc = alloc;
    out->stream.channels = channels;
    out->stream.bits = bits;
    out->stream.block_frames = block_frames;
    out->stream.frames = frames;
    out->stream.block_count = blocks;
    if (blocks > 0U) {
        out->block_offsets = NPH_ALLOC_ARRAY(alloc, u32, blocks);
        if (out->block_offsets == NULL) {
            nph_lossless_encoded_free(out);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    LlScratch sc;
    NphResult r = ll_scratch_init(&sc, alloc, block_frames);
    if (r != NPH_RESULT_OK) {
        nph_lossless_encoded_free(out);
        return r;
    }
    LlWriter w;
    memset(&w, 0, sizeof w);
    w.alloc = alloc;
    for (u32 b = 0U; b < blocks && !w.failed; ++b) {
        const u64 first = (u64)b * block_frames;
        const u32 n = (u32)nph_min_u64(block_frames, frames - first);
        if (w.size > NPH_U32_MAX) {
            r = NPH_RESULT_OVERFLOW;
            break;
        }
        out->block_offsets[b] = (u32)w.size;
        const u64 block_start = w.size;
        for (u32 c = 0U; c < channels; ++c) {
            for (u32 i = 0U; i < n; ++i) {
                sc.channel[c][i] = samples[(first + i) * channels + c];
            }
        }
        // Each channel's candidate; for stereo also the side and the mid,
        // and the cheapest of the four pairings wins (every one coded exactly:
        // an estimate misjudges, notably the side's extra bit on noise).
        u32 stereo = NPH_LOSSLESS_STEREO_INDEPENDENT;
        u32 order[NPH_AUDIO_MAX_CHANNELS];
        if (channels == 2U) {
            i32 *side = sc.channel[NPH_AUDIO_MAX_CHANNELS];
            i32 *mid = sc.channel[NPH_AUDIO_MAX_CHANNELS + 1U];
            for (u32 i = 0U; i < n; ++i) {
                side[i] = sc.channel[0][i] - sc.channel[1][i];
                mid[i] = (sc.channel[0][i] + sc.channel[1][i]) >> 1;
            }
            const u64 l =
                ll_prepare(&sc, &sc.cand[0], sc.channel[0], n, bits, max_order, exhaustive);
            const u64 rr =
                ll_prepare(&sc, &sc.cand[1], sc.channel[1], n, bits, max_order, exhaustive);
            const u64 sd = ll_prepare(&sc, &sc.cand[2], side, n, bits + 1U, max_order, exhaustive);
            const u64 md = ll_prepare(&sc, &sc.cand[3], mid, n, bits, max_order, exhaustive);
            const u64 costs[4] = {l + rr, l + sd, sd + rr, md + sd};
            for (u32 m = 1U; m < 4U; ++m) {
                if (costs[m] < costs[stereo]) {
                    stereo = m;
                }
            }
            static const u32 pairs[4][2] = {{0U, 1U}, {0U, 2U}, {2U, 1U}, {3U, 2U}};
            order[0] = pairs[stereo][0];
            order[1] = pairs[stereo][1];
        } else {
            for (u32 c = 0U; c < channels; ++c) {
                (void)ll_prepare(&sc, &sc.cand[c], sc.channel[c], n, bits, max_order, exhaustive);
                order[c] = c;
            }
        }
        // The header, with the offsets patched in as the subframes land.
        if (!ll_reserve(&w, NPH_LOSSLESS_BLOCK_HEADER_SIZE + 4U * channels)) {
            break;
        }
        memset(w.data + w.size, 0, NPH_LOSSLESS_BLOCK_HEADER_SIZE + 4U * channels);
        w.size += NPH_LOSSLESS_BLOCK_HEADER_SIZE + 4U * channels;
        u32 offsets[NPH_AUDIO_MAX_CHANNELS];
        for (u32 c = 0U; c < channels; ++c) {
            offsets[c] = (u32)(w.size - block_start);
            ll_emit(&w, &sc.cand[order[c]], n);
        }
        if (w.failed) {
            break;
        }
        u8 *block = w.data + block_start;
        const u64 size = w.size - block_start;
        ll_put_u32(block + 4, (u32)size);
        block[8] = (u8)(n & 0xFFU);
        block[9] = (u8)(n >> 8U);
        block[10] = (u8)stereo;
        block[11] = (u8)channels;
        for (u32 c = 0U; c < channels; ++c) {
            ll_put_u32(block + 12U + (u64)c * 4U, offsets[c]);
        }
        ll_put_u32(block, nph_crc32c(block + 4, size - 4U, 0U));
    }
    ll_scratch_free(&sc);
    if (r == NPH_RESULT_OK && w.failed) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    }
    if (r == NPH_RESULT_OK && w.size > NPH_U32_MAX) {
        r = NPH_RESULT_OVERFLOW;
    }
    out->data = w.data;
    out->data_capacity = w.capacity;
    if (r != NPH_RESULT_OK) {
        nph_lossless_encoded_free(out);
        return r;
    }
    out->stream.data = w.data;
    out->stream.size = w.size;
    out->stream.block_offsets = out->block_offsets;
    return NPH_RESULT_OK;
}

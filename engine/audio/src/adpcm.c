// SPDX-License-Identifier: Apache-2.0
// Our ADPCM (adpcm.h): the decoder, and the encoder's predictor fitting and
// closed-loop block search.

#include <nepheline/audio/adpcm.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

// Microsoft ADPCM's seven standard predictor pairs, Q8 there, Q11 here.
static const i16 g_adpcm_standard[7][2] = {
    {2048, 0}, {4096, -2048}, {0, 0}, {1536, 512}, {1920, 0}, {3680, -1664}, {3136, -1856},
};

static i32 adpcm_clamp16(i32 v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static i16 adpcm_get_i16(const u8 *p)
{
    return (i16)(u16)((u32)p[0] | ((u32)p[1] << 8U));
}

static void adpcm_put_i16(u8 *p, i32 v)
{
    const u32 u = (u32)v;
    p[0] = (u8)(u & 0xFFU);
    p[1] = (u8)((u >> 8U) & 0xFFU);
}

u64 nph_adpcm_size(u32 channels, u64 frames)
{
    const u64 blocks = (frames + NPH_ADPCM_BLOCK_FRAMES - 1U) / NPH_ADPCM_BLOCK_FRAMES;
    return NPH_ADPCM_TABLE_BYTES + blocks * channels * NPH_ADPCM_BLOCK_BYTES;
}

NphResult nph_adpcm_validate(const NphAdpcmStream *stream)
{
    NPH_ASSERT(stream != NULL);
    if (stream->channels == 0U || stream->channels > NPH_AUDIO_MAX_CHANNELS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (stream->data == NULL || stream->size != nph_adpcm_size(stream->channels, stream->frames)) {
        return NPH_RESULT_CORRUPT;
    }
    const u64 blocks = (stream->size - NPH_ADPCM_TABLE_BYTES) / NPH_ADPCM_BLOCK_BYTES;
    for (u64 b = 0U; b < blocks; ++b) {
        const u8 head = stream->data[NPH_ADPCM_TABLE_BYTES + b * NPH_ADPCM_BLOCK_BYTES + 4U];
        if ((u32)(head & 0x0FU) > NPH_ADPCM_MAX_SHIFT) {
            return NPH_RESULT_CORRUPT;
        }
    }
    return NPH_RESULT_OK;
}

// Decodes `count` samples of one block from sample `skip` on into `out` (as
// integers). The arithmetic is bounded for any bytes.
static void adpcm_decode_block(const u8 *table, const u8 *block, u32 skip, u32 count, i32 *out)
{
    i32 s1 = adpcm_get_i16(block);
    i32 s2 = adpcm_get_i16(block + 2);
    const u32 head = block[4];
    const u32 predictor = head >> 4U;
    const u32 shift = nph_min_u32(head & 0x0FU, NPH_ADPCM_MAX_SHIFT);
    const i64 c1 = adpcm_get_i16(table + (u64)predictor * 4U);
    const i64 c2 = adpcm_get_i16(table + (u64)predictor * 4U + 2U);
    const u8 *codes = block + 5;
    const u32 end = skip + count;
    for (u32 i = 0U; i < end; ++i) {
        const u32 nibble = ((u32)codes[i >> 1U] >> ((i & 1U) * 4U)) & 0x0FU;
        const i32 code = (i32)(nibble ^ 8U) - 8; // sign-extend four bits
        // 64-bit products: a hostile table's extremes would overflow 32.
        const i32 p = (i32)((c1 * s1 + c2 * s2 + 1024) >> 11);
        const i32 v = adpcm_clamp16(p + code * (1 << shift));
        s2 = s1;
        s1 = v;
        out[i] = v; // the skipped head is overwritten below
    }
    if (skip > 0U) {
        memmove(out, out + skip, (size_t)count * sizeof(i32));
    }
}

// Where nph_adpcm_decode and _decode_int put their samples.
typedef struct AdpcmOut {
    f32 *const *planar; // floats, one array per channel
    i16 *interleaved;   // or 16-bit integers, interleaved
    u32 channels;
} AdpcmOut;

static void adpcm_put(const AdpcmOut *o, u32 at, u32 channel, const i32 *v, u32 n)
{
    if (o->planar != NULL) {
        f32 *dst = o->planar[channel] + at;
        for (u32 i = 0U; i < n; ++i) {
            dst[i] = (f32)v[i] * (1.0F / 32768.0F);
        }
    } else {
        i16 *dst = o->interleaved + (u64)at * o->channels + channel;
        for (u32 i = 0U; i < n; ++i) {
            dst[(u64)i * o->channels] = (i16)v[i];
        }
    }
}

// Frames [first, first + count) into `o`, block by block; silence where the
// stream does not reach.
static void adpcm_decode_range(const NphAdpcmStream *stream, u64 first, u32 count,
                               const AdpcmOut *o)
{
    const u32 channels = stream->channels;
    const u64 blocks = (stream->frames + NPH_ADPCM_BLOCK_FRAMES - 1U) / NPH_ADPCM_BLOCK_FRAMES;
    const u8 *table = stream->data;
    static const i32 g_silence[NPH_ADPCM_BLOCK_FRAMES] = {0};
    const bool have_table = table != NULL && stream->size >= NPH_ADPCM_TABLE_BYTES;
    u32 done = 0U;
    i32 samples[NPH_ADPCM_BLOCK_FRAMES + NPH_ADPCM_BLOCK_FRAMES];
    while (done < count) {
        const u64 frame = first + done;
        const u64 b = frame / NPH_ADPCM_BLOCK_FRAMES;
        const u32 skip = (u32)(frame % NPH_ADPCM_BLOCK_FRAMES);
        u32 n = nph_min_u32(count - done, NPH_ADPCM_BLOCK_FRAMES - skip);
        if (!have_table || b >= blocks || frame >= stream->frames) {
            for (u32 c = 0U; c < channels; ++c) {
                adpcm_put(o, done, c, g_silence, n);
            }
            done += n;
            continue;
        }
        n = (u32)nph_min_u64(n, stream->frames - frame);
        for (u32 c = 0U; c < channels; ++c) {
            const u64 offset = NPH_ADPCM_TABLE_BYTES + (b * channels + c) * NPH_ADPCM_BLOCK_BYTES;
            if (offset + NPH_ADPCM_BLOCK_BYTES > stream->size) {
                adpcm_put(o, done, c, g_silence, n); // a stream shorter than it says
            } else {
                adpcm_decode_block(table, stream->data + offset, skip, n, samples);
                adpcm_put(o, done, c, samples, n);
            }
        }
        done += n;
    }
}

void nph_adpcm_decode(const NphAdpcmStream *stream, u64 first, u32 count, f32 *const *out)
{
    NPH_ASSERT(stream != NULL && out != NULL);
    const AdpcmOut o = {out, NULL, stream->channels};
    adpcm_decode_range(stream, first, count, &o);
}

void nph_adpcm_decode_int(const NphAdpcmStream *stream, u64 first, u32 count, i16 *out)
{
    NPH_ASSERT(stream != NULL && out != NULL);
    const AdpcmOut o = {NULL, out, stream->channels};
    adpcm_decode_range(stream, first, count, &o);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// The least-squares second-order predictor of x[2 .. n) from the two before,
// and the weight (energy) of the block; false for a block too quiet to fit.
static bool adpcm_fit_block(const i32 *x, u32 n, f64 *c1, f64 *c2, f64 *weight)
{
    f64 r11 = 0.0;
    f64 r12 = 0.0;
    f64 r22 = 0.0;
    f64 r01 = 0.0;
    f64 r02 = 0.0;
    f64 energy = 0.0;
    for (u32 i = 2U; i < n; ++i) {
        const f64 a = (f64)x[i - 1U];
        const f64 b = (f64)x[i - 2U];
        const f64 y = (f64)x[i];
        r11 += a * a;
        r12 += a * b;
        r22 += b * b;
        r01 += y * a;
        r02 += y * b;
        energy += y * y;
    }
    const f64 det = r11 * r22 - r12 * r12;
    if (!(energy > 1.0) || !(fabs(det) > 1e-9 * (r11 * r22 + 1.0))) {
        return false;
    }
    *c1 = (r01 * r22 - r02 * r12) / det;
    *c2 = (r02 * r11 - r01 * r12) / det;
    *weight = energy;
    return true;
}

// Keeps a predictor inside the stability triangle (|c2| < 1, |c1| < 1 - c2,
// in real units), a little within it.
static void adpcm_stabilise(f64 *c1, f64 *c2)
{
    const f64 margin = 0.995;
    if (*c2 > margin) {
        *c2 = margin;
    }
    if (*c2 < -margin) {
        *c2 = -margin;
    }
    const f64 lim = margin * (1.0 - *c2);
    if (*c1 > lim) {
        *c1 = lim;
    }
    if (*c1 < -lim) {
        *c1 = -lim;
    }
}

static i16 adpcm_q11(f64 c)
{
    f64 v = floor(c * 2048.0 + 0.5);
    v = v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v);
    return (i16)v;
}

// The clip's 16 predictor pairs: the seven standard ones, then nine fitted
// by weighted k-means to the blocks' own least-squares predictors.
static void adpcm_build_table(const i16 *samples, u32 channels, u64 frames, i16 table[16][2],
                              NphAllocator *alloc)
{
    for (u32 k = 0U; k < 7U; ++k) {
        table[k][0] = g_adpcm_standard[k][0];
        table[k][1] = g_adpcm_standard[k][1];
    }
    // Defaults for the rest, used when the clip is too short or quiet to
    // fit: gentle variations of the first-order predictor.
    for (u32 k = 7U; k < 16U; ++k) {
        table[k][0] = (i16)(1024 + 128 * (i32)(k - 7U));
        table[k][1] = 0;
    }
    const u64 blocks = (frames + NPH_ADPCM_BLOCK_FRAMES - 1U) / NPH_ADPCM_BLOCK_FRAMES;
    const u64 points_max = blocks * channels;
    if (points_max == 0U) {
        return;
    }
    f64 *pts = (f64 *)NPH_ALLOC(alloc, points_max * 3U * sizeof(f64), 16U);
    if (pts == NULL) {
        return; // the defaults still encode, only less well
    }
    u64 count = 0U;
    i32 x[NPH_ADPCM_BLOCK_FRAMES + 2U];
    for (u64 b = 0U; b < blocks; ++b) {
        for (u32 c = 0U; c < channels; ++c) {
            const u64 start = b * NPH_ADPCM_BLOCK_FRAMES;
            const u32 n = (u32)nph_min_u64(NPH_ADPCM_BLOCK_FRAMES, frames - start);
            for (u32 i = 0U; i < n + 2U; ++i) {
                const i64 f = (i64)start - 2 + (i64)i;
                x[i] = f < 0 ? 0 : samples[(u64)f * channels + c];
            }
            f64 c1 = 0.0;
            f64 c2 = 0.0;
            f64 w = 0.0;
            if (adpcm_fit_block(x, n + 2U, &c1, &c2, &w)) {
                adpcm_stabilise(&c1, &c2);
                pts[count * 3U] = c1;
                pts[count * 3U + 1U] = c2;
                pts[count * 3U + 2U] = log(1.0 + w); // loud blocks matter more, not all-deciding
                ++count;
            }
        }
    }
    if (count >= 9U) {
        // Seeds: the nine fitted points farthest from everything chosen.
        f64 centre[9][2];
        for (u32 k = 0U; k < 9U; ++k) {
            f64 best = -1.0;
            u64 pick = 0U;
            for (u64 i = 0U; i < count; ++i) {
                f64 nearest = 1e300;
                for (u32 t = 0U; t < 7U + k; ++t) {
                    const f64 tx = t < 7U ? (f64)table[t][0] / 2048.0 : centre[t - 7U][0];
                    const f64 ty = t < 7U ? (f64)table[t][1] / 2048.0 : centre[t - 7U][1];
                    const f64 dx = pts[i * 3U] - tx;
                    const f64 dy = pts[i * 3U + 1U] - ty;
                    const f64 d = dx * dx + dy * dy;
                    nearest = d < nearest ? d : nearest;
                }
                const f64 score = nearest * pts[i * 3U + 2U];
                if (score > best) {
                    best = score;
                    pick = i;
                }
            }
            centre[k][0] = pts[pick * 3U];
            centre[k][1] = pts[pick * 3U + 1U];
        }
        // Lloyd's iterations, the standard pairs held fixed.
        for (u32 iter = 0U; iter < 24U; ++iter) {
            f64 sum[9][3] = {{0.0}};
            for (u64 i = 0U; i < count; ++i) {
                u32 best = 0U;
                f64 best_d = 1e300;
                for (u32 t = 0U; t < 16U; ++t) {
                    const f64 tx = t < 7U ? (f64)table[t][0] / 2048.0 : centre[t - 7U][0];
                    const f64 ty = t < 7U ? (f64)table[t][1] / 2048.0 : centre[t - 7U][1];
                    const f64 dx = pts[i * 3U] - tx;
                    const f64 dy = pts[i * 3U + 1U] - ty;
                    const f64 d = dx * dx + dy * dy;
                    if (d < best_d) {
                        best_d = d;
                        best = t;
                    }
                }
                if (best >= 7U) {
                    const f64 w = pts[i * 3U + 2U];
                    sum[best - 7U][0] += w * pts[i * 3U];
                    sum[best - 7U][1] += w * pts[i * 3U + 1U];
                    sum[best - 7U][2] += w;
                }
            }
            for (u32 k = 0U; k < 9U; ++k) {
                if (sum[k][2] > 0.0) {
                    centre[k][0] = sum[k][0] / sum[k][2];
                    centre[k][1] = sum[k][1] / sum[k][2];
                }
            }
        }
        for (u32 k = 0U; k < 9U; ++k) {
            f64 c1 = centre[k][0];
            f64 c2 = centre[k][1];
            adpcm_stabilise(&c1, &c2);
            table[7U + k][0] = adpcm_q11(c1);
            table[7U + k][1] = adpcm_q11(c2);
        }
    }
    NPH_FREE(alloc, pts, points_max * 3U * sizeof(f64), 16U);
}

// Encodes one block in closed loop with predictor (c1, c2) and `shift`;
// writes the codes when `codes` is not NULL. Returns the squared error, or
// stops early (returning a value above `bound`) once it exceeds `bound`.
static u64 adpcm_try_block(const i32 *x, u32 n, i32 s1, i32 s2, i32 c1, i32 c2, u32 shift,
                           u64 bound, u8 *codes)
{
    u64 err = 0U;
    for (u32 i = 0U; i < n; ++i) {
        const i32 p = (i32)(((i64)c1 * s1 + (i64)c2 * s2 + 1024) >> 11);
        const i32 d = x[i] - p;
        // Round to nearest, then try the neighbours: clamping to 16 bits can
        // make another code closer.
        const i32 half = shift > 0U ? (1 << (shift - 1U)) : 0;
        i32 n0 = (d + half) >> shift;
        n0 = n0 < -8 ? -8 : (n0 > 7 ? 7 : n0);
        i32 best_code = n0;
        i32 best_s = adpcm_clamp16(p + n0 * (1 << shift));
        i64 best_e = (i64)x[i] - best_s;
        for (i32 dn = -1; dn <= 1; dn += 2) {
            const i32 code = n0 + dn;
            if (code < -8 || code > 7) {
                continue;
            }
            const i32 s = adpcm_clamp16(p + code * (1 << shift));
            const i64 e = (i64)x[i] - s;
            if (e * e < best_e * best_e) {
                best_code = code;
                best_s = s;
                best_e = e;
            }
        }
        err += (u64)(best_e * best_e);
        if (err > bound) {
            return err;
        }
        if (codes != NULL) {
            codes[i >> 1U] |= (u8)(((u32)best_code & 0x0FU) << ((i & 1U) * 4U));
        }
        s2 = s1;
        s1 = best_s;
    }
    return err;
}

void nph_adpcm_encoded_free(NphAdpcmEncoded *encoded)
{
    if (encoded == NULL || encoded->alloc == NULL) {
        return;
    }
    if (encoded->data != NULL) {
        NPH_FREE(encoded->alloc, encoded->data, encoded->stream.size, 16U);
    }
    memset(encoded, 0, sizeof *encoded);
}

NphResult nph_adpcm_encode(NphAllocator *alloc, const i16 *samples, u32 channels, u64 frames,
                           NphAdpcmEncoded *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (channels == 0U || channels > NPH_AUDIO_MAX_CHANNELS || (samples == NULL && frames > 0U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 size = nph_adpcm_size(channels, frames);
    u8 *data = (u8 *)NPH_ALLOC_ZEROED(alloc, size, 16U);
    if (data == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    i16 table[16][2];
    adpcm_build_table(samples, channels, frames, table, alloc);
    for (u32 k = 0U; k < 16U; ++k) {
        adpcm_put_i16(data + (u64)k * 4U, table[k][0]);
        adpcm_put_i16(data + (u64)k * 4U + 2U, table[k][1]);
    }
    const u64 blocks = (frames + NPH_ADPCM_BLOCK_FRAMES - 1U) / NPH_ADPCM_BLOCK_FRAMES;
    i32 x[NPH_ADPCM_BLOCK_FRAMES];
    for (u64 b = 0U; b < blocks; ++b) {
        const u64 start = b * NPH_ADPCM_BLOCK_FRAMES;
        const u32 n = (u32)nph_min_u64(NPH_ADPCM_BLOCK_FRAMES, frames - start);
        for (u32 c = 0U; c < channels; ++c) {
            u8 *block = data + NPH_ADPCM_TABLE_BYTES + (b * channels + c) * NPH_ADPCM_BLOCK_BYTES;
            for (u32 i = 0U; i < n; ++i) {
                x[i] = samples[(start + i) * channels + c];
            }
            // The history is the source itself, so each block starts exact.
            const i32 s1 = start >= 1U ? samples[(start - 1U) * channels + c] : 0;
            const i32 s2 = start >= 2U ? samples[(start - 2U) * channels + c] : 0;
            u64 best = NPH_U64_MAX;
            u32 best_pred = 0U;
            u32 best_shift = 0U;
            for (u32 pred = 0U; pred < NPH_ADPCM_PREDICTORS; ++pred) {
                for (u32 shift = 0U; shift <= NPH_ADPCM_MAX_SHIFT; ++shift) {
                    const u64 e = adpcm_try_block(x, n, s1, s2, table[pred][0], table[pred][1],
                                                  shift, best, NULL);
                    if (e < best) {
                        best = e;
                        best_pred = pred;
                        best_shift = shift;
                    }
                }
            }
            adpcm_put_i16(block, s1);
            adpcm_put_i16(block + 2, s2);
            block[4] = (u8)((best_pred << 4U) | best_shift);
            (void)adpcm_try_block(x, n, s1, s2, table[best_pred][0], table[best_pred][1],
                                  best_shift, NPH_U64_MAX, block + 5);
        }
    }
    out->alloc = alloc;
    out->data = data;
    out->stream.channels = channels;
    out->stream.frames = frames;
    out->stream.data = data;
    out->stream.size = size;
    return NPH_RESULT_OK;
}

// SPDX-License-Identifier: Apache-2.0
// zlib decompression (golden/png.h golden_zlib_decompress): RFC 1950 framing
// around RFC 1951 deflate. The Huffman decoding is the canonical-code walk
// of RFC 1951 §3.2.2, one bit at a time against per-length code counts, as
// in Mark Adler's reference inflater "puff" (zlib contrib): slower than a
// table-driven decoder, but every step is bounds-checked and the code is
// short enough to verify by reading. Golden images are small; speed is not
// the point here.
#include <nepheline/core/assert.h>

#include <golden/png.h>
#include <string.h>

#define MAX_BITS     15U  // longest code
#define MAX_LITLEN   286U // literal/length symbols in use (codes 286 and 287 never occur)
#define MAX_DIST     30U  // distance symbols
#define FIXED_LITLEN 288U // the fixed code defines 288 lengths

typedef struct Bits {
    const u8 *src;
    u64 size;
    u64 pos;   // next byte
    u32 buf;   // bits not yet consumed, least significant first
    u32 count; // how many
    u8 *dst;
    u64 cap;
    u64 out; // bytes written
} Bits;

typedef struct Huffman {
    u16 count[MAX_BITS + 1U]; // codes of each length
    u16 symbol[FIXED_LITLEN]; // symbols in canonical order
} Huffman;

// Reads `n` (<= 16) bits, least significant first. False past the input.
static bool bits(Bits *s, u32 n, u32 *out)
{
    while (s->count < n) {
        if (s->pos == s->size) {
            return false;
        }
        s->buf |= (u32)s->src[s->pos++] << s->count;
        s->count += 8U;
    }
    *out = s->buf & ((1U << n) - 1U);
    s->buf >>= n;
    s->count -= n;
    return true;
}

// Builds the canonical code of `n` symbols with the given code lengths.
// Returns the number of unused codes (0: complete); negative when the
// lengths over-subscribe the code (invalid).
static i32 build(Huffman *h, const u16 *lengths, u32 n)
{
    memset(h->count, 0, sizeof h->count);
    for (u32 i = 0; i < n; ++i) {
        h->count[lengths[i]]++;
    }
    if (h->count[0] == n) {
        return 0; // no codes: complete, and decoding any symbol fails
    }
    i32 left = 1;
    for (u32 len = 1U; len <= MAX_BITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left;
        }
    }
    u16 offs[MAX_BITS + 1U];
    offs[1] = 0U;
    for (u32 len = 1U; len < MAX_BITS; ++len) {
        offs[len + 1U] = (u16)(offs[len] + h->count[len]);
    }
    for (u32 i = 0; i < n; ++i) {
        if (lengths[i] != 0U) {
            h->symbol[offs[lengths[i]]++] = (u16)i;
        }
    }
    return left;
}

// Decodes one symbol; -1 when the input ends or the code is not in use.
static i32 decode(Bits *s, const Huffman *h)
{
    i32 code = 0;  // bits read so far
    i32 first = 0; // first code of the current length
    i32 index = 0; // index of that code's symbol in h->symbol
    for (u32 len = 1U; len <= MAX_BITS; ++len) {
        u32 bit = 0U;
        if (!bits(s, 1U, &bit)) {
            return -1;
        }
        code |= (i32)bit; // Huffman codes are packed most significant bit first
        const i32 count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static bool put(Bits *s, u8 byte)
{
    if (s->out == s->cap) {
        return false;
    }
    s->dst[s->out++] = byte;
    return true;
}

// The length and distance tables of RFC 1951 §3.2.5.
static const u16 g_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 g_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 g_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 g_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decodes the literals, lengths, and distances of one compressed block.
static NphResult codes(Bits *s, const Huffman *litlen, const Huffman *dist)
{
    for (;;) {
        const i32 sym = decode(s, litlen);
        if (sym < 0) {
            return NPH_RESULT_CORRUPT;
        }
        if (sym < 256) {
            if (!put(s, (u8)sym)) {
                return NPH_RESULT_CORRUPT;
            }
            continue;
        }
        if (sym == 256) {
            return NPH_RESULT_OK; // end of block
        }
        const u32 li = (u32)sym - 257U;
        if (li >= 29U) {
            return NPH_RESULT_CORRUPT;
        }
        u32 extra = 0U;
        if (!bits(s, g_len_extra[li], &extra)) {
            return NPH_RESULT_CORRUPT;
        }
        const u32 len = g_len_base[li] + extra;
        const i32 dsym = decode(s, dist);
        if (dsym < 0 || dsym >= 30) {
            return NPH_RESULT_CORRUPT;
        }
        if (!bits(s, g_dist_extra[dsym], &extra)) {
            return NPH_RESULT_CORRUPT;
        }
        const u64 d = (u64)g_dist_base[dsym] + extra;
        if (d > s->out || len > s->cap - s->out) {
            return NPH_RESULT_CORRUPT; // before the start, or past the output
        }
        // Byte by byte: the source may overlap the destination (a run).
        for (u32 i = 0; i < len; ++i) {
            s->dst[s->out] = s->dst[s->out - d];
            ++s->out;
        }
    }
}

static NphResult stored(Bits *s)
{
    s->buf = 0U; // the rest of the current byte is padding
    s->count = 0U;
    if (s->size - s->pos < 4U) {
        return NPH_RESULT_CORRUPT;
    }
    const u32 len = (u32)s->src[s->pos] | ((u32)s->src[s->pos + 1U] << 8U);
    const u32 nlen = (u32)s->src[s->pos + 2U] | ((u32)s->src[s->pos + 3U] << 8U);
    s->pos += 4U;
    if ((len ^ 0xFFFFU) != nlen || len > s->size - s->pos || len > s->cap - s->out) {
        return NPH_RESULT_CORRUPT;
    }
    memcpy(s->dst + s->out, s->src + s->pos, len);
    s->pos += len;
    s->out += len;
    return NPH_RESULT_OK;
}

static NphResult fixed(Bits *s)
{
    u16 lengths[FIXED_LITLEN];
    u32 i = 0U;
    for (; i < 144U; ++i) {
        lengths[i] = 8U;
    }
    for (; i < 256U; ++i) {
        lengths[i] = 9U;
    }
    for (; i < 280U; ++i) {
        lengths[i] = 7U;
    }
    for (; i < FIXED_LITLEN; ++i) {
        lengths[i] = 8U;
    }
    Huffman litlen;
    Huffman dist;
    NPH_IGNORE_RESULT(build(&litlen, lengths, FIXED_LITLEN));
    for (i = 0U; i < MAX_DIST; ++i) {
        lengths[i] = 5U;
    }
    NPH_IGNORE_RESULT(build(&dist, lengths, MAX_DIST)); // incomplete by design
    return codes(s, &litlen, &dist);
}

static NphResult dynamic(Bits *s)
{
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    u32 hlit = 0U;
    u32 hdist = 0U;
    u32 hclen = 0U;
    if (!bits(s, 5U, &hlit) || !bits(s, 5U, &hdist) || !bits(s, 4U, &hclen)) {
        return NPH_RESULT_CORRUPT;
    }
    const u32 nlen = hlit + 257U;
    const u32 ndist = hdist + 1U;
    const u32 ncode = hclen + 4U;
    if (nlen > MAX_LITLEN || ndist > MAX_DIST) {
        return NPH_RESULT_CORRUPT;
    }
    u16 lengths[MAX_LITLEN + MAX_DIST];
    memset(lengths, 0, sizeof lengths);
    for (u32 i = 0; i < ncode; ++i) {
        u32 v = 0U;
        if (!bits(s, 3U, &v)) {
            return NPH_RESULT_CORRUPT;
        }
        lengths[order[i]] = (u16)v;
    }
    Huffman lencode;
    if (build(&lencode, lengths, 19U) != 0) {
        return NPH_RESULT_CORRUPT; // the code-length code must be complete
    }
    memset(lengths, 0, sizeof lengths);
    u32 index = 0U;
    while (index < nlen + ndist) {
        const i32 sym = decode(s, &lencode);
        if (sym < 0) {
            return NPH_RESULT_CORRUPT;
        }
        if (sym < 16) {
            lengths[index++] = (u16)sym;
            continue;
        }
        u16 len = 0U;
        u32 repeat = 0U;
        if (sym == 16) {
            if (index == 0U || !bits(s, 2U, &repeat)) {
                return NPH_RESULT_CORRUPT; // nothing to repeat
            }
            len = lengths[index - 1U];
            repeat += 3U;
        } else if (sym == 17) {
            if (!bits(s, 3U, &repeat)) {
                return NPH_RESULT_CORRUPT;
            }
            repeat += 3U;
        } else {
            if (!bits(s, 7U, &repeat)) {
                return NPH_RESULT_CORRUPT;
            }
            repeat += 11U;
        }
        if (index + repeat > nlen + ndist) {
            return NPH_RESULT_CORRUPT;
        }
        while (repeat-- != 0U) {
            lengths[index++] = len;
        }
    }
    if (lengths[256] == 0U) {
        return NPH_RESULT_CORRUPT; // no end-of-block code
    }
    Huffman litlen;
    Huffman dist;
    const i32 left_lit = build(&litlen, lengths, nlen);
    // An incomplete literal/length code is only valid as a single code.
    if (left_lit < 0 || (left_lit > 0 && nlen - litlen.count[0] != 1U)) {
        return NPH_RESULT_CORRUPT;
    }
    const i32 left_dist = build(&dist, lengths + nlen, ndist);
    if (left_dist < 0 || (left_dist > 0 && ndist - dist.count[0] != 1U)) {
        return NPH_RESULT_CORRUPT;
    }
    return codes(s, &litlen, &dist);
}

static u32 adler32(const u8 *data, u64 size)
{
    u32 a = 1U;
    u32 b = 0U;
    for (u64 i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

NphResult golden_zlib_decompress(const u8 *src, u64 size, u8 *dst, u64 capacity, u64 *out_written)
{
    NPH_ASSERT((src != NULL || size == 0U) && (dst != NULL || capacity == 0U) &&
               out_written != NULL);
    *out_written = 0U;
    // RFC 1950: CMF (deflate, window <= 32 KiB), FLG (check bits, no
    // preset dictionary), the data, and the Adler-32 of the output.
    if (size < 6U || (src[0] & 0x0FU) != 8U || (src[0] >> 4U) > 7U ||
        (((u32)src[0] << 8U) | src[1]) % 31U != 0U || (src[1] & 0x20U) != 0U) {
        return NPH_RESULT_CORRUPT;
    }
    Bits s;
    memset(&s, 0, sizeof s);
    s.src = src + 2U;
    s.size = size - 6U;
    s.dst = dst;
    s.cap = capacity;
    u32 last = 0U;
    do {
        u32 type = 0U;
        if (!bits(&s, 1U, &last) || !bits(&s, 2U, &type)) {
            return NPH_RESULT_CORRUPT;
        }
        NphResult r = NPH_RESULT_CORRUPT;
        if (type == 0U) {
            r = stored(&s);
        } else if (type == 1U) {
            r = fixed(&s);
        } else if (type == 2U) {
            r = dynamic(&s);
        }
        if (r != NPH_RESULT_OK) {
            return r;
        }
    } while (last == 0U);
    const u8 *tail = src + size - 4U;
    const u32 expected =
        ((u32)tail[0] << 24U) | ((u32)tail[1] << 16U) | ((u32)tail[2] << 8U) | tail[3];
    if (adler32(dst, s.out) != expected) {
        return NPH_RESULT_CORRUPT;
    }
    *out_written = s.out;
    return NPH_RESULT_OK;
}

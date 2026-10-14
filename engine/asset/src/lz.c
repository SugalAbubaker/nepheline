// SPDX-License-Identifier: Apache-2.0
// The engine's LZ77 codec over the LZ4 block format (lz.h).
//
// Stream grammar (Collet, "LZ4 Block Format Description", 2011):
//
//     sequence := token literals* [offset match_length_ext*]
//     token    := literal_length:4 match_length:4    (15 means "more follows")
//     ext      := 255* byte                         (adds 255 per 255 byte)
//     offset   := u16 little-endian, 1..65535, distance back from the output
//     match length = token nibble + 4 (+ extension)
//
// The last sequence carries only literals; an encoder keeps the final five
// bytes literal and starts no match within the last twelve, which is what
// lets a decoder's match copy run eight bytes at a time without a bounds
// check in the common case. The decoder here checks anyway: every stream
// it sees may come from a damaged or hostile file.

#include <nepheline/asset/lz.h>
#include <nepheline/core/assert.h>

#include <string.h>

#define LZ_MIN_MATCH     4U
#define LZ_LAST_LITERALS 5U  // the final bytes of a block are literals
#define LZ_MF_LIMIT      12U // no match starts within this many bytes of the end
#define LZ_ML_MASK       15U
#define LZ_RUN_MASK      15U

#define LZ_FAST_HASH_BITS 14U
#define LZ_FAST_TABLE     (1U << LZ_FAST_HASH_BITS)
#define LZ_SKIP_TRIGGER   6U // after 64 failed probes the step grows by one

#define LZ_HC_HASH_BITS    16U
#define LZ_HC_TABLE        (1U << LZ_HC_HASH_BITS)
#define LZ_HC_CHAIN        (1U << 16U) // one slot per position in the window
#define LZ_HC_CHAIN_MASK   (LZ_HC_CHAIN - 1U)
#define LZ_HC_MAX_ATTEMPTS 64U
#define LZ_HC_NONE         0xFFFFFFFFU

static u32 read32(const u8 *p)
{
    u32 v;
    memcpy(&v, p, sizeof v);
    return v;
}

static u64 read64(const u8 *p)
{
    u64 v;
    memcpy(&v, p, sizeof v);
    return v;
}

static u32 hash_bits(u32 v, u32 bits)
{
    return (v * 2654435761U) >> (32U - bits);
}

u64 nph_lz_compress_bound(u64 size)
{
    if (size > NPH_LZ_MAX_INPUT_SIZE) {
        return 0U;
    }
    return size + size / 255U + 16U;
}

u64 nph_lz_workspace_size(NphLzLevel level)
{
    if (level == NPH_LZ_LEVEL_HIGH) {
        return ((u64)LZ_HC_TABLE + (u64)LZ_HC_CHAIN) * sizeof(u32);
    }
    return (u64)LZ_FAST_TABLE * sizeof(u32);
}

// ---------------------------------------------------------------------------
// Emitting sequences
// ---------------------------------------------------------------------------

typedef struct Emitter {
    u8 *op;
    u8 *oend;
} Emitter;

// Bytes needed to encode a length past the nibble's 15.
static u64 ext_bytes(u64 value)
{
    return value < 15U ? 0U : 1U + (value - 15U) / 255U;
}

static void write_ext(u8 **op, u64 value)
{
    u64 rem = value - 15U;
    while (rem >= 255U) {
        *(*op)++ = 255U;
        rem -= 255U;
    }
    *(*op)++ = (u8)rem;
}

// Emits `lit_len` literals from `literals` and, when `match_len` is not
// zero, a match of that length at `offset`. false when the output is full.
static bool emit(Emitter *e, const u8 *literals, u64 lit_len, u32 offset, u64 match_len)
{
    const u64 ml_code = match_len != 0U ? match_len - LZ_MIN_MATCH : 0U;
    const u64 need =
        1U + ext_bytes(lit_len) + lit_len + (match_len != 0U ? 2U + ext_bytes(ml_code) : 0U);
    if ((u64)(e->oend - e->op) < need) {
        return false;
    }
    u8 *op = e->op;
    u8 *token = op++;
    if (lit_len >= LZ_RUN_MASK) {
        *token = (u8)(LZ_RUN_MASK << 4U);
        write_ext(&op, lit_len);
    } else {
        *token = (u8)(lit_len << 4U);
    }
    if (lit_len != 0U) {
        memcpy(op, literals, (size_t)lit_len);
        op += lit_len;
    }
    if (match_len != 0U) {
        op[0] = (u8)(offset & 0xFFU);
        op[1] = (u8)(offset >> 8U);
        op += 2;
        if (ml_code >= LZ_ML_MASK) {
            *token |= (u8)LZ_ML_MASK;
            write_ext(&op, ml_code);
        } else {
            *token |= (u8)ml_code;
        }
    }
    e->op = op;
    return true;
}

// Number of equal bytes at p and q, with p stopping at `limit`.
static u64 count_match(const u8 *p, const u8 *q, const u8 *limit)
{
    const u8 *start = p;
    while (p + 8 <= limit) {
        const u64 diff = read64(p) ^ read64(q);
        if (diff != 0U) {
            return (u64)(p - start) + (nph_ctz_u64(diff) >> 3U);
        }
        p += 8;
        q += 8;
    }
    while (p < limit && *p == *q) {
        ++p;
        ++q;
    }
    return (u64)(p - start);
}

// ---------------------------------------------------------------------------
// FAST: one hash-table candidate per position, greedy, with the skip
// strategy that makes incompressible input cheap (the probe step grows
// after repeated misses).
// ---------------------------------------------------------------------------

static NphResult compress_fast(const u8 *src, u64 size, Emitter *e, u32 *table)
{
    const u8 *ip = src;
    const u8 *anchor = src;
    const u8 *const iend = src + size;
    const u8 *const mflimit = iend - LZ_MF_LIMIT;
    const u8 *const matchlimit = iend - LZ_LAST_LITERALS;

    if (size < LZ_MF_LIMIT + 1U) {
        goto last_literals;
    }
    memset(table, 0, (size_t)LZ_FAST_TABLE * sizeof(u32));
    table[hash_bits(read32(ip), LZ_FAST_HASH_BITS)] = 0U;
    ++ip;
    u32 forward_h = hash_bits(read32(ip), LZ_FAST_HASH_BITS);

    for (;;) {
        const u8 *ref;
        {
            const u8 *forward_ip = ip;
            u32 step = 1U;
            u32 search = 1U << LZ_SKIP_TRIGGER;
            do {
                const u32 h = forward_h;
                ip = forward_ip;
                forward_ip += step;
                step = search++ >> LZ_SKIP_TRIGGER;
                if (forward_ip > mflimit) {
                    goto last_literals;
                }
                ref = src + table[h];
                forward_h = hash_bits(read32(forward_ip), LZ_FAST_HASH_BITS);
                table[h] = (u32)(ip - src);
            } while ((u64)(ip - ref) > NPH_LZ_MAX_DISTANCE || read32(ref) != read32(ip));
        }
        // One or more matches back to back.
        for (;;) {
            while (ip > anchor && ref > src && ip[-1] == ref[-1]) {
                --ip;
                --ref;
            }
            const u64 ml =
                LZ_MIN_MATCH + count_match(ip + LZ_MIN_MATCH, ref + LZ_MIN_MATCH, matchlimit);
            if (!emit(e, anchor, (u64)(ip - anchor), (u32)(ip - ref), ml)) {
                return NPH_RESULT_OVERFLOW;
            }
            ip += ml;
            anchor = ip;
            if (ip >= mflimit) {
                goto last_literals;
            }
            table[hash_bits(read32(ip - 2), LZ_FAST_HASH_BITS)] = (u32)(ip - 2 - src);
            const u32 h = hash_bits(read32(ip), LZ_FAST_HASH_BITS);
            ref = src + table[h];
            table[h] = (u32)(ip - src);
            if ((u64)(ip - ref) > NPH_LZ_MAX_DISTANCE || read32(ref) != read32(ip)) {
                break;
            }
        }
        forward_h = hash_bits(read32(++ip), LZ_FAST_HASH_BITS);
    }

last_literals:
    if (!emit(e, anchor, (u64)(iend - anchor), 0U, 0U)) {
        return NPH_RESULT_OVERFLOW;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// HIGH: hash chains over the whole window (every position is inserted), the
// longest of up to LZ_HC_MAX_ATTEMPTS candidates, and one step of lazy
// matching (a match is deferred when the next position starts a longer
// one). This is the structure of LZ4 HC and of zlib's deflate_slow.
// ---------------------------------------------------------------------------

typedef struct HcState {
    const u8 *src;
    u32 *head;  // hash -> newest position with that hash
    u32 *chain; // position & mask -> the previous position with the same hash
    const u8 *matchlimit;
} HcState;

static void hc_insert(HcState *s, u32 pos)
{
    const u32 h = hash_bits(read32(s->src + pos), LZ_HC_HASH_BITS);
    s->chain[pos & LZ_HC_CHAIN_MASK] = s->head[h];
    s->head[h] = pos;
}

// The longest match for the position `pos` among the chain's candidates;
// 0 when none reaches LZ_MIN_MATCH.
static u64 hc_find(const HcState *s, u32 pos, u32 *out_ref)
{
    const u8 *ip = s->src + pos;
    const u32 h = hash_bits(read32(ip), LZ_HC_HASH_BITS);
    u32 cand = s->head[h];
    if (cand == pos) {
        cand = s->chain[pos & LZ_HC_CHAIN_MASK]; // the position itself was just inserted
    }
    u64 best = 0U;
    u32 attempts = LZ_HC_MAX_ATTEMPTS;
    while (cand != LZ_HC_NONE && pos - cand <= NPH_LZ_MAX_DISTANCE && attempts-- != 0U) {
        const u8 *ref = s->src + cand;
        if (ref[best] == ip[best] && read32(ref) == read32(ip)) {
            const u64 len =
                LZ_MIN_MATCH + count_match(ip + LZ_MIN_MATCH, ref + LZ_MIN_MATCH, s->matchlimit);
            if (len > best) {
                best = len;
                *out_ref = cand;
            }
        }
        const u32 next = s->chain[cand & LZ_HC_CHAIN_MASK];
        if (next >= cand) {
            break; // the slot was reused by a newer position: the chain ends here
        }
        cand = next;
    }
    return best;
}

static NphResult compress_high(const u8 *src, u64 size, Emitter *e, u32 *workspace)
{
    const u8 *const iend = src + size;
    const u8 *anchor = src;
    if (size < LZ_MF_LIMIT + 1U) {
        goto last_literals;
    }
    HcState s;
    s.src = src;
    s.head = workspace;
    s.chain = workspace + LZ_HC_TABLE;
    s.matchlimit = iend - LZ_LAST_LITERALS;
    memset(s.head, 0xFF, (size_t)LZ_HC_TABLE * sizeof(u32));
    memset(s.chain, 0xFF, (size_t)LZ_HC_CHAIN * sizeof(u32));

    const u32 mflimit = (u32)(size - LZ_MF_LIMIT);
    u32 ip = 0U;
    u32 next_insert = 0U; // every position below this is in the chains
    while (ip < mflimit) {
        while (next_insert <= ip) {
            hc_insert(&s, next_insert++);
        }
        u32 ref = 0U;
        u64 len = hc_find(&s, ip, &ref);
        if (len < LZ_MIN_MATCH) {
            ++ip;
            continue;
        }
        // Lazy evaluation: a longer match one byte later wins, and the
        // skipped byte joins the literals.
        while (ip + 1U < mflimit) {
            while (next_insert <= ip + 1U) {
                hc_insert(&s, next_insert++);
            }
            u32 ref1 = 0U;
            const u64 len1 = hc_find(&s, ip + 1U, &ref1);
            if (len1 > len) {
                ++ip;
                len = len1;
                ref = ref1;
            } else {
                break;
            }
        }
        // Extend backwards into the pending literals.
        while (ip > (u32)(anchor - src) && ref > 0U && src[ip - 1U] == src[ref - 1U]) {
            --ip;
            --ref;
            ++len;
        }
        const u8 *const start = src + ip;
        if (!emit(e, anchor, (u64)(start - anchor), ip - ref, len)) {
            return NPH_RESULT_OVERFLOW;
        }
        // Keep the dictionary complete across the match, so later matches
        // can reference its interior.
        const u32 end = (u32)(ip + len);
        const u32 insert_end = end < mflimit ? end : mflimit;
        while (next_insert < insert_end) {
            hc_insert(&s, next_insert++);
        }
        ip = end;
        anchor = src + ip;
    }

last_literals:
    if (!emit(e, anchor, (u64)(iend - anchor), 0U, 0U)) {
        return NPH_RESULT_OVERFLOW;
    }
    return NPH_RESULT_OK;
}

NphResult nph_lz_compress(const void *src, u64 src_size, void *dst, u64 dst_cap, NphLzLevel level,
                          void *workspace, u64 *out_size)
{
    NPH_ASSERT(dst != NULL && workspace != NULL && out_size != NULL);
    NPH_ASSERT(src != NULL || src_size == 0U);
    if (src_size > NPH_LZ_MAX_INPUT_SIZE) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Emitter e;
    e.op = (u8 *)dst;
    e.oend = (u8 *)dst + dst_cap;
    const u8 *in = src != NULL ? (const u8 *)src : (const u8 *)"";
    const NphResult r = level == NPH_LZ_LEVEL_HIGH
                            ? compress_high(in, src_size, &e, (u32 *)workspace)
                            : compress_fast(in, src_size, &e, (u32 *)workspace);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    *out_size = (u64)(e.op - (u8 *)dst);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

NphResult nph_lz_decompress(const void *src, u64 src_size, void *dst, u64 dst_size)
{
    NPH_ASSERT(src != NULL || src_size == 0U);
    NPH_ASSERT(dst != NULL || dst_size == 0U);
    const u8 *ip = (const u8 *)src;
    const u8 *const iend = ip + src_size;
    u8 *op = (u8 *)dst;
    u8 *const obase = op;
    u8 *const oend = op + dst_size;

    for (;;) {
        if (ip >= iend) {
            return NPH_RESULT_CORRUPT; // every block has at least one token
        }
        const u32 token = *ip++;
        u64 lit = token >> 4U;
        if (lit == LZ_RUN_MASK) {
            u8 b;
            do {
                if (ip >= iend) {
                    return NPH_RESULT_CORRUPT;
                }
                b = *ip++;
                lit += b;
            } while (b == 255U);
        }
        if ((u64)(iend - ip) < lit || (u64)(oend - op) < lit) {
            return NPH_RESULT_CORRUPT;
        }
        if (lit != 0U) {
            memcpy(op, ip, (size_t)lit);
            op += lit;
            ip += lit;
        }
        if (ip == iend) {
            break; // the final, literal-only sequence
        }
        if ((u64)(iend - ip) < 2U) {
            return NPH_RESULT_CORRUPT;
        }
        const u32 offset = (u32)ip[0] | ((u32)ip[1] << 8U);
        ip += 2;
        if (offset == 0U || offset > (u64)(op - obase)) {
            return NPH_RESULT_CORRUPT;
        }
        u64 ml = (token & LZ_ML_MASK) + LZ_MIN_MATCH;
        if ((token & LZ_ML_MASK) == LZ_ML_MASK) {
            u8 b;
            do {
                if (ip >= iend) {
                    return NPH_RESULT_CORRUPT;
                }
                b = *ip++;
                ml += b;
            } while (b == 255U);
        }
        if ((u64)(oend - op) < ml) {
            return NPH_RESULT_CORRUPT;
        }
        const u8 *match = op - offset;
        if (offset < 8U) {
            // A short offset repeats a pattern of period `offset`. The
            // first bytes are copied one at a time (each may read a byte
            // the copy just wrote); once a whole number of periods of at
            // least eight bytes exists, the source can point that far back
            // and the copy proceeds eight bytes at a time like any other.
            const u64 period8 = (u64)offset * ((8U + offset - 1U) / offset);
            u64 head = ml < period8 ? ml : period8;
            ml -= head;
            while (head != 0U) {
                *op++ = *match++;
                --head;
            }
            match = op - period8;
        }
        // Source and destination never overlap within one 8-byte step.
        while (ml >= 8U) {
            memcpy(op, match, 8U);
            op += 8;
            match += 8;
            ml -= 8U;
        }
        while (ml != 0U) {
            *op++ = *match++;
            --ml;
        }
    }
    return op == oend ? NPH_RESULT_OK : NPH_RESULT_CORRUPT;
}

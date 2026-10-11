// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/bitset.h>

#include <string.h>

// Mask of the valid bits in the last word (all ones when the count is a
// multiple of 64, or when there are no words).
static u64 last_word_mask(const NphBitset *b)
{
    const u32 rem = b->bit_count & 63U;
    return rem == 0U ? NPH_U64_MAX : (((u64)1U << rem) - 1U);
}

static void trim(NphBitset *b)
{
    if (b->word_count > 0U) {
        b->words[b->word_count - 1U] &= last_word_mask(b);
    }
}

NphResult nph_bitset_init(NphBitset *b, NphAllocator *alloc, u32 bit_count)
{
    NPH_ASSERT(b != NULL && alloc != NULL);
    memset(b, 0, sizeof *b);
    b->alloc = alloc;
    return nph_bitset_resize(b, bit_count);
}

void nph_bitset_free(NphBitset *b)
{
    if (b == NULL || b->alloc == NULL) {
        return;
    }
    if (b->words != NULL) {
        NPH_FREE_ARRAY(b->alloc, u64, b->words, b->word_count);
    }
    memset(b, 0, sizeof *b);
}

NphResult nph_bitset_resize(NphBitset *b, u32 bit_count)
{
    NPH_ASSERT(b != NULL && b->alloc != NULL);
    const u32 word_count = NPH_BITSET_WORDS(bit_count);
    if (word_count != b->word_count) {
        u64 *words = NULL;
        if (word_count > 0U) {
            words = (u64 *)NPH_REALLOC(b->alloc, b->words, (u64)b->word_count * sizeof(u64),
                                       (u64)word_count * sizeof(u64), NPH_ALIGNOF(u64));
            if (words == NULL) {
                return NPH_RESULT_OUT_OF_MEMORY;
            }
            if (word_count > b->word_count) {
                memset(words + b->word_count, 0,
                       (size_t)(word_count - b->word_count) * sizeof(u64));
            }
        } else if (b->words != NULL) {
            NPH_FREE_ARRAY(b->alloc, u64, b->words, b->word_count);
        }
        b->words = words;
        b->word_count = word_count;
    }
    b->bit_count = bit_count;
    trim(b);
    return NPH_RESULT_OK;
}

void nph_bitset_set_all(NphBitset *b)
{
    for (u32 i = 0; i < b->word_count; ++i) {
        b->words[i] = NPH_U64_MAX;
    }
    trim(b);
}

void nph_bitset_clear_all(NphBitset *b)
{
    if (b->word_count > 0U) {
        memset(b->words, 0, (size_t)b->word_count * sizeof(u64));
    }
}

u32 nph_bitset_count(const NphBitset *b)
{
    u32 count = 0U;
    for (u32 i = 0; i < b->word_count; ++i) {
        count += nph_popcount_u64(b->words[i]);
    }
    return count;
}

bool nph_bitset_any(const NphBitset *b)
{
    for (u32 i = 0; i < b->word_count; ++i) {
        if (b->words[i] != 0U) {
            return true;
        }
    }
    return false;
}

bool nph_bitset_none(const NphBitset *b)
{
    return !nph_bitset_any(b);
}

bool nph_bitset_all(const NphBitset *b)
{
    if (b->word_count == 0U) {
        return true;
    }
    for (u32 i = 0; i + 1U < b->word_count; ++i) {
        if (b->words[i] != NPH_U64_MAX) {
            return false;
        }
    }
    return b->words[b->word_count - 1U] == last_word_mask(b);
}

u32 nph_bitset_find_next_set(const NphBitset *b, u32 from)
{
    if (from >= b->bit_count) {
        return NPH_U32_MAX;
    }
    u32 w = from >> 6U;
    u64 word = b->words[w] & (NPH_U64_MAX << (from & 63U));
    for (;;) {
        if (word != 0U) {
            return (w << 6U) + nph_ctz_u64(word);
        }
        if (++w >= b->word_count) {
            return NPH_U32_MAX;
        }
        word = b->words[w];
    }
}

u32 nph_bitset_find_next_clear(const NphBitset *b, u32 from)
{
    if (from >= b->bit_count) {
        return NPH_U32_MAX;
    }
    u32 w = from >> 6U;
    u64 word = ~b->words[w] & (NPH_U64_MAX << (from & 63U));
    for (;;) {
        if (word != 0U) {
            const u32 index = (w << 6U) + nph_ctz_u64(word);
            return index < b->bit_count ? index : NPH_U32_MAX;
        }
        if (++w >= b->word_count) {
            return NPH_U32_MAX;
        }
        word = ~b->words[w];
    }
}

#define BITSET_BINARY_OP(name, expr)                                                               \
    void name(NphBitset *dst, const NphBitset *src)                                                \
    {                                                                                              \
        NPH_ASSERT(dst->bit_count == src->bit_count);                                              \
        if (dst->bit_count != src->bit_count) {                                                    \
            return;                                                                                \
        }                                                                                          \
        for (u32 i = 0; i < dst->word_count; ++i) {                                                \
            const u64 a = dst->words[i];                                                           \
            const u64 b = src->words[i];                                                           \
            dst->words[i] = (expr);                                                                \
        }                                                                                          \
    }

BITSET_BINARY_OP(nph_bitset_and, a &b)
BITSET_BINARY_OP(nph_bitset_or, a | b)
BITSET_BINARY_OP(nph_bitset_xor, a ^ b)
BITSET_BINARY_OP(nph_bitset_and_not, a & ~b)

void nph_bitset_not(NphBitset *b)
{
    for (u32 i = 0; i < b->word_count; ++i) {
        b->words[i] = ~b->words[i];
    }
    trim(b);
}

bool nph_bitset_equals(const NphBitset *a, const NphBitset *b)
{
    if (a->bit_count != b->bit_count) {
        return false;
    }
    for (u32 i = 0; i < a->word_count; ++i) {
        if (a->words[i] != b->words[i]) {
            return false;
        }
    }
    return true;
}

void nph_bitset_copy(NphBitset *dst, const NphBitset *src)
{
    NPH_ASSERT(dst->bit_count == src->bit_count);
    if (dst->bit_count != src->bit_count) {
        return;
    }
    if (dst->word_count > 0U) {
        memcpy(dst->words, src->words, (size_t)dst->word_count * sizeof(u64));
    }
}

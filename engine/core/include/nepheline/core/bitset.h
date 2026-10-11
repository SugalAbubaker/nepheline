// SPDX-License-Identifier: Apache-2.0
#pragma once

// Dynamic bitset (CLAUDE.md §8.1 "Bitset and sparse set").
//
// Bits are stored in 64-bit words, least significant bit first. Invariant:
// the bits of the last word beyond `bit_count` are always zero, so counting
// and comparing operate on whole words. Set operations between two bitsets
// require equal sizes (asserted).
//
// Thread-safety: none.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphBitset {
    u64 *words;
    u32 bit_count;
    u32 word_count;
    NphAllocator *alloc;
} NphBitset;

// Number of u64 words needed for `bits` bits, for callers with fixed storage.
#define NPH_BITSET_WORDS(bits) (((bits) + 63U) / 64U)

// Allocates `bit_count` zeroed bits (0 is allowed; nothing is allocated).
NPH_MUST_USE NphResult nph_bitset_init(NphBitset *b, NphAllocator *alloc, u32 bit_count);
void nph_bitset_free(NphBitset *b);

// Changes the size; new bits are zero, dropped bits are gone.
NPH_MUST_USE NphResult nph_bitset_resize(NphBitset *b, u32 bit_count);

NPH_INLINE bool nph_bitset_test(const NphBitset *b, u32 index)
{
    return (b->words[index >> 6U] >> (index & 63U)) & 1U;
}
NPH_INLINE void nph_bitset_set(NphBitset *b, u32 index)
{
    b->words[index >> 6U] |= (u64)1U << (index & 63U);
}
NPH_INLINE void nph_bitset_clear(NphBitset *b, u32 index)
{
    b->words[index >> 6U] &= ~((u64)1U << (index & 63U));
}
NPH_INLINE void nph_bitset_flip(NphBitset *b, u32 index)
{
    b->words[index >> 6U] ^= (u64)1U << (index & 63U);
}
NPH_INLINE void nph_bitset_assign(NphBitset *b, u32 index, bool value)
{
    if (value) {
        nph_bitset_set(b, index);
    } else {
        nph_bitset_clear(b, index);
    }
}

void nph_bitset_set_all(NphBitset *b);
void nph_bitset_clear_all(NphBitset *b);

// Number of set bits.
u32 nph_bitset_count(const NphBitset *b);
bool nph_bitset_any(const NphBitset *b);
bool nph_bitset_none(const NphBitset *b);
bool nph_bitset_all(const NphBitset *b);

// Index of the first set bit at or after `from`, or NPH_U32_MAX.
u32 nph_bitset_find_next_set(const NphBitset *b, u32 from);
// Index of the first clear bit at or after `from`, or NPH_U32_MAX.
u32 nph_bitset_find_next_clear(const NphBitset *b, u32 from);
NPH_INLINE u32 nph_bitset_find_first_set(const NphBitset *b)
{
    return nph_bitset_find_next_set(b, 0U);
}
NPH_INLINE u32 nph_bitset_find_first_clear(const NphBitset *b)
{
    return nph_bitset_find_next_clear(b, 0U);
}

// In-place set operations on equal-sized bitsets: dst = dst OP src.
void nph_bitset_and(NphBitset *dst, const NphBitset *src);
void nph_bitset_or(NphBitset *dst, const NphBitset *src);
void nph_bitset_xor(NphBitset *dst, const NphBitset *src);
void nph_bitset_and_not(NphBitset *dst, const NphBitset *src);
void nph_bitset_not(NphBitset *b);

bool nph_bitset_equals(const NphBitset *a, const NphBitset *b);

// Copies `src` into `dst` (same size).
void nph_bitset_copy(NphBitset *dst, const NphBitset *src);

// Iterates set bits: for (u32 i = first_set; i != NPH_U32_MAX; i = next_set(i + 1)).
// (`index` is a declarator and cannot be parenthesised.)
// NOLINTBEGIN(bugprone-macro-parentheses)
#define NPH_BITSET_FOREACH_SET(index, b)                                                           \
    for (u32 index = nph_bitset_find_first_set(b); index != NPH_U32_MAX;                           \
         index = nph_bitset_next_set_after((b), index))
// NOLINTEND(bugprone-macro-parentheses)

// The set bit after `index`, or NPH_U32_MAX; safe at the last bit.
NPH_INLINE u32 nph_bitset_next_set_after(const NphBitset *b, u32 index)
{
    return index + 1U < b->bit_count ? nph_bitset_find_next_set(b, index + 1U) : NPH_U32_MAX;
}

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/hash.h>

#include "test_support.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Regression vectors. These pin the output of nph_hash64 and nph_crc32c:
// cooked assets depend on them (hash.h). If a vector changes, the asset
// format version changes with it.
// ---------------------------------------------------------------------------

typedef struct HashVector {
    const char *input;
    u64 seed;
    u64 expected;
} HashVector;

// Recorded from the first implementation (2026-10-11, MSVC x64); every
// platform must reproduce them exactly.
static const HashVector g_hash_vectors[] = {
    {"", 0U, 0x7B4CCDCEFE90894CULL},
    {"", NPH_HASH_DEFAULT_SEED, 0x411C93B9BCA3ED3AULL},
    {"a", 0U, 0x943DB8306D803398ULL},
    {"abc", 0U, 0x834B087AACAB5094ULL},
    {"message digest", 0U, 0x491AC5EEDACE5058ULL},
    {"abcdefghijklmnop", 0U, 0x637305800D88373DULL},
    {"abcdefghijklmnopq", 0U, 0x6C4803A35122813BULL},
    {"The quick brown fox jumps over the lazy dog", 0U, 0x57C55C4DB52C6985ULL},
    {"The quick brown fox jumps over the lazy dog", NPH_HASH_DEFAULT_SEED, 0xF01C3FDC8A981C4FULL},
    {"0123456789012345678901234567890123456789012345678901234567890123456789", 7U,
     0xEA0E9338CABB8FB5ULL},
};

NTEST(hash, regression_vectors)
{
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_hash_vectors); ++i) {
        const HashVector *v = &g_hash_vectors[i];
        const u64 h = nph_hash64(v->input, strlen(v->input), v->seed);
        NTEST_LOG("{\"%s\", %lluU, 0x%016llXULL},", v->input, (unsigned long long)v->seed,
                  (unsigned long long)h);
        NTEST_ASSERT_EQ_UINT(h, v->expected);
    }
}

NTEST(hash, lengths_do_not_collide_on_zero_input)
{
    u8 zeros[512];
    memset(zeros, 0, sizeof zeros);
    u64 seen[512];
    for (u32 len = 0; len < 512U; ++len) {
        seen[len] = nph_hash64(zeros, len, 0U);
        for (u32 j = 0; j < len; ++j) {
            NTEST_ASSERT_NE_UINT(seen[len], seen[j]);
        }
    }
}

NTEST(hash, seed_changes_output)
{
    const char *s = "same input";
    NTEST_ASSERT_NE_UINT(nph_hash64(s, strlen(s), 1U), nph_hash64(s, strlen(s), 2U));
    NTEST_ASSERT_NE_UINT(nph_hash64(NULL, 0U, 1U), nph_hash64(NULL, 0U, 2U));
    NTEST_ASSERT_EQ_UINT(nph_hash64(s, strlen(s), 9U), nph_hash64_cstr(s, 9U));
    NTEST_ASSERT_EQ_UINT(nph_hash64(NULL, 0U, 9U), nph_hash64_cstr(NULL, 9U));
    NTEST_ASSERT_EQ_UINT(nph_hash64(NULL, 0U, 9U), nph_hash64_cstr("", 9U));
}

NTEST(hash, prefix_and_alignment_independence)
{
    // The hash of a buffer must not depend on where it sits in memory.
    u8 buffer[300];
    for (u32 i = 0; i < sizeof buffer; ++i) {
        buffer[i] = (u8)(i * 7U + 3U);
    }
    for (u32 len = 0; len < 100U; ++len) {
        const u64 h0 = nph_hash64(buffer, len, 5U);
        u8 moved[300];
        for (u32 shift = 1; shift < 9U; ++shift) {
            memcpy(moved + shift, buffer, len);
            NTEST_ASSERT_EQ_UINT(nph_hash64(moved + shift, len, 5U), h0);
        }
    }
}

// Flip every input bit of random inputs and check each output bit flips
// about half the time: the standard avalanche test (Mulvey's SMHasher).
static void avalanche(u64 len, u32 samples)
{
    u64 rng = 0xA5A5A5A5ULL + len;
    u32 flips[64];
    memset(flips, 0, sizeof flips);
    u32 trials = 0U;
    u8 input[256];
    for (u32 s = 0; s < samples; ++s) {
        for (u64 i = 0; i < len; ++i) {
            input[i] = (u8)test_rng_next(&rng);
        }
        const u64 base = nph_hash64(input, len, 0U);
        for (u64 bit = 0; bit < len * 8U; ++bit) {
            input[bit / 8U] ^= (u8)(1U << (bit % 8U));
            const u64 diff = base ^ nph_hash64(input, len, 0U);
            input[bit / 8U] ^= (u8)(1U << (bit % 8U));
            for (u32 o = 0; o < 64U; ++o) {
                flips[o] += (u32)((diff >> o) & 1U);
            }
            ++trials;
        }
    }
    for (u32 o = 0; o < 64U; ++o) {
        const double rate = (double)flips[o] / (double)trials;
        if (rate < 0.42 || rate > 0.58) {
            NTEST_FAIL("avalanche: length %llu, output bit %u flips with rate %.3f",
                       (unsigned long long)len, o, rate);
        }
    }
}

NTEST(hash, avalanche_short_inputs)
{
    avalanche(1U, 2000U);
    avalanche(3U, 1000U);
    avalanche(4U, 600U);
    avalanche(7U, 400U);
    avalanche(8U, 400U);
    avalanche(13U, 300U);
    avalanche(16U, 300U);
}

NTEST(hash, avalanche_long_inputs)
{
    avalanche(17U, 200U);
    avalanche(32U, 150U);
    avalanche(48U, 100U);
    avalanche(49U, 100U);
    avalanche(100U, 60U);
    avalanche(200U, 40U);
}

NTEST(hash, bucket_distribution_of_sequential_keys)
{
    // Sequential integers as bytes, into 1024 buckets: no bucket may be
    // more than 2x the mean (chi-square would be stricter; this catches
    // structural failures).
    u32 buckets[1024];
    memset(buckets, 0, sizeof buckets);
    const u32 n = 65536U;
    for (u32 i = 0; i < n; ++i) {
        const u64 h = nph_hash64(&i, sizeof i, NPH_HASH_DEFAULT_SEED);
        buckets[h & 1023U] += 1U;
    }
    const u32 mean = n / 1024U;
    for (u32 b = 0; b < 1024U; ++b) {
        NTEST_ASSERT_LT_UINT(buckets[b], mean * 2U);
        NTEST_ASSERT_GT_UINT(buckets[b], mean / 2U);
    }
}

NTEST(hash, integer_mixers_are_bijective_on_a_range)
{
    // No two of 1<<16 consecutive inputs may collide in the low 16 bits more
    // than chance allows... stronger: the full 64-bit outputs are all distinct.
    static u64 outputs[1U << 16U];
    for (u32 i = 0; i < (1U << 16U); ++i) {
        outputs[i] = nph_hash_mix_u64(i);
    }
    // Sort-free distinctness via a small open-addressing set.
    static u64 table[1U << 18U];
    memset(table, 0, sizeof table);
    for (u32 i = 0; i < (1U << 16U); ++i) {
        const u64 v = outputs[i] | 1U; // 0 marks empty; the low bit is irrelevant here
        u32 slot = (u32)(v >> 46U);
        while (table[slot] != 0U) {
            NTEST_ASSERT_NE_UINT(table[slot], v);
            slot = (slot + 1U) & ((1U << 18U) - 1U);
        }
        table[slot] = v;
    }
    NTEST_ASSERT_NE_UINT(nph_hash_mix_u32(1U), nph_hash_mix_u32(2U));
    NTEST_ASSERT_EQ_UINT(nph_hash_mix_u32(7U), nph_hash_mix_u64(7U));
}

NTEST(hash, mixer_avalanche)
{
    u32 flips[64];
    memset(flips, 0, sizeof flips);
    u64 rng = 42U;
    const u32 samples = 4000U;
    for (u32 s = 0; s < samples; ++s) {
        const u64 x = test_rng_next(&rng);
        const u64 base = nph_hash_mix_u64(x);
        for (u32 bit = 0; bit < 64U; ++bit) {
            const u64 diff = base ^ nph_hash_mix_u64(x ^ ((u64)1U << bit));
            for (u32 o = 0; o < 64U; ++o) {
                flips[o] += (u32)((diff >> o) & 1U);
            }
        }
    }
    for (u32 o = 0; o < 64U; ++o) {
        const double rate = (double)flips[o] / (double)(samples * 64U);
        if (rate < 0.45 || rate > 0.55) {
            NTEST_FAIL("mixer avalanche: output bit %u flips with rate %.3f", o, rate);
        }
    }
}

NTEST(hash, combine_and_fold)
{
    NTEST_ASSERT_NE_UINT(nph_hash_combine(1U, 2U), nph_hash_combine(2U, 1U));
    NTEST_ASSERT_NE_UINT(nph_hash_combine(0U, 0U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_hash_fold_u32(0x00000001FFFFFFFFULL), 0xFFFFFFFEU);
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

NTEST(crc32c, standard_vectors)
{
    // RFC 3720 (iSCSI) appendix B.4 and the common "123456789" check value.
    NTEST_ASSERT_EQ_UINT(nph_crc32c("123456789", 9U, 0U), 0xE3069283U);
    NTEST_ASSERT_EQ_UINT(nph_crc32c_portable("123456789", 9U, 0U), 0xE3069283U);
    u8 block[32];
    memset(block, 0, sizeof block);
    NTEST_ASSERT_EQ_UINT(nph_crc32c(block, 32U, 0U), 0x8A9136AAU);
    NTEST_ASSERT_EQ_UINT(nph_crc32c_portable(block, 32U, 0U), 0x8A9136AAU);
    memset(block, 0xFF, sizeof block);
    NTEST_ASSERT_EQ_UINT(nph_crc32c(block, 32U, 0U), 0x62A8AB43U);
    for (u32 i = 0; i < 32U; ++i) {
        block[i] = (u8)i;
    }
    NTEST_ASSERT_EQ_UINT(nph_crc32c(block, 32U, 0U), 0x46DD794EU);
    for (u32 i = 0; i < 32U; ++i) {
        block[i] = (u8)(31U - i);
    }
    NTEST_ASSERT_EQ_UINT(nph_crc32c(block, 32U, 0U), 0x113FDB5CU);
    NTEST_ASSERT_EQ_UINT(nph_crc32c(NULL, 0U, 0U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_crc32c(NULL, 0U, 0x12345678U), 0x12345678U);
}

NTEST(crc32c, hardware_matches_portable_and_is_incremental)
{
    u8 data[1000];
    u64 rng = 99U;
    for (u32 i = 0; i < sizeof data; ++i) {
        data[i] = (u8)test_rng_next(&rng);
    }
    for (u32 len = 0; len < 300U; ++len) {
        for (u32 offset = 0; offset < 9U; ++offset) {
            const u32 whole = nph_crc32c(data + offset, len, 0U);
            NTEST_ASSERT_EQ_UINT(whole, nph_crc32c_portable(data + offset, len, 0U));
            const u32 split = len / 3U;
            const u32 first = nph_crc32c(data + offset, split, 0U);
            NTEST_ASSERT_EQ_UINT(nph_crc32c(data + offset + split, len - split, first), whole);
        }
    }
}

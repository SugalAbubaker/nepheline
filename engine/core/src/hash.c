// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/hash.h>

#include <string.h>

#if NPH_ARCH_X64
#if NPH_COMPILER_MSVC
#include <intrin.h>
#else
#include <nmmintrin.h>
#endif
#endif

// ---------------------------------------------------------------------------
// nph_hash64
// ---------------------------------------------------------------------------

// Secret constants: odd, with a balanced bit count, from the first bits of
// the golden ratio, sqrt(2), sqrt(3), and sqrt(5).
#define HASH_SECRET_0 0x9E3779B97F4A7C15ULL
#define HASH_SECRET_1 0x6A09E667F3BCC909ULL
#define HASH_SECRET_2 0xBB67AE8584CAA73BULL
#define HASH_SECRET_3 0x3C6EF372FE94F82BULL

// 64x64 -> 128 multiply, both halves.
static void hash_mul128(u64 *a, u64 *b)
{
#if NPH_COMPILER_MSVC && NPH_ARCH_X64
    u64 hi;
    const u64 lo = _umul128(*a, *b, &hi);
    *a = lo;
    *b = hi;
#elif NPH_COMPILER_MSVC
    const u64 hi = __umulh(*a, *b);
    const u64 lo = *a * *b;
    *a = lo;
    *b = hi;
#else
    __extension__ typedef unsigned __int128 hash_u128;
    const hash_u128 r = (hash_u128)*a * (hash_u128)*b;
    *a = (u64)r;
    *b = (u64)(r >> 64U);
#endif
}

static u64 hash_mix(u64 a, u64 b)
{
    hash_mul128(&a, &b);
    return a ^ b;
}

// Unaligned little-endian reads. memcpy compiles to a single load.
static u64 hash_read8(const u8 *p)
{
    u64 v;
    memcpy(&v, p, 8U);
    return v;
}

static u64 hash_read4(const u8 *p)
{
    u32 v;
    memcpy(&v, p, 4U);
    return v;
}

// 1..3 bytes: first, middle, last (with overlap).
static u64 hash_read3(const u8 *p, u64 len)
{
    return ((u64)p[0] << 16U) | ((u64)p[len >> 1U] << 8U) | (u64)p[len - 1U];
}

u64 nph_hash64(const void *data, u64 len, u64 seed)
{
    const u8 *p = (const u8 *)data;
    seed ^= hash_mix(seed ^ HASH_SECRET_0, HASH_SECRET_1);
    u64 a;
    u64 b;

    if (NPH_LIKELY(len <= 16U)) {
        if (NPH_LIKELY(len >= 4U)) {
            // Two overlapping 4-byte reads from each end cover 4..16 bytes.
            a = (hash_read4(p) << 32U) | hash_read4(p + ((len >> 3U) << 2U));
            b = (hash_read4(p + len - 4U) << 32U) | hash_read4(p + len - 4U - ((len >> 3U) << 2U));
        } else if (NPH_LIKELY(len > 0U)) {
            a = hash_read3(p, len);
            b = 0U;
        } else {
            a = 0U;
            b = 0U;
        }
    } else {
        u64 i = len;
        if (NPH_UNLIKELY(i > 48U)) {
            u64 see1 = seed;
            u64 see2 = seed;
            do {
                seed = hash_mix(hash_read8(p) ^ HASH_SECRET_1, hash_read8(p + 8) ^ seed);
                see1 = hash_mix(hash_read8(p + 16) ^ HASH_SECRET_2, hash_read8(p + 24) ^ see1);
                see2 = hash_mix(hash_read8(p + 32) ^ HASH_SECRET_3, hash_read8(p + 40) ^ see2);
                p += 48;
                i -= 48U;
            } while (NPH_LIKELY(i > 48U));
            seed ^= see1 ^ see2;
        }
        while (NPH_UNLIKELY(i > 16U)) {
            seed = hash_mix(hash_read8(p) ^ HASH_SECRET_1, hash_read8(p + 8) ^ seed);
            i -= 16U;
            p += 16;
        }
        // The last 16 bytes, overlapping the previous block when i < 16.
        a = hash_read8(p + i - 16U);
        b = hash_read8(p + i - 8U);
    }

    a ^= HASH_SECRET_1;
    b ^= seed;
    hash_mul128(&a, &b);
    return hash_mix(a ^ HASH_SECRET_0 ^ len, b ^ HASH_SECRET_1);
}

u64 nph_hash64_cstr(const char *s, u64 seed)
{
    return nph_hash64(s, s != NULL ? (u64)strlen(s) : 0U, seed);
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

// Byte-at-a-time table for the reflected Castagnoli polynomial, built on
// first use (256 entries). A slice-by-8 table would be faster but the
// portable path only runs where the hardware instruction is unavailable.
static u32 g_crc32c_table[256];
static bool g_crc32c_table_ready;

static void crc32c_build_table(void)
{
    for (u32 i = 0; i < 256U; ++i) {
        u32 c = i;
        for (u32 k = 0; k < 8U; ++k) {
            c = (c & 1U) ? (0x82F63B78U ^ (c >> 1U)) : (c >> 1U);
        }
        g_crc32c_table[i] = c;
    }
    g_crc32c_table_ready = true;
}

u32 nph_crc32c_portable(const void *data, u64 len, u32 crc)
{
    // Benign race: every thread that builds the table writes identical values.
    if (!g_crc32c_table_ready) {
        crc32c_build_table();
    }
    const u8 *p = (const u8 *)data;
    crc = ~crc;
    for (u64 i = 0; i < len; ++i) {
        crc = g_crc32c_table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

u32 nph_crc32c(const void *data, u64 len, u32 crc)
{
#if NPH_ARCH_X64
    // SSE4.2 is the CPU baseline (CLAUDE.md §3), so the instruction is used
    // unconditionally: eight bytes per step, then the tail.
    const u8 *p = (const u8 *)data;
    u64 c = ~(u64)crc;
    while (len >= 8U) {
        u64 v;
        memcpy(&v, p, 8U);
        c = _mm_crc32_u64(c, v);
        p += 8;
        len -= 8U;
    }
    u32 c32 = (u32)c;
    while (len > 0U) {
        c32 = _mm_crc32_u8(c32, *p);
        ++p;
        --len;
    }
    return ~c32;
#else
    return nph_crc32c_portable(data, len, crc);
#endif
}

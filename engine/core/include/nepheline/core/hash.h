// SPDX-License-Identifier: Apache-2.0
#pragma once

// Hashing (CLAUDE.md §8.1): a 64-bit non-cryptographic hash for hash maps
// and string IDs, integer mixers, and CRC32C for file integrity.
//
// STABILITY: nph_hash64 and nph_crc32c are part of the asset format. Cooked
// files store string IDs computed with nph_hash64 and checksums computed
// with nph_crc32c, so their output must never change for a given input, on
// any platform. They are pinned by regression vectors in test_hash.c; a
// change to either is a new asset format version.
//
// nph_hash64 follows the wyhash construction (Wang Yi, 2019-2023): a
// 64x64->128-bit multiply whose halves are folded is the mixing primitive,
// 48 bytes are consumed per step with three independent lanes, and short
// inputs are read with overlapping loads so there is no byte loop. The
// implementation and constants are our own. It is not cryptographic and
// not resistant to adversarial inputs; the seed defeats accidental
// collisions, not attacks.
//
// Thread-safety: every function is pure and NPH_THREADSAFE.

#include <nepheline/core/base.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hashes `len` bytes at `data` with `seed`. data may be NULL when len == 0.
u64 nph_hash64(const void *data, u64 len, u64 seed);

// Default seed for engine-wide hashing (string IDs, hash maps).
#define NPH_HASH_DEFAULT_SEED 0x4E65706865ULL // "Nephe"

// Hashes a NUL-terminated string (without the terminator).
u64 nph_hash64_cstr(const char *s, u64 seed);

// Bijective 64-bit integer mixer: cheap, full avalanche, never collides for
// distinct inputs. For hash maps keyed by integers and handles.
NPH_INLINE u64 nph_hash_mix_u64(u64 x)
{
    // moremur (Pelle Evensen, 2019): stronger finaliser than splitmix64's.
    x ^= x >> 27;
    x *= 0x3C79AC492BA7B653ULL;
    x ^= x >> 33;
    x *= 0x1C69B3F74AC4AE35ULL;
    x ^= x >> 27;
    return x;
}

NPH_INLINE u64 nph_hash_mix_u32(u32 x)
{
    return nph_hash_mix_u64((u64)x);
}

// Folds `b` into `a`, order-dependent (combine(a, b) != combine(b, a)).
NPH_INLINE u64 nph_hash_combine(u64 a, u64 b)
{
    return nph_hash_mix_u64(a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6U) + (a >> 2U)));
}

// Reduces a 64-bit hash to a 32-bit one.
NPH_INLINE u32 nph_hash_fold_u32(u64 h)
{
    return (u32)(h ^ (h >> 32U));
}

// CRC-32C (Castagnoli polynomial 0x1EDC6F41, reflected 0x82F63B78), the
// variant with hardware support on x86-64 (SSE4.2) and ARMv8. Incremental:
// pass the previous result as `crc` (0 to start); the same bytes in one or
// several calls give the same result. Output matches the IETF/iSCSI CRC32C.
u32 nph_crc32c(const void *data, u64 len, u32 crc);

// Table-driven reference implementation of nph_crc32c, for platforms
// without the instruction and for cross-checking. Same contract.
u32 nph_crc32c_portable(const void *data, u64 len, u32 crc);

#ifdef __cplusplus
}
#endif

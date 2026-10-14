// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/guid.h>
#include <nepheline/core/assert.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

// Two seeds for the two words of a derived GUID. They are part of the
// asset format: cooked references depend on them, so they never change.
#define GUID_SEED_LO 0x4E50484755494431ULL // "NPHGUID1"
#define GUID_SEED_HI 0x4E50484755494432ULL // "NPHGUID2"

static NphGuid guid_non_null(NphGuid g)
{
    if (nph_guid_is_null(g)) {
        g.lo = 1U; // 2^-128 of inputs land here; the null GUID stays "none"
    }
    return g;
}

NphGuid nph_guid_from_str(NphStr s)
{
    NphGuid g;
    g.lo = nph_hash64(s.data, s.len, GUID_SEED_LO);
    g.hi = nph_hash64(s.data, s.len, GUID_SEED_HI);
    return guid_non_null(g);
}

NphGuid nph_guid_from_cstr(const char *s)
{
    return nph_guid_from_str(nph_str_from_cstr(s));
}

void nph_guid_to_string(NphGuid g, char out[NPH_GUID_STRING_LEN + 1U])
{
    static const char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < 16U; ++i) {
        out[i] = digits[(g.hi >> (60U - 4U * i)) & 0xFU];
        out[16U + i] = digits[(g.lo >> (60U - 4U * i)) & 0xFU];
    }
    out[NPH_GUID_STRING_LEN] = '\0';
}

static bool hex_value(char c, u32 *out)
{
    if (c >= '0' && c <= '9') {
        *out = (u32)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (u32)(c - 'a') + 10U;
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (u32)(c - 'A') + 10U;
        return true;
    }
    return false;
}

bool nph_guid_parse(NphStr s, NphGuid *out)
{
    NPH_ASSERT(out != NULL);
    u64 words[2] = {0U, 0U};
    u32 digits = 0U;
    for (u64 i = 0; i < s.len; ++i) {
        const char c = s.data[i];
        if (c == '-') {
            continue;
        }
        u32 v;
        if (!hex_value(c, &v) || digits >= NPH_GUID_STRING_LEN) {
            return false;
        }
        words[digits / 16U] = (words[digits / 16U] << 4U) | v;
        ++digits;
    }
    if (digits != NPH_GUID_STRING_LEN) {
        return false;
    }
    out->hi = words[0];
    out->lo = words[1];
    return true;
}

// ---------------------------------------------------------------------------
// Random GUIDs: a PCG32-style generator (O'Neill 2014) over two 64-bit
// outputs per GUID; the same construction as math/random.h, kept local so
// the seeding here (which reads clocks) stays out of the pure math module.
// ---------------------------------------------------------------------------

static u32 generator_next_u32(NphGuidGenerator *g)
{
    const u64 old = g->state;
    g->state = old * 6364136223846793005ULL + g->inc;
    const u32 xorshifted = (u32)(((old >> 18U) ^ old) >> 27U);
    const u32 rot = (u32)(old >> 59U);
    return (xorshifted >> rot) | (xorshifted << ((32U - rot) & 31U));
}

void nph_guid_generator_seed(NphGuidGenerator *g, u64 seed)
{
    NPH_ASSERT(g != NULL);
    g->state = 0U;
    g->inc = (nph_hash_mix_u64(seed ^ 0xDA3E39CB94B95BDBULL) << 1U) | 1U;
    (void)generator_next_u32(g);
    g->state += nph_hash_mix_u64(seed);
    (void)generator_next_u32(g);
}

void nph_guid_generator_init(NphGuidGenerator *g)
{
    static NphAtomicU64 counter = NPH_ATOMIC_U64_INIT(0U);
    const u64 serial = nph_atomic_fetch_add_u64(&counter, 1U, NPH_MEMORY_ORDER_RELAXED);
    u64 seed = nph_time_wall_ns();
    seed = nph_hash_combine(seed, nph_time_cycles());
    seed = nph_hash_combine(seed, nph_platform_process_id());
    seed = nph_hash_combine(seed, serial);
    seed = nph_hash_combine(seed, (u64)(uptr)g);
    nph_guid_generator_seed(g, seed);
}

NphGuid nph_guid_generate(NphGuidGenerator *g)
{
    NPH_ASSERT(g != NULL);
    NphGuid out;
    out.lo = ((u64)generator_next_u32(g) << 32U) | generator_next_u32(g);
    out.hi = ((u64)generator_next_u32(g) << 32U) | generator_next_u32(g);
    return guid_non_null(out);
}

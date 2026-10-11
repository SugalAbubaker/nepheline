// SPDX-License-Identifier: Apache-2.0
// Hashing and container benchmarks (docs/core.md).
//
//   hash64_<N>B            nph_hash64 over N bytes (ns per call; bytes/ns in the label)
//   crc32c_<N>B            CRC-32C over N bytes
//   mix_u64                the integer mixer
//   hashmap_insert_<N>     insert N distinct u64 keys into an empty map (ns per insert)
//   hashmap_find_hit_<N>   look up every key of an N-entry map
//   hashmap_find_miss_<N>  look up N absent keys
//   hashmap_remove_<N>     remove every key of an N-entry map
//   array_push_u32         push into a dynamic array (amortised, including growth)
//   sparse_set_insert_get  insert then get 4096 keys
//   bitset_count_1M        popcount of a million-bit set

#include <nepheline/core/array.h>
#include <nepheline/core/bitset.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/sparse_set.h>

#include <nbench/nbench.h>
#include <stdlib.h>
#include <string.h>

static u8 g_data[65536];
static bool g_data_ready;

static void prepare_data(void)
{
    if (g_data_ready) {
        return;
    }
    u64 s = 0xDA7AULL;
    for (u32 i = 0; i < sizeof g_data; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        g_data[i] = (u8)(s >> 56U);
    }
    g_data_ready = true;
}

#define DEFINE_HASH_BENCH(bytes)                                                                   \
    NBENCH(hash64_##bytes##B)                                                                      \
    {                                                                                              \
        prepare_data();                                                                            \
        nbench_set_label(state, #bytes " bytes");                                                  \
        u64 acc = 0U;                                                                              \
        while (nbench_keep_running(state)) {                                                       \
            acc += nph_hash64(g_data, bytes, acc);                                                 \
        }                                                                                          \
        nbench_do_not_optimize_u64(acc);                                                           \
    }                                                                                              \
    NBENCH(crc32c_##bytes##B)                                                                      \
    {                                                                                              \
        prepare_data();                                                                            \
        nbench_set_label(state, #bytes " bytes");                                                  \
        u32 acc = 0U;                                                                              \
        while (nbench_keep_running(state)) {                                                       \
            acc = nph_crc32c(g_data, bytes, acc);                                                  \
        }                                                                                          \
        nbench_do_not_optimize_u64(acc);                                                           \
    }

DEFINE_HASH_BENCH(8)
DEFINE_HASH_BENCH(16)
DEFINE_HASH_BENCH(32)
DEFINE_HASH_BENCH(64)
DEFINE_HASH_BENCH(256)
DEFINE_HASH_BENCH(4096)
DEFINE_HASH_BENCH(65536)

NBENCH(mix_u64)
{
    u64 acc = 1U;
    while (nbench_keep_running(state)) {
        acc = nph_hash_mix_u64(acc);
    }
    nbench_do_not_optimize_u64(acc);
}

// ---------------------------------------------------------------------------
// Hash map
// ---------------------------------------------------------------------------

static u64 key_of(u32 i)
{
    return (u64)i * 0x9E3779B97F4A7C15ULL; // distinct, spread keys
}

static void map_fill(NphHashMap *m, u32 n)
{
    for (u32 i = 0; i < (n); ++i) {
        const u32 v = i;
        if (nph_hashmap_insert_u64(m, key_of(i), &v) != NPH_RESULT_OK) {
            abort();
        }
    }
}

#define DEFINE_MAP_BENCH(n)                                                                        \
    NBENCH(hashmap_insert_##n)                                                                     \
    {                                                                                              \
        nbench_set_items_per_iteration(state, (n));                                                \
        while (nbench_keep_running(state)) {                                                       \
            nbench_pause_timing(state);                                                            \
            NphHashMap m;                                                                          \
            const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));                         \
            if (nph_hashmap_init(&m, nph_allocator_system(), &desc) != NPH_RESULT_OK) {            \
                abort();                                                                           \
            }                                                                                      \
            nbench_resume_timing(state);                                                           \
            map_fill(&m, (n));                                                                     \
            nbench_pause_timing(state);                                                            \
            nph_hashmap_free(&m);                                                                  \
            nbench_resume_timing(state);                                                           \
        }                                                                                          \
    }                                                                                              \
    NBENCH(hashmap_find_hit_##n)                                                                   \
    {                                                                                              \
        NphHashMap m;                                                                              \
        const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));                             \
        if (nph_hashmap_init(&m, nph_allocator_system(), &desc) != NPH_RESULT_OK) {                \
            abort();                                                                               \
        }                                                                                          \
        map_fill(&m, (n));                                                                         \
        nbench_set_items_per_iteration(state, (n));                                                \
        u64 acc = 0U;                                                                              \
        while (nbench_keep_running(state)) {                                                       \
            for (u32 i = 0; i < (n); ++i) {                                                        \
                const u32 *v = (const u32 *)nph_hashmap_find_u64(&m, key_of(i));                   \
                acc += v != NULL ? *v : 0U;                                                        \
            }                                                                                      \
        }                                                                                          \
        nbench_do_not_optimize_u64(acc);                                                           \
        nph_hashmap_free(&m);                                                                      \
    }                                                                                              \
    NBENCH(hashmap_find_miss_##n)                                                                  \
    {                                                                                              \
        NphHashMap m;                                                                              \
        const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));                             \
        if (nph_hashmap_init(&m, nph_allocator_system(), &desc) != NPH_RESULT_OK) {                \
            abort();                                                                               \
        }                                                                                          \
        map_fill(&m, (n));                                                                         \
        nbench_set_items_per_iteration(state, (n));                                                \
        u64 acc = 0U;                                                                              \
        while (nbench_keep_running(state)) {                                                       \
            for (u32 i = 0; i < (n); ++i) {                                                        \
                acc += nph_hashmap_find_u64(&m, key_of(i) + 1U) != NULL ? 1U : 0U;                 \
            }                                                                                      \
        }                                                                                          \
        nbench_do_not_optimize_u64(acc);                                                           \
        nph_hashmap_free(&m);                                                                      \
    }                                                                                              \
    NBENCH(hashmap_remove_##n)                                                                     \
    {                                                                                              \
        nbench_set_items_per_iteration(state, (n));                                                \
        NphHashMap m;                                                                              \
        const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));                             \
        if (nph_hashmap_init(&m, nph_allocator_system(), &desc) != NPH_RESULT_OK) {                \
            abort();                                                                               \
        }                                                                                          \
        while (nbench_keep_running(state)) {                                                       \
            nbench_pause_timing(state);                                                            \
            nph_hashmap_clear(&m);                                                                 \
            map_fill(&m, (n));                                                                     \
            nbench_resume_timing(state);                                                           \
            for (u32 i = 0; i < (n); ++i) {                                                        \
                nph_hashmap_remove_u64(&m, key_of(i));                                             \
            }                                                                                      \
        }                                                                                          \
        nph_hashmap_free(&m);                                                                      \
    }

DEFINE_MAP_BENCH(1000)
DEFINE_MAP_BENCH(100000)
DEFINE_MAP_BENCH(1000000)

// ---------------------------------------------------------------------------
// Array, sparse set, bitset
// ---------------------------------------------------------------------------

NBENCH(array_push_u32)
{
    nbench_set_items_per_iteration(state, 100000U);
    while (nbench_keep_running(state)) {
        NphArrayU32 arr;
        NPH_ARRAY_INIT(&arr, nph_allocator_system());
        for (u32 i = 0; i < 100000U; ++i) {
            if (NPH_ARRAY_PUSH(&arr, i) != NPH_RESULT_OK) {
                abort();
            }
        }
        nbench_do_not_optimize(arr.data);
        NPH_ARRAY_FREE(&arr);
    }
}

NBENCH(sparse_set_insert_get)
{
    nbench_set_items_per_iteration(state, (u64)4096U * 2U);
    NphSparseSet s;
    nph_sparse_set_init(&s, nph_allocator_system(), sizeof(u32));
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        nph_sparse_set_clear(&s);
        for (u32 i = 0; i < 4096U; ++i) {
            const u32 key = (i * 7919U) & 0xFFFFU;
            if (nph_sparse_set_set(&s, key, &i) != NPH_RESULT_OK) {
                abort();
            }
        }
        for (u32 i = 0; i < 4096U; ++i) {
            const u32 *v = (const u32 *)nph_sparse_set_get(&s, (i * 7919U) & 0xFFFFU);
            acc += v != NULL ? *v : 0U;
        }
    }
    nbench_do_not_optimize_u64(acc);
    nph_sparse_set_free(&s);
}

NBENCH(bitset_count_1M)
{
    NphBitset b;
    if (nph_bitset_init(&b, nph_allocator_system(), 1000000U) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < 1000000U; i += 3U) {
        nph_bitset_set(&b, i);
    }
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_bitset_count(&b);
        acc += nph_bitset_find_next_set(&b, 500000U);
    }
    nbench_do_not_optimize_u64(acc);
    nph_bitset_free(&b);
}

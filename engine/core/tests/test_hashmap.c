// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/hashmap.h>

#include "test_support.h"

#include <string.h>

typedef struct MapFixture {
    NphDebugAllocator da;
    NphAllocator *a;
    NphHashMap map; // u64 -> u32
} MapFixture;

NTEST_SETUP(MapFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&f->map, f->a, &desc), NPH_RESULT_OK);
}

NTEST_TEARDOWN(MapFixture)
{
    nph_hashmap_free(&f->map);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(MapFixture, empty_map_allocates_nothing)
{
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), 0U);
    NTEST_ASSERT_EQ_UINT(f->map.capacity, 0U);
    NTEST_ASSERT_NULL(nph_hashmap_find_u64(&f->map, 1U));
    NTEST_ASSERT_FALSE(nph_hashmap_contains_u64(&f->map, 1U));
    NTEST_ASSERT_FALSE(nph_hashmap_remove_u64(&f->map, 1U));
    nph_hashmap_clear(&f->map);
    NphHashMapIter it = nph_hashmap_iter(&f->map);
    NTEST_ASSERT_FALSE(nph_hashmap_next(&it, NULL, NULL));
    NphDebugAllocStats s;
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.total_allocations, 0U);
    // Slot layout for u64 -> u32: value at 8, slot 16 bytes.
    NTEST_ASSERT_EQ_UINT(f->map.val_offset, 8U);
    NTEST_ASSERT_EQ_UINT(f->map.slot_size, 16U);
}

NTEST_F(MapFixture, insert_find_set_remove)
{
    const u32 v1 = 10U;
    const u32 v2 = 20U;
    NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, 1U, &v1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, 1U, &v2), NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), 1U);
    NTEST_ASSERT_EQ_UINT(f->map.capacity, 16U);
    u32 *found = NPH_HASHMAP_FIND(&f->map, u32, &(u64){1U});
    NTEST_ASSERT_NOT_NULL(found);
    NTEST_ASSERT_EQ_UINT(*found, 10U);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(found, 4U));
    *found = 11U;
    NTEST_ASSERT_EQ_UINT(*(const u32 *)nph_hashmap_find_u64(&f->map, 1U), 11U);

    NTEST_ASSERT_EQ_INT(nph_hashmap_set_u64(&f->map, 1U, &v2), NPH_RESULT_OK); // overwrite
    NTEST_ASSERT_EQ_UINT(*(const u32 *)nph_hashmap_find_u64(&f->map, 1U), 20U);
    NTEST_ASSERT_EQ_INT(nph_hashmap_set_u64(&f->map, 2U, &v1), NPH_RESULT_OK); // insert
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), 2U);
    NTEST_ASSERT_TRUE(nph_hashmap_contains_u64(&f->map, 2U));
    NTEST_ASSERT_FALSE(nph_hashmap_contains_u64(&f->map, 3U));

    NTEST_ASSERT_TRUE(nph_hashmap_remove_u64(&f->map, 1U));
    NTEST_ASSERT_FALSE(nph_hashmap_remove_u64(&f->map, 1U));
    NTEST_ASSERT_NULL(nph_hashmap_find_u64(&f->map, 1U));
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), 1U);
    NTEST_ASSERT_EQ_UINT(*(const u32 *)nph_hashmap_find_u64(&f->map, 2U), 10U);
    // The key is reinsertable after removal (tombstone reuse).
    NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, 1U, &v1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), 2U);
}

NTEST_F(MapFixture, grows_and_keeps_every_entry)
{
    const u32 n = 100000U;
    for (u32 i = 0; i < n; ++i) {
        const u32 v = i * 2U;
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, (u64)i * 7919U, &v), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), n);
    // Load factor stays at or below 7/8.
    NTEST_ASSERT_LE_UINT(n, f->map.capacity - f->map.capacity / 8U);
    NTEST_ASSERT_GE_UINT(n, f->map.capacity / 4U);
    for (u32 i = 0; i < n; ++i) {
        const u32 *v = (const u32 *)nph_hashmap_find_u64(&f->map, (u64)i * 7919U);
        NTEST_ASSERT_NOT_NULL(v);
        NTEST_ASSERT_EQ_UINT(*v, i * 2U);
    }
    NTEST_ASSERT_NULL(nph_hashmap_find_u64(&f->map, 1U));
    NTEST_ASSERT_NULL(nph_hashmap_find_u64(&f->map, (u64)n * 7919U));
    // Iteration visits each entry exactly once.
    u64 key_sum = 0U;
    u64 val_sum = 0U;
    u32 count = 0U;
    NphHashMapIter it = nph_hashmap_iter(&f->map);
    const void *key;
    void *val;
    while (nph_hashmap_next(&it, &key, &val)) {
        u64 k;
        memcpy(&k, key, sizeof k);
        key_sum += k;
        val_sum += *(const u32 *)val;
        ++count;
    }
    NTEST_ASSERT_EQ_UINT(count, n);
    NTEST_ASSERT_EQ_UINT(key_sum, (u64)n * (n - 1U) / 2U * 7919U);
    NTEST_ASSERT_EQ_UINT(val_sum, (u64)n * (n - 1U));
}

NTEST_F(MapFixture, remove_half_then_reinsert_reclaims_tombstones)
{
    const u32 n = 5000U;
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, i, &i), NPH_RESULT_OK);
    }
    const u32 cap_before = f->map.capacity;
    for (u32 i = 0; i < n; i += 2U) {
        NTEST_ASSERT_TRUE(nph_hashmap_remove_u64(&f->map, i));
    }
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), n / 2U);
    for (u32 i = 1; i < n; i += 2U) {
        NTEST_ASSERT_EQ_UINT(*(const u32 *)nph_hashmap_find_u64(&f->map, i), i);
    }
    for (u32 i = 0; i < n; i += 2U) {
        NTEST_ASSERT_NULL(nph_hashmap_find_u64(&f->map, i));
    }
    // Churn: repeatedly remove and reinsert; the table must not grow
    // because tombstones are reclaimed by in-place rehashes.
    for (u32 round = 0; round < 20U; ++round) {
        for (u32 i = 0; i < n; i += 2U) {
            const u32 v = i + round;
            NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, i, &v), NPH_RESULT_OK);
        }
        for (u32 i = 0; i < n; i += 2U) {
            NTEST_ASSERT_EQ_UINT(*(const u32 *)nph_hashmap_find_u64(&f->map, i), i + round);
            NTEST_ASSERT_TRUE(nph_hashmap_remove_u64(&f->map, i));
        }
    }
    NTEST_ASSERT_EQ_UINT(f->map.capacity, cap_before);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), n / 2U);
}

NTEST_F(MapFixture, reserve_prevents_rehash_and_clear_keeps_storage)
{
    NTEST_ASSERT_EQ_INT(nph_hashmap_reserve(&f->map, 1000U), NPH_RESULT_OK);
    const u32 cap = f->map.capacity;
    NTEST_ASSERT_GE_UINT(cap - cap / 8U, 1000U);
    const u8 *ctrl = f->map.ctrl;
    for (u32 i = 0; i < 1000U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, i, &i), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_PTR(f->map.ctrl, ctrl);
    NTEST_ASSERT_EQ_UINT(f->map.capacity, cap);
    NTEST_ASSERT_EQ_INT(nph_hashmap_reserve(&f->map, 10U), NPH_RESULT_OK); // no-op
    NTEST_ASSERT_EQ_PTR(f->map.ctrl, ctrl);
    nph_hashmap_clear(&f->map);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), 0U);
    NTEST_ASSERT_EQ_UINT(f->map.capacity, cap);
    NTEST_ASSERT_NULL(nph_hashmap_find_u64(&f->map, 5U));
    for (u32 i = 0; i < 1000U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&f->map, i + 5000U, &i), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_PTR(f->map.ctrl, ctrl);
}

// Model check against a sorted-array reference with random operations.
NTEST_F(MapFixture, randomized_against_reference)
{
    enum {
        KEYS = 512
    };
    static u32 reference[KEYS];
    static bool present[KEYS];
    memset(present, 0, sizeof present);
    u64 rng = 0x51A7E5ULL;
    for (u32 op = 0; op < 60000U; ++op) {
        const u32 k = (u32)test_rng_below(&rng, KEYS);
        const u64 key = (u64)k * 0x9E3779B97F4A7C15ULL; // spread keys
        switch (test_rng_below(&rng, 4U)) {
        case 0:
        case 1: {
            const u32 v = (u32)test_rng_next(&rng);
            NTEST_ASSERT_EQ_INT(nph_hashmap_set_u64(&f->map, key, &v), NPH_RESULT_OK);
            reference[k] = v;
            present[k] = true;
            break;
        }
        case 2:
            NTEST_ASSERT_EQ_INT(nph_hashmap_remove_u64(&f->map, key), present[k]);
            present[k] = false;
            break;
        default: {
            const u32 *v = (const u32 *)nph_hashmap_find_u64(&f->map, key);
            if (present[k]) {
                NTEST_ASSERT_NOT_NULL(v);
                NTEST_ASSERT_EQ_UINT(*v, reference[k]);
            } else {
                NTEST_ASSERT_NULL(v);
            }
            break;
        }
        }
    }
    u32 expected_len = 0U;
    for (u32 k = 0; k < KEYS; ++k) {
        expected_len += present[k] ? 1U : 0U;
        const u32 *v = (const u32 *)nph_hashmap_find_u64(&f->map, (u64)k * 0x9E3779B97F4A7C15ULL);
        NTEST_ASSERT_EQ_INT(v != NULL, present[k]);
    }
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->map), expected_len);
}

// String keys through the default byte hash and memcmp, with 16-byte keys.
typedef struct Name {
    char text[16];
} Name;

static Name make_name(u32 i)
{
    Name n;
    memset(&n, 0, sizeof n);
    n.text[0] = 'n';
    for (u32 d = 0; d < 8U; ++d) {
        n.text[1U + d] = (char)('a' + ((i >> (d * 4U)) & 0xFU));
    }
    return n;
}

NTEST(hashmap, byte_keys_with_default_hash_and_values_of_different_sizes)
{
    NphHashMap map;
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = sizeof(Name);
    desc.val_size = sizeof(f64);
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&map, nph_allocator_system(), &desc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(map.val_offset, 16U);
    NTEST_ASSERT_EQ_UINT(map.slot_size, 32U);
    for (u32 i = 0; i < 2000U; ++i) {
        const Name key = make_name(i);
        const f64 value = (f64)i * 0.5;
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert(&map, &key, &value), NPH_RESULT_OK);
    }
    for (u32 i = 0; i < 2000U; ++i) {
        const Name key = make_name(i);
        const f64 *v = NPH_HASHMAP_FIND(&map, f64, &key);
        NTEST_ASSERT_NOT_NULL(v);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(v, 8U));
        NTEST_ASSERT_TRUE(*v == (f64)i * 0.5);
    }
    const Name missing = make_name(2000U);
    NTEST_ASSERT_NULL(nph_hashmap_find(&map, &missing));
    nph_hashmap_free(&map);
    nph_hashmap_free(&map); // idempotent
}

NTEST(hashmap, set_semantics_with_zero_value_size)
{
    NphHashMap set;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(0U);
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&set, nph_allocator_system(), &desc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(set.slot_size, 8U);
    NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&set, 42U, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_hashmap_insert_u64(&set, 42U, NULL), NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_TRUE(nph_hashmap_contains_u64(&set, 42U));
    NTEST_ASSERT_NOT_NULL(nph_hashmap_find_u64(&set, 42U));
    NTEST_ASSERT_FALSE(nph_hashmap_contains_u64(&set, 43U));
    nph_hashmap_free(&set);
}

NTEST(hashmap, small_key_and_value_layout)
{
    NphHashMap map;
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = 4U;
    desc.val_size = 1U;
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&map, nph_allocator_system(), &desc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(map.val_offset, 4U);
    NTEST_ASSERT_EQ_UINT(map.slot_size, 8U);
    for (u32 i = 0; i < 300U; ++i) {
        const u8 v = (u8)i;
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert(&map, &i, &v), NPH_RESULT_OK);
    }
    for (u32 i = 0; i < 300U; ++i) {
        NTEST_ASSERT_EQ_UINT(*(const u8 *)nph_hashmap_find(&map, &i), (u8)i);
    }
    nph_hashmap_free(&map);

    desc.key_size = 0U;
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&map, nph_allocator_system(), &desc),
                        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(hashmap, allocation_failure_is_reported_and_harmless)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[2048];
    nph_arena_init(&arena, memory, sizeof memory);
    NphHashMap map;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u64));
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&map, nph_arena_allocator(&arena), &desc), NPH_RESULT_OK);
    u32 inserted = 0U;
    for (u32 i = 0; i < 1000U; ++i) {
        const u64 v = i;
        const NphResult r = nph_hashmap_insert_u64(&map, i, &v);
        if (r != NPH_RESULT_OK) {
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
            break;
        }
        ++inserted;
    }
    NTEST_ASSERT_GT_UINT(inserted, 14U);
    NTEST_ASSERT_LT_UINT(inserted, 1000U);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&map), inserted);
    for (u32 i = 0; i < inserted; ++i) {
        NTEST_ASSERT_EQ_UINT(*(const u64 *)nph_hashmap_find_u64(&map, i), (u64)i);
    }
    NTEST_ASSERT_EQ_INT(nph_hashmap_reserve(&map, 100000U), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&map), inserted);
    nph_hashmap_free(&map);
    nph_arena_destroy(&arena);
}

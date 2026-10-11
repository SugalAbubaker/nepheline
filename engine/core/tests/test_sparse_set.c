// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/sparse_set.h>

#include "test_support.h"

#include <string.h>

typedef struct Pos {
    f32 x, y, z;
} Pos;

typedef struct SparseFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} SparseFixture;

NTEST_SETUP(SparseFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(SparseFixture)
{
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(SparseFixture, empty_set_allocates_nothing)
{
    NphSparseSet s;
    nph_sparse_set_init(&s, f->a, sizeof(Pos));
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 0U);
    NTEST_ASSERT_FALSE(nph_sparse_set_contains(&s, 0U));
    NTEST_ASSERT_FALSE(nph_sparse_set_contains(&s, NPH_U32_MAX));
    NTEST_ASSERT_NULL(nph_sparse_set_get(&s, 5U));
    NTEST_ASSERT_FALSE(nph_sparse_set_remove(&s, 5U));
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_position(&s, 5U), NPH_U32_MAX);
    nph_sparse_set_clear(&s);
    NphDebugAllocStats st;
    nph_debug_allocator_get_stats(&f->da, &st);
    NTEST_ASSERT_EQ_UINT(st.total_allocations, 0U);
    nph_sparse_set_free(&s);
    nph_sparse_set_free(&s);
}

NTEST_F(SparseFixture, insert_get_remove_with_values)
{
    NphSparseSet s;
    nph_sparse_set_init(&s, f->a, sizeof(Pos));
    const Pos p1 = {1.0F, 2.0F, 3.0F};
    const Pos p2 = {4.0F, 5.0F, 6.0F};
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 10U, &p1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 10U, &p2), NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 1000U, &p2), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 2U);
    NTEST_ASSERT_TRUE(nph_sparse_set_contains(&s, 10U));
    NTEST_ASSERT_TRUE(nph_sparse_set_contains(&s, 1000U));
    NTEST_ASSERT_FALSE(nph_sparse_set_contains(&s, 11U));
    NTEST_ASSERT_FALSE(nph_sparse_set_contains(&s, 100000U));
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_position(&s, 10U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_position(&s, 1000U), 1U);
    const Pos *got = (const Pos *)nph_sparse_set_get(&s, 1000U);
    NTEST_ASSERT_NOT_NULL(got);
    NTEST_ASSERT_TRUE(got->y == 5.0F);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_keys(&s)[1], 1000U);
    NTEST_ASSERT_EQ_PTR(nph_sparse_set_value_at(&s, 1U), got);
    NTEST_ASSERT_EQ_PTR(nph_sparse_set_values(&s), nph_sparse_set_value_at(&s, 0U));

    NTEST_ASSERT_EQ_INT(nph_sparse_set_set(&s, 10U, &p2), NPH_RESULT_OK); // overwrite
    NTEST_ASSERT_TRUE(((const Pos *)nph_sparse_set_get(&s, 10U))->x == 4.0F);
    NTEST_ASSERT_EQ_INT(nph_sparse_set_set(&s, 20U, &p1), NPH_RESULT_OK); // insert
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 3U);

    // Removing the first entry moves the last (key 20) into position 0.
    NTEST_ASSERT_TRUE(nph_sparse_set_remove(&s, 10U));
    NTEST_ASSERT_FALSE(nph_sparse_set_remove(&s, 10U));
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 2U);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_position(&s, 20U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_keys(&s)[0], 20U);
    NTEST_ASSERT_TRUE(((const Pos *)nph_sparse_set_get(&s, 20U))->x == 1.0F);
    NTEST_ASSERT_NULL(nph_sparse_set_get(&s, 10U));
    // Reinsertion after removal works and lands at the end.
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 10U, &p1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_position(&s, 10U), 2U);
    nph_sparse_set_clear(&s);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 0U);
    NTEST_ASSERT_FALSE(nph_sparse_set_contains(&s, 10U));
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 10U, &p1), NPH_RESULT_OK);
    nph_sparse_set_free(&s);
}

NTEST_F(SparseFixture, plain_set_without_values)
{
    NphSparseSet s;
    nph_sparse_set_init(&s, f->a, 0U);
    for (u32 i = 0; i < 1000U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, i * 3U, NULL), NPH_RESULT_OK);
    }
    NTEST_ASSERT_NULL(s.values);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 1000U);
    NTEST_ASSERT_NOT_NULL(nph_sparse_set_get(&s, 999U * 3U));
    NTEST_ASSERT_NULL(nph_sparse_set_get(&s, 1U));
    for (u32 i = 0; i < 1000U; i += 2U) {
        NTEST_ASSERT_TRUE(nph_sparse_set_remove(&s, i * 3U));
    }
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), 500U);
    for (u32 i = 1; i < 1000U; i += 2U) {
        NTEST_ASSERT_TRUE(nph_sparse_set_contains(&s, i * 3U));
    }
    // Every dense key is a live member and maps back to its position.
    for (u32 pos = 0; pos < nph_sparse_set_len(&s); ++pos) {
        const u32 key = nph_sparse_set_keys(&s)[pos];
        NTEST_ASSERT_EQ_UINT(nph_sparse_set_position(&s, key), pos);
    }
    nph_sparse_set_free(&s);
}

NTEST_F(SparseFixture, reserve_and_large_keys)
{
    NphSparseSet s;
    nph_sparse_set_init(&s, f->a, sizeof(u64));
    NTEST_ASSERT_EQ_INT(nph_sparse_set_reserve(&s, 5000U, 100U), NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(s.sparse_cap, 5001U);
    NTEST_ASSERT_GE_UINT(s.dense_cap, 100U);
    const u32 *sparse = s.sparse;
    const u32 *dense = s.dense;
    for (u32 i = 0; i < 100U; ++i) {
        const u64 v = i;
        NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 5000U - i, &v), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_PTR(s.sparse, sparse);
    NTEST_ASSERT_EQ_PTR(s.dense, dense);
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, NPH_U32_MAX, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_sparse_set_insert(&s, 1U << 20U, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(s.sparse_cap, (1U << 20U) + 1U);
    NTEST_ASSERT_TRUE(nph_sparse_set_contains(&s, 1U << 20U));
    NTEST_ASSERT_EQ_UINT(*(const u64 *)nph_sparse_set_get(&s, 4950U), 50U);
    nph_sparse_set_free(&s);
}

NTEST_F(SparseFixture, randomized_against_reference)
{
    enum {
        KEYS = 2048
    };
    static bool present[KEYS];
    static u32 reference[KEYS];
    memset(present, 0, sizeof present);
    NphSparseSet s;
    nph_sparse_set_init(&s, f->a, sizeof(u32));
    u64 rng = 0x5E7ULL;
    u32 live = 0U;
    for (u32 op = 0; op < 50000U; ++op) {
        const u32 key = (u32)test_rng_below(&rng, KEYS);
        switch (test_rng_below(&rng, 3U)) {
        case 0: {
            const u32 v = (u32)test_rng_next(&rng);
            const NphResult r = nph_sparse_set_insert(&s, key, &v);
            NTEST_ASSERT_EQ_INT(r, present[key] ? NPH_RESULT_ALREADY_EXISTS : NPH_RESULT_OK);
            if (!present[key]) {
                present[key] = true;
                reference[key] = v;
                ++live;
            }
            break;
        }
        case 1:
            NTEST_ASSERT_EQ_INT(nph_sparse_set_remove(&s, key), present[key]);
            if (present[key]) {
                present[key] = false;
                --live;
            }
            break;
        default: {
            const u32 *v = (const u32 *)nph_sparse_set_get(&s, key);
            NTEST_ASSERT_EQ_INT(v != NULL, present[key]);
            if (v != NULL) {
                NTEST_ASSERT_EQ_UINT(*v, reference[key]);
            }
            break;
        }
        }
        NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), live);
    }
    // The dense arrays describe exactly the present keys.
    u32 seen = 0U;
    for (u32 pos = 0; pos < nph_sparse_set_len(&s); ++pos) {
        const u32 key = nph_sparse_set_keys(&s)[pos];
        NTEST_ASSERT_TRUE(present[key]);
        NTEST_ASSERT_EQ_UINT(*(const u32 *)nph_sparse_set_value_at(&s, pos), reference[key]);
        ++seen;
    }
    NTEST_ASSERT_EQ_UINT(seen, live);
    nph_sparse_set_free(&s);
}

NTEST(sparse_set, allocation_failure)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[512];
    nph_arena_init(&arena, memory, sizeof memory);
    NphSparseSet s;
    nph_sparse_set_init(&s, nph_arena_allocator(&arena), sizeof(u64));
    u32 inserted = 0U;
    for (u32 i = 0; i < 1000U; ++i) {
        const u64 v = i;
        const NphResult r = nph_sparse_set_insert(&s, i, &v);
        if (r != NPH_RESULT_OK) {
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
            break;
        }
        ++inserted;
    }
    NTEST_ASSERT_GT_UINT(inserted, 0U);
    NTEST_ASSERT_LT_UINT(inserted, 1000U);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), inserted);
    for (u32 i = 0; i < inserted; ++i) {
        NTEST_ASSERT_EQ_UINT(*(const u64 *)nph_sparse_set_get(&s, i), (u64)i);
    }
    NTEST_ASSERT_EQ_INT(nph_sparse_set_reserve(&s, 1U << 16U, 1U), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_UINT(nph_sparse_set_len(&s), inserted);
    nph_sparse_set_free(&s);
    nph_arena_destroy(&arena);
}

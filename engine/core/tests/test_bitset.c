// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/bitset.h>

#include "test_support.h"

typedef struct BitsetFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} BitsetFixture;

NTEST_SETUP(BitsetFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(BitsetFixture)
{
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(BitsetFixture, init_zeroed_and_sized)
{
    NphBitset b;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, f->a, 130U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(b.bit_count, 130U);
    NTEST_ASSERT_EQ_UINT(b.word_count, 3U);
    NTEST_ASSERT_EQ_UINT(NPH_BITSET_WORDS(130U), 3U);
    NTEST_ASSERT_EQ_UINT(NPH_BITSET_WORDS(128U), 2U);
    NTEST_ASSERT_EQ_UINT(NPH_BITSET_WORDS(0U), 0U);
    NTEST_ASSERT_TRUE(nph_bitset_none(&b));
    NTEST_ASSERT_FALSE(nph_bitset_any(&b));
    NTEST_ASSERT_FALSE(nph_bitset_all(&b));
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&b), 0U);
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_first_set(&b), NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_first_clear(&b), 0U);
    for (u32 i = 0; i < 130U; ++i) {
        NTEST_ASSERT_FALSE(nph_bitset_test(&b, i));
    }
    nph_bitset_free(&b);
    nph_bitset_free(&b);

    NphBitset empty;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&empty, f->a, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_NULL(empty.words);
    NTEST_ASSERT_TRUE(nph_bitset_all(&empty));
    NTEST_ASSERT_TRUE(nph_bitset_none(&empty));
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_first_clear(&empty), NPH_U32_MAX);
    nph_bitset_clear_all(&empty);
    nph_bitset_set_all(&empty);
    nph_bitset_free(&empty);
}

NTEST_F(BitsetFixture, set_clear_flip_test)
{
    NphBitset b;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, f->a, 200U), NPH_RESULT_OK);
    nph_bitset_set(&b, 0U);
    nph_bitset_set(&b, 63U);
    nph_bitset_set(&b, 64U);
    nph_bitset_set(&b, 199U);
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 0U));
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 63U));
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 64U));
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 199U));
    NTEST_ASSERT_FALSE(nph_bitset_test(&b, 1U));
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&b), 4U);
    NTEST_ASSERT_TRUE(nph_bitset_any(&b));
    nph_bitset_clear(&b, 63U);
    NTEST_ASSERT_FALSE(nph_bitset_test(&b, 63U));
    nph_bitset_flip(&b, 63U);
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 63U));
    nph_bitset_flip(&b, 63U);
    nph_bitset_assign(&b, 5U, true);
    nph_bitset_assign(&b, 0U, false);
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 5U));
    NTEST_ASSERT_FALSE(nph_bitset_test(&b, 0U));
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&b), 3U);
    // Tail bits past bit_count stay zero even after set_all/not.
    nph_bitset_set_all(&b);
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&b), 200U);
    NTEST_ASSERT_TRUE(nph_bitset_all(&b));
    NTEST_ASSERT_EQ_UINT(b.words[3] >> 8U, 0U);
    nph_bitset_not(&b);
    NTEST_ASSERT_TRUE(nph_bitset_none(&b));
    nph_bitset_not(&b);
    NTEST_ASSERT_TRUE(nph_bitset_all(&b));
    nph_bitset_clear_all(&b);
    NTEST_ASSERT_TRUE(nph_bitset_none(&b));
    nph_bitset_free(&b);
}

NTEST_F(BitsetFixture, find_next_set_and_clear)
{
    NphBitset b;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, f->a, 300U), NPH_RESULT_OK);
    const u32 set_bits[] = {3U, 64U, 65U, 127U, 128U, 255U, 299U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(set_bits); ++i) {
        nph_bitset_set(&b, set_bits[i]);
    }
    u32 found = 0U;
    for (u32 i = nph_bitset_find_first_set(&b); i != NPH_U32_MAX;
         i = nph_bitset_find_next_set(&b, i + 1U)) {
        NTEST_ASSERT_LT_UINT(found, NPH_ARRAY_COUNT(set_bits));
        NTEST_ASSERT_EQ_UINT(i, set_bits[found]);
        ++found;
    }
    NTEST_ASSERT_EQ_UINT(found, NPH_ARRAY_COUNT(set_bits));
    found = 0U;
    NPH_BITSET_FOREACH_SET (i, &b) {
        NTEST_ASSERT_EQ_UINT(i, set_bits[found++]);
    }
    NTEST_ASSERT_EQ_UINT(found, NPH_ARRAY_COUNT(set_bits));
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_next_set(&b, 300U), NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_next_set(&b, 256U), 299U);
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_next_set(&b, 64U), 64U);

    nph_bitset_not(&b);
    found = 0U;
    for (u32 i = nph_bitset_find_first_clear(&b); i != NPH_U32_MAX;
         i = nph_bitset_find_next_clear(&b, i + 1U)) {
        NTEST_ASSERT_EQ_UINT(i, set_bits[found++]);
    }
    NTEST_ASSERT_EQ_UINT(found, NPH_ARRAY_COUNT(set_bits));
    // A full set has no clear bit, and the search never reports tail bits.
    nph_bitset_set_all(&b);
    NTEST_ASSERT_EQ_UINT(nph_bitset_find_first_clear(&b), NPH_U32_MAX);
    nph_bitset_free(&b);
}

NTEST_F(BitsetFixture, set_operations_and_equality)
{
    NphBitset a;
    NphBitset b;
    NphBitset c;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&a, f->a, 100U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, f->a, 100U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&c, f->a, 100U), NPH_RESULT_OK);
    for (u32 i = 0; i < 100U; i += 2U) {
        nph_bitset_set(&a, i); // evens
    }
    for (u32 i = 0; i < 100U; i += 3U) {
        nph_bitset_set(&b, i); // multiples of 3
    }
    nph_bitset_copy(&c, &a);
    NTEST_ASSERT_TRUE(nph_bitset_equals(&a, &c));
    NTEST_ASSERT_FALSE(nph_bitset_equals(&a, &b));
    nph_bitset_and(&c, &b); // multiples of 6
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&c), 17U);
    NTEST_ASSERT_TRUE(nph_bitset_test(&c, 96U));
    NTEST_ASSERT_FALSE(nph_bitset_test(&c, 98U));
    nph_bitset_copy(&c, &a);
    nph_bitset_or(&c, &b); // evens or multiples of 3: 50 + 34 - 17
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&c), 67U);
    nph_bitset_copy(&c, &a);
    nph_bitset_xor(&c, &b); // 67 - 17
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&c), 50U);
    nph_bitset_copy(&c, &a);
    nph_bitset_and_not(&c, &b); // evens not multiples of 3: 50 - 17
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&c), 33U);
    NTEST_ASSERT_FALSE(nph_bitset_test(&c, 6U));
    NTEST_ASSERT_TRUE(nph_bitset_test(&c, 4U));

    NphBitset other;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&other, f->a, 101U), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_bitset_equals(&a, &other));
    NPH_TEST_EXPECT_ASSERT(nph_bitset_and(&c, &other)); // size mismatch is rejected
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&c), 33U);
    nph_bitset_free(&other);
    nph_bitset_free(&a);
    nph_bitset_free(&b);
    nph_bitset_free(&c);
}

NTEST_F(BitsetFixture, resize_preserves_bits_and_clears_new_ones)
{
    NphBitset b;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, f->a, 10U), NPH_RESULT_OK);
    nph_bitset_set(&b, 9U);
    nph_bitset_set(&b, 2U);
    NTEST_ASSERT_EQ_INT(nph_bitset_resize(&b, 500U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(b.word_count, 8U);
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&b), 2U);
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 9U));
    NTEST_ASSERT_FALSE(nph_bitset_test(&b, 10U));
    nph_bitset_set_all(&b);
    NTEST_ASSERT_EQ_INT(nph_bitset_resize(&b, 70U), NPH_RESULT_OK); // shrink: tail trimmed
    NTEST_ASSERT_EQ_UINT(nph_bitset_count(&b), 70U);
    NTEST_ASSERT_TRUE(nph_bitset_all(&b));
    NTEST_ASSERT_EQ_INT(nph_bitset_resize(&b, 64U), NPH_RESULT_OK); // exact word boundary
    NTEST_ASSERT_EQ_UINT(b.word_count, 1U);
    NTEST_ASSERT_TRUE(nph_bitset_all(&b));
    NTEST_ASSERT_EQ_INT(nph_bitset_resize(&b, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_NULL(b.words);
    NTEST_ASSERT_EQ_INT(nph_bitset_resize(&b, 1U), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_bitset_test(&b, 0U));
    nph_bitset_free(&b);
}

NTEST(bitset, allocation_failure)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[64];
    nph_arena_init(&arena, memory, sizeof memory);
    NphBitset b;
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, nph_arena_allocator(&arena), 64U * 100U),
                        NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_INT(nph_bitset_init(&b, nph_arena_allocator(&arena), 64U * 4U), NPH_RESULT_OK);
    nph_bitset_set(&b, 255U);
    NTEST_ASSERT_EQ_INT(nph_bitset_resize(&b, 64U * 100U), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_UINT(b.bit_count, 256U);
    NTEST_ASSERT_TRUE(nph_bitset_test(&b, 255U));
    nph_bitset_free(&b);
    nph_arena_destroy(&arena);
}

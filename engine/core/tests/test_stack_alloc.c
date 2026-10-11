// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/stack_alloc.h>

#include "test_support.h"

#include <string.h>

typedef struct StackFixture {
    NPH_ALIGNAS(64) u8 memory[1024];
    NphStackAlloc stack;
} StackFixture;

NTEST_SETUP(StackFixture)
{
    nph_stack_alloc_init(&f->stack, f->memory, sizeof f->memory);
}

NTEST_TEARDOWN(StackFixture)
{
    nph_stack_alloc_destroy(&f->stack);
}

NTEST_F(StackFixture, push_pop_lifo)
{
    NTEST_ASSERT_EQ_STR(f->stack.allocator.name, "stack");
    u8 *a = (u8 *)nph_stack_alloc_push(&f->stack, 10U, 1U);
    u8 *b = (u8 *)nph_stack_alloc_push(&f->stack, 20U, 16U);
    u8 *c = (u8 *)nph_stack_alloc_push(&f->stack, 30U, 64U);
    NTEST_ASSERT_NOT_NULL(a);
    NTEST_ASSERT_NOT_NULL(b);
    NTEST_ASSERT_NOT_NULL(c);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(a, 16U)); // header alignment
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(b, 16U));
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(c, 64U));
    NTEST_ASSERT_TRUE(a < b && b < c);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_live(&f->stack), 3U);
    const u64 used_after_three = nph_stack_alloc_used(&f->stack);
    test_fill_pattern(a, 10U, 1U);
    test_fill_pattern(b, 20U, 2U);
    test_fill_pattern(c, 30U, 3U);
    NTEST_ASSERT_TRUE(test_check_pattern(a, 10U, 1U));
    NTEST_ASSERT_TRUE(test_check_pattern(b, 20U, 2U));

    nph_stack_alloc_pop(&f->stack, c);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_live(&f->stack), 2U);
    // The padding before c is reclaimed too: pushing the same again lands at c.
    NTEST_ASSERT_EQ_PTR(nph_stack_alloc_push(&f->stack, 30U, 64U), c);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_used(&f->stack), used_after_three);
    nph_stack_alloc_pop(&f->stack, c);
    nph_stack_alloc_pop(&f->stack, b);
    nph_stack_alloc_pop(&f->stack, a);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_used(&f->stack), 0U);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_live(&f->stack), 0U);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_peak(&f->stack), used_after_three);
    nph_stack_alloc_pop(&f->stack, NULL);
}

NTEST_F(StackFixture, out_of_order_pop_is_rejected)
{
    void *a = nph_stack_alloc_push(&f->stack, 10U, 1U);
    void *b = nph_stack_alloc_push(&f->stack, 10U, 1U);
    NPH_TEST_EXPECT_ASSERT(nph_stack_alloc_pop(&f->stack, a));
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_live(&f->stack), 2U);
    nph_stack_alloc_pop(&f->stack, b);
    nph_stack_alloc_pop(&f->stack, a);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_live(&f->stack), 0U);
}

NTEST_F(StackFixture, exhaustion)
{
    // 1024 bytes minus a 16-byte header.
    NTEST_ASSERT_NOT_NULL(nph_stack_alloc_push(&f->stack, 1008U, 1U));
    NTEST_ASSERT_NULL(nph_stack_alloc_push(&f->stack, 1U, 1U));
    nph_stack_alloc_reset(&f->stack);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_used(&f->stack), 0U);
    NTEST_ASSERT_NULL(nph_stack_alloc_push(&f->stack, 1009U, 1U));
    NTEST_ASSERT_NULL(nph_stack_alloc_push(&f->stack, NPH_U64_MAX, 1U));
    NTEST_ASSERT_NOT_NULL(nph_stack_alloc_push(&f->stack, 1008U, 1U));
}

NTEST_F(StackFixture, vtable_realloc_top_and_free)
{
    NphAllocator *a = nph_stack_alloc_allocator(&f->stack);
    u8 *p = (u8 *)NPH_ALLOC(a, 32U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    test_fill_pattern(p, 32U, 5U);
    u8 *q = (u8 *)NPH_REALLOC(a, p, 32U, 200U, 16U);
    NTEST_ASSERT_EQ_PTR(q, p);
    NTEST_ASSERT_TRUE(test_check_pattern(q, 32U, 5U));
    q = (u8 *)NPH_REALLOC(a, q, 200U, 8U, 16U);
    NTEST_ASSERT_EQ_PTR(q, p);
    NTEST_ASSERT_NULL(NPH_REALLOC(a, q, 8U, 5000U, 16U));
    // A second block makes the first non-top: realloc refuses.
    void *r = NPH_ALLOC(a, 8U, 16U);
    void *moved = NULL;
    NPH_TEST_EXPECT_ASSERT(moved = NPH_REALLOC(a, q, 8U, 64U, 16U));
    NTEST_ASSERT_NULL(moved);
    NPH_FREE(a, r, 8U, 16U);
    NPH_FREE(a, q, 8U, 16U);
    NTEST_ASSERT_EQ_UINT(nph_stack_alloc_used(&f->stack), 0U);
}

NTEST(stack_alloc, init_from_backing)
{
    NphStackAlloc stack;
    NTEST_ASSERT_EQ_INT(nph_stack_alloc_init_from(&stack, nph_allocator_system(), 4096U),
                        NPH_RESULT_OK);
    void *p = nph_stack_alloc_push(&stack, 4000U, 1U);
    NTEST_ASSERT_NOT_NULL(p);
    memset(p, 0, 4000U);
    nph_stack_alloc_destroy(&stack);
    NTEST_ASSERT_NULL(stack.base);
    nph_stack_alloc_destroy(&stack);
    NTEST_ASSERT_EQ_INT(nph_stack_alloc_init_from(&stack, nph_allocator_system(), 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
}

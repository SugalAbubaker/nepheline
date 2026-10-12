// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/vmem.h>

#include "test_platform_support.h"

#include <string.h>

NTEST(vmem, sizes_are_sane)
{
    const u64 page = nph_vmem_page_size();
    NTEST_ASSERT_TRUE(nph_is_pow2_u64(page));
    NTEST_ASSERT_GE_UINT(page, 4096U);
    const u64 granularity = nph_vmem_allocation_granularity();
    NTEST_ASSERT_TRUE(nph_is_pow2_u64(granularity));
    NTEST_ASSERT_GE_UINT(granularity, page);
    const u64 large = nph_vmem_large_page_size();
    NTEST_LOG("page %llu, granularity %llu, large page %llu", (unsigned long long)page,
              (unsigned long long)granularity, (unsigned long long)large);
    if (large != 0U) {
        NTEST_ASSERT_TRUE(nph_is_pow2_u64(large));
        NTEST_ASSERT_GT_UINT(large, page);
    }
}

NTEST(vmem, reserve_commit_use_decommit_release)
{
    const u64 page = nph_vmem_page_size();
    const u64 size = 64U * page;
    void *base = NULL;
    NTEST_ASSERT_EQ_INT(nph_vmem_reserve(size, &base), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(base);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(base, page));

    // Commit the middle pages, write, and read back.
    u8 *middle = (u8 *)base + 8U * page;
    NTEST_ASSERT_EQ_INT(nph_vmem_commit(middle, 4U * page), NPH_RESULT_OK);
    for (u64 i = 0; i < 4U * page; ++i) {
        NTEST_ASSERT_EQ_UINT(middle[i], 0U); // committed memory reads as zero
    }
    memset(middle, 0xAB, (size_t)(4U * page));
    NTEST_ASSERT_EQ_UINT(middle[0], 0xABU);
    NTEST_ASSERT_EQ_UINT(middle[4U * page - 1U], 0xABU);

    // Re-committing keeps the contents.
    NTEST_ASSERT_EQ_INT(nph_vmem_commit(middle, 4U * page), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(middle[page + 1U], 0xABU);

    // Decommit, then commit again: zero once more.
    NTEST_ASSERT_EQ_INT(nph_vmem_decommit(middle, 4U * page), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vmem_commit(middle, 4U * page), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(middle[0], 0U);
    NTEST_ASSERT_EQ_UINT(middle[4U * page - 1U], 0U);

    NTEST_ASSERT_EQ_INT(nph_vmem_release(base, size), NPH_RESULT_OK);
}

NTEST(vmem, commit_whole_reservation_and_touch_every_page)
{
    const u64 page = nph_vmem_page_size();
    const u64 size = NPH_MIB(8);
    void *base = NULL;
    NTEST_ASSERT_EQ_INT(nph_vmem_reserve(size, &base), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vmem_commit(base, size), NPH_RESULT_OK);
    u8 *bytes = (u8 *)base;
    for (u64 off = 0; off < size; off += page) {
        bytes[off] = (u8)(off / page);
    }
    for (u64 off = 0; off < size; off += page) {
        NTEST_ASSERT_EQ_UINT(bytes[off], (u8)(off / page));
    }
    NTEST_ASSERT_EQ_INT(nph_vmem_decommit(base, size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vmem_release(base, size), NPH_RESULT_OK);
}

NTEST(vmem, protect_read_only_and_back)
{
    const u64 page = nph_vmem_page_size();
    void *base = NULL;
    NTEST_ASSERT_EQ_INT(nph_vmem_reserve(page, &base), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vmem_commit(base, page), NPH_RESULT_OK);
    u8 *bytes = (u8 *)base;
    bytes[0] = 42U;
    NTEST_ASSERT_EQ_INT(nph_vmem_protect(base, page, NPH_VMEM_PROTECT_READ), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(bytes[0], 42U); // readable
    NTEST_ASSERT_EQ_INT(nph_vmem_protect(base, page, NPH_VMEM_PROTECT_NONE), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vmem_protect(base, page, NPH_VMEM_PROTECT_READ_WRITE), NPH_RESULT_OK);
    bytes[0] = 43U; // writable again
    NTEST_ASSERT_EQ_UINT(bytes[0], 43U);
    NTEST_ASSERT_EQ_INT(nph_vmem_release(base, page), NPH_RESULT_OK);
}

NTEST(vmem, reservations_are_cheap_and_independent)
{
    // 64 GiB of address space costs nothing until committed; several such
    // reservations must coexist on a 64-bit machine.
    void *blocks[4] = {NULL, NULL, NULL, NULL};
    const u64 size = NPH_GIB(16);
    for (u32 i = 0; i < NPH_ARRAY_COUNT(blocks); ++i) {
        NTEST_ASSERT_EQ_INT(nph_vmem_reserve(size, &blocks[i]), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_vmem_commit(blocks[i], nph_vmem_page_size()), NPH_RESULT_OK);
        ((u8 *)blocks[i])[0] = (u8)i;
    }
    for (u32 i = 0; i < NPH_ARRAY_COUNT(blocks); ++i) {
        NTEST_ASSERT_EQ_UINT(((u8 *)blocks[i])[0], (u8)i);
        NTEST_ASSERT_EQ_INT(nph_vmem_release(blocks[i], size), NPH_RESULT_OK);
    }
}

NTEST(vmem, large_pages_when_available)
{
    const u64 large = nph_vmem_large_page_size();
    void *p = NULL;
    const NphResult r = nph_vmem_alloc_large(1U, &p);
    if (large == 0U) {
        NTEST_ASSERT_EQ_INT(r, NPH_RESULT_UNSUPPORTED);
        NTEST_ASSERT_NULL(p);
        NTEST_SKIP("large pages not available to this process");
    }
    if (r == NPH_RESULT_OUT_OF_MEMORY) {
        NTEST_SKIP("no large pages free right now");
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(p, large));
    memset(p, 0x5A, (size_t)large);
    NTEST_ASSERT_EQ_UINT(((u8 *)p)[large - 1U], 0x5AU);
    NTEST_ASSERT_EQ_INT(nph_vmem_free_large(p, 1U), NPH_RESULT_OK);
}

NTEST(vmem, advise_huge_is_harmless)
{
    const u64 size = NPH_MIB(4);
    void *base = NULL;
    NTEST_ASSERT_EQ_INT(nph_vmem_reserve(size, &base), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vmem_commit(base, size), NPH_RESULT_OK);
    nph_vmem_advise_huge(base, size);
    memset(base, 1, (size_t)size);
    NTEST_ASSERT_EQ_UINT(((u8 *)base)[size - 1U], 1U);
    NTEST_ASSERT_EQ_INT(nph_vmem_release(base, size), NPH_RESULT_OK);
}

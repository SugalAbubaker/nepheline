// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>
#include <nepheline/platform/vmem.h>

#include "platform_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

#define LINUX_HUGE_PAGE_SIZE NPH_MIB(2)

static struct {
    NphOnce once;
    u64 page_size;
    u64 large_page_size;
} g_vmem = {NPH_ONCE_INIT, 0U, 0U};

// Explicit huge pages come from the hugetlbfs pool, which is empty unless
// an administrator reserved pages (vm.nr_hugepages). Report large pages as
// available only when the pool exists and has pages.
static u64 detect_large_page_size(void)
{
    FILE *f = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", "r");
    if (f == NULL) {
        return 0U;
    }
    char line[64];
    u64 count = 0U;
    if (fgets(line, (int)sizeof line, f) != NULL) {
        count = strtoull(line, NULL, 10);
    }
    fclose(f);
    return count > 0U ? LINUX_HUGE_PAGE_SIZE : 0U;
}

static void vmem_init(void *user)
{
    NPH_UNUSED(user);
    g_vmem.page_size = (u64)sysconf(_SC_PAGESIZE);
    g_vmem.large_page_size = detect_large_page_size();
}

static void ensure_init(void)
{
    nph_once(&g_vmem.once, vmem_init, NULL);
}

u64 nph_vmem_page_size(void)
{
    ensure_init();
    return g_vmem.page_size;
}

u64 nph_vmem_allocation_granularity(void)
{
    return nph_vmem_page_size();
}

u64 nph_vmem_large_page_size(void)
{
    ensure_init();
    return g_vmem.large_page_size;
}

static u64 round_to_pages(u64 size)
{
    return nph_align_up_u64(size, nph_vmem_page_size());
}

NphResult nph_vmem_reserve(u64 size, void **out_ptr)
{
    NPH_ASSERT(out_ptr != NULL);
    NPH_ASSERT(size > 0U);
    *out_ptr = NULL;
    // PROT_NONE + MAP_NORESERVE: address space only, no commit charge.
    void *p = mmap(NULL, (size_t)round_to_pages(size), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        return nph_posix_fail("mmap(PROT_NONE)", errno);
    }
    *out_ptr = p;
    return NPH_RESULT_OK;
}

NphResult nph_vmem_commit(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    // Pages are backed lazily on first touch and read as zero; a range that
    // is already committed keeps its contents.
    if (mprotect(ptr, (size_t)round_to_pages(size), PROT_READ | PROT_WRITE) != 0) {
        return nph_posix_fail("mprotect(PROT_READ | PROT_WRITE)", errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_decommit(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    const size_t len = (size_t)round_to_pages(size);
    // Instrumented code that ran on this memory (a fiber stack) left its
    // redzones in the sanitizer's shadow; clear them before the addresses
    // can be handed to something else.
    NPH_ASAN_UNPOISON(ptr, len);
    // MADV_DONTNEED drops the physical pages; the next access would fault
    // in fresh zero pages, which PROT_NONE turns into a visible fault.
    if (madvise(ptr, len, MADV_DONTNEED) != 0) {
        return nph_posix_fail("madvise(MADV_DONTNEED)", errno);
    }
    if (mprotect(ptr, len, PROT_NONE) != 0) {
        return nph_posix_fail("mprotect(PROT_NONE)", errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_release(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    NPH_ASAN_UNPOISON(ptr, round_to_pages(size)); // see nph_vmem_decommit
    if (munmap(ptr, (size_t)round_to_pages(size)) != 0) {
        return nph_posix_fail("munmap", errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_protect(void *ptr, u64 size, NphVmemProtect protect)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    int prot;
    switch (protect) {
    case NPH_VMEM_PROTECT_NONE:
        prot = PROT_NONE;
        break;
    case NPH_VMEM_PROTECT_READ:
        prot = PROT_READ;
        break;
    case NPH_VMEM_PROTECT_READ_WRITE:
        prot = PROT_READ | PROT_WRITE;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (mprotect(ptr, (size_t)round_to_pages(size), prot) != 0) {
        return nph_posix_fail("mprotect", errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_alloc_large(u64 size, void **out_ptr)
{
    NPH_ASSERT(out_ptr != NULL);
    NPH_ASSERT(size > 0U);
    *out_ptr = NULL;
    const u64 large = nph_vmem_large_page_size();
    if (large == 0U) {
        return NPH_RESULT_UNSUPPORTED;
    }
    const u64 rounded = nph_align_up_u64(size, large);
    void *p = mmap(NULL, (size_t)rounded, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        const NphResult r = nph_posix_result(errno);
        return r == NPH_RESULT_IO_ERROR ? NPH_RESULT_OUT_OF_MEMORY : r;
    }
    *out_ptr = p;
    return NPH_RESULT_OK;
}

NphResult nph_vmem_free_large(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    if (munmap(ptr, (size_t)nph_align_up_u64(size, LINUX_HUGE_PAGE_SIZE)) != 0) {
        return nph_posix_fail("munmap(huge)", errno);
    }
    return NPH_RESULT_OK;
}

void nph_vmem_advise_huge(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    (void)madvise(ptr, (size_t)round_to_pages(size), MADV_HUGEPAGE);
}

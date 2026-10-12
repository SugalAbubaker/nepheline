// SPDX-License-Identifier: Apache-2.0
#pragma once

// Virtual memory (CLAUDE.md §8.3 "reserve/commit/decommit and large pages").
//
// Address space is reserved in large regions and committed page by page as
// it is used, so an arena, a fiber stack, or a streaming buffer can claim a
// generous range without paying for it: reserved pages cost nothing until
// committed, and decommitted pages return their physical memory while the
// addresses stay usable for a later commit. Guard pages (no access) catch
// stack overflows in fibers.
//
// Every address and size is in whole pages: addresses are page aligned and
// sizes are rounded up to a page by the functions, except that a
// reservation is rounded to the allocation granularity (64 KiB on Windows).
// Committed memory reads as zero.
//
// Backed by VirtualAlloc/VirtualFree/VirtualProtect on Windows and
// mmap/mprotect/madvise on Linux. These are system calls costing a few
// microseconds each; nothing here is for per-frame use.
//
// Thread-safety: NPH_THREADSAFE. The caller owns the regions and must not
// commit, protect, or release overlapping ranges concurrently.

#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphVmemProtect {
    NPH_VMEM_PROTECT_NONE,       // no access: a guard page
    NPH_VMEM_PROTECT_READ,       // read-only
    NPH_VMEM_PROTECT_READ_WRITE, // the state of freshly committed memory
} NphVmemProtect;

// The page size (4 KiB on every supported platform). Never fails.
u64 nph_vmem_page_size(void);

// The unit reservations are rounded to: 64 KiB on Windows, the page size
// on Linux.
u64 nph_vmem_allocation_granularity(void);

// The large page size (2 MiB on x86-64), or 0 when the OS or the process
// cannot use large pages (on Windows the SeLockMemoryPrivilege is needed;
// on Linux hugetlbfs pages must have been reserved by the administrator).
u64 nph_vmem_large_page_size(void);

// Reserves `size` bytes (rounded up to the allocation granularity) of
// inaccessible address space. Release with nph_vmem_release.
// NPH_RESULT_OUT_OF_MEMORY when the address space is exhausted.
NPH_MUST_USE NphResult nph_vmem_reserve(u64 size, void **out_ptr);

// Makes [ptr, ptr + size) of a reservation readable and writable, backing it
// with zero pages on first touch. Committing an already committed range is
// allowed and preserves its contents.
NPH_MUST_USE NphResult nph_vmem_commit(void *ptr, u64 size);

// Returns the physical memory behind [ptr, ptr + size) and makes the range
// inaccessible again; contents are lost. The range stays reserved.
NphResult nph_vmem_decommit(void *ptr, u64 size);

// Releases a whole reservation made by nph_vmem_reserve. `size` must be the
// size that was passed to reserve.
NphResult nph_vmem_release(void *ptr, u64 size);

// Changes the protection of committed pages.
NphResult nph_vmem_protect(void *ptr, u64 size, NphVmemProtect protect);

// Reserves and commits `size` bytes (rounded up to the large page size) on
// large pages. NPH_RESULT_UNSUPPORTED when nph_vmem_large_page_size() is 0,
// NPH_RESULT_OUT_OF_MEMORY when no large pages are available right now.
// Release with nph_vmem_free_large.
NPH_MUST_USE NphResult nph_vmem_alloc_large(u64 size, void **out_ptr);
NphResult nph_vmem_free_large(void *ptr, u64 size);

// Asks the kernel to back a committed range with transparent huge pages
// where it can (madvise(MADV_HUGEPAGE) on Linux; a no-op on Windows, which
// has no equivalent). A hint only; never fails for a valid range.
void nph_vmem_advise_huge(void *ptr, u64 size);

#ifdef __cplusplus
}
#endif

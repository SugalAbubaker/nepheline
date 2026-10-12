// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/platform/vmem.h>

#include "platform_internal.h"

// SYSTEM_INFO is constant for the process; read it once.
static struct {
    NphOnce once;
    u64 page_size;
    u64 granularity;
    u64 large_page_size; // 0 when the privilege could not be enabled
} g_vmem = {NPH_ONCE_INIT, 0U, 0U, 0U};

// Large pages need SeLockMemoryPrivilege, which is granted to no account by
// default ("Lock pages in memory" in the local security policy). Try to
// enable it; the result decides whether large pages are offered at all.
static bool enable_lock_memory_privilege(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1U;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(NULL, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid) != 0;
    if (ok) {
        // AdjustTokenPrivileges succeeds even when the privilege is not
        // held; GetLastError tells the difference.
        ok = AdjustTokenPrivileges(token, FALSE, &tp, 0U, NULL, NULL) != 0 &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

static void vmem_init(void *user)
{
    NPH_UNUSED(user);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_vmem.page_size = si.dwPageSize;
    g_vmem.granularity = si.dwAllocationGranularity;
    const SIZE_T large = GetLargePageMinimum();
    g_vmem.large_page_size = (large != 0U && enable_lock_memory_privilege()) ? (u64)large : 0U;
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
    ensure_init();
    return g_vmem.granularity;
}

u64 nph_vmem_large_page_size(void)
{
    ensure_init();
    return g_vmem.large_page_size;
}

NphResult nph_vmem_reserve(u64 size, void **out_ptr)
{
    NPH_ASSERT(out_ptr != NULL);
    NPH_ASSERT(size > 0U);
    *out_ptr = NULL;
    void *p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    if (p == NULL) {
        return nph_win32_fail("VirtualAlloc(MEM_RESERVE)", GetLastError());
    }
    *out_ptr = p;
    return NPH_RESULT_OK;
}

NphResult nph_vmem_commit(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    if (VirtualAlloc(ptr, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        return nph_win32_fail("VirtualAlloc(MEM_COMMIT)", GetLastError());
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_decommit(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    // MEM_DECOMMIT on a range that is already decommitted succeeds.
    if (!VirtualFree(ptr, (SIZE_T)size, MEM_DECOMMIT)) {
        return nph_win32_fail("VirtualFree(MEM_DECOMMIT)", GetLastError());
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_release(void *ptr, u64 size)
{
    NPH_ASSERT(ptr != NULL);
    NPH_UNUSED(size); // Windows releases the whole reservation by base address
    if (!VirtualFree(ptr, 0U, MEM_RELEASE)) {
        return nph_win32_fail("VirtualFree(MEM_RELEASE)", GetLastError());
    }
    return NPH_RESULT_OK;
}

NphResult nph_vmem_protect(void *ptr, u64 size, NphVmemProtect protect)
{
    NPH_ASSERT(ptr != NULL && size > 0U);
    DWORD flags;
    switch (protect) {
    case NPH_VMEM_PROTECT_NONE:
        flags = PAGE_NOACCESS;
        break;
    case NPH_VMEM_PROTECT_READ:
        flags = PAGE_READONLY;
        break;
    case NPH_VMEM_PROTECT_READ_WRITE:
        flags = PAGE_READWRITE;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    DWORD old = 0U;
    if (!VirtualProtect(ptr, (SIZE_T)size, flags, &old)) {
        return nph_win32_fail("VirtualProtect", GetLastError());
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
    void *p = VirtualAlloc(NULL, (SIZE_T)rounded, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                           PAGE_READWRITE);
    if (p == NULL) {
        // Fragmented physical memory is the usual cause; not an error worth
        // a log line since callers fall back to normal pages.
        const DWORD error = GetLastError();
        const NphResult r = nph_win32_result(error);
        return r == NPH_RESULT_IO_ERROR ? NPH_RESULT_OUT_OF_MEMORY : r;
    }
    *out_ptr = p;
    return NPH_RESULT_OK;
}

NphResult nph_vmem_free_large(void *ptr, u64 size)
{
    return nph_vmem_release(ptr, size);
}

void nph_vmem_advise_huge(void *ptr, u64 size)
{
    NPH_UNUSED(ptr);
    NPH_UNUSED(size);
}

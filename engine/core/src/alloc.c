// SPDX-License-Identifier: Apache-2.0
// Allocator entry points and the malloc-backed system allocator.

// posix_memalign is POSIX.1-2001, not C17.
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/realtime.h>

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Entry points. Each reports a call made inside a real-time scope
// (realtime.h): an allocator's cost and its lock, if it has one, are what a
// real-time thread must never wait on.
// ---------------------------------------------------------------------------

void *nph_alloc_at(NphAllocator *a, u64 size, u32 align, NphCallsite site)
{
    NPH_REALTIME_CHECK(NPH_REALTIME_VIOLATION_ALLOC, "nph_alloc", site);
    NPH_ASSERT(a != NULL);
    NPH_ASSERT_MSG(size > 0U, "zero-byte allocation at %s:%u", site.file, site.line);
    NPH_ASSERT_MSG(nph_is_pow2_u32(align), "alignment %u is not a power of two at %s:%u", align,
                   site.file, site.line);
    if (size == 0U || !nph_is_pow2_u32(align)) {
        return NULL;
    }
    return a->vtable->alloc(a, size, nph_alloc_normalize_align(align), site);
}

void *nph_alloc_zeroed_at(NphAllocator *a, u64 size, u32 align, NphCallsite site)
{
    void *p = nph_alloc_at(a, size, align, site);
    if (p != NULL) {
        memset(p, 0, (size_t)size);
    }
    return p;
}

void *nph_alloc_array_at(NphAllocator *a, u64 elem_size, u32 align, u64 count, bool zeroed,
                         NphCallsite site)
{
    NPH_ASSERT_MSG(count > 0U, "zero-element array allocation at %s:%u", site.file, site.line);
    if (count == 0U || elem_size == 0U) {
        return NULL;
    }
    if (count > NPH_U64_MAX / elem_size) {
        NPH_ASSERT_MSG(false, "array allocation overflow (%llu x %llu bytes) at %s:%u",
                       (unsigned long long)count, (unsigned long long)elem_size, site.file,
                       site.line);
        return NULL;
    }
    const u64 size = elem_size * count;
    return zeroed ? nph_alloc_zeroed_at(a, size, align, site) : nph_alloc_at(a, size, align, site);
}

void *nph_realloc_at(NphAllocator *a, void *ptr, u64 old_size, u64 new_size, u32 align,
                     NphCallsite site)
{
    NPH_REALTIME_CHECK(NPH_REALTIME_VIOLATION_ALLOC, "nph_realloc", site);
    NPH_ASSERT(a != NULL);
    NPH_ASSERT_MSG(new_size > 0U, "zero-byte reallocation at %s:%u", site.file, site.line);
    NPH_ASSERT_MSG(nph_is_pow2_u32(align), "alignment %u is not a power of two at %s:%u", align,
                   site.file, site.line);
    if (new_size == 0U || !nph_is_pow2_u32(align)) {
        return NULL;
    }
    align = nph_alloc_normalize_align(align);
    if (ptr == NULL) {
        return a->vtable->alloc(a, new_size, align, site);
    }
    return a->vtable->realloc(a, ptr, old_size, new_size, align, site);
}

void nph_free_at(NphAllocator *a, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_ASSERT(a != NULL);
    if (ptr == NULL) {
        return;
    }
    NPH_REALTIME_CHECK(NPH_REALTIME_VIOLATION_ALLOC, "nph_free", site);
    a->vtable->free(a, ptr, size, nph_alloc_normalize_align(align), site);
}

void *nph_allocator_realloc_fallback(NphAllocator *a, void *ptr, u64 old_size, u64 new_size,
                                     u32 align, NphCallsite site)
{
    void *fresh = a->vtable->alloc(a, new_size, align, site);
    if (fresh == NULL) {
        return NULL;
    }
    if (ptr != NULL) {
        memcpy(fresh, ptr, (size_t)nph_min_u64(old_size, new_size));
        a->vtable->free(a, ptr, old_size, align, site);
    }
    return fresh;
}

// ---------------------------------------------------------------------------
// System allocator
// ---------------------------------------------------------------------------

#if NPH_MSVC_ABI

static void *system_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(site);
    return _aligned_malloc((size_t)size, (size_t)align);
}

static void *system_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                            NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(old_size);
    NPH_UNUSED(site);
    return _aligned_realloc(ptr, (size_t)new_size, (size_t)align);
}

static void system_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    _aligned_free(ptr);
}

#else

// glibc and musl malloc return 16-byte-aligned blocks on x86-64 and ARM64,
// so the default alignment uses plain malloc, which keeps realloc available.
// Larger alignments go through posix_memalign and copy on realloc.

static void *system_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(site);
    if (align <= NPH_ALLOC_DEFAULT_ALIGN) {
        return malloc((size_t)size);
    }
    void *p = NULL;
    if (posix_memalign(&p, (size_t)align, (size_t)size) != 0) {
        return NULL;
    }
    return p;
}

static void *system_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                            NphCallsite site)
{
    if (align <= NPH_ALLOC_DEFAULT_ALIGN) {
        return realloc(ptr, (size_t)new_size);
    }
    return nph_allocator_realloc_fallback(self, ptr, old_size, new_size, align, site);
}

static void system_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    free(ptr);
}

#endif

static const NphAllocatorVtable g_system_vtable = {system_alloc, system_realloc, system_free};
static NphAllocator g_system_allocator = {&g_system_vtable, "system"};

NphAllocator *nph_allocator_system(void)
{
    return &g_system_allocator;
}

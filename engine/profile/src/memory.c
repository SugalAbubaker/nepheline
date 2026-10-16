// SPDX-License-Identifier: Apache-2.0
// Tracked allocators: counting wrappers over another allocator. See memory.h.
#include <nepheline/core/assert.h>
#include <nepheline/profile/memory.h>

#include <string.h>

static NphTrackedAllocator *from(NphAllocator *a)
{
    return (NphTrackedAllocator *)a; // `allocator` is the first member
}

// Raises `peak` to at least `value`.
static void raise_peak(NphAtomicU64 *peak, u64 value)
{
    u64 seen = nph_atomic_load_u64(peak, NPH_MEMORY_ORDER_RELAXED);
    while (value > seen &&
           !nph_atomic_compare_exchange_u64(peak, &seen, value, NPH_MEMORY_ORDER_RELAXED,
                                            NPH_MEMORY_ORDER_RELAXED)) {
    }
}

static void count_alloc(NphTrackedAllocator *t, u64 size)
{
    const u64 bytes = nph_atomic_fetch_add_u64(&t->bytes, size, NPH_MEMORY_ORDER_RELAXED) + size;
    const u64 count = nph_atomic_fetch_add_u64(&t->allocations, 1U, NPH_MEMORY_ORDER_RELAXED) + 1U;
    nph_atomic_fetch_add_u64(&t->total_allocations, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_fetch_add_u64(&t->total_bytes, size, NPH_MEMORY_ORDER_RELAXED);
    raise_peak(&t->peak_bytes, bytes);
    raise_peak(&t->peak_allocations, count);
}

static void *tracked_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NphTrackedAllocator *t = from(self);
    void *p = nph_alloc_at(t->backing, size, align, site);
    if (p != NULL) {
        count_alloc(t, size);
    }
    return p;
}

static void *tracked_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                             NphCallsite site)
{
    NphTrackedAllocator *t = from(self);
    void *p = nph_realloc_at(t->backing, ptr, old_size, new_size, align, site);
    if (p == NULL) {
        return NULL; // the old block is untouched and still counted
    }
    if (ptr == NULL) {
        count_alloc(t, new_size);
        return p;
    }
    if (new_size >= old_size) {
        const u64 grown = new_size - old_size;
        const u64 bytes =
            nph_atomic_fetch_add_u64(&t->bytes, grown, NPH_MEMORY_ORDER_RELAXED) + grown;
        nph_atomic_fetch_add_u64(&t->total_bytes, grown, NPH_MEMORY_ORDER_RELAXED);
        raise_peak(&t->peak_bytes, bytes);
    } else {
        nph_atomic_fetch_sub_u64(&t->bytes, old_size - new_size, NPH_MEMORY_ORDER_RELAXED);
    }
    return p;
}

static void tracked_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    if (ptr == NULL) {
        return;
    }
    NphTrackedAllocator *t = from(self);
    nph_free_at(t->backing, ptr, size, align, site);
    nph_atomic_fetch_sub_u64(&t->bytes, size, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_fetch_sub_u64(&t->allocations, 1U, NPH_MEMORY_ORDER_RELAXED);
}

static const NphAllocatorVtable g_tracked_vtable = {tracked_alloc, tracked_realloc, tracked_free};

void nph_tracked_allocator_init(NphTrackedAllocator *t, NphAllocator *backing, const char *tag)
{
    NPH_ASSERT(t != NULL && backing != NULL && tag != NULL);
    memset(t, 0, sizeof *t);
    t->allocator.vtable = &g_tracked_vtable;
    t->allocator.name = tag;
    t->backing = backing;
    t->tag = tag;
}

void nph_tracked_allocator_stats(const NphTrackedAllocator *t, NphMemoryStats *out)
{
    NPH_ASSERT(t != NULL && out != NULL);
    out->tag = t->tag;
    out->bytes = nph_atomic_load_u64(&t->bytes, NPH_MEMORY_ORDER_RELAXED);
    out->peak_bytes = nph_atomic_load_u64(&t->peak_bytes, NPH_MEMORY_ORDER_RELAXED);
    out->allocations = nph_atomic_load_u64(&t->allocations, NPH_MEMORY_ORDER_RELAXED);
    out->peak_allocations = nph_atomic_load_u64(&t->peak_allocations, NPH_MEMORY_ORDER_RELAXED);
    out->total_allocations = nph_atomic_load_u64(&t->total_allocations, NPH_MEMORY_ORDER_RELAXED);
    out->total_bytes = nph_atomic_load_u64(&t->total_bytes, NPH_MEMORY_ORDER_RELAXED);
}

void nph_tracked_allocator_reset_peaks(NphTrackedAllocator *t)
{
    NPH_ASSERT(t != NULL);
    nph_atomic_store_u64(&t->peak_bytes, nph_atomic_load_u64(&t->bytes, NPH_MEMORY_ORDER_RELAXED),
                         NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&t->peak_allocations,
                         nph_atomic_load_u64(&t->allocations, NPH_MEMORY_ORDER_RELAXED),
                         NPH_MEMORY_ORDER_RELAXED);
}

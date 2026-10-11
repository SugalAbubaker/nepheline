// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/assert.h>

#include <string.h>

// Placed immediately before the payload, inside the inner allocation. The
// trailing guard follows the payload.
//
//   [inner block ........................................................]
//   [padding][NphDebugAllocBlock + front guard][payload][trailing guard]
//                                              ^ ptr (aligned to `align`)
struct NphDebugAllocBlock {
    NphDebugAllocBlock *prev;
    NphDebugAllocBlock *next;
    void *block;    // the inner allocation
    u64 block_size; // its size
    NphCallsite site;
    u64 size; // payload size
    u64 sequence;
    u64 reserved; // pads the header to 96 bytes
    u32 align;    // payload alignment
    u32 magic;
    u8 guard[NPH_DEBUG_ALLOC_GUARD_SIZE];
};

#define DEBUG_MAGIC_LIVE  0xA110CA7EU
#define DEBUG_MAGIC_FREED 0xDEADBEEFU

NPH_STATIC_ASSERT(sizeof(NphDebugAllocBlock) % NPH_ALLOC_DEFAULT_ALIGN == 0U,
                  "header must keep the payload at the default alignment");

static NphDebugAllocBlock *header_of(const void *ptr)
{
    return (NphDebugAllocBlock *)(uptr)((const u8 *)ptr - sizeof(NphDebugAllocBlock));
}

static bool guard_intact(const u8 *guard)
{
    for (u32 i = 0; i < NPH_DEBUG_ALLOC_GUARD_SIZE; ++i) {
        if (guard[i] != NPH_DEBUG_ALLOC_GUARD_PATTERN) {
            return false;
        }
    }
    return true;
}

static NphResult check_block(const NphDebugAllocBlock *h, const void *ptr)
{
    if (h->magic != DEBUG_MAGIC_LIVE) {
        return NPH_RESULT_CORRUPT;
    }
    if (!guard_intact(h->guard)) {
        return NPH_RESULT_CORRUPT; // underrun
    }
    if (!guard_intact((const u8 *)ptr + h->size)) {
        return NPH_RESULT_CORRUPT; // overrun
    }
    return NPH_RESULT_OK;
}

static void fill_info(const NphDebugAllocBlock *h, NphDebugAllocInfo *info)
{
    info->ptr = (u8 *)h + sizeof *h;
    info->size = h->size;
    info->align = h->align;
    info->site = h->site;
    info->sequence = h->sequence;
}

static void *debug_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NphDebugAllocator *da = (NphDebugAllocator *)self;
    // Room for the header at any alignment up to `align`, plus the trailing guard.
    const u64 lead = nph_align_up_u64(sizeof(NphDebugAllocBlock), align);
    const u64 block_size = lead + size + NPH_DEBUG_ALLOC_GUARD_SIZE;
    u8 *block = (u8 *)da->inner->vtable->alloc(da->inner, block_size, align, site);
    if (block == NULL) {
        return NULL;
    }
    u8 *ptr = block + lead;
    NphDebugAllocBlock *h = header_of(ptr);
    h->block = block;
    h->block_size = block_size;
    h->site = site;
    h->size = size;
    h->align = align;
    h->magic = DEBUG_MAGIC_LIVE;
    memset(h->guard, NPH_DEBUG_ALLOC_GUARD_PATTERN, sizeof h->guard);
    memset(ptr, NPH_DEBUG_ALLOC_POISON_FRESH, (size_t)size);
    memset(ptr + size, NPH_DEBUG_ALLOC_GUARD_PATTERN, NPH_DEBUG_ALLOC_GUARD_SIZE);

    nph_spin_lock_acquire(&da->lock);
    h->sequence = da->stats.total_allocations++;
    h->prev = NULL;
    h->next = da->live_head;
    if (da->live_head != NULL) {
        da->live_head->prev = h;
    }
    da->live_head = h;
    da->stats.live_count += 1U;
    da->stats.live_bytes += size;
    da->stats.peak_live_count = nph_max_u32(da->stats.peak_live_count, da->stats.live_count);
    da->stats.peak_live_bytes = nph_max_u64(da->stats.peak_live_bytes, da->stats.live_bytes);
    nph_spin_lock_release(&da->lock);
    return ptr;
}

static void debug_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NphDebugAllocator *da = (NphDebugAllocator *)self;
    NphDebugAllocBlock *h = header_of(ptr);
    NPH_ASSERT_MSG(h->magic == DEBUG_MAGIC_LIVE, "free of %p at %s:%u: %s", ptr, site.file,
                   site.line,
                   h->magic == DEBUG_MAGIC_FREED ? "double free" : "not a live debug allocation");
    if (h->magic != DEBUG_MAGIC_LIVE) {
        return;
    }
    NPH_ASSERT_MSG(guard_intact(h->guard), "buffer underrun before %p (allocated at %s:%u)", ptr,
                   h->site.file, h->site.line);
    NPH_ASSERT_MSG(guard_intact((const u8 *)ptr + h->size),
                   "buffer overrun after %p + %llu (allocated at %s:%u)", ptr,
                   (unsigned long long)h->size, h->site.file, h->site.line);
    NPH_ASSERT_MSG(h->size == size, "free of %p with size %llu, allocated with %llu at %s:%u", ptr,
                   (unsigned long long)size, (unsigned long long)h->size, h->site.file,
                   h->site.line);
    NPH_ASSERT_MSG(h->align == align, "free of %p with align %u, allocated with %u at %s:%u", ptr,
                   align, h->align, h->site.file, h->site.line);
    NPH_UNUSED(size);
    NPH_UNUSED(align);

    nph_spin_lock_acquire(&da->lock);
    if (h->prev != NULL) {
        h->prev->next = h->next;
    } else {
        da->live_head = h->next;
    }
    if (h->next != NULL) {
        h->next->prev = h->prev;
    }
    da->stats.live_count -= 1U;
    da->stats.live_bytes -= h->size;
    da->stats.total_frees += 1U;
    nph_spin_lock_release(&da->lock);

    void *block = h->block;
    const u64 block_size = h->block_size;
    const u32 block_align = h->align;
    memset(ptr, NPH_DEBUG_ALLOC_POISON_FREED, (size_t)h->size);
    h->magic = DEBUG_MAGIC_FREED;
    da->inner->vtable->free(da->inner, block, block_size, block_align, site);
}

static void *debug_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                           NphCallsite site)
{
    // Always move: it keeps every block guarded and makes stale pointers
    // after a realloc fail loudly (the old payload is poisoned).
    void *fresh = debug_alloc(self, new_size, align, site);
    if (fresh == NULL) {
        return NULL;
    }
    const NphDebugAllocBlock *h = header_of(ptr);
    NPH_ASSERT_MSG(h->size == old_size, "realloc of %p with old size %llu, allocated with %llu",
                   ptr, (unsigned long long)old_size, (unsigned long long)h->size);
    memcpy(fresh, ptr, (size_t)nph_min_u64(h->size, new_size));
    debug_free(self, ptr, old_size, align, site);
    return fresh;
}

static const NphAllocatorVtable g_debug_vtable = {debug_alloc, debug_realloc, debug_free};

void nph_debug_allocator_init(NphDebugAllocator *da, NphAllocator *inner)
{
    NPH_ASSERT(da != NULL && inner != NULL);
    memset(da, 0, sizeof *da);
    da->allocator.vtable = &g_debug_vtable;
    da->allocator.name = "debug";
    da->inner = inner;
    nph_spin_lock_init(&da->lock);
}

u32 nph_debug_allocator_report(NphDebugAllocator *da, NphDebugAllocReportFn fn, void *user)
{
    NPH_ASSERT(da != NULL);
    u32 count = 0U;
    nph_spin_lock_acquire(&da->lock);
    for (const NphDebugAllocBlock *h = da->live_head; h != NULL; h = h->next) {
        if (fn != NULL) {
            NphDebugAllocInfo info;
            fill_info(h, &info);
            fn(&info, user);
        }
        ++count;
    }
    nph_spin_lock_release(&da->lock);
    return count;
}

u32 nph_debug_allocator_destroy(NphDebugAllocator *da, NphDebugAllocReportFn on_leak, void *user)
{
    if (da == NULL || da->allocator.vtable == NULL) {
        return 0U;
    }
    const u32 leaks = nph_debug_allocator_report(da, on_leak, user);
    memset(da, 0, sizeof *da);
    return leaks;
}

NphResult nph_debug_allocator_check_block(const void *ptr)
{
    if (ptr == NULL) {
        return NPH_RESULT_CORRUPT;
    }
    return check_block(header_of(ptr), ptr);
}

NphResult nph_debug_allocator_check(NphDebugAllocator *da, NphDebugAllocInfo *out_info)
{
    NPH_ASSERT(da != NULL);
    NphResult result = NPH_RESULT_OK;
    nph_spin_lock_acquire(&da->lock);
    for (const NphDebugAllocBlock *h = da->live_head; h != NULL; h = h->next) {
        const void *ptr = (const u8 *)h + sizeof *h;
        if (check_block(h, ptr) != NPH_RESULT_OK) {
            if (out_info != NULL) {
                fill_info(h, out_info);
            }
            result = NPH_RESULT_CORRUPT;
            break;
        }
    }
    nph_spin_lock_release(&da->lock);
    return result;
}

void nph_debug_allocator_get_stats(NphDebugAllocator *da, NphDebugAllocStats *out)
{
    NPH_ASSERT(da != NULL && out != NULL);
    nph_spin_lock_acquire(&da->lock);
    *out = da->stats;
    nph_spin_lock_release(&da->lock);
}

// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/pool.h>

#include <string.h>

static void *pool_vt_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(site);
    NphPool *pool = (NphPool *)self;
    NPH_ASSERT_MSG(size <= pool->slot_size, "pool slot is %u bytes, %llu requested",
                   pool->slot_size, (unsigned long long)size);
    NPH_ASSERT_MSG(align <= pool->slot_align, "pool slot alignment is %u, %u requested",
                   pool->slot_align, align);
    if (size > pool->slot_size || align > pool->slot_align) {
        return NULL;
    }
    return nph_pool_alloc(pool);
}

static void *pool_vt_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                             NphCallsite site)
{
    NPH_UNUSED(old_size);
    NphPool *pool = (NphPool *)self;
    // A slot never moves; it either already holds the new size or cannot.
    if (new_size <= pool->slot_size && align <= pool->slot_align) {
        return ptr;
    }
    NPH_UNUSED(site);
    return NULL;
}

static void pool_vt_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    nph_pool_free((NphPool *)self, ptr);
}

static const NphAllocatorVtable g_pool_vtable = {pool_vt_alloc, pool_vt_realloc, pool_vt_free};

static u32 pool_slot_size(u32 elem_size, u32 elem_align)
{
    const u32 min_size = nph_max_u32(elem_size, (u32)sizeof(void *));
    return nph_align_up_u32(min_size, elem_align);
}

void nph_pool_init(NphPool *pool, void *memory, u64 size, u32 elem_size, u32 elem_align)
{
    NPH_ASSERT(pool != NULL);
    NPH_ASSERT(elem_size > 0U);
    NPH_ASSERT(nph_is_pow2_u32(elem_align));
    memset(pool, 0, sizeof *pool);
    pool->allocator.vtable = &g_pool_vtable;
    pool->allocator.name = "pool";
    pool->slot_align = nph_max_u32(elem_align, (u32)sizeof(void *));
    pool->slot_size = pool_slot_size(elem_size, pool->slot_align);

    // Align the first slot; the block itself may be unaligned.
    const u64 skipped =
        memory != NULL ? nph_align_up_u64((uptr)memory, pool->slot_align) - (uptr)memory : 0U;
    if (memory == NULL || skipped >= size) {
        return;
    }
    pool->base = (u8 *)memory + skipped;
    pool->capacity = size - skipped;
    const u64 count = pool->capacity / pool->slot_size;
    pool->slot_count = count > NPH_U32_MAX ? NPH_U32_MAX : (u32)count;
    NPH_ASAN_POISON(pool->base, pool->capacity);
}

NphResult nph_pool_init_from(NphPool *pool, NphAllocator *backing, u32 elem_size, u32 elem_align,
                             u32 count)
{
    NPH_ASSERT(pool != NULL);
    NPH_ASSERT(backing != NULL);
    if (count == 0U || elem_size == 0U || !nph_is_pow2_u32(elem_align)) {
        memset(pool, 0, sizeof *pool);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 slot_align = nph_max_u32(elem_align, (u32)sizeof(void *));
    const u64 bytes = (u64)pool_slot_size(elem_size, slot_align) * count;
    void *memory = NPH_ALLOC(backing, bytes, nph_max_u32(slot_align, NPH_ALLOC_DEFAULT_ALIGN));
    if (memory == NULL) {
        memset(pool, 0, sizeof *pool);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    nph_pool_init(pool, memory, bytes, elem_size, elem_align);
    NPH_ASSERT(pool->slot_count == count);
    pool->backing = backing;
    return NPH_RESULT_OK;
}

void nph_pool_destroy(NphPool *pool)
{
    if (pool == NULL || pool->base == NULL) {
        return;
    }
    NPH_ASAN_UNPOISON(pool->base, pool->capacity);
    if (pool->backing != NULL) {
        NPH_FREE(pool->backing, pool->base, pool->capacity,
                 nph_max_u32(pool->slot_align, NPH_ALLOC_DEFAULT_ALIGN));
    }
    memset(pool, 0, sizeof *pool);
}

void *nph_pool_alloc(NphPool *pool)
{
    NPH_ASSERT(pool != NULL);
    void *slot;
    if (pool->free_head != NULL) {
        slot = pool->free_head;
        memcpy((void *)&pool->free_head, slot, sizeof pool->free_head);
    } else if (pool->bump < pool->slot_count) {
        slot = pool->base + (u64)pool->bump * pool->slot_size;
        pool->bump += 1U;
    } else {
        return NULL;
    }
    pool->live += 1U;
    pool->peak = nph_max_u32(pool->peak, pool->live);
    NPH_ASAN_UNPOISON(slot, pool->slot_size);
    return slot;
}

void *nph_pool_alloc_zeroed(NphPool *pool)
{
    void *p = nph_pool_alloc(pool);
    if (p != NULL) {
        memset(p, 0, pool->slot_size);
    }
    return p;
}

bool nph_pool_owns(const NphPool *pool, const void *ptr)
{
    const uptr p = (uptr)ptr;
    const uptr base = (uptr)pool->base;
    if (pool->base == NULL || p < base || p >= base + (u64)pool->slot_count * pool->slot_size) {
        return false;
    }
    return (p - base) % pool->slot_size == 0U;
}

void nph_pool_free(NphPool *pool, void *ptr)
{
    NPH_ASSERT(pool != NULL);
    if (ptr == NULL) {
        return;
    }
    NPH_ASSERT_MSG(nph_pool_owns(pool, ptr), "pointer %p is not a slot of this pool", ptr);
    NPH_ASSERT(pool->live > 0U);
    if (!nph_pool_owns(pool, ptr) || pool->live == 0U) {
        return;
    }
    // Poison everything but the link word, which the free list keeps using.
    NPH_ASAN_UNPOISON(ptr, pool->slot_size);
    memcpy(ptr, (const void *)&pool->free_head, sizeof pool->free_head);
    pool->free_head = ptr;
    pool->live -= 1U;
    NPH_ASAN_POISON((u8 *)ptr + sizeof(void *), pool->slot_size - sizeof(void *));
}

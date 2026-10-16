// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/handle.h>

#include <string.h>

static NphHandle make_handle(const NphHandlePool *pool, u32 index, u32 generation)
{
    const NphHandle h = {(generation << pool->index_bits) | index};
    return h;
}

NphResult nph_handle_pool_init(NphHandlePool *pool, NphAllocator *alloc, u32 capacity,
                               u32 index_bits, u32 min_free_before_reuse)
{
    NPH_ASSERT(pool != NULL && alloc != NULL);
    memset(pool, 0, sizeof *pool);
    if (index_bits < NPH_HANDLE_POOL_MIN_INDEX_BITS ||
        index_bits > NPH_HANDLE_POOL_MAX_INDEX_BITS || capacity == 0U ||
        capacity > ((u32)1U << index_bits)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    pool->generations = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, capacity);
    if (pool->generations == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    pool->free_queue = NPH_ALLOC_ARRAY(alloc, u32, capacity);
    if (pool->free_queue == NULL) {
        NPH_FREE_ARRAY(alloc, u32, pool->generations, capacity);
        pool->generations = NULL;
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    pool->alloc = alloc;
    pool->capacity = capacity;
    pool->index_bits = index_bits;
    pool->index_mask = ((u32)1U << index_bits) - 1U;
    pool->generation_max = (u32)(((u64)1U << (32U - index_bits)) - 1U);
    pool->min_free_before_reuse = min_free_before_reuse;
    return NPH_RESULT_OK;
}

void nph_handle_pool_free(NphHandlePool *pool)
{
    if (pool == NULL || pool->alloc == NULL) {
        return;
    }
    NPH_FREE_ARRAY(pool->alloc, u32, pool->generations, pool->capacity);
    NPH_FREE_ARRAY(pool->alloc, u32, pool->free_queue, pool->capacity);
    memset(pool, 0, sizeof *pool);
}

NphResult nph_handle_pool_alloc(NphHandlePool *pool, NphHandle *out)
{
    NPH_ASSERT(pool != NULL && out != NULL);
    u32 index;
    // Prefer never-used slots until the free queue is long enough that a
    // reused slot has been out of circulation for a while.
    const bool reuse = pool->free_count > 0U && (pool->free_count > pool->min_free_before_reuse ||
                                                 pool->bump == pool->capacity);
    if (reuse) {
        index = pool->free_queue[pool->free_head];
        pool->free_head = (pool->free_head + 1U) % pool->capacity;
        pool->free_count -= 1U;
    } else if (pool->bump < pool->capacity) {
        index = pool->bump++;
        pool->generations[index] = 1U;
    } else {
        *out = NPH_HANDLE_NULL;
        return NPH_RESULT_FULL;
    }
    pool->live += 1U;
    *out = make_handle(pool, index, pool->generations[index]);
    return NPH_RESULT_OK;
}

bool nph_handle_pool_is_valid(const NphHandlePool *pool, NphHandle h)
{
    NPH_ASSERT(pool != NULL);
    if (h.value == 0U) {
        return false;
    }
    const u32 index = nph_handle_pool_index(pool, h);
    const u32 generation = nph_handle_pool_generation(pool, h);
    return index < pool->bump && pool->generations[index] == generation;
}

NphResult nph_handle_pool_copy(NphHandlePool *dst, const NphHandlePool *src)
{
    NPH_ASSERT(dst != NULL && src != NULL);
    if (dst->capacity != src->capacity || dst->index_bits != src->index_bits) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    memcpy(dst->generations, src->generations, sizeof(u32) * src->capacity);
    // Only the queued part of the ring holds slot indices; it is unrolled
    // to the start of dst's ring.
    for (u32 i = 0; i < src->free_count; ++i) {
        dst->free_queue[i] = src->free_queue[(src->free_head + i) % src->capacity];
    }
    dst->free_head = 0U;
    dst->free_count = src->free_count;
    dst->bump = src->bump;
    dst->live = src->live;
    dst->min_free_before_reuse = src->min_free_before_reuse;
    return NPH_RESULT_OK;
}

bool nph_handle_pool_release(NphHandlePool *pool, NphHandle h)
{
    NPH_ASSERT(pool != NULL);
    if (!nph_handle_pool_is_valid(pool, h)) {
        return false;
    }
    const u32 index = nph_handle_pool_index(pool, h);
    // Bump the generation so the released handle stops matching; 0 is
    // reserved for "never used" and as the null handle's generation.
    u32 generation = pool->generations[index];
    generation = generation >= pool->generation_max ? 1U : generation + 1U;
    pool->generations[index] = generation;
    pool->free_queue[(pool->free_head + pool->free_count) % pool->capacity] = index;
    pool->free_count += 1U;
    pool->live -= 1U;
    return true;
}

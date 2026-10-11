// SPDX-License-Identifier: Apache-2.0
// Two-Level Segregated Fit allocator (Masmano et al., ECRTS 2004). See tlsf.h
// for the layout and docs/core.md for the design notes.
#include <nepheline/core/assert.h>
#include <nepheline/core/tlsf.h>

#include <string.h>

// ---------------------------------------------------------------------------
// Block header
// ---------------------------------------------------------------------------

typedef struct TlsfBlock {
    struct TlsfBlock *prev_phys; // physically preceding block; NULL for a pool's first block
    u64 size_flags;              // payload size (multiple of 16) | flags
} TlsfBlock;

// The first 16 payload bytes of a free block.
typedef struct TlsfFreeLinks {
    TlsfBlock *next_free;
    TlsfBlock *prev_free;
} TlsfFreeLinks;

#define BLOCK_FLAG_FREE      ((u64)1U)
#define BLOCK_FLAG_PREV_FREE ((u64)2U)
#define BLOCK_FLAG_MASK      ((u64)NPH_TLSF_ALIGN - 1U)
#define BLOCK_HEADER_SIZE    ((u64)sizeof(TlsfBlock))
#define BLOCK_PAYLOAD_MIN    ((u64)sizeof(TlsfFreeLinks))
#define BLOCK_SPAN_MIN       (BLOCK_HEADER_SIZE + BLOCK_PAYLOAD_MIN)
#define SMALL_BLOCK_SIZE     ((u64)1U << NPH_TLSF_FL_SHIFT)

NPH_STATIC_ASSERT(sizeof(TlsfBlock) == NPH_TLSF_ALIGN, "block header must be one alignment unit");
NPH_STATIC_ASSERT(sizeof(TlsfFreeLinks) == NPH_TLSF_ALIGN,
                  "free links must fit the minimum payload");
NPH_STATIC_ASSERT(sizeof(((NphTlsf *)0)->null_block) >= sizeof(TlsfBlock) + sizeof(TlsfFreeLinks),
                  "null_block must hold a header and its links");
NPH_STATIC_ASSERT(NPH_TLSF_FL_COUNT <= 64U, "fl_bitmap is 64 bits wide");
NPH_STATIC_ASSERT(NPH_TLSF_SL_COUNT == 32U, "sl_bitmap entries are 32 bits wide");

static u64 block_size(const TlsfBlock *b)
{
    return b->size_flags & ~BLOCK_FLAG_MASK;
}

static void block_set_size(TlsfBlock *b, u64 size)
{
    b->size_flags = size | (b->size_flags & BLOCK_FLAG_MASK);
}

static bool block_is_free(const TlsfBlock *b)
{
    return (b->size_flags & BLOCK_FLAG_FREE) != 0U;
}

static bool block_is_prev_free(const TlsfBlock *b)
{
    return (b->size_flags & BLOCK_FLAG_PREV_FREE) != 0U;
}

static void block_set_free(TlsfBlock *b, bool free)
{
    b->size_flags = free ? (b->size_flags | BLOCK_FLAG_FREE) : (b->size_flags & ~BLOCK_FLAG_FREE);
}

static void block_set_prev_free(TlsfBlock *b, bool free)
{
    b->size_flags =
        free ? (b->size_flags | BLOCK_FLAG_PREV_FREE) : (b->size_flags & ~BLOCK_FLAG_PREV_FREE);
}

// The sentinel at the end of a pool has a zero payload size and is never free.
static bool block_is_sentinel(const TlsfBlock *b)
{
    return block_size(b) == 0U;
}

static u8 *block_payload(TlsfBlock *b)
{
    return (u8 *)b + BLOCK_HEADER_SIZE;
}

static TlsfBlock *block_from_payload(void *payload)
{
    return (TlsfBlock *)((u8 *)payload - BLOCK_HEADER_SIZE);
}

static TlsfBlock *block_next(TlsfBlock *b)
{
    return (TlsfBlock *)(block_payload(b) + block_size(b));
}

static TlsfFreeLinks *block_links(TlsfBlock *b)
{
    return (TlsfFreeLinks *)block_payload(b);
}

// ASan bookkeeping: a used block's payload is addressable; a free block
// exposes only its links. Headers are always addressable.
static void block_asan_mark_used(TlsfBlock *b)
{
    NPH_ASAN_UNPOISON(block_payload(b), block_size(b));
}

static void block_asan_mark_free(TlsfBlock *b)
{
    const u64 size = block_size(b);
    NPH_ASAN_UNPOISON(block_payload(b), BLOCK_PAYLOAD_MIN);
    if (size > BLOCK_PAYLOAD_MIN) {
        NPH_ASAN_POISON(block_payload(b) + BLOCK_PAYLOAD_MIN, size - BLOCK_PAYLOAD_MIN);
    }
}

// ---------------------------------------------------------------------------
// Size-class mapping (paper §3.1)
// ---------------------------------------------------------------------------

static void mapping_insert(u64 size, u32 *fl, u32 *sl)
{
    if (size < SMALL_BLOCK_SIZE) {
        *fl = 0U;
        *sl = (u32)(size / (SMALL_BLOCK_SIZE / NPH_TLSF_SL_COUNT));
    } else {
        const u32 f = nph_log2_floor_u64(size);
        *sl = (u32)((size >> (f - NPH_TLSF_SL_LOG2)) ^ ((u64)1U << NPH_TLSF_SL_LOG2));
        *fl = f - (NPH_TLSF_FL_SHIFT - 1U);
    }
}

// Rounds the request up to the start of the next class, so every block in
// the class found by search is guaranteed to fit.
static void mapping_search(u64 size, u32 *fl, u32 *sl)
{
    if (size >= SMALL_BLOCK_SIZE) {
        const u64 round = ((u64)1U << (nph_log2_floor_u64(size) - NPH_TLSF_SL_LOG2)) - 1U;
        size += round;
    }
    mapping_insert(size, fl, sl);
}

static TlsfBlock *null_block(NphTlsf *tlsf)
{
    return (TlsfBlock *)tlsf->null_block;
}

static const TlsfBlock *null_block_const(const NphTlsf *tlsf)
{
    return (const TlsfBlock *)tlsf->null_block;
}

static const TlsfFreeLinks *block_links_const(const TlsfBlock *b)
{
    return (const TlsfFreeLinks *)((const u8 *)b + BLOCK_HEADER_SIZE);
}

static TlsfBlock *search_suitable_block(NphTlsf *tlsf, u32 *fl, u32 *sl)
{
    u32 f = *fl;
    u32 s = *sl;
    u32 sl_map = tlsf->sl_bitmap[f] & (~0U << s);
    if (sl_map == 0U) {
        // Nothing in this first-level class at or above sl: try larger classes.
        const u64 fl_map = tlsf->fl_bitmap & (~(u64)0U << (f + 1U));
        if (fl_map == 0U) {
            return NULL;
        }
        f = nph_ctz_u64(fl_map);
        sl_map = tlsf->sl_bitmap[f];
    }
    s = nph_ctz_u32(sl_map);
    *fl = f;
    *sl = s;
    return (TlsfBlock *)tlsf->blocks[f][s];
}

// ---------------------------------------------------------------------------
// Free lists
// ---------------------------------------------------------------------------

static void remove_free_block(NphTlsf *tlsf, TlsfBlock *block, u32 fl, u32 sl)
{
    TlsfFreeLinks *links = block_links(block);
    TlsfBlock *prev = links->prev_free;
    TlsfBlock *next = links->next_free;
    block_links(next)->prev_free = prev;
    block_links(prev)->next_free = next;
    if (tlsf->blocks[fl][sl] == block) {
        tlsf->blocks[fl][sl] = next;
        if (next == null_block(tlsf)) {
            tlsf->sl_bitmap[fl] &= ~(1U << sl);
            if (tlsf->sl_bitmap[fl] == 0U) {
                tlsf->fl_bitmap &= ~((u64)1U << fl);
            }
        }
    }
}

static void insert_free_block(NphTlsf *tlsf, TlsfBlock *block, u32 fl, u32 sl)
{
    TlsfBlock *current = (TlsfBlock *)tlsf->blocks[fl][sl];
    TlsfFreeLinks *links = block_links(block);
    links->next_free = current;
    links->prev_free = null_block(tlsf);
    block_links(current)->prev_free = block;
    tlsf->blocks[fl][sl] = block;
    tlsf->fl_bitmap |= (u64)1U << fl;
    tlsf->sl_bitmap[fl] |= 1U << sl;
}

static void block_remove(NphTlsf *tlsf, TlsfBlock *block)
{
    u32 fl;
    u32 sl;
    mapping_insert(block_size(block), &fl, &sl);
    remove_free_block(tlsf, block, fl, sl);
}

static void block_insert(NphTlsf *tlsf, TlsfBlock *block)
{
    u32 fl;
    u32 sl;
    mapping_insert(block_size(block), &fl, &sl);
    insert_free_block(tlsf, block, fl, sl);
}

// ---------------------------------------------------------------------------
// Splitting and merging
// ---------------------------------------------------------------------------

// Marks `block` free and records that in its physical successor.
static void block_mark_as_free(TlsfBlock *block)
{
    TlsfBlock *next = block_next(block);
    block_set_prev_free(next, true);
    next->prev_phys = block;
    block_set_free(block, true);
}

static void block_mark_as_used(TlsfBlock *block)
{
    TlsfBlock *next = block_next(block);
    block_set_prev_free(next, false);
    block_set_free(block, false);
}

static bool block_can_split(const TlsfBlock *block, u64 size)
{
    return block_size(block) >= size + BLOCK_SPAN_MIN;
}

// Splits `block` so that it keeps `size` payload bytes; returns the new
// block made of the remainder. The remainder is not yet in any list.
static TlsfBlock *block_split(TlsfBlock *block, u64 size)
{
    const u64 remain_size = block_size(block) - size - BLOCK_HEADER_SIZE;
    TlsfBlock *remaining = (TlsfBlock *)(block_payload(block) + size);
    NPH_ASAN_UNPOISON(remaining, BLOCK_HEADER_SIZE);
    remaining->prev_phys = block;
    remaining->size_flags = remain_size; // flags set by the caller
    block_set_size(block, size);
    return remaining;
}

// Merges `second`, the block physically following `first`, into `first`.
static TlsfBlock *block_absorb(TlsfBlock *first, TlsfBlock *second)
{
    block_set_size(first, block_size(first) + BLOCK_HEADER_SIZE + block_size(second));
    block_next(first)->prev_phys = first;
    return first;
}

static TlsfBlock *block_merge_prev(NphTlsf *tlsf, TlsfBlock *block)
{
    if (block_is_prev_free(block)) {
        TlsfBlock *prev = block->prev_phys;
        NPH_ASSERT(prev != NULL && block_is_free(prev));
        block_remove(tlsf, prev);
        block = block_absorb(prev, block);
    }
    return block;
}

static TlsfBlock *block_merge_next(NphTlsf *tlsf, TlsfBlock *block)
{
    TlsfBlock *next = block_next(block);
    if (block_is_free(next)) {
        NPH_ASSERT(!block_is_sentinel(next));
        block_remove(tlsf, next);
        block = block_absorb(block, next);
    }
    return block;
}

// Trims a free block to `size` and returns the remainder to the free lists.
static void block_trim_free(NphTlsf *tlsf, TlsfBlock *block, u64 size)
{
    NPH_ASSERT(block_is_free(block));
    if (block_can_split(block, size)) {
        TlsfBlock *remaining = block_split(block, size);
        block_set_prev_free(remaining, true);
        block_mark_as_free(remaining);
        block_asan_mark_free(remaining);
        block_insert(tlsf, remaining);
    }
}

// Trims a used block to `size`; the remainder becomes a free block, merged
// with a free successor.
static void block_trim_used(NphTlsf *tlsf, TlsfBlock *block, u64 size)
{
    NPH_ASSERT(!block_is_free(block));
    if (block_can_split(block, size)) {
        TlsfBlock *remaining = block_split(block, size);
        block_set_prev_free(remaining, false);
        block_set_free(remaining, true);
        block_next(remaining)->prev_phys = remaining;
        remaining = block_merge_next(tlsf, remaining);
        block_mark_as_free(remaining);
        block_asan_mark_free(remaining);
        block_insert(tlsf, remaining);
    }
}

// Splits off the first `size` payload bytes of a free block as a new free
// block (used to create a gap before an over-aligned allocation) and
// returns the second part.
static TlsfBlock *block_trim_free_leading(NphTlsf *tlsf, TlsfBlock *block, u64 size)
{
    TlsfBlock *remaining = block;
    if (block_can_split(block, size)) {
        remaining = block_split(block, size);
        block_set_prev_free(remaining, true);
        block_set_free(remaining, true);
        block_next(remaining)->prev_phys = remaining;
        block_mark_as_free(block);
        block_asan_mark_free(block);
        block_insert(tlsf, block);
    }
    return remaining;
}

// Departure from the paper: the good-fit search rounds the request up to the
// next class, so blocks in the request's own class are never considered even
// though some of them may fit (a request for all of a fresh pool is the
// common case). When the O(1) search fails, that one list is scanned for a
// first fit before giving up. Only the failure path pays for the scan, and
// it is bounded by the number of free blocks in a single class.
static TlsfBlock *block_locate_free_exact_class(NphTlsf *tlsf, u64 size)
{
    u32 fl;
    u32 sl;
    mapping_insert(size, &fl, &sl);
    if (fl >= NPH_TLSF_FL_COUNT) {
        return NULL;
    }
    TlsfBlock *nb = null_block(tlsf);
    for (TlsfBlock *b = (TlsfBlock *)tlsf->blocks[fl][sl]; b != nb; b = block_links(b)->next_free) {
        if (block_size(b) >= size) {
            remove_free_block(tlsf, b, fl, sl);
            return b;
        }
    }
    return NULL;
}

static TlsfBlock *block_locate_free(NphTlsf *tlsf, u64 size)
{
    u32 fl = 0U;
    u32 sl = 0U;
    mapping_search(size, &fl, &sl);
    // A rounded-up search size can map past the last class; only the
    // request's own class can hold a fitting block then.
    TlsfBlock *block = fl < NPH_TLSF_FL_COUNT ? search_suitable_block(tlsf, &fl, &sl) : NULL;
    if (block == NULL) {
        return block_locate_free_exact_class(tlsf, size);
    }
    NPH_ASSERT(block_size(block) >= size);
    remove_free_block(tlsf, block, fl, sl);
    return block;
}

static void *block_prepare_used(NphTlsf *tlsf, TlsfBlock *block, u64 size)
{
    block_trim_free(tlsf, block, size);
    block_mark_as_used(block);
    block_asan_mark_used(block);
    tlsf->live_count += 1U;
    tlsf->live_bytes += block_size(block);
    tlsf->peak_live_bytes = nph_max_u64(tlsf->peak_live_bytes, tlsf->live_bytes);
    return block_payload(block);
}

static u64 adjust_request_size(u64 size)
{
    const u64 adjusted = nph_align_up_u64(size, NPH_TLSF_ALIGN);
    return nph_max_u64(adjusted, BLOCK_PAYLOAD_MIN);
}

// ---------------------------------------------------------------------------
// Vtable
// ---------------------------------------------------------------------------

static void *tlsf_vt_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(site);
    return nph_tlsf_alloc((NphTlsf *)self, size, align);
}

static void *tlsf_vt_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                             NphCallsite site)
{
    NPH_UNUSED(site);
    NPH_ASSERT(nph_tlsf_usable_size(ptr) >= old_size);
    NPH_UNUSED(old_size);
    return nph_tlsf_realloc((NphTlsf *)self, ptr, new_size, align);
}

static void tlsf_vt_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    NPH_ASSERT(nph_tlsf_usable_size(ptr) >= size);
    NPH_UNUSED(size);
    nph_tlsf_free((NphTlsf *)self, ptr);
}

static const NphAllocatorVtable g_tlsf_vtable = {tlsf_vt_alloc, tlsf_vt_realloc, tlsf_vt_free};

// ---------------------------------------------------------------------------
// Pools
// ---------------------------------------------------------------------------

static NphResult add_pool(NphTlsf *tlsf, void *memory, u64 size, NphAllocator *backing)
{
    if (memory == NULL || size < NPH_TLSF_MIN_POOL_SIZE || size > NPH_TLSF_BLOCK_SIZE_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (tlsf->pool_count >= NPH_TLSF_MAX_POOLS) {
        return NPH_RESULT_FULL;
    }
    // Layout: [slack][first header][payload][sentinel header]. The region is
    // trimmed so the first block starts aligned.
    const u64 slack = nph_align_up_u64((uptr)memory, NPH_TLSF_ALIGN) - (uptr)memory;
    if (slack >= size) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 usable = nph_align_down_u64(size - slack, NPH_TLSF_ALIGN);
    if (usable < BLOCK_SPAN_MIN + BLOCK_HEADER_SIZE) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 payload = usable - BLOCK_HEADER_SIZE - BLOCK_HEADER_SIZE;

    TlsfBlock *block = (TlsfBlock *)((u8 *)memory + slack);
    block->prev_phys = NULL;
    block->size_flags = payload | BLOCK_FLAG_FREE;

    TlsfBlock *sentinel = block_next(block);
    sentinel->prev_phys = block;
    sentinel->size_flags = BLOCK_FLAG_PREV_FREE;

    block_asan_mark_free(block);
    block_insert(tlsf, block);

    NphTlsfPool *pool = &tlsf->pools[tlsf->pool_count++];
    pool->base = (u8 *)memory;
    pool->size = size;
    pool->backing = backing;
    return NPH_RESULT_OK;
}

static TlsfBlock *pool_first_block(const NphTlsfPool *pool)
{
    const u64 slack = nph_align_up_u64((uptr)pool->base, NPH_TLSF_ALIGN) - (uptr)pool->base;
    return (TlsfBlock *)(pool->base + slack);
}

static NphResult release_pool(NphTlsf *tlsf, u32 index)
{
    NphTlsfPool *pool = &tlsf->pools[index];
    TlsfBlock *block = pool_first_block(pool);
    TlsfBlock *next = block_next(block);
    if (!block_is_free(block) || !block_is_sentinel(next)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    block_remove(tlsf, block);
    NPH_ASAN_UNPOISON(pool->base, pool->size);
    if (pool->backing != NULL) {
        NPH_FREE(pool->backing, pool->base, pool->size, NPH_ALLOC_MAX_ALIGN);
    }
    for (u32 i = index + 1U; i < tlsf->pool_count; ++i) {
        tlsf->pools[i - 1U] = tlsf->pools[i];
    }
    tlsf->pool_count -= 1U;
    memset(&tlsf->pools[tlsf->pool_count], 0, sizeof tlsf->pools[0]);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

NphResult nph_tlsf_init(NphTlsf *tlsf, void *memory, u64 size)
{
    NPH_ASSERT(tlsf != NULL);
    memset(tlsf, 0, sizeof *tlsf);
    tlsf->allocator.vtable = &g_tlsf_vtable;
    tlsf->allocator.name = "tlsf";
    TlsfBlock *nb = null_block(tlsf);
    nb->size_flags = 0U;
    block_links(nb)->next_free = nb;
    block_links(nb)->prev_free = nb;
    for (u32 f = 0; f < NPH_TLSF_FL_COUNT; ++f) {
        for (u32 s = 0; s < NPH_TLSF_SL_COUNT; ++s) {
            tlsf->blocks[f][s] = nb;
        }
    }
    const NphResult r = add_pool(tlsf, memory, size, NULL);
    if (r != NPH_RESULT_OK) {
        memset(tlsf, 0, sizeof *tlsf);
    }
    return r;
}

NphResult nph_tlsf_init_from(NphTlsf *tlsf, NphAllocator *backing, u64 size)
{
    NPH_ASSERT(backing != NULL);
    if (size < NPH_TLSF_MIN_POOL_SIZE || size > NPH_TLSF_BLOCK_SIZE_MAX) {
        memset(tlsf, 0, sizeof *tlsf);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    void *memory = NPH_ALLOC(backing, size, NPH_ALLOC_MAX_ALIGN);
    if (memory == NULL) {
        memset(tlsf, 0, sizeof *tlsf);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const NphResult r = nph_tlsf_init(tlsf, memory, size);
    if (r != NPH_RESULT_OK) {
        NPH_FREE(backing, memory, size, NPH_ALLOC_MAX_ALIGN);
        return r;
    }
    tlsf->pools[0].backing = backing;
    return NPH_RESULT_OK;
}

void nph_tlsf_destroy(NphTlsf *tlsf)
{
    if (tlsf == NULL || tlsf->allocator.vtable == NULL) {
        return;
    }
    for (u32 i = 0; i < tlsf->pool_count; ++i) {
        NphTlsfPool *pool = &tlsf->pools[i];
        NPH_ASAN_UNPOISON(pool->base, pool->size);
        if (pool->backing != NULL) {
            NPH_FREE(pool->backing, pool->base, pool->size, NPH_ALLOC_MAX_ALIGN);
        }
    }
    memset(tlsf, 0, sizeof *tlsf);
}

NphResult nph_tlsf_add_pool(NphTlsf *tlsf, void *memory, u64 size)
{
    NPH_ASSERT(tlsf != NULL && tlsf->allocator.vtable != NULL);
    return add_pool(tlsf, memory, size, NULL);
}

NphResult nph_tlsf_add_pool_from(NphTlsf *tlsf, NphAllocator *backing, u64 size)
{
    NPH_ASSERT(tlsf != NULL && tlsf->allocator.vtable != NULL);
    NPH_ASSERT(backing != NULL);
    if (size < NPH_TLSF_MIN_POOL_SIZE || size > NPH_TLSF_BLOCK_SIZE_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (tlsf->pool_count >= NPH_TLSF_MAX_POOLS) {
        return NPH_RESULT_FULL;
    }
    void *memory = NPH_ALLOC(backing, size, NPH_ALLOC_MAX_ALIGN);
    if (memory == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const NphResult r = add_pool(tlsf, memory, size, backing);
    if (r != NPH_RESULT_OK) {
        NPH_FREE(backing, memory, size, NPH_ALLOC_MAX_ALIGN);
    }
    return r;
}

NphResult nph_tlsf_remove_pool(NphTlsf *tlsf, void *memory)
{
    NPH_ASSERT(tlsf != NULL);
    for (u32 i = 0; i < tlsf->pool_count; ++i) {
        if (tlsf->pools[i].base == (u8 *)memory) {
            return release_pool(tlsf, i);
        }
    }
    return NPH_RESULT_NOT_FOUND;
}

void *nph_tlsf_alloc(NphTlsf *tlsf, u64 size, u32 align)
{
    NPH_ASSERT(tlsf != NULL);
    NPH_ASSERT(size > 0U);
    NPH_ASSERT(nph_is_pow2_u32(align));
    if (size == 0U || size > NPH_TLSF_BLOCK_SIZE_MAX - NPH_TLSF_ALIGN) {
        return NULL;
    }
    const u64 adjust = adjust_request_size(size);

    if (align <= NPH_TLSF_ALIGN) {
        TlsfBlock *block = block_locate_free(tlsf, adjust);
        if (block == NULL) {
            return NULL;
        }
        return block_prepare_used(tlsf, block, adjust);
    }

    // Over-aligned: find a block large enough to contain the payload at an
    // aligned address with room to split the leading gap off as a free
    // block (which needs at least BLOCK_SPAN_MIN bytes).
    const u64 gap_min = BLOCK_SPAN_MIN;
    const u64 size_with_gap = adjust_request_size(adjust + align + gap_min);
    if (size_with_gap > NPH_TLSF_BLOCK_SIZE_MAX) {
        return NULL;
    }
    TlsfBlock *block = block_locate_free(tlsf, size_with_gap);
    if (block == NULL) {
        return NULL;
    }
    u8 *payload = block_payload(block);
    uptr aligned = nph_align_up_u64((uptr)payload, align);
    u64 gap = aligned - (uptr)payload;
    if (gap != 0U && gap < gap_min) {
        // Too small for a free block: move to the next aligned address.
        const u64 shortfall = gap_min - gap;
        const u64 bump = nph_max_u64(shortfall, (u64)align);
        aligned = nph_align_up_u64((uptr)payload + bump, align);
        gap = aligned - (uptr)payload;
    }
    if (gap != 0U) {
        NPH_ASSERT(gap >= gap_min);
        // The leading free block keeps `gap - header` payload bytes.
        block = block_trim_free_leading(tlsf, block, gap - BLOCK_HEADER_SIZE);
        NPH_ASSERT((uptr)block_payload(block) == aligned);
    }
    return block_prepare_used(tlsf, block, adjust);
}

void nph_tlsf_free(NphTlsf *tlsf, void *ptr)
{
    NPH_ASSERT(tlsf != NULL);
    if (ptr == NULL) {
        return;
    }
    NPH_ASSERT_MSG(nph_tlsf_owns(tlsf, ptr), "pointer %p is not from this heap", ptr);
    TlsfBlock *block = block_from_payload(ptr);
    NPH_ASSERT_MSG(!block_is_free(block), "double free of %p", ptr);
    if (block_is_free(block)) {
        return;
    }
    tlsf->live_count -= 1U;
    tlsf->live_bytes -= block_size(block);
    block_mark_as_free(block);
    block = block_merge_prev(tlsf, block);
    block = block_merge_next(tlsf, block);
    block_mark_as_free(block); // the merged block's successor must see it free
    block_asan_mark_free(block);
    block_insert(tlsf, block);
}

void *nph_tlsf_realloc(NphTlsf *tlsf, void *ptr, u64 new_size, u32 align)
{
    NPH_ASSERT(tlsf != NULL);
    NPH_ASSERT(new_size > 0U);
    if (ptr == NULL) {
        return nph_tlsf_alloc(tlsf, new_size, align);
    }
    if (new_size == 0U || new_size > NPH_TLSF_BLOCK_SIZE_MAX - NPH_TLSF_ALIGN) {
        return NULL;
    }
    TlsfBlock *block = block_from_payload(ptr);
    NPH_ASSERT(!block_is_free(block));
    const u64 cur_size = block_size(block);
    const u64 adjust = adjust_request_size(new_size);

    if (nph_is_ptr_aligned(ptr, align)) {
        TlsfBlock *next = block_next(block);
        const u64 combined =
            block_is_free(next) ? cur_size + BLOCK_HEADER_SIZE + block_size(next) : cur_size;
        if (adjust <= cur_size) {
            // Shrink in place.
            tlsf->live_bytes -= cur_size;
            block_trim_used(tlsf, block, adjust);
            tlsf->live_bytes += block_size(block);
            NPH_ASAN_UNPOISON(ptr, block_size(block));
            return ptr;
        }
        if (adjust <= combined) {
            // Grow into the free successor.
            tlsf->live_bytes -= cur_size;
            block_merge_next(tlsf, block);
            block_mark_as_used(block);
            block_trim_used(tlsf, block, adjust);
            tlsf->live_bytes += block_size(block);
            tlsf->peak_live_bytes = nph_max_u64(tlsf->peak_live_bytes, tlsf->live_bytes);
            block_asan_mark_used(block);
            return ptr;
        }
    }

    void *fresh = nph_tlsf_alloc(tlsf, new_size, align);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, (size_t)nph_min_u64(cur_size, new_size));
    nph_tlsf_free(tlsf, ptr);
    return fresh;
}

u64 nph_tlsf_usable_size(const void *ptr)
{
    NPH_ASSERT(ptr != NULL);
    const TlsfBlock *block = (const TlsfBlock *)((const u8 *)ptr - BLOCK_HEADER_SIZE);
    return block_size(block);
}

bool nph_tlsf_owns(const NphTlsf *tlsf, const void *ptr)
{
    const uptr p = (uptr)ptr;
    for (u32 i = 0; i < tlsf->pool_count; ++i) {
        const NphTlsfPool *pool = &tlsf->pools[i];
        if (p >= (uptr)pool->base && p < (uptr)pool->base + pool->size) {
            return true;
        }
    }
    return false;
}

void nph_tlsf_get_stats(const NphTlsf *tlsf, NphTlsfStats *out)
{
    NPH_ASSERT(tlsf != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->live_bytes = tlsf->live_bytes;
    out->live_count = tlsf->live_count;
    out->peak_live_bytes = tlsf->peak_live_bytes;
    out->pool_count = tlsf->pool_count;
    for (u32 i = 0; i < tlsf->pool_count; ++i) {
        const NphTlsfPool *pool = &tlsf->pools[i];
        out->pool_bytes += pool->size;
        for (TlsfBlock *b = pool_first_block(pool); !block_is_sentinel(b); b = block_next(b)) {
            if (block_is_free(b)) {
                out->free_bytes += block_size(b);
                out->free_count += 1U;
            }
        }
    }
}

void nph_tlsf_walk(const NphTlsf *tlsf, NphTlsfWalkFn fn, void *user)
{
    NPH_ASSERT(tlsf != NULL && fn != NULL);
    for (u32 i = 0; i < tlsf->pool_count; ++i) {
        for (TlsfBlock *b = pool_first_block(&tlsf->pools[i]); !block_is_sentinel(b);
             b = block_next(b)) {
            fn(block_payload(b), block_size(b), !block_is_free(b), user);
        }
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

#define TLSF_CHECK(cond, reason)                                                                   \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            if (out_reason != NULL) {                                                              \
                *out_reason = (reason);                                                            \
            }                                                                                      \
            return NPH_RESULT_CORRUPT;                                                             \
        }                                                                                          \
    } while (0)

// True if `block` is present in the free list for class (fl, sl).
static bool free_list_contains(const NphTlsf *tlsf, const TlsfBlock *block, u32 fl, u32 sl)
{
    const TlsfBlock *nb = null_block_const(tlsf);
    for (const TlsfBlock *b = (const TlsfBlock *)tlsf->blocks[fl][sl]; b != nb;
         b = block_links_const(b)->next_free) {
        if (b == block) {
            return true;
        }
    }
    return false;
}

NphResult nph_tlsf_validate(const NphTlsf *tlsf, const char **out_reason)
{
    TLSF_CHECK(tlsf != NULL && tlsf->allocator.vtable == &g_tlsf_vtable, "not an initialised heap");
    const TlsfBlock *nb = null_block_const(tlsf);
    TLSF_CHECK(tlsf->pool_count <= NPH_TLSF_MAX_POOLS, "pool_count out of range");

    // Physical walk of every pool.
    u64 live_bytes = 0U;
    u32 live_count = 0U;
    u32 free_count_phys = 0U;
    for (u32 i = 0; i < tlsf->pool_count; ++i) {
        const NphTlsfPool *pool = &tlsf->pools[i];
        const uptr end = (uptr)pool->base + pool->size;
        const TlsfBlock *prev = NULL;
        TlsfBlock *b = pool_first_block(pool);
        TLSF_CHECK(b->prev_phys == NULL, "first block has a physical predecessor");
        TLSF_CHECK(!block_is_prev_free(b), "first block claims a free predecessor");
        for (;;) {
            TLSF_CHECK((uptr)b + BLOCK_HEADER_SIZE <= end, "block header outside its pool");
            TLSF_CHECK(nph_is_ptr_aligned(b, NPH_TLSF_ALIGN), "block header misaligned");
            TLSF_CHECK(b->prev_phys == prev, "prev_phys link is wrong");
            if (prev != NULL) {
                TLSF_CHECK(block_is_prev_free(b) == block_is_free(prev),
                           "prev_free flag disagrees with the predecessor");
            }
            if (block_is_sentinel(b)) {
                TLSF_CHECK(!block_is_free(b), "sentinel marked free");
                TLSF_CHECK((uptr)b + BLOCK_HEADER_SIZE <= end, "sentinel past the pool end");
                break;
            }
            const u64 size = block_size(b);
            TLSF_CHECK(size >= BLOCK_PAYLOAD_MIN, "block smaller than the minimum");
            TLSF_CHECK(nph_is_aligned_u64(size, NPH_TLSF_ALIGN), "block size not a multiple of 16");
            TLSF_CHECK((uptr)block_payload(b) + size + BLOCK_HEADER_SIZE <= end,
                       "block extends past its pool");
            if (block_is_free(b)) {
                TLSF_CHECK(prev == NULL || !block_is_free(prev), "two adjacent free blocks");
                u32 fl;
                u32 sl;
                mapping_insert(size, &fl, &sl);
                TLSF_CHECK(fl < NPH_TLSF_FL_COUNT && sl < NPH_TLSF_SL_COUNT,
                           "free block maps outside the classes");
                TLSF_CHECK((tlsf->fl_bitmap & ((u64)1U << fl)) != 0U,
                           "free block's class is clear in fl_bitmap");
                TLSF_CHECK((tlsf->sl_bitmap[fl] & (1U << sl)) != 0U,
                           "free block's class is clear in sl_bitmap");
                TLSF_CHECK(free_list_contains(tlsf, b, fl, sl), "free block missing from its list");
                free_count_phys += 1U;
            } else {
                live_bytes += size;
                live_count += 1U;
            }
            prev = b;
            b = block_next(b);
        }
    }
    TLSF_CHECK(live_bytes == tlsf->live_bytes, "live_bytes counter disagrees with the heap");
    TLSF_CHECK(live_count == tlsf->live_count, "live_count counter disagrees with the heap");

    // Free lists and bitmaps.
    u32 free_count_lists = 0U;
    for (u32 f = 0; f < NPH_TLSF_FL_COUNT; ++f) {
        const bool fl_set = (tlsf->fl_bitmap & ((u64)1U << f)) != 0U;
        TLSF_CHECK(fl_set == (tlsf->sl_bitmap[f] != 0U), "fl_bitmap disagrees with sl_bitmap");
        for (u32 s = 0; s < NPH_TLSF_SL_COUNT; ++s) {
            const TlsfBlock *head = (const TlsfBlock *)tlsf->blocks[f][s];
            const bool sl_set = (tlsf->sl_bitmap[f] & (1U << s)) != 0U;
            TLSF_CHECK(sl_set == (head != nb), "sl_bitmap disagrees with the list head");
            const TlsfBlock *prev_free = nb;
            for (const TlsfBlock *b = head; b != nb; b = block_links_const(b)->next_free) {
                TLSF_CHECK(block_is_free(b), "used block in a free list");
                TLSF_CHECK(block_links_const(b)->prev_free == prev_free,
                           "free list prev link is wrong");
                u32 fl;
                u32 sl;
                mapping_insert(block_size(b), &fl, &sl);
                TLSF_CHECK(fl == f && sl == s, "free block in the wrong list");
                free_count_lists += 1U;
                TLSF_CHECK(free_count_lists <= free_count_phys, "free list cycle or stray block");
                prev_free = b;
            }
        }
    }
    TLSF_CHECK(free_count_lists == free_count_phys, "free list count differs from the heap");
    return NPH_RESULT_OK;
}

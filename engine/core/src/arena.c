// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/arena.h>
#include <nepheline/core/assert.h>

#include <string.h>

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

static void *arena_vt_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(site);
    return nph_arena_alloc((NphArena *)self, size, align);
}

// Grows the most recent allocation in place when it is at the top of the
// arena; otherwise allocates fresh and copies (the old block is not reclaimed).
static void *arena_vt_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                              NphCallsite site)
{
    NphArena *arena = (NphArena *)self;
    NPH_ASSERT(nph_arena_owns(arena, ptr));
    const u64 begin = nph_ptr_diff(arena->base, ptr);
    if (begin + old_size == arena->offset && nph_is_ptr_aligned(ptr, align)) {
        if (begin + new_size > arena->capacity) {
            return NULL;
        }
        if (new_size > old_size) {
            NPH_ASAN_UNPOISON(arena->base + begin + old_size, new_size - old_size);
        } else {
            NPH_ASAN_POISON(arena->base + begin + new_size, old_size - new_size);
        }
        arena->offset = begin + new_size;
        arena->peak = nph_max_u64(arena->peak, arena->offset);
        return ptr;
    }
    void *fresh = nph_arena_alloc(arena, new_size, align);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, (size_t)nph_min_u64(old_size, new_size));
    NPH_UNUSED(site);
    return fresh;
}

// Only the most recent allocation can be reclaimed; any other free is a no-op.
static void arena_vt_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    NphArena *arena = (NphArena *)self;
    NPH_ASSERT(nph_arena_owns(arena, ptr));
    const u64 begin = nph_ptr_diff(arena->base, ptr);
    if (begin + size == arena->offset) {
        NPH_ASAN_POISON(ptr, size);
        arena->offset = begin;
    }
}

static const NphAllocatorVtable g_arena_vtable = {arena_vt_alloc, arena_vt_realloc, arena_vt_free};

void nph_arena_init(NphArena *arena, void *memory, u64 size)
{
    NPH_ASSERT(arena != NULL);
    NPH_ASSERT(memory != NULL || size == 0U);
    arena->allocator.vtable = &g_arena_vtable;
    arena->allocator.name = "arena";
    arena->base = (u8 *)memory;
    arena->capacity = size;
    arena->offset = 0U;
    arena->peak = 0U;
    arena->backing = NULL;
    NPH_ASAN_POISON(arena->base, arena->capacity);
}

NphResult nph_arena_init_from(NphArena *arena, NphAllocator *backing, u64 size)
{
    NPH_ASSERT(arena != NULL);
    NPH_ASSERT(backing != NULL);
    if (size == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    void *memory = NPH_ALLOC(backing, size, NPH_ALLOC_MAX_ALIGN);
    if (memory == NULL) {
        memset(arena, 0, sizeof *arena);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    nph_arena_init(arena, memory, size);
    arena->backing = backing;
    return NPH_RESULT_OK;
}

void nph_arena_destroy(NphArena *arena)
{
    if (arena == NULL || arena->base == NULL) {
        return;
    }
    NPH_ASAN_UNPOISON(arena->base, arena->capacity);
    if (arena->backing != NULL) {
        NPH_FREE(arena->backing, arena->base, arena->capacity, NPH_ALLOC_MAX_ALIGN);
    }
    memset(arena, 0, sizeof *arena);
}

void *nph_arena_alloc(NphArena *arena, u64 size, u32 align)
{
    NPH_ASSERT(arena != NULL);
    NPH_ASSERT(size > 0U);
    NPH_ASSERT(nph_is_pow2_u32(align));
    const uptr cursor = (uptr)arena->base + arena->offset;
    const uptr aligned = nph_align_up_u64(cursor, align);
    const u64 padding = aligned - cursor;
    // Overflow-safe exhaustion check: compare against remaining space.
    const u64 remaining = arena->capacity - arena->offset;
    if (padding > remaining || size > remaining - padding) {
        return NULL;
    }
    arena->offset += padding + size;
    arena->peak = nph_max_u64(arena->peak, arena->offset);
    NPH_ASAN_UNPOISON((void *)aligned, size);
    return (void *)aligned;
}

void *nph_arena_alloc_zeroed(NphArena *arena, u64 size, u32 align)
{
    void *p = nph_arena_alloc(arena, size, align);
    if (p != NULL) {
        memset(p, 0, (size_t)size);
    }
    return p;
}

void nph_arena_reset_to(NphArena *arena, NphArenaMarker marker)
{
    NPH_ASSERT(arena != NULL);
    NPH_ASSERT_MSG(marker.offset <= arena->offset, "marker %llu is past the arena offset %llu",
                   (unsigned long long)marker.offset, (unsigned long long)arena->offset);
    if (marker.offset > arena->offset) {
        return;
    }
    NPH_ASAN_POISON(arena->base + marker.offset, arena->offset - marker.offset);
    arena->offset = marker.offset;
}

void nph_arena_reset(NphArena *arena)
{
    const NphArenaMarker start = {0U};
    nph_arena_reset_to(arena, start);
}

// ---------------------------------------------------------------------------
// Frame arena
// ---------------------------------------------------------------------------

NphResult nph_frame_arena_init(NphFrameArena *fa, NphAllocator *backing, u64 bytes_per_frame,
                               u32 frame_count)
{
    NPH_ASSERT(fa != NULL);
    NPH_ASSERT(backing != NULL);
    memset(fa, 0, sizeof *fa);
    if (frame_count < 2U || frame_count > NPH_FRAME_ARENA_MAX_FRAMES || bytes_per_frame == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < frame_count; ++i) {
        const NphResult r = nph_arena_init_from(&fa->arenas[i], backing, bytes_per_frame);
        if (r != NPH_RESULT_OK) {
            for (u32 j = 0; j < i; ++j) {
                nph_arena_destroy(&fa->arenas[j]);
            }
            memset(fa, 0, sizeof *fa);
            return r;
        }
        fa->arenas[i].allocator.name = "frame-arena";
    }
    fa->frame_count = frame_count;
    fa->current = 0U;
    fa->frame_index = 0U;
    return NPH_RESULT_OK;
}

void nph_frame_arena_destroy(NphFrameArena *fa)
{
    if (fa == NULL) {
        return;
    }
    for (u32 i = 0; i < fa->frame_count; ++i) {
        nph_arena_destroy(&fa->arenas[i]);
    }
    memset(fa, 0, sizeof *fa);
}

void nph_frame_arena_begin_frame(NphFrameArena *fa)
{
    NPH_ASSERT(fa != NULL && fa->frame_count >= 2U);
    fa->frame_index += 1U;
    fa->current = (fa->current + 1U) % fa->frame_count;
    nph_arena_reset(&fa->arenas[fa->current]);
}

// SPDX-License-Identifier: Apache-2.0
#pragma once

// Linear (bump) arena with markers, and the double-buffered per-frame arena
// built on it (CLAUDE.md §8.1 "Allocators").
//
// An arena owns one contiguous block and hands out pieces of it by bumping a
// cursor. Allocation is a few instructions and never fails until the block
// is exhausted, at which point it returns NULL: there is no hidden growth
// (CLAUDE.md §1.4). Individual frees are no-ops, except that freeing the most
// recent allocation rolls the cursor back; memory is reclaimed in bulk by
// resetting to a marker or to the start.
//
// With AddressSanitizer, the unallocated part of the block is poisoned, so a
// read of reset memory is reported at the faulting instruction.
//
// Thread-safety: an arena is single-threaded. Each thread or job uses its own.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphArena {
    NphAllocator allocator; // first, so &arena->allocator == (NphAllocator *)arena
    u8 *base;
    u64 capacity;
    u64 offset;            // bytes in use; the next allocation starts at base + offset
    u64 peak;              // high-water mark of `offset` since init or the last reset_stats
    NphAllocator *backing; // owner of `base` when created with init_from, else NULL
} NphArena;

// A position in the arena, taken with nph_arena_mark and restored with
// nph_arena_reset_to. Markers from a different arena, or taken before a
// reset that already passed them, are invalid (asserted).
typedef struct NphArenaMarker {
    u64 offset;
} NphArenaMarker;

// Initialises `arena` over caller-owned memory [memory, memory + size).
// The caller keeps ownership of the block and must not touch it except
// through the arena. Never fails.
void nph_arena_init(NphArena *arena, void *memory, u64 size);

// Initialises `arena` over a block of `size` bytes obtained from `backing`.
// Returns NPH_RESULT_OUT_OF_MEMORY if the block cannot be allocated; the
// arena is then unusable. nph_arena_destroy returns the block.
NPH_MUST_USE NphResult nph_arena_init_from(NphArena *arena, NphAllocator *backing, u64 size);

// Releases the backing block if the arena owns one. Safe on an arena that
// was initialised with caller-owned memory (nothing is freed) and on a
// zero-initialised arena.
void nph_arena_destroy(NphArena *arena);

// The arena as an NphAllocator, for passing to code that takes one. Its
// free only reclaims the most recent allocation; realloc grows the most
// recent allocation in place and copies otherwise.
NPH_INLINE NphAllocator *nph_arena_allocator(NphArena *arena)
{
    return &arena->allocator;
}

// Allocates `size` bytes aligned to `align` (power of two, at least 1).
// NULL when the remaining space is insufficient. Memory is uninitialised.
void *nph_arena_alloc(NphArena *arena, u64 size, u32 align);

// As nph_arena_alloc, zero-filled.
void *nph_arena_alloc_zeroed(NphArena *arena, u64 size, u32 align);

// Typed convenience: NPH_ARENA_ALLOC_TYPE zeroes, NPH_ARENA_ALLOC_ARRAY does not.
#define NPH_ARENA_ALLOC_TYPE(arena, T)                                                             \
    ((T *)nph_arena_alloc_zeroed((arena), sizeof(T), NPH_ALIGNOF(T)))
#define NPH_ARENA_ALLOC_ARRAY(arena, T, count)                                                     \
    ((T *)nph_arena_alloc((arena), sizeof(T) * (u64)(count), NPH_ALIGNOF(T)))

// Captures the current position.
NPH_INLINE NphArenaMarker nph_arena_mark(const NphArena *arena)
{
    const NphArenaMarker m = {arena->offset};
    return m;
}

// Frees everything allocated after `marker` was taken.
void nph_arena_reset_to(NphArena *arena, NphArenaMarker marker);

// Frees everything.
void nph_arena_reset(NphArena *arena);

NPH_INLINE u64 nph_arena_used(const NphArena *arena)
{
    return arena->offset;
}
NPH_INLINE u64 nph_arena_remaining(const NphArena *arena)
{
    return arena->capacity - arena->offset;
}
NPH_INLINE u64 nph_arena_capacity(const NphArena *arena)
{
    return arena->capacity;
}
NPH_INLINE u64 nph_arena_peak(const NphArena *arena)
{
    return arena->peak;
}
// True if `ptr` points into the arena's block.
NPH_INLINE bool nph_arena_owns(const NphArena *arena, const void *ptr)
{
    const uptr p = (uptr)ptr;
    return p >= (uptr)arena->base && p < (uptr)arena->base + arena->capacity;
}

// ---------------------------------------------------------------------------
// Frame arena: N arenas used round-robin, one per frame in flight. Memory
// allocated during frame F stays valid until nph_frame_arena_begin_frame is
// called for frame F + frame_count, so a frame's data can be read by the
// render thread (or by the GPU upload path) while the next frame is being
// built. CLAUDE.md §10.5: zero heap allocations per frame outside frame arenas.
//
// Thread-safety: single-threaded, like NphArena. The job system (Phase 4)
// gives each worker its own frame arena.
// ---------------------------------------------------------------------------

#define NPH_FRAME_ARENA_MAX_FRAMES 4U

typedef struct NphFrameArena {
    NphArena arenas[NPH_FRAME_ARENA_MAX_FRAMES];
    u32 frame_count;
    u32 current;     // index into arenas
    u64 frame_index; // number of begin_frame calls so far
} NphFrameArena;

// Allocates `frame_count` blocks (2..NPH_FRAME_ARENA_MAX_FRAMES) of
// `bytes_per_frame` from `backing`. On failure nothing is leaked and
// NPH_RESULT_OUT_OF_MEMORY (or INVALID_ARGUMENT) is returned.
NPH_MUST_USE NphResult nph_frame_arena_init(NphFrameArena *fa, NphAllocator *backing,
                                            u64 bytes_per_frame, u32 frame_count);

void nph_frame_arena_destroy(NphFrameArena *fa);

// Advances to the next frame and resets the arena that becomes current (the
// one last used frame_count frames ago).
void nph_frame_arena_begin_frame(NphFrameArena *fa);

// The arena for the current frame.
NPH_INLINE NphArena *nph_frame_arena_current(NphFrameArena *fa)
{
    return &fa->arenas[fa->current];
}

NPH_INLINE NphAllocator *nph_frame_arena_allocator(NphFrameArena *fa)
{
    return &fa->arenas[fa->current].allocator;
}

#ifdef __cplusplus
}
#endif

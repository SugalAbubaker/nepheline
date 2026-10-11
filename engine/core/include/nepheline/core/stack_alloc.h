// SPDX-License-Identifier: Apache-2.0
#pragma once

// Stack allocator (CLAUDE.md §8.1 "Allocators"): LIFO allocation over one
// block. Unlike the arena, every allocation can be freed, but only in the
// reverse order of allocation, which is asserted. Each allocation carries a
// small header that records where the stack top was before it, so the
// alignment padding is reclaimed too.
//
// Use it for scoped temporaries that are released in nested order (parsers,
// recursive traversals). For bulk release use NphArena.
//
// Thread-safety: single-threaded.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphStackAlloc {
    NphAllocator allocator; // first, so &stack->allocator == (NphAllocator *)stack
    u8 *base;
    u64 capacity;
    u64 offset; // top of the stack
    u64 peak;
    u32 live; // allocations currently outstanding
    NphAllocator *backing;
} NphStackAlloc;

// Initialises over caller-owned memory. Never fails.
void nph_stack_alloc_init(NphStackAlloc *stack, void *memory, u64 size);

// Allocates the block from `backing`.
NPH_MUST_USE NphResult nph_stack_alloc_init_from(NphStackAlloc *stack, NphAllocator *backing,
                                                 u64 size);

void nph_stack_alloc_destroy(NphStackAlloc *stack);

NPH_INLINE NphAllocator *nph_stack_alloc_allocator(NphStackAlloc *stack)
{
    return &stack->allocator;
}

// Pushes `size` bytes aligned to `align`. NULL when exhausted.
void *nph_stack_alloc_push(NphStackAlloc *stack, u64 size, u32 align);

// Pops the most recent allocation. `ptr` must be the live top; asserted.
void nph_stack_alloc_pop(NphStackAlloc *stack, void *ptr);

// Pops everything.
void nph_stack_alloc_reset(NphStackAlloc *stack);

NPH_INLINE u64 nph_stack_alloc_used(const NphStackAlloc *stack)
{
    return stack->offset;
}
NPH_INLINE u64 nph_stack_alloc_peak(const NphStackAlloc *stack)
{
    return stack->peak;
}
NPH_INLINE u32 nph_stack_alloc_live(const NphStackAlloc *stack)
{
    return stack->live;
}

#ifdef __cplusplus
}
#endif

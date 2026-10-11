// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/stack_alloc.h>

#include <string.h>

// Stored immediately before each returned pointer.
typedef struct StackHeader {
    u64 prev_offset; // stack->offset before this allocation
    u64 size;        // payload size, to verify sized frees and LIFO order
} StackHeader;

static StackHeader *header_of(void *ptr)
{
    return (StackHeader *)((u8 *)ptr - sizeof(StackHeader));
}

static void *stack_vt_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(site);
    return nph_stack_alloc_push((NphStackAlloc *)self, size, align);
}

static void *stack_vt_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                              NphCallsite site)
{
    NphStackAlloc *stack = (NphStackAlloc *)self;
    const u64 begin = nph_ptr_diff(stack->base, ptr);
    StackHeader *h = header_of(ptr);
    NPH_ASSERT(h->size == old_size);
    // Only the top allocation can change size in place.
    if (begin + old_size == stack->offset && nph_is_ptr_aligned(ptr, align)) {
        if (begin + new_size > stack->capacity) {
            return NULL;
        }
        if (new_size > old_size) {
            NPH_ASAN_UNPOISON(stack->base + begin + old_size, new_size - old_size);
        } else {
            NPH_ASAN_POISON(stack->base + begin + new_size, old_size - new_size);
        }
        h->size = new_size;
        stack->offset = begin + new_size;
        stack->peak = nph_max_u64(stack->peak, stack->offset);
        return ptr;
    }
    // Not the top: a copy must be pushed, and the old block cannot be popped
    // until everything above it is. Callers that need this use a general
    // allocator; refuse rather than hide the leak.
    NPH_ASSERT_MSG(false, "stack allocator realloc of a non-top block at %s:%u", site.file,
                   site.line);
    NPH_UNUSED(site);
    return NULL;
}

static void stack_vt_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    NPH_ASSERT(header_of(ptr)->size == size);
    NPH_UNUSED(size);
    nph_stack_alloc_pop((NphStackAlloc *)self, ptr);
}

static const NphAllocatorVtable g_stack_vtable = {stack_vt_alloc, stack_vt_realloc, stack_vt_free};

void nph_stack_alloc_init(NphStackAlloc *stack, void *memory, u64 size)
{
    NPH_ASSERT(stack != NULL);
    memset(stack, 0, sizeof *stack);
    stack->allocator.vtable = &g_stack_vtable;
    stack->allocator.name = "stack";
    stack->base = (u8 *)memory;
    stack->capacity = memory != NULL ? size : 0U;
    NPH_ASAN_POISON(stack->base, stack->capacity);
}

NphResult nph_stack_alloc_init_from(NphStackAlloc *stack, NphAllocator *backing, u64 size)
{
    NPH_ASSERT(stack != NULL);
    NPH_ASSERT(backing != NULL);
    if (size == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    void *memory = NPH_ALLOC(backing, size, NPH_ALLOC_MAX_ALIGN);
    if (memory == NULL) {
        memset(stack, 0, sizeof *stack);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    nph_stack_alloc_init(stack, memory, size);
    stack->backing = backing;
    return NPH_RESULT_OK;
}

void nph_stack_alloc_destroy(NphStackAlloc *stack)
{
    if (stack == NULL || stack->base == NULL) {
        return;
    }
    NPH_ASAN_UNPOISON(stack->base, stack->capacity);
    if (stack->backing != NULL) {
        NPH_FREE(stack->backing, stack->base, stack->capacity, NPH_ALLOC_MAX_ALIGN);
    }
    memset(stack, 0, sizeof *stack);
}

void *nph_stack_alloc_push(NphStackAlloc *stack, u64 size, u32 align)
{
    NPH_ASSERT(stack != NULL);
    NPH_ASSERT(size > 0U);
    NPH_ASSERT(nph_is_pow2_u32(align));
    const u64 header_align = (u64)nph_max_u32(align, NPH_ALIGNOF(StackHeader));
    const uptr cursor = (uptr)stack->base + stack->offset;
    // Payload at an aligned address with room for the header before it.
    const uptr payload = nph_align_up_u64(cursor + sizeof(StackHeader), header_align);
    const u64 overhead = payload - cursor; // header plus alignment padding
    const u64 remaining = stack->capacity - stack->offset;
    // Two comparisons so that overhead + size cannot wrap around.
    if (overhead > remaining || size > remaining - overhead) {
        return NULL;
    }
    const u64 needed = overhead + size;
    NPH_ASAN_UNPOISON((void *)(payload - sizeof(StackHeader)), sizeof(StackHeader) + size);
    StackHeader *h = header_of((void *)payload);
    h->prev_offset = stack->offset;
    h->size = size;
    stack->offset += needed;
    stack->peak = nph_max_u64(stack->peak, stack->offset);
    stack->live += 1U;
    return (void *)payload;
}

void nph_stack_alloc_pop(NphStackAlloc *stack, void *ptr)
{
    NPH_ASSERT(stack != NULL);
    if (ptr == NULL) {
        return;
    }
    NPH_ASSERT((uptr)ptr >= (uptr)stack->base + sizeof(StackHeader) &&
               (uptr)ptr < (uptr)stack->base + stack->capacity);
    const StackHeader *h = header_of(ptr);
    const u64 end = nph_ptr_diff(stack->base, ptr) + h->size;
    NPH_ASSERT_MSG(end == stack->offset, "stack allocator pop out of LIFO order");
    NPH_ASSERT(stack->live > 0U);
    if (end != stack->offset || stack->live == 0U) {
        return;
    }
    const u64 prev = h->prev_offset;
    NPH_ASAN_POISON(stack->base + prev, stack->offset - prev);
    stack->offset = prev;
    stack->live -= 1U;
}

void nph_stack_alloc_reset(NphStackAlloc *stack)
{
    NPH_ASSERT(stack != NULL);
    NPH_ASAN_POISON(stack->base, stack->offset);
    stack->offset = 0U;
    stack->live = 0U;
}

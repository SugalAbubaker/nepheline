// SPDX-License-Identifier: Apache-2.0
#pragma once

// The allocator interface every Nepheline allocation goes through
// (CLAUDE.md §1.4, §2 "Explicit memory", §5; docs/adr/0004-allocator-interface.md).
//
// An NphAllocator is a vtable pointer plus a name. Concrete allocators
// (arena, pool, stack, TLSF, the debug wrapper, the system allocator) embed
// it as their first member and hand out `&self->allocator`. Code that needs
// memory takes an `NphAllocator *` parameter; nothing allocates from a
// global.
//
// Contract, for every allocator:
//   - `size` is in bytes and must be > 0; `align` is a power of two. Every
//     allocator honours alignments up to at least NPH_ALLOC_MAX_ALIGN.
//     Requests with align < NPH_ALLOC_DEFAULT_ALIGN are rounded up to it.
//   - alloc returns NULL only when the allocator cannot satisfy the request
//     (exhausted or out of memory). That is the documented failure signal;
//     callers check it and typically propagate NPH_RESULT_OUT_OF_MEMORY.
//     Memory is uninitialised unless a *_zeroed variant was used.
//   - free and realloc receive the same size and align that were passed to
//     the matching alloc ("sized free"). This lets arenas, pools, and stacks
//     work without per-allocation headers and lets the debug wrapper verify
//     the caller. free(NULL) is a no-op.
//   - realloc(NULL, 0, n) behaves as alloc(n); new_size must be > 0. The
//     contents up to min(old_size, new_size) are preserved; on failure the
//     original block is untouched and NULL is returned.
//   - The callsite (file and line of the NPH_ALLOC macro) is passed through
//     so the debug wrapper can attribute leaks. Allocators that do not
//     track it ignore it.
//   - Thread-safety is a property of each concrete allocator and documented
//     on its type. The system allocator is NPH_THREADSAFE.

#include <nepheline/core/base.h>

#ifdef __cplusplus
extern "C" {
#endif

// Alignment given to requests that ask for less. 16 bytes covers every
// scalar, pointer, and SSE register type.
#define NPH_ALLOC_DEFAULT_ALIGN 16U

// The largest alignment every allocator supports. A page.
#define NPH_ALLOC_MAX_ALIGN 4096U

typedef struct NphCallsite {
    const char *file;
    u32 line;
} NphCallsite;

// The current source location, for passing to the *_at functions.
#define NPH_CALLSITE_HERE() ((NphCallsite){__FILE__, (u32)__LINE__})

typedef struct NphAllocator NphAllocator;

typedef struct NphAllocatorVtable {
    void *(*alloc)(NphAllocator *self, u64 size, u32 align, NphCallsite site);
    void *(*realloc)(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                     NphCallsite site);
    void (*free)(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site);
} NphAllocatorVtable;

struct NphAllocator {
    const NphAllocatorVtable *vtable;
    const char *name; // static string for diagnostics; never NULL
};

// ---------------------------------------------------------------------------
// Allocation entry points. Use the macros in ordinary code; they capture the
// callsite. The *_at functions exist for code that forwards a callsite it
// received (containers, wrappers).
// ---------------------------------------------------------------------------

// Allocates `size` bytes aligned to `align`. NULL when the allocator cannot
// satisfy the request. Asserts on size == 0 or a non-power-of-two align.
void *nph_alloc_at(NphAllocator *a, u64 size, u32 align, NphCallsite site);

// As nph_alloc_at, with the memory zero-filled.
void *nph_alloc_zeroed_at(NphAllocator *a, u64 size, u32 align, NphCallsite site);

// Allocates `count` elements of `elem_size` bytes, checking that the product
// does not overflow. count == 0 is an error (NULL is returned and, with
// asserts enabled, an assertion fires).
void *nph_alloc_array_at(NphAllocator *a, u64 elem_size, u32 align, u64 count, bool zeroed,
                         NphCallsite site);

// Resizes `ptr` (allocated with old_size/align) to new_size bytes. See the
// contract above. Asserts on new_size == 0.
void *nph_realloc_at(NphAllocator *a, void *ptr, u64 old_size, u64 new_size, u32 align,
                     NphCallsite site);

// Releases `ptr` (allocated with size/align). NULL is ignored.
void nph_free_at(NphAllocator *a, void *ptr, u64 size, u32 align, NphCallsite site);

#define NPH_ALLOC(a, size, align) nph_alloc_at((a), (size), (align), NPH_CALLSITE_HERE())
#define NPH_ALLOC_ZEROED(a, size, align)                                                           \
    nph_alloc_zeroed_at((a), (size), (align), NPH_CALLSITE_HERE())
// Pointer arguments are cast to void * so pointer-to-pointer values (an
// array of pointers) convert in one explicit step.
#define NPH_REALLOC(a, ptr, old_size, new_size, align)                                             \
    nph_realloc_at((a), (void *)(ptr), (old_size), (new_size), (align), NPH_CALLSITE_HERE())
#define NPH_FREE(a, ptr, size, align)                                                              \
    nph_free_at((a), (void *)(ptr), (size), (align), NPH_CALLSITE_HERE())

// Typed helpers. NPH_ALLOC_TYPE returns zeroed memory: a struct that starts
// with every field zero is the engine's default "empty" state.
#define NPH_ALLOC_TYPE(a, T)                                                                       \
    ((T *)nph_alloc_zeroed_at((a), sizeof(T), NPH_ALIGNOF(T), NPH_CALLSITE_HERE()))
#define NPH_ALLOC_TYPE_UNINIT(a, T)                                                                \
    ((T *)nph_alloc_at((a), sizeof(T), NPH_ALIGNOF(T), NPH_CALLSITE_HERE()))
#define NPH_ALLOC_ARRAY(a, T, count)                                                               \
    ((T *)nph_alloc_array_at((a), sizeof(T), NPH_ALIGNOF(T), (count), false, NPH_CALLSITE_HERE()))
#define NPH_ALLOC_ARRAY_ZEROED(a, T, count)                                                        \
    ((T *)nph_alloc_array_at((a), sizeof(T), NPH_ALIGNOF(T), (count), true, NPH_CALLSITE_HERE()))
#define NPH_FREE_TYPE(a, T, ptr)                                                                   \
    nph_free_at((a), (void *)(ptr), sizeof(T), NPH_ALIGNOF(T), NPH_CALLSITE_HERE())
#define NPH_FREE_ARRAY(a, T, ptr, count)                                                           \
    nph_free_at((a), (void *)(ptr), sizeof(T) * (u64)(count), NPH_ALIGNOF(T), NPH_CALLSITE_HERE())

// ---------------------------------------------------------------------------
// Helpers for allocator implementations
// ---------------------------------------------------------------------------

// A realloc implementation for allocators that cannot grow a block in place:
// alloc new, copy min(old, new), free old. Allocators store this in their
// vtable or call it from their own realloc after the in-place path fails.
void *nph_allocator_realloc_fallback(NphAllocator *a, void *ptr, u64 old_size, u64 new_size,
                                     u32 align, NphCallsite site);

// Normalises a requested alignment to the allocator contract: at least
// NPH_ALLOC_DEFAULT_ALIGN. Asserts that it is a power of two.
NPH_INLINE u32 nph_alloc_normalize_align(u32 align)
{
    return align < NPH_ALLOC_DEFAULT_ALIGN ? NPH_ALLOC_DEFAULT_ALIGN : align;
}

// The process allocator (malloc-backed). The only place in the engine that
// calls malloc. Used to obtain the backing blocks of the engine's own
// allocators and by tools; engine subsystems receive an allocator explicitly
// and never reach for this one themselves.
// Thread-safety: NPH_THREADSAFE. Never fails to return the allocator.
NphAllocator *nph_allocator_system(void);

// ---------------------------------------------------------------------------
// AddressSanitizer integration. Allocators poison the memory they own but
// have not handed out, so a use of freed or not-yet-allocated arena memory
// is reported by ASan in Debug builds instead of silently corrupting data.
// No-ops without ASan. Poisoning may cover a sub-range and unpoisoning a
// super-range, both rounded to ASan's 8-byte granularity, which can never
// produce a false report.
//
// Consequence for callers: memory handed to an allocator (nph_*_init over a
// caller-owned block) must be released with the matching nph_*_destroy
// before it is used for anything else. Destroy unpoisons it; without that,
// a stack buffer stays poisoned after its frame returns and the next
// function to occupy that address is reported.
// ---------------------------------------------------------------------------

#if NPH_ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#define NPH_ASAN_POISON(ptr, size)   __asan_poison_memory_region((ptr), (size_t)(size))
#define NPH_ASAN_UNPOISON(ptr, size) __asan_unpoison_memory_region((ptr), (size_t)(size))
#else
#define NPH_ASAN_POISON(ptr, size)   ((void)(ptr), (void)(size))
#define NPH_ASAN_UNPOISON(ptr, size) ((void)(ptr), (void)(size))
#endif

#ifdef __cplusplus
}
#endif

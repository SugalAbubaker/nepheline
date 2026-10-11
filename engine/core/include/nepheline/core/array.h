// SPDX-License-Identifier: Apache-2.0
#pragma once

// Type-generic dynamic array (CLAUDE.md §8.1 "Containers").
//
//     typedef NPH_ARRAY(f32) FloatArray;
//
//     FloatArray a;
//     NPH_ARRAY_INIT(&a, allocator);
//     NPH_TRY(NPH_ARRAY_PUSH(&a, 1.0F));
//     f32 *slot = NPH_ARRAY_PUSH_SLOT(&a);     // NULL when out of memory
//     NPH_ARRAY_FOREACH(f32, it, &a) { *it *= 2.0F; }
//     NPH_ARRAY_FREE(&a);
//
// NPH_ARRAY(T) expands to an anonymous struct {T *data; u32 len; u32 cap;
// NphAllocator *alloc;}. Typedef it to pass arrays between functions. The
// macros below are type-safe where C allows (assignments go through
// `data[i]`, so a wrong value type is a compile error) and forward to
// untyped functions that do the memory work through NphArrayRef.
//
// Growth doubles the capacity (minimum 8) with NPH_REALLOC, so a TLSF heap
// grows in place when the block has room. `data` moves on growth: pointers
// and slots are invalidated by every call that can grow. Elements are plain
// data; nothing is constructed or destroyed.
//
// Alignment: elements are allocated at 16 bytes, or at their own size for
// power-of-two sizes of 32 and 64 (SIMD register types), so AVX/AVX-512
// types stay aligned. Larger alignments are not supported in arrays.
//
// Capacity and length are u32 (at most 4 294 967 295 elements). Growth past
// that reports NPH_RESULT_OUT_OF_MEMORY like any other capacity failure.
//
// Thread-safety: none; an array belongs to one thread at a time.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_ARRAY(T)                                                                               \
    struct {                                                                                       \
        T *data;                                                                                   \
        u32 len;                                                                                   \
        u32 cap;                                                                                   \
        NphAllocator *alloc;                                                                       \
    }

typedef NPH_ARRAY(u8) NphArrayU8;
typedef NPH_ARRAY(u32) NphArrayU32;
typedef NPH_ARRAY(u64) NphArrayU64;
typedef NPH_ARRAY(i32) NphArrayI32;
typedef NPH_ARRAY(f32) NphArrayF32;
typedef NPH_ARRAY(void *) NphArrayPtr;

// A view of any NPH_ARRAY's fields for the untyped functions. `data_field`
// points at the `T *data` member; it is read and written with memcpy so no
// pointer type is punned.
typedef struct NphArrayRef {
    void *data_field;
    u32 *len;
    u32 *cap;
    NphAllocator *alloc;
    u32 elem_size;
} NphArrayRef;

#define NPH_ARRAY_REF_(a)                                                                          \
    ((NphArrayRef){(void *)&(a)->data, &(a)->len, &(a)->cap, (a)->alloc, (u32)sizeof *(a)->data})

// Compile-time check that `src` points at elements of the array's size.
#define NPH_ARRAY_SAME_ELEM_(a, src)                                                               \
    ((void)sizeof(char[sizeof *(src) == sizeof *(a)->data ? 1 : -1]))

// ---------------------------------------------------------------------------
// Macros (the public API)
// ---------------------------------------------------------------------------

// Starts empty with no allocation. `allocator` is used for every growth.
#define NPH_ARRAY_INIT(a, allocator)                                                               \
    do {                                                                                           \
        (a)->data = NULL;                                                                          \
        (a)->len = 0U;                                                                             \
        (a)->cap = 0U;                                                                             \
        (a)->alloc = (allocator);                                                                  \
    } while (0)

// Releases the storage; the array is empty and reusable afterwards.
#define NPH_ARRAY_FREE(a) nph_array_free(NPH_ARRAY_REF_(a), NPH_CALLSITE_HERE())

// Ensures capacity for at least `min_cap` elements. NphResult.
#define NPH_ARRAY_RESERVE(a, min_cap)                                                              \
    nph_array_reserve(NPH_ARRAY_REF_(a), (min_cap), NPH_CALLSITE_HERE())

// Sets the length; new elements are zero-filled. NphResult.
#define NPH_ARRAY_RESIZE(a, new_len)                                                               \
    nph_array_resize(NPH_ARRAY_REF_(a), (new_len), NPH_CALLSITE_HERE())

// True when one more element fits, growing only when it does not: the
// common case stays inline with no call.
#define NPH_ARRAY_HAS_ROOM_(a)                                                                     \
    ((a)->len < (a)->cap || nph_array_grow(NPH_ARRAY_REF_(a), 1U, NPH_CALLSITE_HERE()))

// Appends `value`. NphResult (OUT_OF_MEMORY on failure; the array is unchanged).
// `value` must not read the array's length (the slot index is claimed in
// the same expression, in unspecified order).
#define NPH_ARRAY_PUSH(a, value)                                                                   \
    (NPH_ARRAY_HAS_ROOM_(a)                                                                        \
         ? ((a)->data[nph_array_claim_slot(&(a)->len)] = (value), NPH_RESULT_OK)                   \
         : NPH_RESULT_OUT_OF_MEMORY)

// Appends an uninitialised element and returns its address, or NULL.
#define NPH_ARRAY_PUSH_SLOT(a)                                                                     \
    (NPH_ARRAY_HAS_ROOM_(a) ? &(a)->data[nph_array_claim_slot(&(a)->len)] : NULL)

// Appends `n` elements copied from `src` (same element type). NphResult.
#define NPH_ARRAY_PUSH_N(a, src, n)                                                                \
    (NPH_ARRAY_SAME_ELEM_(a, src),                                                                 \
     nph_array_push_n(NPH_ARRAY_REF_(a), (const void *)(src), (n), NPH_CALLSITE_HERE()))

// Inserts `value` at `index` (0..len), shifting later elements. `index` is
// evaluated twice. Asserts the index; NphResult for memory.
#define NPH_ARRAY_INSERT(a, index, value)                                                          \
    (nph_array_make_gap(NPH_ARRAY_REF_(a), (index), 1U, NPH_CALLSITE_HERE())                       \
         ? ((a)->data[(index)] = (value), NPH_RESULT_OK)                                           \
         : NPH_RESULT_OUT_OF_MEMORY)

// Removes and returns the last element. Asserts non-empty.
#define NPH_ARRAY_POP(a) ((a)->data[nph_array_pop_index(&(a)->len)])

// Address of the last element. Asserts non-empty.
#define NPH_ARRAY_LAST(a) (&(a)->data[nph_array_last_index((a)->len)])

// Removes `n` elements starting at `index`, preserving order. Asserts bounds.
#define NPH_ARRAY_REMOVE_RANGE(a, index, n) nph_array_remove(NPH_ARRAY_REF_(a), (index), (n))
#define NPH_ARRAY_REMOVE_AT(a, index)       nph_array_remove(NPH_ARRAY_REF_(a), (index), 1U)

// Removes the element at `index` by moving the last element into its place.
#define NPH_ARRAY_REMOVE_SWAP(a, index) nph_array_remove_swap(NPH_ARRAY_REF_(a), (index))

// Keeps the storage, drops the elements.
#define NPH_ARRAY_CLEAR(a) ((void)((a)->len = 0U))

// Reallocates so that cap == len (frees everything when empty). NphResult.
#define NPH_ARRAY_SHRINK_TO_FIT(a) nph_array_shrink(NPH_ARRAY_REF_(a), NPH_CALLSITE_HERE())

#define NPH_ARRAY_IS_EMPTY(a) ((a)->len == 0U)
#define NPH_ARRAY_BYTES(a)    ((u64)(a)->len * sizeof *(a)->data)

// for (T *it = first; it != end; ++it). Do not grow the array inside.
// (T is a type and `it` a declarator: neither can be parenthesised.)
// NOLINTBEGIN(bugprone-macro-parentheses)
#define NPH_ARRAY_FOREACH(T, it, a) for (T *it = (a)->data; it != (a)->data + (a)->len; ++it)
// NOLINTEND(bugprone-macro-parentheses)

// Increments *len and returns the previous value: the index of a slot the
// caller has made room for.
NPH_INLINE u32 nph_array_claim_slot(u32 *len)
{
    const u32 index = *len;
    *len = index + 1U;
    return index;
}

// ---------------------------------------------------------------------------
// Untyped implementation (called by the macros; usable directly by code
// that works on NphArrayRef, such as serialization)
// ---------------------------------------------------------------------------

// Ensures room for `extra` more elements. false when the allocator fails or
// the length would overflow u32; the array is unchanged then.
bool nph_array_grow(NphArrayRef r, u32 extra, NphCallsite site);

NphResult nph_array_reserve(NphArrayRef r, u32 min_cap, NphCallsite site);
NphResult nph_array_resize(NphArrayRef r, u32 new_len, NphCallsite site);
NphResult nph_array_push_n(NphArrayRef r, const void *src, u32 n, NphCallsite site);
NphResult nph_array_shrink(NphArrayRef r, NphCallsite site);
void nph_array_free(NphArrayRef r, NphCallsite site);

// Opens `n` uninitialised slots at `index` (asserted <= len) and adds them
// to the length. false on allocation failure.
bool nph_array_make_gap(NphArrayRef r, u32 index, u32 n, NphCallsite site);

void nph_array_remove(NphArrayRef r, u32 index, u32 n);
void nph_array_remove_swap(NphArrayRef r, u32 index);

// Asserts *len > 0, decrements it, and returns the removed index.
u32 nph_array_pop_index(u32 *len);

// Asserts len > 0 and returns len - 1.
u32 nph_array_last_index(u32 len);

// The alignment used for an element size (see the header comment).
u32 nph_array_elem_align(u32 elem_size);

#ifdef __cplusplus
}
#endif

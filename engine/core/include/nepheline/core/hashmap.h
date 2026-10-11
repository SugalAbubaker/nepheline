// SPDX-License-Identifier: Apache-2.0
#pragma once

// Open-addressing hash map with SIMD group probing (CLAUDE.md §8.1
// "Containers": Swiss-table style).
//
// Design (after Abseil's flat_hash_map, Kulukundis, "Designing a Fast,
// Efficient, Cache-friendly Hash Table", CppCon 2017): one control byte per
// slot holds EMPTY, DELETED, or the top 7 bits of the key's hash (h2). A
// lookup loads 16 control bytes at once, compares them with h2 in one SSE2
// instruction, and only touches the slots whose byte matched, so a probe
// costs one cache line of control bytes plus, almost always, one slot. The
// low bits of the hash (h1) choose the start group; probing is triangular
// over groups, which visits every group of a power-of-two table. Load
// factor is 7/8; removal leaves a tombstone, which the next rehash drops.
//
// Keys and values are untyped byte blobs of fixed size, stored together in
// one slot so a hit touches one line. The hash and equality functions are
// supplied at init (defaults: nph_hash64 over the key bytes, memcmp); for
// integer keys the *_u64 helpers use the integer mixer instead.
//
// Value pointers returned by find are valid until the next insert, set,
// remove, reserve, or clear. Keys and values are plain data; nothing is
// constructed or destroyed. Key and value alignment up to 16 is honoured
// (the slot layout aligns to the larger of the two sizes, capped at 16).
//
// Thread-safety: none; readers and writers must be externally serialised.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_HASHMAP_GROUP_SIZE 16U

typedef u64 (*NphHashMapHashFn)(const void *key, u32 key_size, u64 seed);
typedef bool (*NphHashMapEqFn)(const void *a, const void *b, u32 key_size);

typedef struct NphHashMapDesc {
    u32 key_size;          // > 0
    u32 val_size;          // 0 makes a set
    NphHashMapHashFn hash; // NULL: nph_hash64 over the key bytes
    NphHashMapEqFn eq;     // NULL: memcmp
    u64 seed;              // 0: NPH_HASH_DEFAULT_SEED
} NphHashMapDesc;

typedef struct NphHashMap {
    u8 *ctrl;     // capacity + NPH_HASHMAP_GROUP_SIZE control bytes (the tail mirrors the head)
    u8 *slots;    // capacity * slot_size bytes
    u32 capacity; // 0, or a power of two >= 16
    u32 len;
    u32 growth_left; // inserts allowed before a rehash
    u32 key_size;
    u32 val_size;
    u32 val_offset;
    u32 slot_size;
    u64 seed;
    NphHashMapHashFn hash;
    NphHashMapEqFn eq;
    NphAllocator *alloc;
} NphHashMap;

// Initialises an empty map; no memory is allocated until the first insert.
// NPH_RESULT_INVALID_ARGUMENT for key_size == 0.
NPH_MUST_USE NphResult nph_hashmap_init(NphHashMap *m, NphAllocator *alloc,
                                        const NphHashMapDesc *desc);

// Releases the storage. The map may be reused after another init.
void nph_hashmap_free(NphHashMap *m);

// Ensures `count` entries fit without a rehash.
NPH_MUST_USE NphResult nph_hashmap_reserve(NphHashMap *m, u32 count);

// Removes every entry, keeping the storage.
void nph_hashmap_clear(NphHashMap *m);

// Adds `key` -> `val` (val may be NULL when val_size == 0, or to leave the
// value uninitialised). NPH_RESULT_ALREADY_EXISTS if the key is present.
NPH_MUST_USE NphResult nph_hashmap_insert(NphHashMap *m, const void *key, const void *val);

// Adds or overwrites `key` -> `val`.
NPH_MUST_USE NphResult nph_hashmap_set(NphHashMap *m, const void *key, const void *val);

// Address of the value for `key`, or NULL. For a set (val_size == 0) the
// address of the stored key is returned, so the result is still non-NULL.
void *nph_hashmap_find(const NphHashMap *m, const void *key);

bool nph_hashmap_contains(const NphHashMap *m, const void *key);

// Removes `key`; true if it was present.
bool nph_hashmap_remove(NphHashMap *m, const void *key);

NPH_INLINE u32 nph_hashmap_len(const NphHashMap *m)
{
    return m->len;
}

// Iteration in unspecified order; the map must not be modified meanwhile.
//
//     NphHashMapIter it = nph_hashmap_iter(&m);
//     const void *key; void *val;
//     while (nph_hashmap_next(&it, &key, &val)) { ... }
typedef struct NphHashMapIter {
    const NphHashMap *map;
    u32 index;
} NphHashMapIter;

NphHashMapIter nph_hashmap_iter(const NphHashMap *m);
bool nph_hashmap_next(NphHashMapIter *it, const void **out_key, void **out_val);

// Typed convenience: `(T *)` cast around find.
#define NPH_HASHMAP_FIND(m, T, key_ptr) ((T *)nph_hashmap_find((m), (key_ptr)))

// ---------------------------------------------------------------------------
// u64 keys (entity IDs, handles, string IDs): the integer mixer as the hash.
// ---------------------------------------------------------------------------

// A descriptor for u64 keys with `val_size`-byte values.
NphHashMapDesc nph_hashmap_desc_u64(u32 val_size);

NPH_MUST_USE NphResult nph_hashmap_insert_u64(NphHashMap *m, u64 key, const void *val);
NPH_MUST_USE NphResult nph_hashmap_set_u64(NphHashMap *m, u64 key, const void *val);
void *nph_hashmap_find_u64(const NphHashMap *m, u64 key);
bool nph_hashmap_contains_u64(const NphHashMap *m, u64 key);
bool nph_hashmap_remove_u64(NphHashMap *m, u64 key);

// The u64 hash function used by nph_hashmap_desc_u64, for custom descriptors.
u64 nph_hashmap_hash_u64(const void *key, u32 key_size, u64 seed);

#ifdef __cplusplus
}
#endif

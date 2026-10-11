// SPDX-License-Identifier: Apache-2.0
// Swiss-table hash map. See hashmap.h for the design and docs/core.md.
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/hashmap.h>

#include <string.h>

#if NPH_ARCH_X64
#include <emmintrin.h>
#endif

// Control bytes. FULL bytes are 0x00..0x7F (the hash's top 7 bits); the
// two special values have the high bit set so "empty or deleted" is one
// sign-bit test.
#define CTRL_EMPTY   0x80U
#define CTRL_DELETED 0xFEU
#define GROUP        NPH_HASHMAP_GROUP_SIZE
#define MIN_CAPACITY 16U

NPH_STATIC_ASSERT(GROUP == 16U, "group operations assume 16-byte groups");

// ---------------------------------------------------------------------------
// Group operations: each returns a 16-bit mask with bit i set for byte i.
// ---------------------------------------------------------------------------

#if NPH_ARCH_X64

typedef struct Group {
    __m128i v;
} Group;

static Group group_load(const u8 *ctrl)
{
    Group g;
    g.v = _mm_loadu_si128((const __m128i *)ctrl);
    return g;
}

static u32 group_match(Group g, u8 h2)
{
    return (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(g.v, _mm_set1_epi8((char)h2)));
}

static u32 group_match_empty(Group g)
{
    return (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(g.v, _mm_set1_epi8((char)CTRL_EMPTY)));
}

static u32 group_match_empty_or_deleted(Group g)
{
    return (u32)_mm_movemask_epi8(g.v);
}

#else

typedef struct Group {
    u8 bytes[GROUP];
} Group;

static Group group_load(const u8 *ctrl)
{
    Group g;
    memcpy(g.bytes, ctrl, GROUP);
    return g;
}

static u32 group_match(Group g, u8 h2)
{
    u32 mask = 0U;
    for (u32 i = 0; i < GROUP; ++i) {
        mask |= (g.bytes[i] == h2 ? 1U : 0U) << i;
    }
    return mask;
}

static u32 group_match_empty(Group g)
{
    return group_match(g, (u8)CTRL_EMPTY);
}

static u32 group_match_empty_or_deleted(Group g)
{
    u32 mask = 0U;
    for (u32 i = 0; i < GROUP; ++i) {
        mask |= ((g.bytes[i] & 0x80U) != 0U ? 1U : 0U) << i;
    }
    return mask;
}

#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static u64 default_hash(const void *key, u32 key_size, u64 seed)
{
    return nph_hash64(key, key_size, seed);
}

static bool default_eq(const void *a, const void *b, u32 key_size)
{
    return memcmp(a, b, key_size) == 0;
}

u64 nph_hashmap_hash_u64(const void *key, u32 key_size, u64 seed)
{
    NPH_ASSERT(key_size == sizeof(u64));
    NPH_UNUSED(key_size);
    u64 k;
    memcpy(&k, key, sizeof k);
    return nph_hash_mix_u64(k ^ seed);
}

static u32 ctrl_bytes(u32 capacity)
{
    return capacity + GROUP;
}

static u64 storage_bytes(const NphHashMap *m, u32 capacity)
{
    return nph_align_up_u64(ctrl_bytes(capacity), 16U) + (u64)capacity * m->slot_size;
}

static u8 *slot_at(const NphHashMap *m, u32 index)
{
    return m->slots + (u64)index * m->slot_size;
}

static void *value_at(const NphHashMap *m, u32 index)
{
    return slot_at(m, index) + m->val_offset;
}

static u64 h1(u64 hash)
{
    return hash >> 7U;
}

static u8 h2(u64 hash)
{
    return (u8)(hash & 0x7FU);
}

// Writes a control byte and its mirror in the trailing group, so unaligned
// group loads near the end of the table wrap without branching.
static void set_ctrl(NphHashMap *m, u32 index, u8 value)
{
    const u32 mask = m->capacity - 1U;
    m->ctrl[index] = value;
    m->ctrl[((index - GROUP) & mask) + GROUP] = value;
}

static u32 max_len(u32 capacity)
{
    return capacity - capacity / 8U; // load factor 7/8
}

// Index of the slot holding `key`, or NPH_U32_MAX.
static u32 find_index(const NphHashMap *m, const void *key, u64 hash)
{
    if (m->capacity == 0U) {
        return NPH_U32_MAX;
    }
    const u32 mask = m->capacity - 1U;
    const u8 tag = h2(hash);
    u32 pos = (u32)(h1(hash) & mask);
    u32 stride = 0U;
    for (;;) {
        const Group g = group_load(m->ctrl + pos);
        u32 matches = group_match(g, tag);
        while (matches != 0U) {
            const u32 index = (pos + nph_ctz_u32(matches)) & mask;
            if (m->eq(key, slot_at(m, index), m->key_size)) {
                return index;
            }
            matches &= matches - 1U;
        }
        if (group_match_empty(g) != 0U) {
            return NPH_U32_MAX;
        }
        stride += GROUP;
        pos = (pos + stride) & mask;
    }
}

// First empty or deleted slot on the probe sequence of `hash`. The table
// always has at least one such slot (load factor), so this terminates.
static u32 find_insert_index(const NphHashMap *m, u64 hash)
{
    const u32 mask = m->capacity - 1U;
    u32 pos = (u32)(h1(hash) & mask);
    u32 stride = 0U;
    for (;;) {
        const Group g = group_load(m->ctrl + pos);
        const u32 free = group_match_empty_or_deleted(g);
        if (free != 0U) {
            return (pos + nph_ctz_u32(free)) & mask;
        }
        stride += GROUP;
        pos = (pos + stride) & mask;
    }
}

static NphResult allocate_storage(NphHashMap *m, u32 capacity, u8 **out_ctrl, u8 **out_slots)
{
    u8 *block = (u8 *)NPH_ALLOC(m->alloc, storage_bytes(m, capacity), 16U);
    if (block == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(block, (int)CTRL_EMPTY, ctrl_bytes(capacity));
    *out_ctrl = block;
    *out_slots = block + nph_align_up_u64(ctrl_bytes(capacity), 16U);
    return NPH_RESULT_OK;
}

static void release_storage(NphHashMap *m, u8 *ctrl, u32 capacity)
{
    if (ctrl != NULL) {
        NPH_FREE(m->alloc, ctrl, storage_bytes(m, capacity), 16U);
    }
}

// Rebuilds the table at `new_capacity`, dropping tombstones.
static NphResult rehash(NphHashMap *m, u32 new_capacity)
{
    NPH_ASSERT(nph_is_pow2_u32(new_capacity) && new_capacity >= MIN_CAPACITY);
    NPH_ASSERT(m->len <= max_len(new_capacity));
    u8 *old_ctrl = m->ctrl;
    u8 *old_slots = m->slots;
    const u32 old_capacity = m->capacity;

    u8 *ctrl;
    u8 *slots;
    NPH_TRY(allocate_storage(m, new_capacity, &ctrl, &slots));

    m->ctrl = ctrl;
    m->slots = slots;
    m->capacity = new_capacity;
    for (u32 i = 0; i < old_capacity; ++i) {
        if ((old_ctrl[i] & 0x80U) != 0U) {
            continue; // empty or deleted
        }
        const u8 *slot = old_slots + (u64)i * m->slot_size;
        const u64 hash = m->hash(slot, m->key_size, m->seed);
        const u32 index = find_insert_index(m, hash);
        set_ctrl(m, index, h2(hash));
        memcpy(slot_at(m, index), slot, m->slot_size);
    }
    m->growth_left = max_len(new_capacity) - m->len;
    release_storage(m, old_ctrl, old_capacity);
    return NPH_RESULT_OK;
}

// Makes room for one more entry. Doubles when the table is at least half
// full of live entries; otherwise rehashes in place to reclaim tombstones.
static NphResult grow_if_needed(NphHashMap *m)
{
    if (m->growth_left > 0U) {
        return NPH_RESULT_OK;
    }
    if (m->capacity == 0U) {
        return rehash(m, MIN_CAPACITY);
    }
    if (m->len + 1U > m->capacity / 2U) {
        if (m->capacity > NPH_U32_MAX / 2U) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        return rehash(m, m->capacity * 2U);
    }
    return rehash(m, m->capacity);
}

// Inserts a key known to be absent at the slot the probe sequence yields.
static u32 insert_new(NphHashMap *m, const void *key, const void *val, u64 hash)
{
    const u32 index = find_insert_index(m, hash);
    // Reusing a tombstone does not consume growth; an empty slot does.
    if (m->ctrl[index] == CTRL_EMPTY) {
        m->growth_left -= 1U;
    }
    set_ctrl(m, index, h2(hash));
    memcpy(slot_at(m, index), key, m->key_size);
    if (val != NULL && m->val_size > 0U) {
        memcpy(value_at(m, index), val, m->val_size);
    }
    m->len += 1U;
    return index;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

NphResult nph_hashmap_init(NphHashMap *m, NphAllocator *alloc, const NphHashMapDesc *desc)
{
    NPH_ASSERT(m != NULL && alloc != NULL && desc != NULL);
    memset(m, 0, sizeof *m);
    if (desc->key_size == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    m->alloc = alloc;
    m->key_size = desc->key_size;
    m->val_size = desc->val_size;
    // Slot layout: key, then value, each aligned to the smaller of 16 and
    // the largest power of two not above the larger size.
    const u32 largest = nph_max_u32(desc->key_size, desc->val_size);
    const u32 align = nph_min_u32(16U, (u32)1U << nph_log2_floor_u32(largest));
    m->val_offset = desc->val_size > 0U ? nph_align_up_u32(desc->key_size, align) : desc->key_size;
    m->slot_size = nph_align_up_u32(m->val_offset + desc->val_size, align);
    m->seed = desc->seed != 0U ? desc->seed : NPH_HASH_DEFAULT_SEED;
    m->hash = desc->hash != NULL ? desc->hash : default_hash;
    m->eq = desc->eq != NULL ? desc->eq : default_eq;
    return NPH_RESULT_OK;
}

void nph_hashmap_free(NphHashMap *m)
{
    if (m == NULL || m->alloc == NULL) {
        return;
    }
    release_storage(m, m->ctrl, m->capacity);
    m->ctrl = NULL;
    m->slots = NULL;
    m->capacity = 0U;
    m->len = 0U;
    m->growth_left = 0U;
}

NphResult nph_hashmap_reserve(NphHashMap *m, u32 count)
{
    NPH_ASSERT(m != NULL && m->alloc != NULL);
    if (count <= m->len + m->growth_left) {
        return NPH_RESULT_OK;
    }
    // Smallest power of two whose 7/8 holds `count`.
    if (count > NPH_U32_MAX / 8U * 7U) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 capacity = MIN_CAPACITY;
    while (max_len(capacity) < count) {
        capacity *= 2U;
    }
    return rehash(m, capacity);
}

void nph_hashmap_clear(NphHashMap *m)
{
    NPH_ASSERT(m != NULL);
    if (m->capacity == 0U) {
        return;
    }
    memset(m->ctrl, (int)CTRL_EMPTY, ctrl_bytes(m->capacity));
    m->len = 0U;
    m->growth_left = max_len(m->capacity);
}

NphResult nph_hashmap_insert(NphHashMap *m, const void *key, const void *val)
{
    NPH_ASSERT(m != NULL && m->alloc != NULL && key != NULL);
    const u64 hash = m->hash(key, m->key_size, m->seed);
    if (find_index(m, key, hash) != NPH_U32_MAX) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    NPH_TRY(grow_if_needed(m));
    insert_new(m, key, val, hash);
    return NPH_RESULT_OK;
}

NphResult nph_hashmap_set(NphHashMap *m, const void *key, const void *val)
{
    NPH_ASSERT(m != NULL && m->alloc != NULL && key != NULL);
    const u64 hash = m->hash(key, m->key_size, m->seed);
    const u32 existing = find_index(m, key, hash);
    if (existing != NPH_U32_MAX) {
        if (val != NULL && m->val_size > 0U) {
            memcpy(value_at(m, existing), val, m->val_size);
        }
        return NPH_RESULT_OK;
    }
    NPH_TRY(grow_if_needed(m));
    insert_new(m, key, val, hash);
    return NPH_RESULT_OK;
}

void *nph_hashmap_find(const NphHashMap *m, const void *key)
{
    NPH_ASSERT(m != NULL && key != NULL);
    const u32 index = find_index(m, key, m->hash(key, m->key_size, m->seed));
    return index == NPH_U32_MAX ? NULL : value_at(m, index);
}

bool nph_hashmap_contains(const NphHashMap *m, const void *key)
{
    return nph_hashmap_find(m, key) != NULL;
}

bool nph_hashmap_remove(NphHashMap *m, const void *key)
{
    NPH_ASSERT(m != NULL && key != NULL);
    const u32 index = find_index(m, key, m->hash(key, m->key_size, m->seed));
    if (index == NPH_U32_MAX) {
        return false;
    }
    set_ctrl(m, index, (u8)CTRL_DELETED);
    m->len -= 1U;
    return true;
}

NphHashMapIter nph_hashmap_iter(const NphHashMap *m)
{
    const NphHashMapIter it = {m, 0U};
    return it;
}

bool nph_hashmap_next(NphHashMapIter *it, const void **out_key, void **out_val)
{
    const NphHashMap *m = it->map;
    while (it->index < m->capacity) {
        const u32 index = it->index++;
        if ((m->ctrl[index] & 0x80U) == 0U) {
            if (out_key != NULL) {
                *out_key = slot_at(m, index);
            }
            if (out_val != NULL) {
                *out_val = value_at(m, index);
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// u64 keys
// ---------------------------------------------------------------------------

NphHashMapDesc nph_hashmap_desc_u64(u32 val_size)
{
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = sizeof(u64);
    desc.val_size = val_size;
    desc.hash = nph_hashmap_hash_u64;
    return desc;
}

NphResult nph_hashmap_insert_u64(NphHashMap *m, u64 key, const void *val)
{
    return nph_hashmap_insert(m, &key, val);
}

NphResult nph_hashmap_set_u64(NphHashMap *m, u64 key, const void *val)
{
    return nph_hashmap_set(m, &key, val);
}

void *nph_hashmap_find_u64(const NphHashMap *m, u64 key)
{
    return nph_hashmap_find(m, &key);
}

bool nph_hashmap_contains_u64(const NphHashMap *m, u64 key)
{
    return nph_hashmap_find(m, &key) != NULL;
}

bool nph_hashmap_remove_u64(NphHashMap *m, u64 key)
{
    return nph_hashmap_remove(m, &key);
}

// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/string_id.h>

#include <string.h>

#define STR_TABLE_DEFAULT_CHUNK (64U * 1024U)

NphStrId nph_str_id(NphStr s)
{
    const u64 h = nph_hash64(s.data, s.len, NPH_HASH_DEFAULT_SEED);
    const NphStrId id = {h == 0U ? 1U : h};
    return id;
}

NphStrId nph_str_id_cstr(const char *s)
{
    return nph_str_id(nph_str_from_cstr(s));
}

NphResult nph_str_table_init(NphStrTable *t, NphAllocator *alloc, u32 chunk_size)
{
    NPH_ASSERT(t != NULL && alloc != NULL);
    memset(t, 0, sizeof *t);
    t->alloc = alloc;
    t->chunk_size = chunk_size != 0U ? chunk_size : STR_TABLE_DEFAULT_CHUNK;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(NphStr));
    NPH_TRY(nph_hashmap_init(&t->map, alloc, &desc));
    NPH_ARRAY_INIT(&t->chunks, alloc);
    return NPH_RESULT_OK;
}

void nph_str_table_free(NphStrTable *t)
{
    if (t == NULL || t->alloc == NULL) {
        return;
    }
    // Chunks are freed with the size they were allocated with: the
    // default chunk size, or the string's own size for oversized strings,
    // which is recorded in the first 8 bytes of every chunk.
    NPH_ARRAY_FOREACH (void *, chunk, &t->chunks) {
        u64 size;
        memcpy(&size, *chunk, sizeof size);
        NPH_FREE(t->alloc, *chunk, size, 16U);
    }
    NPH_ARRAY_FREE(&t->chunks);
    nph_hashmap_free(&t->map);
    memset(t, 0, sizeof *t);
}

// Reserves `bytes` of chunk storage; returns the destination or NULL.
static char *table_reserve(NphStrTable *t, u64 bytes)
{
    if (bytes > t->chunk_remaining) {
        // Each chunk starts with its own allocation size so free can return it.
        const u64 header = 16U;
        const u64 size = nph_max_u64((u64)t->chunk_size, bytes + header);
        u8 *chunk = (u8 *)NPH_ALLOC(t->alloc, size, 16U);
        if (chunk == NULL) {
            return NULL;
        }
        if (NPH_ARRAY_PUSH(&t->chunks, (void *)chunk) != NPH_RESULT_OK) {
            NPH_FREE(t->alloc, chunk, size, 16U);
            return NULL;
        }
        memcpy(chunk, &size, sizeof size);
        t->chunk_cursor = chunk + header;
        t->chunk_remaining = size - header;
    }
    char *dest = (char *)t->chunk_cursor;
    t->chunk_cursor += bytes;
    t->chunk_remaining -= bytes;
    return dest;
}

NphResult nph_str_table_intern(NphStrTable *t, NphStr s, NphStrId *out_id)
{
    NPH_ASSERT(t != NULL && t->alloc != NULL && out_id != NULL);
    const NphStrId id = nph_str_id(s);
    const NphStr *existing = (const NphStr *)nph_hashmap_find_u64(&t->map, id.value);
    if (existing != NULL) {
        *out_id = id;
        if (!nph_str_equals(*existing, s)) {
            NPH_ASSERT_MSG(false, "string ID collision: \"%.*s\" and \"%.*s\"", (int)existing->len,
                           existing->data, (int)s.len, s.data);
            return NPH_RESULT_CORRUPT;
        }
        return NPH_RESULT_OK;
    }
    char *copy = table_reserve(t, s.len + 1U);
    if (copy == NULL) {
        *out_id = NPH_STR_ID_NULL;
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (s.len > 0U) {
        memcpy(copy, s.data, (size_t)s.len);
    }
    copy[s.len] = '\0';
    const NphStr stored = {copy, s.len};
    const NphResult r = nph_hashmap_insert_u64(&t->map, id.value, &stored);
    if (r != NPH_RESULT_OK) {
        // The reserved bytes stay in the chunk; they are reclaimed with it.
        *out_id = NPH_STR_ID_NULL;
        return r;
    }
    *out_id = id;
    return NPH_RESULT_OK;
}

NphStr nph_str_table_lookup(const NphStrTable *t, NphStrId id)
{
    NPH_ASSERT(t != NULL);
    const NphStr *found = (const NphStr *)nph_hashmap_find_u64(&t->map, id.value);
    if (found == NULL) {
        const NphStr none = {NULL, 0U};
        return none;
    }
    return *found;
}

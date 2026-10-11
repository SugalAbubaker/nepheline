// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/sparse_set.h>

#include <string.h>

#define SPARSE_SET_MIN_CAP 16U

static u32 value_align(const NphSparseSet *s)
{
    return s->elem_size == 32U || s->elem_size == 64U ? s->elem_size : NPH_ALLOC_DEFAULT_ALIGN;
}

void nph_sparse_set_init(NphSparseSet *s, NphAllocator *alloc, u32 elem_size)
{
    NPH_ASSERT(s != NULL && alloc != NULL);
    memset(s, 0, sizeof *s);
    s->alloc = alloc;
    s->elem_size = elem_size;
}

void nph_sparse_set_free(NphSparseSet *s)
{
    if (s == NULL || s->alloc == NULL) {
        return;
    }
    if (s->sparse != NULL) {
        NPH_FREE_ARRAY(s->alloc, u32, s->sparse, s->sparse_cap);
    }
    if (s->dense != NULL) {
        NPH_FREE_ARRAY(s->alloc, u32, s->dense, s->dense_cap);
    }
    if (s->values != NULL) {
        NPH_FREE(s->alloc, s->values, (u64)s->dense_cap * s->elem_size, value_align(s));
    }
    memset(s, 0, sizeof *s);
}

static NphResult grow_sparse(NphSparseSet *s, u32 max_key)
{
    if (max_key < s->sparse_cap) {
        return NPH_RESULT_OK;
    }
    if (max_key == NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT; // NPH_U32_MAX is the "absent" marker
    }
    const u32 new_cap = nph_max_u32(nph_next_pow2_u32(max_key + 1U), SPARSE_SET_MIN_CAP);
    u32 *sparse = (u32 *)NPH_REALLOC(s->alloc, s->sparse, (u64)s->sparse_cap * sizeof(u32),
                                     (u64)new_cap * sizeof(u32), NPH_ALIGNOF(u32));
    if (sparse == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(sparse + s->sparse_cap, 0xFF, (size_t)(new_cap - s->sparse_cap) * sizeof(u32));
    s->sparse = sparse;
    s->sparse_cap = new_cap;
    return NPH_RESULT_OK;
}

static NphResult grow_dense(NphSparseSet *s, u32 count)
{
    if (count <= s->dense_cap) {
        return NPH_RESULT_OK;
    }
    const u32 new_cap =
        nph_max_u32(nph_max_u32(nph_next_pow2_u32(count), SPARSE_SET_MIN_CAP),
                    s->dense_cap >= NPH_U32_MAX / 2U ? NPH_U32_MAX : s->dense_cap * 2U);
    u32 *dense = (u32 *)NPH_REALLOC(s->alloc, s->dense, (u64)s->dense_cap * sizeof(u32),
                                    (u64)new_cap * sizeof(u32), NPH_ALIGNOF(u32));
    if (dense == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    s->dense = dense;
    if (s->elem_size > 0U) {
        u8 *values = (u8 *)NPH_REALLOC(s->alloc, s->values, (u64)s->dense_cap * s->elem_size,
                                       (u64)new_cap * s->elem_size, value_align(s));
        if (values == NULL) {
            // `dense` already grew; the set stays consistent with the old
            // capacity recorded until the next successful growth.
            s->dense_cap = new_cap;
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        s->values = values;
    }
    s->dense_cap = new_cap;
    return NPH_RESULT_OK;
}

NphResult nph_sparse_set_reserve(NphSparseSet *s, u32 max_key, u32 count)
{
    NPH_ASSERT(s != NULL && s->alloc != NULL);
    NPH_TRY(grow_sparse(s, max_key));
    return grow_dense(s, count);
}

u32 nph_sparse_set_position(const NphSparseSet *s, u32 key)
{
    if (key >= s->sparse_cap) {
        return NPH_U32_MAX;
    }
    const u32 pos = s->sparse[key];
    return (pos < s->len && s->dense[pos] == key) ? pos : NPH_U32_MAX;
}

bool nph_sparse_set_contains(const NphSparseSet *s, u32 key)
{
    return nph_sparse_set_position(s, key) != NPH_U32_MAX;
}

void *nph_sparse_set_get(const NphSparseSet *s, u32 key)
{
    const u32 pos = nph_sparse_set_position(s, key);
    if (pos == NPH_U32_MAX) {
        return NULL;
    }
    return s->elem_size > 0U ? (void *)nph_sparse_set_value_at(s, pos) : (void *)&s->dense[pos];
}

static NphResult append(NphSparseSet *s, u32 key, const void *value)
{
    NPH_TRY(grow_sparse(s, key));
    NPH_TRY(grow_dense(s, s->len + 1U));
    // dense_cap may exceed the values capacity after a partial failure;
    // grow_dense succeeded here, so both arrays hold `len + 1`.
    const u32 pos = s->len;
    s->sparse[key] = pos;
    s->dense[pos] = key;
    if (s->elem_size > 0U && value != NULL) {
        memcpy(nph_sparse_set_value_at(s, pos), value, s->elem_size);
    }
    s->len = pos + 1U;
    return NPH_RESULT_OK;
}

NphResult nph_sparse_set_insert(NphSparseSet *s, u32 key, const void *value)
{
    NPH_ASSERT(s != NULL && s->alloc != NULL);
    if (nph_sparse_set_contains(s, key)) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    return append(s, key, value);
}

NphResult nph_sparse_set_set(NphSparseSet *s, u32 key, const void *value)
{
    NPH_ASSERT(s != NULL && s->alloc != NULL);
    const u32 pos = nph_sparse_set_position(s, key);
    if (pos != NPH_U32_MAX) {
        if (s->elem_size > 0U && value != NULL) {
            memcpy(nph_sparse_set_value_at(s, pos), value, s->elem_size);
        }
        return NPH_RESULT_OK;
    }
    return append(s, key, value);
}

bool nph_sparse_set_remove(NphSparseSet *s, u32 key)
{
    NPH_ASSERT(s != NULL);
    const u32 pos = nph_sparse_set_position(s, key);
    if (pos == NPH_U32_MAX) {
        return false;
    }
    const u32 last = s->len - 1U;
    if (pos != last) {
        const u32 moved_key = s->dense[last];
        s->dense[pos] = moved_key;
        s->sparse[moved_key] = pos;
        if (s->elem_size > 0U) {
            memcpy(nph_sparse_set_value_at(s, pos), nph_sparse_set_value_at(s, last), s->elem_size);
        }
    }
    s->sparse[key] = NPH_U32_MAX;
    s->len = last;
    return true;
}

void nph_sparse_set_clear(NphSparseSet *s)
{
    NPH_ASSERT(s != NULL);
    if (s->sparse != NULL) {
        memset(s->sparse, 0xFF, (size_t)s->sparse_cap * sizeof(u32));
    }
    s->len = 0U;
}

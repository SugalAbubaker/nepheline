// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/array.h>
#include <nepheline/core/assert.h>

#include <string.h>

#define ARRAY_MIN_CAP 8U

static u8 *load_data(const NphArrayRef *r)
{
    void *p;
    memcpy((void *)&p, r->data_field, sizeof p);
    return (u8 *)p;
}

static void store_data(const NphArrayRef *r, void *p)
{
    memcpy(r->data_field, (const void *)&p, sizeof p);
}

u32 nph_array_elem_align(u32 elem_size)
{
    if ((elem_size == 32U || elem_size == 64U)) {
        return elem_size;
    }
    return NPH_ALLOC_DEFAULT_ALIGN;
}

// Reallocates to exactly `new_cap` elements (which must hold the length).
static NphResult set_capacity(const NphArrayRef *r, u32 new_cap, NphCallsite site)
{
    NPH_ASSERT(new_cap >= *r->len);
    if (new_cap == *r->cap) {
        return NPH_RESULT_OK;
    }
    const u32 align = nph_array_elem_align(r->elem_size);
    u8 *data = load_data(r);
    if (new_cap == 0U) {
        nph_free_at(r->alloc, data, (u64)*r->cap * r->elem_size, align, site);
        store_data(r, NULL);
        *r->cap = 0U;
        return NPH_RESULT_OK;
    }
    void *fresh = nph_realloc_at(r->alloc, data, (u64)*r->cap * r->elem_size,
                                 (u64)new_cap * r->elem_size, align, site);
    if (fresh == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    store_data(r, fresh);
    *r->cap = new_cap;
    return NPH_RESULT_OK;
}

bool nph_array_grow(NphArrayRef r, u32 extra, NphCallsite site)
{
    NPH_ASSERT(r.alloc != NULL);
    const u32 len = *r.len;
    if (extra > NPH_U32_MAX - len) {
        return false;
    }
    const u32 needed = len + extra;
    if (needed <= *r.cap) {
        return true;
    }
    u32 new_cap =
        *r.cap >= NPH_U32_MAX / 2U ? NPH_U32_MAX : nph_max_u32(*r.cap * 2U, ARRAY_MIN_CAP);
    if (new_cap < needed) {
        new_cap = needed;
    }
    return set_capacity(&r, new_cap, site) == NPH_RESULT_OK;
}

NphResult nph_array_reserve(NphArrayRef r, u32 min_cap, NphCallsite site)
{
    NPH_ASSERT(r.alloc != NULL);
    if (min_cap <= *r.cap) {
        return NPH_RESULT_OK;
    }
    return set_capacity(&r, min_cap, site);
}

NphResult nph_array_resize(NphArrayRef r, u32 new_len, NphCallsite site)
{
    NPH_ASSERT(r.alloc != NULL);
    const u32 len = *r.len;
    if (new_len > len) {
        if (!nph_array_grow(r, new_len - len, site)) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        memset(load_data(&r) + (u64)len * r.elem_size, 0,
               (size_t)((u64)(new_len - len) * r.elem_size));
    }
    *r.len = new_len;
    return NPH_RESULT_OK;
}

NphResult nph_array_push_n(NphArrayRef r, const void *src, u32 n, NphCallsite site)
{
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_ASSERT(src != NULL);
    if (!nph_array_grow(r, n, site)) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memcpy(load_data(&r) + (u64)*r.len * r.elem_size, src, (size_t)((u64)n * r.elem_size));
    *r.len += n;
    return NPH_RESULT_OK;
}

bool nph_array_make_gap(NphArrayRef r, u32 index, u32 n, NphCallsite site)
{
    const u32 len = *r.len;
    NPH_ASSERT_MSG(index <= len, "insert index %u past length %u", index, len);
    if (index > len) {
        return false;
    }
    if (!nph_array_grow(r, n, site)) {
        return false;
    }
    u8 *data = load_data(&r);
    memmove(data + (u64)(index + n) * r.elem_size, data + (u64)index * r.elem_size,
            (size_t)((u64)(len - index) * r.elem_size));
    *r.len = len + n;
    return true;
}

void nph_array_remove(NphArrayRef r, u32 index, u32 n)
{
    const u32 len = *r.len;
    NPH_ASSERT_MSG(index <= len && n <= len - index, "remove [%u, %u) past length %u", index,
                   index + n, len);
    if (index > len || n > len - index) {
        return;
    }
    u8 *data = load_data(&r);
    memmove(data + (u64)index * r.elem_size, data + (u64)(index + n) * r.elem_size,
            (size_t)((u64)(len - index - n) * r.elem_size));
    *r.len = len - n;
}

void nph_array_remove_swap(NphArrayRef r, u32 index)
{
    const u32 len = *r.len;
    NPH_ASSERT_MSG(index < len, "remove index %u past length %u", index, len);
    if (index >= len) {
        return;
    }
    u8 *data = load_data(&r);
    if (index != len - 1U) {
        memcpy(data + (u64)index * r.elem_size, data + (u64)(len - 1U) * r.elem_size, r.elem_size);
    }
    *r.len = len - 1U;
}

NphResult nph_array_shrink(NphArrayRef r, NphCallsite site)
{
    return set_capacity(&r, *r.len, site);
}

void nph_array_free(NphArrayRef r, NphCallsite site)
{
    *r.len = 0U;
    (void)set_capacity(&r, 0U, site);
}

u32 nph_array_pop_index(u32 *len)
{
    NPH_ASSERT_MSG(*len > 0U, "pop from an empty array");
    if (*len == 0U) {
        return 0U;
    }
    *len -= 1U;
    return *len;
}

u32 nph_array_last_index(u32 len)
{
    NPH_ASSERT_MSG(len > 0U, "last element of an empty array");
    return len > 0U ? len - 1U : 0U;
}

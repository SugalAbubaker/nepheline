// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>

#include <cook/cook.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_COOK, "cook", NPH_LOG_LEVEL_INFO);

// ---------------------------------------------------------------------------
// Sized blocks: a size header makes an allocator's sized free possible for
// libraries that free by pointer alone.
// ---------------------------------------------------------------------------

#define COOK_BLOCK_HEADER 16U

static u64 cook_block_size(const void *ptr)
{
    u64 size;
    memcpy(&size, (const u8 *)ptr - COOK_BLOCK_HEADER, sizeof size);
    return size;
}

void *cook_block_alloc(NphAllocator *alloc, u64 size)
{
    NPH_ASSERT(alloc != NULL);
    if (size == 0U) {
        size = 1U;
    }
    if (size > NPH_U64_MAX - COOK_BLOCK_HEADER) {
        return NULL; // the header would wrap the size around
    }
    u8 *block = (u8 *)NPH_ALLOC(alloc, size + COOK_BLOCK_HEADER, 16U);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &size, sizeof size);
    return block + COOK_BLOCK_HEADER;
}

void *cook_block_realloc(NphAllocator *alloc, void *ptr, u64 new_size)
{
    if (ptr == NULL) {
        return cook_block_alloc(alloc, new_size);
    }
    if (new_size == 0U) {
        new_size = 1U;
    }
    if (new_size > NPH_U64_MAX - COOK_BLOCK_HEADER) {
        return NULL;
    }
    const u64 old_size = cook_block_size(ptr);
    u8 *block = (u8 *)NPH_REALLOC(alloc, (u8 *)ptr - COOK_BLOCK_HEADER,
                                  old_size + COOK_BLOCK_HEADER, new_size + COOK_BLOCK_HEADER, 16U);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &new_size, sizeof new_size);
    return block + COOK_BLOCK_HEADER;
}

void cook_block_free(NphAllocator *alloc, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    const u64 size = cook_block_size(ptr);
    NPH_FREE(alloc, (u8 *)ptr - COOK_BLOCK_HEADER, size + COOK_BLOCK_HEADER, 16U);
}

void *cook_thirdparty_alloc(u64 size)
{
    return cook_block_alloc(nph_allocator_system(), size);
}

void *cook_thirdparty_realloc(void *ptr, u64 new_size)
{
    return cook_block_realloc(nph_allocator_system(), ptr, new_size);
}

void cook_thirdparty_free(void *ptr)
{
    cook_block_free(nph_allocator_system(), ptr);
}

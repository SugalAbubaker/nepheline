// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/platform/locked_alloc.h>

static void *locked_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NphLockedAllocator *la = (NphLockedAllocator *)self;
    nph_mutex_lock(&la->mutex);
    void *p = nph_alloc_at(la->inner, size, align, site);
    nph_mutex_unlock(&la->mutex);
    return p;
}

static void *locked_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                            NphCallsite site)
{
    NphLockedAllocator *la = (NphLockedAllocator *)self;
    nph_mutex_lock(&la->mutex);
    void *p = nph_realloc_at(la->inner, ptr, old_size, new_size, align, site);
    nph_mutex_unlock(&la->mutex);
    return p;
}

static void locked_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NphLockedAllocator *la = (NphLockedAllocator *)self;
    nph_mutex_lock(&la->mutex);
    nph_free_at(la->inner, ptr, size, align, site);
    nph_mutex_unlock(&la->mutex);
}

static const NphAllocatorVtable g_locked_vtable = {locked_alloc, locked_realloc, locked_free};

void nph_locked_allocator_init(NphLockedAllocator *la, NphAllocator *inner)
{
    NPH_ASSERT(la != NULL && inner != NULL);
    la->allocator.vtable = &g_locked_vtable;
    la->allocator.name = inner->name;
    la->inner = inner;
    nph_mutex_init(&la->mutex);
}

void nph_locked_allocator_destroy(NphLockedAllocator *la)
{
    NPH_ASSERT(la != NULL);
    nph_mutex_destroy(&la->mutex);
    la->inner = NULL;
    la->allocator.vtable = NULL;
}

// SPDX-License-Identifier: Apache-2.0
// The render graph (docs/gpu.md "Render graph"; O'Donnell, "FrameGraph:
// Extensible Rendering Architecture in Frostbite", GDC 2017): pass culling
// by reference counting, barrier derivation from declared uses, transient
// aliasing by lifetime, async compute batches joined with timeline
// semaphores, and parallel recording, consecutive passes sharing a command
// buffer so their number does not grow with the passes.
#include <nepheline/core/hash.h>
#include <nepheline/core/profile.h>

#include "gpu_internal.h"

#define RG_INVALID NPH_U32_MAX

typedef struct RgUse {
    u32 resource;
    NphGpuRgUse use;
    u32 base_mip;
    u32 mip_count;
    b32 write;
    u32 next; // next use of the same pass
} RgUse;

typedef struct RgAttachment {
    u32 resource;
    NphGpuRgAttachment desc;
} RgAttachment;

struct NphGpuRgPass {
    NphGpuRenderGraph *graph;
    NphGpuRgPassDesc desc;
    u32 index;
    u32 use_head;
    u32 use_tail;
    u32 use_count;
    u32 ref_count;
    b32 side_effects;
    b32 culled;
    u32 order; // execution index among live passes
    NphGpuQueueType queue;
    u32 batch;
    RgAttachment colors[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    u32 color_count;
    RgAttachment depth;
    b32 has_depth;
    b32 depth_read_only;
    u32 pre_head;
    u32 pre_count;
    u32 post_head;
    u32 post_count;
    u32 ts_begin;
    u32 ts_end;
};

typedef struct RgResource {
    b32 is_image;
    b32 imported;
    NphGpuImageDesc image_desc;
    NphGpuBufferDesc buffer_desc;
    NphGpuImage image;
    NphGpuBuffer buffer;
    u32 first_pass; // execution order of the first and last live use
    u32 last_pass;
    u32 reads;  // live readers (culling)
    u32 writer; // last declared writer pass (RG_INVALID: none)
    b32 cross_queue;
    // Images per mip; buffers use [0]. `state` holds the layout, the owning
    // queue, and the stages and access of the last write (or of the pass a
    // layout transition was ordered before): what a later use must wait
    // for. read_stages / read_access are the reads already ordered after
    // that write, so a read at a stage, or of a kind, not among them needs
    // a barrier of its own (a write made visible to compute shaders is not
    // visible to an indirect-argument or transfer read).
    NphGpuMipState state[NPH_GPU_MAX_MIPS];
    u32 read_stages[NPH_GPU_MAX_MIPS];
    u32 read_access[NPH_GPU_MAX_MIPS];
    b32 first_use_done;
    NphGpuSwapchain *present;
    // transient placement
    VkMemoryRequirements req;
    u32 block;
    u64 offset;
    u32 alias_src_stages; // final state of resources this one aliases
    u32 alias_src_access;
    u64 cache_key;
    b32 present_transition_done;
} RgResource;

typedef struct RgBarrier {
    b32 is_image;
    NphGpuImageBarrier image;
    NphGpuBufferBarrier buffer;
    u32 next;
} RgBarrier;

typedef struct RgBlockMember {
    u32 resource;
    u64 offset;
    u64 size;
} RgBlockMember;

#define RG_MAX_BLOCKS        16U
#define RG_BLOCK_MAX_MEMBERS 256U

typedef struct RgBlock {
    b32 is_image;
    u32 type_bits;
    u64 alignment;
    u64 size; // needed this frame
    RgBlockMember members[RG_BLOCK_MAX_MEMBERS];
    u32 member_count;
    // Allocation kept across frames
    NphGpuAllocation alloc;
    u64 alloc_size;
    u32 alloc_type_bits;
    b32 alloc_is_image;
} RgBlock;

typedef struct RgCacheEntry {
    u64 key;
    b32 is_image;
    NphGpuImage image;
    NphGpuBuffer buffer;
    u32 block;
    b32 used;
    b32 live;
} RgCacheEntry;

typedef struct RgBatch {
    NphGpuQueueType queue;
    u32 first_pass; // index into the live pass order
    u32 pass_count;
    u32 first_group; // its command buffers (RgGroup), in order
    u32 group_count;
    u32 depends_on[NPH_GPU_QUEUE_COUNT];     // batch index per queue, RG_INVALID: none
    b32 wait_submitted[NPH_GPU_QUEUE_COUNT]; // wait for all earlier submits on that queue
    u64 value;                               // timeline value after submit
    b32 wait_acquire;
    b32 signal_present;
} RgBatch;

// Consecutive passes of a batch recorded into one command buffer by one
// thread (plan_groups).
typedef struct RgGroup {
    NphGpuRenderGraph *graph;
    u32 first; // index into the live pass order
    u32 count;
    NphGpuCmd *cmd;
    NphResult result;
} RgGroup;

typedef struct RgTiming {
    const char *name;
    u32 ts_begin;
    u32 ts_end;
    NphGpuQueueType queue;
    u64 read_bytes;
    u64 write_bytes;
} RgTiming;

struct NphGpuRenderGraph {
    NphGpuDevice *dev;
    NphAllocator *alloc;
    NphJobSystem *jobs;
    NphGpuRenderGraphDesc desc;
    char name[NPH_GPU_NAME_MAX];

    NphGpuRgPass *passes;
    u32 pass_count;
    RgResource *resources;
    u32 resource_count;
    RgUse *uses;
    u32 use_count;
    RgBarrier *barriers;
    u32 barrier_count;
    u32 *order; // live passes in execution order
    u32 order_count;
    RgBatch *batches;
    u32 batch_count;
    RgGroup *groups; // at most one per live pass
    u32 group_count;
    RgBlock blocks[RG_MAX_BLOCKS];
    u32 block_count;
    RgCacheEntry *cache;
    u32 cache_count;
    u32 cache_cap;

    NphGpuSwapchain *present_swapchain;
    u32 present_resource;

    b32 compiled;
    b32 executing;
    NphGpuRgStats stats;

    RgTiming *timings[NPH_GPU_FRAMES_IN_FLIGHT];
    u32 timing_count[NPH_GPU_FRAMES_IN_FLIGHT];
    u64 timing_frame[NPH_GPU_FRAMES_IN_FLIGHT];
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult nph_gpu_render_graph_create(NphGpuDevice *dev, const NphGpuRenderGraphDesc *desc,
                                      NphGpuRenderGraph **out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuRenderGraph *g = NPH_ALLOC_TYPE(desc->alloc, NphGpuRenderGraph);
    if (g == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    g->dev = dev;
    g->alloc = desc->alloc;
    g->jobs = desc->jobs;
    g->desc = *desc;
    if (g->desc.max_passes == 0U) {
        g->desc.max_passes = 128U;
    }
    if (g->desc.max_resources == 0U) {
        g->desc.max_resources = 256U;
    }
    if (g->desc.max_uses == 0U) {
        g->desc.max_uses = 1024U;
    }
    if (g->desc.max_barriers == 0U) {
        g->desc.max_barriers = 1024U;
    }
    if (g->desc.transient_cache == 0U) {
        g->desc.transient_cache = 256U;
    }
    gpu_copy_name(g->name, desc->name, "render graph", 0U);
    g->desc.name = g->name;
    g->passes = NPH_ALLOC_ARRAY_ZEROED(g->alloc, NphGpuRgPass, g->desc.max_passes);
    g->resources = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgResource, g->desc.max_resources);
    g->uses = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgUse, g->desc.max_uses);
    g->barriers = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgBarrier, g->desc.max_barriers);
    g->order = NPH_ALLOC_ARRAY_ZEROED(g->alloc, u32, g->desc.max_passes);
    g->batches = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgBatch, g->desc.max_passes);
    g->groups = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgGroup, g->desc.max_passes);
    g->cache_cap = g->desc.transient_cache;
    g->cache = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgCacheEntry, g->cache_cap);
    bool ok = g->passes != NULL && g->resources != NULL && g->uses != NULL && g->barriers != NULL &&
              g->order != NULL && g->batches != NULL && g->groups != NULL && g->cache != NULL;
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        g->timings[i] = NPH_ALLOC_ARRAY_ZEROED(g->alloc, RgTiming, g->desc.max_passes);
        ok = ok && g->timings[i] != NULL;
    }
    if (!ok) {
        nph_gpu_render_graph_destroy(g);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    *out = g;
    return NPH_RESULT_OK;
}

static void cache_entry_destroy(NphGpuRenderGraph *g, RgCacheEntry *e)
{
    if (!e->live) {
        return;
    }
    if (e->is_image) {
        nph_gpu_image_destroy(g->dev, e->image);
    } else {
        nph_gpu_buffer_destroy(g->dev, e->buffer);
    }
    e->live = 0;
}

void nph_gpu_render_graph_destroy(NphGpuRenderGraph *g)
{
    if (g == NULL) {
        return;
    }
    NphAllocator *alloc = g->alloc;
    NphGpuDevice *dev = g->dev;
    if (g->cache != NULL) {
        for (u32 i = 0; i < g->cache_count; ++i) {
            cache_entry_destroy(g, &g->cache[i]);
        }
        NPH_FREE_ARRAY(alloc, RgCacheEntry, g->cache, g->cache_cap);
    }
    for (u32 i = 0; i < g->block_count; ++i) {
        if (g->blocks[i].alloc != NULL) {
            NphGpuDeferred e;
            e.type = NPH_GPU_DEFERRED_ALLOCATION;
            e.index = 0U;
            e.handle = 0U;
            e.alloc = g->blocks[i].alloc;
            gpu_defer(dev, &e);
        }
    }
    const NphResult idle = nph_gpu_device_wait_idle(dev); // logged on failure
    NPH_UNUSED(idle);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        if (g->timings[i] != NULL) {
            NPH_FREE_ARRAY(alloc, RgTiming, g->timings[i], g->desc.max_passes);
        }
    }
    if (g->passes != NULL) {
        NPH_FREE_ARRAY(alloc, NphGpuRgPass, g->passes, g->desc.max_passes);
    }
    if (g->resources != NULL) {
        NPH_FREE_ARRAY(alloc, RgResource, g->resources, g->desc.max_resources);
    }
    if (g->uses != NULL) {
        NPH_FREE_ARRAY(alloc, RgUse, g->uses, g->desc.max_uses);
    }
    if (g->barriers != NULL) {
        NPH_FREE_ARRAY(alloc, RgBarrier, g->barriers, g->desc.max_barriers);
    }
    if (g->order != NULL) {
        NPH_FREE_ARRAY(alloc, u32, g->order, g->desc.max_passes);
    }
    if (g->batches != NULL) {
        NPH_FREE_ARRAY(alloc, RgBatch, g->batches, g->desc.max_passes);
    }
    if (g->groups != NULL) {
        NPH_FREE_ARRAY(alloc, RgGroup, g->groups, g->desc.max_passes);
    }
    NPH_FREE_TYPE(alloc, NphGpuRenderGraph, g);
}

// ---------------------------------------------------------------------------
// Declaration
// ---------------------------------------------------------------------------

void nph_gpu_rg_begin(NphGpuRenderGraph *g)
{
    NPH_ASSERT(g != NULL && !g->executing);
    g->pass_count = 0U;
    g->resource_count = 0U;
    g->use_count = 0U;
    g->barrier_count = 0U;
    g->order_count = 0U;
    g->batch_count = 0U;
    g->present_swapchain = NULL;
    g->present_resource = RG_INVALID;
    g->compiled = 0;
    memset(&g->stats, 0, sizeof g->stats);
}

static RgResource *new_resource(NphGpuRenderGraph *g, u32 *out_index)
{
    if (g->resource_count == g->desc.max_resources) {
        GPU_LOG_ERROR("render graph '%s': more than %u resources", g->name, g->desc.max_resources);
        *out_index = RG_INVALID;
        return NULL;
    }
    *out_index = g->resource_count++;
    RgResource *r = &g->resources[*out_index];
    memset(r, 0, sizeof *r);
    r->first_pass = RG_INVALID;
    r->last_pass = RG_INVALID;
    r->writer = RG_INVALID;
    r->block = RG_INVALID;
    return r;
}

NphGpuRgImage nph_gpu_rg_create_image(NphGpuRenderGraph *g, const NphGpuImageDesc *desc)
{
    NPH_ASSERT(g != NULL && desc != NULL);
    u32 index = RG_INVALID;
    RgResource *r = new_resource(g, &index);
    NphGpuRgImage h = NPH_GPU_RG_IMAGE_NULL;
    if (r == NULL) {
        return h;
    }
    r->is_image = 1;
    r->image_desc = *desc;
    gpu_image_normalize_desc(&r->image_desc);
    for (u32 m = 0; m < NPH_GPU_MAX_MIPS; ++m) {
        r->state[m].layout = NPH_GPU_LAYOUT_UNDEFINED;
        r->state[m].queue = (u32)NPH_GPU_QUEUE_GRAPHICS;
    }
    h.value = index + 1U;
    return h;
}

NphGpuRgBuffer nph_gpu_rg_create_buffer(NphGpuRenderGraph *g, const NphGpuBufferDesc *desc)
{
    NPH_ASSERT(g != NULL && desc != NULL);
    u32 index = RG_INVALID;
    RgResource *r = new_resource(g, &index);
    NphGpuRgBuffer h = NPH_GPU_RG_BUFFER_NULL;
    if (r == NULL) {
        return h;
    }
    r->buffer_desc = *desc;
    r->buffer_desc.memory = NPH_GPU_MEMORY_DEVICE;
    h.value = index + 1U;
    return h;
}

NphGpuRgImage nph_gpu_rg_import_image(NphGpuRenderGraph *g, NphGpuImage image)
{
    NPH_ASSERT(g != NULL);
    const NphGpuImageSlot *slot = gpu_image(g->dev, image);
    NphGpuRgImage h = NPH_GPU_RG_IMAGE_NULL;
    if (slot == NULL) {
        return h;
    }
    u32 index = RG_INVALID;
    RgResource *r = new_resource(g, &index);
    if (r == NULL) {
        return h;
    }
    r->is_image = 1;
    r->imported = 1;
    r->image = image;
    r->image_desc = slot->desc;
    memcpy(r->state, slot->state, sizeof r->state);
    h.value = index + 1U;
    return h;
}

NphGpuRgBuffer nph_gpu_rg_import_buffer(NphGpuRenderGraph *g, NphGpuBuffer buffer)
{
    NPH_ASSERT(g != NULL);
    const NphGpuBufferSlot *slot = gpu_buffer(g->dev, buffer);
    NphGpuRgBuffer h = NPH_GPU_RG_BUFFER_NULL;
    if (slot == NULL) {
        return h;
    }
    u32 index = RG_INVALID;
    RgResource *r = new_resource(g, &index);
    if (r == NULL) {
        return h;
    }
    r->imported = 1;
    r->buffer = buffer;
    r->buffer_desc = slot->desc;
    r->state[0] = slot->state;
    h.value = index + 1U;
    return h;
}

NphGpuRgPass *nph_gpu_rg_add_pass(NphGpuRenderGraph *g, const NphGpuRgPassDesc *desc)
{
    NPH_ASSERT(g != NULL && desc != NULL);
    if (g->pass_count == g->desc.max_passes) {
        GPU_LOG_ERROR("render graph '%s': more than %u passes", g->name, g->desc.max_passes);
        return NULL;
    }
    NphGpuRgPass *p = &g->passes[g->pass_count];
    memset(p, 0, sizeof *p);
    p->graph = g;
    p->desc = *desc;
    if (p->desc.name == NULL) {
        p->desc.name = "pass";
    }
    p->index = g->pass_count++;
    p->use_head = RG_INVALID;
    p->use_tail = RG_INVALID;
    p->side_effects = (desc->flags & (u32)NPH_GPU_RG_PASS_SIDE_EFFECTS) != 0U;
    p->pre_head = RG_INVALID;
    p->post_head = RG_INVALID;
    p->ts_begin = NPH_GPU_INVALID_INDEX;
    p->ts_end = NPH_GPU_INVALID_INDEX;
    return p;
}

static void add_use(NphGpuRenderGraph *g, NphGpuRgPass *pass, u32 resource, NphGpuRgUse use,
                    u32 base_mip, u32 mip_count, bool write)
{
    if (resource == RG_INVALID || resource >= g->resource_count) {
        GPU_LOG_ERROR("render graph '%s': pass '%s' uses an invalid resource", g->name,
                      pass->desc.name);
        return;
    }
    if (g->use_count == g->desc.max_uses) {
        GPU_LOG_ERROR("render graph '%s': more than %u resource uses", g->name, g->desc.max_uses);
        return;
    }
    const u32 index = g->use_count++;
    RgUse *u = &g->uses[index];
    u->resource = resource;
    u->use = use;
    u->base_mip = base_mip;
    u->mip_count = mip_count;
    u->write = write;
    u->next = RG_INVALID;
    if (pass->use_tail == RG_INVALID) {
        pass->use_head = index;
    } else {
        g->uses[pass->use_tail].next = index;
    }
    pass->use_tail = index;
    ++pass->use_count;
    RgResource *r = &g->resources[resource];
    if (write) {
        r->writer = pass->index;
        if (r->imported) {
            pass->side_effects = 1; // writes the outside world
        }
    }
}

#define OWNER(pass) ((pass)->graph)

void nph_gpu_rg_read_image(NphGpuRgPass *pass, NphGpuRgImage image, NphGpuRgUse use, u32 base_mip,
                           u32 mip_count)
{
    NPH_ASSERT(pass != NULL);
    NPH_ASSERT(use == NPH_GPU_RG_USE_SAMPLED || use == NPH_GPU_RG_USE_STORAGE_READ ||
               use == NPH_GPU_RG_USE_TRANSFER_SRC || use == NPH_GPU_RG_USE_DEPTH_READ_ONLY);
    NphGpuRenderGraph *g = OWNER(pass);
    NPH_ASSERT(g != NULL);
    add_use(g, pass, image.value - 1U, use, base_mip, mip_count, false);
}

void nph_gpu_rg_write_image(NphGpuRgPass *pass, NphGpuRgImage image, NphGpuRgUse use, u32 base_mip,
                            u32 mip_count)
{
    NPH_ASSERT(pass != NULL);
    NPH_ASSERT(use == NPH_GPU_RG_USE_STORAGE_WRITE || use == NPH_GPU_RG_USE_TRANSFER_DST);
    NphGpuRenderGraph *g = OWNER(pass);
    NPH_ASSERT(g != NULL);
    add_use(g, pass, image.value - 1U, use, base_mip, mip_count, true);
}

void nph_gpu_rg_color_attachment(NphGpuRgPass *pass, NphGpuRgImage image,
                                 const NphGpuRgAttachment *attachment)
{
    NPH_ASSERT(pass != NULL && attachment != NULL);
    NPH_ASSERT(pass->desc.type == NPH_GPU_RG_PASS_GRAPHICS);
    NphGpuRenderGraph *g = OWNER(pass);
    NPH_ASSERT(g != NULL);
    if (pass->color_count == NPH_GPU_MAX_COLOR_ATTACHMENTS) {
        GPU_LOG_ERROR("render graph: pass '%s' has more than %u colour attachments",
                      pass->desc.name, NPH_GPU_MAX_COLOR_ATTACHMENTS);
        return;
    }
    RgAttachment *a = &pass->colors[pass->color_count++];
    a->resource = image.value - 1U;
    a->desc = *attachment;
    add_use(g, pass, a->resource, NPH_GPU_RG_USE_COLOR_ATTACHMENT, attachment->mip, 1U, true);
}

void nph_gpu_rg_depth_attachment(NphGpuRgPass *pass, NphGpuRgImage image,
                                 const NphGpuRgAttachment *attachment, b32 read_only)
{
    NPH_ASSERT(pass != NULL && attachment != NULL);
    NPH_ASSERT(pass->desc.type == NPH_GPU_RG_PASS_GRAPHICS);
    NphGpuRenderGraph *g = OWNER(pass);
    NPH_ASSERT(g != NULL);
    pass->depth.resource = image.value - 1U;
    pass->depth.desc = *attachment;
    pass->has_depth = 1;
    pass->depth_read_only = read_only;
    add_use(g, pass, pass->depth.resource,
            read_only ? NPH_GPU_RG_USE_DEPTH_READ_ONLY : NPH_GPU_RG_USE_DEPTH_ATTACHMENT,
            attachment->mip, 1U, !read_only);
}

void nph_gpu_rg_read_buffer(NphGpuRgPass *pass, NphGpuRgBuffer buffer, NphGpuRgUse use)
{
    NPH_ASSERT(pass != NULL);
    NPH_ASSERT(use == NPH_GPU_RG_USE_SHADER_READ || use == NPH_GPU_RG_USE_STORAGE_READ ||
               use == NPH_GPU_RG_USE_INDEX || use == NPH_GPU_RG_USE_INDIRECT ||
               use == NPH_GPU_RG_USE_TRANSFER_SRC || use == NPH_GPU_RG_USE_ACCEL_INPUT ||
               use == NPH_GPU_RG_USE_ACCEL_READ);
    NphGpuRenderGraph *g = OWNER(pass);
    NPH_ASSERT(g != NULL);
    add_use(g, pass, buffer.value - 1U, use, 0U, 1U, false);
}

void nph_gpu_rg_write_buffer(NphGpuRgPass *pass, NphGpuRgBuffer buffer, NphGpuRgUse use)
{
    NPH_ASSERT(pass != NULL);
    NPH_ASSERT(use == NPH_GPU_RG_USE_SHADER_WRITE || use == NPH_GPU_RG_USE_STORAGE_WRITE ||
               use == NPH_GPU_RG_USE_TRANSFER_DST || use == NPH_GPU_RG_USE_ACCEL_SCRATCH ||
               use == NPH_GPU_RG_USE_ACCEL_BUILD);
    NphGpuRenderGraph *g = OWNER(pass);
    NPH_ASSERT(g != NULL);
    add_use(g, pass, buffer.value - 1U, use, 0U, 1U, true);
}

void nph_gpu_rg_present(NphGpuRenderGraph *g, NphGpuRgImage image, NphGpuSwapchain *swapchain)
{
    NPH_ASSERT(g != NULL && swapchain != NULL);
    const u32 index = image.value - 1U;
    if (index >= g->resource_count || !g->resources[index].imported) {
        GPU_LOG_ERROR("render graph '%s': present needs an imported swapchain image", g->name);
        return;
    }
    g->present_swapchain = swapchain;
    g->present_resource = index;
    g->resources[index].present = swapchain;
}

// ---------------------------------------------------------------------------
// Compile: culling
// ---------------------------------------------------------------------------

static void cull(NphGpuRenderGraph *g)
{
    // Reference counts: a pass is referenced by every resource it writes
    // that someone reads, plus one for side effects; a resource by every
    // pass that reads it.
    for (u32 i = 0; i < g->resource_count; ++i) {
        g->resources[i].reads = 0U;
    }
    for (u32 p = 0; p < g->pass_count; ++p) {
        NphGpuRgPass *pass = &g->passes[p];
        pass->ref_count = pass->side_effects ? 1U : 0U;
        pass->culled = 0;
        for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
            if (g->uses[u].write) {
                ++pass->ref_count;
            } else {
                ++g->resources[g->uses[u].resource].reads;
            }
        }
    }
    // Any resource nobody reads releases its writer; a writer with nothing
    // left releases what it read, until nothing changes.
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 p = 0; p < g->pass_count; ++p) {
            NphGpuRgPass *pass = &g->passes[p];
            if (pass->culled || pass->side_effects) {
                continue;
            }
            u32 useful_writes = 0U;
            for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
                const RgUse *use = &g->uses[u];
                if (use->write && g->resources[use->resource].reads != 0U) {
                    ++useful_writes;
                }
            }
            if (useful_writes == 0U) {
                pass->culled = 1;
                changed = true;
                for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
                    const RgUse *use = &g->uses[u];
                    if (!use->write) {
                        --g->resources[use->resource].reads;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Compile: queues, order, lifetimes
// ---------------------------------------------------------------------------

static u32 pass_shader_stages(const NphGpuRenderGraph *g, const NphGpuRgPass *pass)
{
    switch (pass->desc.type) {
    case NPH_GPU_RG_PASS_GRAPHICS: {
        u32 s = (u32)NPH_GPU_STAGE_VERTEX_SHADER | (u32)NPH_GPU_STAGE_FRAGMENT_SHADER;
        if (g->dev->info.features.mesh_shader) {
            s |= (u32)NPH_GPU_STAGE_TASK_SHADER | (u32)NPH_GPU_STAGE_MESH_SHADER;
        }
        return s;
    }
    case NPH_GPU_RG_PASS_COMPUTE:
        return (u32)NPH_GPU_STAGE_COMPUTE;
    case NPH_GPU_RG_PASS_TRANSFER:
    default:
        return (u32)NPH_GPU_STAGE_TRANSFER;
    }
}

static void schedule(NphGpuRenderGraph *g)
{
    g->order_count = 0U;
    for (u32 i = 0; i < g->resource_count; ++i) {
        g->resources[i].first_pass = RG_INVALID;
        g->resources[i].last_pass = RG_INVALID;
        g->resources[i].cross_queue = 0;
    }
    for (u32 p = 0; p < g->pass_count; ++p) {
        NphGpuRgPass *pass = &g->passes[p];
        if (pass->culled) {
            continue;
        }
        pass->order = g->order_count;
        g->order[g->order_count++] = p;
        pass->queue = NPH_GPU_QUEUE_GRAPHICS;
        if (pass->desc.type == NPH_GPU_RG_PASS_COMPUTE &&
            (pass->desc.flags & (u32)NPH_GPU_RG_PASS_ASYNC_COMPUTE) != 0U &&
            g->dev->info.features.async_compute) {
            pass->queue = NPH_GPU_QUEUE_COMPUTE;
        }
        for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
            RgResource *r = &g->resources[g->uses[u].resource];
            if (r->first_pass == RG_INVALID) {
                r->first_pass = pass->order;
            }
            r->last_pass = pass->order;
        }
    }
    // Resources touched from more than one queue are shared across families.
    for (u32 o = 0; o < g->order_count; ++o) {
        const NphGpuRgPass *pass = &g->passes[g->order[o]];
        for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
            RgResource *r = &g->resources[g->uses[u].resource];
            if (pass->queue != NPH_GPU_QUEUE_GRAPHICS) {
                r->cross_queue = 1;
            }
        }
    }
    // Batches: runs of consecutive live passes on one queue.
    g->batch_count = 0U;
    for (u32 o = 0; o < g->order_count; ++o) {
        NphGpuRgPass *pass = &g->passes[g->order[o]];
        if (g->batch_count == 0U || g->batches[g->batch_count - 1U].queue != pass->queue) {
            RgBatch *b = &g->batches[g->batch_count++];
            memset(b, 0, sizeof *b);
            b->queue = pass->queue;
            b->first_pass = o;
            for (u32 q = 0; q < NPH_GPU_QUEUE_COUNT; ++q) {
                b->depends_on[q] = RG_INVALID;
            }
        }
        RgBatch *b = &g->batches[g->batch_count - 1U];
        pass->batch = g->batch_count - 1U;
        ++b->pass_count;
    }
}

// ---------------------------------------------------------------------------
// Compile: transient placement
// ---------------------------------------------------------------------------

static bool lifetimes_overlap(const RgResource *a, const RgResource *b)
{
    return a->first_pass <= b->last_pass && b->first_pass <= a->last_pass;
}

static bool block_fits(const NphGpuRenderGraph *g, const RgBlock *block, const RgResource *r,
                       u64 offset)
{
    const u64 end = offset + r->req.size;
    for (u32 i = 0; i < block->member_count; ++i) {
        const RgBlockMember *m = &block->members[i];
        const RgResource *other = &g->resources[m->resource];
        if (offset < m->offset + m->size && m->offset < end && lifetimes_overlap(r, other)) {
            return false;
        }
    }
    return true;
}

static bool place_in_block(NphGpuRenderGraph *g, RgBlock *block, u32 resource)
{
    RgResource *r = &g->resources[resource];
    if ((block->type_bits & r->req.memoryTypeBits) == 0U ||
        block->member_count == RG_BLOCK_MAX_MEMBERS) {
        return false;
    }
    // Candidate offsets: 0 and the end of every member, aligned.
    u64 best = NPH_U64_MAX;
    for (u32 c = 0; c <= block->member_count; ++c) {
        u64 offset =
            c == 0U ? 0U
                    : nph_align_up_u64(block->members[c - 1U].offset + block->members[c - 1U].size,
                                       r->req.alignment);
        if (offset < best && block_fits(g, block, r, offset)) {
            best = offset;
        }
    }
    if (best == NPH_U64_MAX) {
        return false;
    }
    RgBlockMember *m = &block->members[block->member_count++];
    m->resource = resource;
    m->offset = best;
    m->size = r->req.size;
    r->block = (u32)(block - g->blocks);
    r->offset = best;
    block->type_bits &= r->req.memoryTypeBits;
    block->alignment = nph_max_u64(block->alignment, r->req.alignment);
    block->size = nph_max_u64(block->size, best + r->req.size);
    return true;
}

static NphResult pack_transients(NphGpuRenderGraph *g)
{
    NphGpuDevice *dev = g->dev;
    for (u32 b = 0; b < RG_MAX_BLOCKS; ++b) {
        RgBlock *block = &g->blocks[b];
        block->member_count = 0U;
        block->size = 0U;
        block->type_bits = NPH_U32_MAX;
        block->alignment = 1U;
    }
    // Requirements, then a size-descending stable order.
    u32 count = 0U;
    u32 sorted[256];
    for (u32 i = 0; i < g->resource_count; ++i) {
        RgResource *r = &g->resources[i];
        if (r->imported || r->first_pass == RG_INVALID) {
            continue;
        }
        if (r->is_image) {
            NPH_TRY(
                gpu_image_memory_requirements(dev, &r->image_desc, r->cross_queue != 0, &r->req));
        } else {
            NPH_TRY(gpu_buffer_memory_requirements(dev, &r->buffer_desc, &r->req));
        }
        g->stats.transient_bytes_requested += r->req.size;
        if (r->is_image) {
            ++g->stats.transient_images;
        } else {
            ++g->stats.transient_buffers;
        }
        if (count < NPH_ARRAY_COUNT(sorted)) {
            sorted[count++] = i;
        } else {
            GPU_LOG_ERROR("render graph '%s': more than %u transient resources", g->name,
                          (u32)NPH_ARRAY_COUNT(sorted));
            return NPH_RESULT_FULL;
        }
    }
    for (u32 i = 1; i < count; ++i) {
        const u32 key = sorted[i];
        u32 j = i;
        while (j > 0U && g->resources[sorted[j - 1U]].req.size < g->resources[key].req.size) {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = key;
    }
    u32 block_count = 0U;
    for (u32 s = 0; s < count; ++s) {
        RgResource *r = &g->resources[sorted[s]];
        bool placed = false;
        for (u32 b = 0; b < block_count && !placed; ++b) {
            if (g->blocks[b].is_image == r->is_image) {
                placed = place_in_block(g, &g->blocks[b], sorted[s]);
            }
        }
        if (!placed) {
            if (block_count == RG_MAX_BLOCKS) {
                GPU_LOG_ERROR("render graph '%s': more than %u memory blocks", g->name,
                              RG_MAX_BLOCKS);
                return NPH_RESULT_FULL;
            }
            RgBlock *block = &g->blocks[block_count++];
            block->is_image = r->is_image;
            block->type_bits = r->req.memoryTypeBits;
            if (!place_in_block(g, block, sorted[s])) {
                return NPH_RESULT_UNKNOWN;
            }
        }
    }
    // Blocks beyond this frame's count keep their allocations idle; they
    // are released when the graph is destroyed.
    g->block_count = nph_max_u32(g->block_count, block_count);
    for (u32 b = 0; b < block_count; ++b) {
        RgBlock *block = &g->blocks[b];
        g->stats.transient_bytes_allocated += block->size;
        const bool reuse = block->alloc != NULL && block->alloc_size >= block->size &&
                           (block->alloc_type_bits & block->type_bits) != 0U &&
                           block->alloc_is_image == block->is_image;
        if (reuse) {
            continue;
        }
        // Everything bound to the old memory dies with it.
        for (u32 c = 0; c < g->cache_count; ++c) {
            if (g->cache[c].live && g->cache[c].block == b) {
                cache_entry_destroy(g, &g->cache[c]);
            }
        }
        if (block->alloc != NULL) {
            NphGpuDeferred e;
            e.type = NPH_GPU_DEFERRED_ALLOCATION;
            e.index = 0U;
            e.handle = 0U;
            e.alloc = block->alloc;
            gpu_defer(dev, &e);
            block->alloc = NULL;
        }
        const VkResult vr = nph_gpu_alloc_allocate_block(dev->vma, block->size, block->alignment,
                                                         block->type_bits, &block->alloc);
        if (vr != VK_SUCCESS) {
            block->alloc = NULL;
            return nph_gpu_vk_result(vr, "nph_gpu_alloc_allocate_block");
        }
        block->alloc_size = block->size;
        block->alloc_type_bits = block->type_bits;
        block->alloc_is_image = block->is_image;
        GPU_LOG_DEBUG("render graph '%s': %s block %u allocated, %llu KiB", g->name,
                      block->is_image ? "image" : "buffer", b,
                      (unsigned long long)(block->size >> 10U));
    }
    // Alias predecessors: members sharing bytes with an earlier lifetime.
    for (u32 b = 0; b < block_count; ++b) {
        const RgBlock *block = &g->blocks[b];
        for (u32 i = 0; i < block->member_count; ++i) {
            RgResource *r = &g->resources[block->members[i].resource];
            r->alias_src_stages = 0U;
            r->alias_src_access = 0U;
        }
    }
    return NPH_RESULT_OK;
}

static u64 transient_key(const RgResource *r, u32 block, u64 offset)
{
    u64 h = r->is_image ? nph_hash64(&r->image_desc, offsetof(NphGpuImageDesc, name), 1U)
                        : nph_hash64(&r->buffer_desc, offsetof(NphGpuBufferDesc, name), 2U);
    h = nph_hash_combine(h, nph_hash_mix_u64(((u64)block << 32U) ^ (u64)r->cross_queue));
    return nph_hash_combine(h, nph_hash_mix_u64(offset));
}

static NphResult realise_transients(NphGpuRenderGraph *g)
{
    NphGpuDevice *dev = g->dev;
    for (u32 c = 0; c < g->cache_count; ++c) {
        g->cache[c].used = 0;
    }
    for (u32 i = 0; i < g->resource_count; ++i) {
        RgResource *r = &g->resources[i];
        if (r->imported || r->first_pass == RG_INVALID) {
            continue;
        }
        r->cache_key = transient_key(r, r->block, r->offset);
        RgCacheEntry *hit = NULL;
        for (u32 c = 0; c < g->cache_count; ++c) {
            RgCacheEntry *e = &g->cache[c];
            if (e->live && !e->used && e->key == r->cache_key && e->is_image == r->is_image) {
                hit = e;
                break;
            }
        }
        if (hit == NULL) {
            RgCacheEntry *e = NULL;
            for (u32 c = 0; c < g->cache_count; ++c) {
                if (!g->cache[c].live) {
                    e = &g->cache[c];
                    break;
                }
            }
            if (e == NULL) {
                if (g->cache_count == g->cache_cap) {
                    GPU_LOG_ERROR("render graph '%s': transient cache full (%u)", g->name,
                                  g->cache_cap);
                    return NPH_RESULT_FULL;
                }
                e = &g->cache[g->cache_count++];
            }
            memset(e, 0, sizeof *e);
            const RgBlock *block = &g->blocks[r->block];
            if (r->is_image) {
                NPH_TRY(
                    gpu_image_create_unbound(dev, &r->image_desc, r->cross_queue != 0, &e->image));
                NPH_TRY(gpu_image_bind_and_finish(dev, e->image, block->alloc, r->offset));
            } else {
                NPH_TRY(gpu_buffer_create_unbound(dev, &r->buffer_desc, &e->buffer));
                NPH_TRY(gpu_buffer_bind_and_finish(dev, e->buffer, block->alloc, r->offset));
            }
            e->key = r->cache_key;
            e->is_image = r->is_image;
            e->block = r->block;
            e->live = 1;
            hit = e;
        }
        hit->used = 1;
        if (r->is_image) {
            r->image = hit->image;
        } else {
            r->buffer = hit->buffer;
        }
    }
    // Transients not needed this frame go away.
    for (u32 c = 0; c < g->cache_count; ++c) {
        if (g->cache[c].live && !g->cache[c].used) {
            cache_entry_destroy(g, &g->cache[c]);
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Compile: barriers
// ---------------------------------------------------------------------------

typedef struct RequiredState {
    NphGpuLayout layout;
    u32 stages;
    u32 access;
} RequiredState;

static RequiredState required_state(const NphGpuRenderGraph *g, const NphGpuRgPass *pass,
                                    const RgResource *r, NphGpuRgUse use)
{
    RequiredState s;
    const u32 shader = pass_shader_stages(g, pass);
    const bool depth = r->is_image && nph_gpu_format_is_depth(r->image_desc.format);
    s.layout = NPH_GPU_LAYOUT_GENERAL;
    s.stages = shader;
    s.access = (u32)NPH_GPU_ACCESS_SHADER_READ;
    switch (use) {
    case NPH_GPU_RG_USE_SAMPLED:
        s.layout = depth ? NPH_GPU_LAYOUT_DEPTH_READ_ONLY : NPH_GPU_LAYOUT_SHADER_READ_ONLY;
        break;
    case NPH_GPU_RG_USE_STORAGE_READ:
    case NPH_GPU_RG_USE_SHADER_READ:
        break;
    case NPH_GPU_RG_USE_STORAGE_WRITE:
    case NPH_GPU_RG_USE_SHADER_WRITE:
        s.access = (u32)NPH_GPU_ACCESS_SHADER_READ | (u32)NPH_GPU_ACCESS_SHADER_WRITE;
        break;
    case NPH_GPU_RG_USE_INDEX:
        s.stages = (u32)NPH_GPU_STAGE_INDEX_INPUT;
        s.access = (u32)NPH_GPU_ACCESS_INDEX_READ;
        break;
    case NPH_GPU_RG_USE_INDIRECT:
        s.stages = (u32)NPH_GPU_STAGE_INDIRECT;
        s.access = (u32)NPH_GPU_ACCESS_INDIRECT_READ;
        break;
    case NPH_GPU_RG_USE_TRANSFER_SRC:
        s.layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
        s.stages = (u32)NPH_GPU_STAGE_TRANSFER;
        s.access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
        break;
    case NPH_GPU_RG_USE_TRANSFER_DST:
        s.layout = NPH_GPU_LAYOUT_TRANSFER_DST;
        s.stages = (u32)NPH_GPU_STAGE_TRANSFER;
        s.access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        break;
    case NPH_GPU_RG_USE_COLOR_ATTACHMENT:
        s.layout = NPH_GPU_LAYOUT_COLOR_ATTACHMENT;
        s.stages = (u32)NPH_GPU_STAGE_COLOR_OUTPUT;
        s.access = (u32)NPH_GPU_ACCESS_COLOR_READ | (u32)NPH_GPU_ACCESS_COLOR_WRITE;
        break;
    case NPH_GPU_RG_USE_DEPTH_ATTACHMENT:
        s.layout = NPH_GPU_LAYOUT_DEPTH_ATTACHMENT;
        s.stages = (u32)NPH_GPU_STAGE_DEPTH_STENCIL;
        s.access = (u32)NPH_GPU_ACCESS_DEPTH_READ | (u32)NPH_GPU_ACCESS_DEPTH_WRITE;
        break;
    case NPH_GPU_RG_USE_DEPTH_READ_ONLY:
        s.layout = NPH_GPU_LAYOUT_DEPTH_READ_ONLY;
        s.stages = (u32)NPH_GPU_STAGE_DEPTH_STENCIL;
        s.access = (u32)NPH_GPU_ACCESS_DEPTH_READ;
        break;
    case NPH_GPU_RG_USE_PRESENT:
        s.layout = NPH_GPU_LAYOUT_PRESENT;
        s.stages = (u32)NPH_GPU_STAGE_ALL;
        s.access = (u32)NPH_GPU_ACCESS_NONE;
        break;
    case NPH_GPU_RG_USE_ACCEL_INPUT:
        s.stages = (u32)NPH_GPU_STAGE_ACCEL_BUILD;
        s.access = (u32)NPH_GPU_ACCESS_SHADER_READ;
        break;
    case NPH_GPU_RG_USE_ACCEL_SCRATCH:
    case NPH_GPU_RG_USE_ACCEL_BUILD:
        s.stages = (u32)NPH_GPU_STAGE_ACCEL_BUILD;
        s.access = (u32)NPH_GPU_ACCESS_ACCEL_READ | (u32)NPH_GPU_ACCESS_ACCEL_WRITE;
        break;
    case NPH_GPU_RG_USE_ACCEL_READ:
        // Traced by the pass's shaders, or referenced by a top-level build.
        s.stages = shader | (u32)NPH_GPU_STAGE_ACCEL_BUILD;
        s.access = (u32)NPH_GPU_ACCESS_ACCEL_READ;
        break;
    case NPH_GPU_RG_USE_COUNT:
        break;
    }
    return s;
}

static bool access_writes(u32 access)
{
    return (access & ((u32)NPH_GPU_ACCESS_SHADER_WRITE | (u32)NPH_GPU_ACCESS_COLOR_WRITE |
                      (u32)NPH_GPU_ACCESS_DEPTH_WRITE | (u32)NPH_GPU_ACCESS_TRANSFER_WRITE |
                      (u32)NPH_GPU_ACCESS_HOST_WRITE | (u32)NPH_GPU_ACCESS_MEMORY_WRITE |
                      (u32)NPH_GPU_ACCESS_ACCEL_WRITE)) != 0U;
}

static RgBarrier *push_barrier(NphGpuRenderGraph *g, NphGpuRgPass *pass, bool post)
{
    if (g->barrier_count == g->desc.max_barriers) {
        return NULL;
    }
    const u32 index = g->barrier_count++;
    RgBarrier *b = &g->barriers[index];
    memset(b, 0, sizeof *b);
    b->next = post ? pass->post_head : pass->pre_head;
    if (post) {
        pass->post_head = index;
        ++pass->post_count;
    } else {
        pass->pre_head = index;
        ++pass->pre_count;
    }
    ++g->stats.barriers;
    return b;
}

// The source scope of a transient's first barrier in the graph. After the
// resources that shared its memory earlier in the graph, their final
// stages and accesses: their own first barriers ordered them after
// everything before. With none, every stage, and every write made
// available: the graph caches its transients across frames, so the same
// image or buffer (or memory a later transient aliased) was last written
// by an earlier frame still in flight, maybe last of all (a depth
// attachment stored DONT_CARE). A write-after-write needs that write
// made available, not only finished.
static void first_use_source(const RgResource *r, u32 *stages, u32 *access)
{
    if (r->alias_src_stages != 0U) {
        *stages = r->alias_src_stages;
        *access = r->alias_src_access;
    } else {
        *stages = (u32)NPH_GPU_STAGE_ALL;
        *access = (u32)NPH_GPU_ACCESS_MEMORY_WRITE;
    }
}

// Transitions one mip range of an image into the required state, emitting
// barriers (and a release on the previous owner for exclusive images).
static NphResult transition_image(NphGpuRenderGraph *g, NphGpuRgPass *pass, RgResource *r,
                                  u32 base_mip, u32 mip_count, const RequiredState *need)
{
    const u32 total = r->image_desc.mip_count;
    if (base_mip >= total) {
        return NPH_RESULT_OK;
    }
    if (mip_count == NPH_GPU_ALL_MIPS || mip_count == 0U || base_mip + mip_count > total) {
        mip_count = total - base_mip;
    }
    const NphGpuImageSlot *slot = gpu_image(g->dev, r->image);
    const bool exclusive = slot != NULL && slot->exclusive;
    u32 m = base_mip;
    while (m < base_mip + mip_count) {
        // Run of mips in the same state.
        const NphGpuMipState cur = r->state[m];
        const u32 rs = r->read_stages[m];
        const u32 ra = r->read_access[m];
        u32 run = 1U;
        while (m + run < base_mip + mip_count &&
               memcmp(&r->state[m + run], &cur, sizeof cur) == 0 && r->read_stages[m + run] == rs &&
               r->read_access[m + run] == ra) {
            ++run;
        }
        // Work on another queue is ordered by a semaphore that waits at
        // every stage, which also makes its writes available and visible,
        // so the barrier here names no source stages (a stage of the other
        // family would not even be valid on this queue). Ownership
        // transfers only exist between distinct families.
        const bool cross_queue = g->dev->queues[cur.queue] != g->dev->queues[pass->queue];
        const bool same_family =
            g->dev->queues[cur.queue]->family == g->dev->queues[pass->queue]->family;
        const bool need_layout = cur.layout != need->layout;
        const bool writes = access_writes(need->access);
        // A read at a stage or of a kind the last write was not yet made
        // visible to.
        const bool uncovered =
            cur.stages != 0U && ((need->stages & ~rs) != 0U || (need->access & ~ra) != 0U);
        const bool first_use = !r->imported && !r->first_use_done;
        bool transition = false; // the barrier changes the layout or the owner
        if (first_use) {
            // Transient: contents undefined; order after whatever aliased it
            // earlier in this graph or, with nothing before it here, after
            // everything submitted before (first_use_source).
            RgBarrier *b = push_barrier(g, pass, false);
            if (b == NULL) {
                return NPH_RESULT_FULL;
            }
            b->is_image = 1;
            NphGpuImageBarrier *ib = &b->image;
            ib->image = r->image;
            first_use_source(r, &ib->src_stages, &ib->src_access);
            ib->old_layout = NPH_GPU_LAYOUT_UNDEFINED;
            ib->dst_stages = need->stages;
            ib->dst_access = need->access;
            ib->new_layout = need->layout;
            ib->base_mip = m;
            ib->mip_count = run;
            ib->layer_count = NPH_GPU_ALL_LAYERS;
            ib->src_queue = pass->queue;
            ib->dst_queue = pass->queue;
            transition = true;
        } else if (!same_family && exclusive) {
            // Queue family ownership transfer: release on the owner's last
            // pass in this graph, acquire here.
            if (!r->imported || r->first_use_done) {
                NphGpuRgPass *owner = NULL;
                for (u32 o = pass->order; o-- > 0U;) {
                    NphGpuRgPass *cand = &g->passes[g->order[o]];
                    if ((u32)cand->queue == cur.queue) {
                        owner = cand;
                        break;
                    }
                }
                if (owner == NULL) {
                    GPU_LOG_ERROR("render graph '%s': image '%s' is owned by another queue and "
                                  "no pass in this graph can release it",
                                  g->name, r->image_desc.name != NULL ? r->image_desc.name : "");
                    return NPH_RESULT_INVALID_ARGUMENT;
                }
                RgBarrier *rel = push_barrier(g, owner, true);
                RgBarrier *acq = push_barrier(g, pass, false);
                if (rel == NULL || acq == NULL) {
                    return NPH_RESULT_FULL;
                }
                rel->is_image = 1;
                rel->image.image = r->image;
                const u32 src = cur.stages | rs;
                rel->image.src_stages = src == 0U ? (u32)NPH_GPU_STAGE_ALL : src;
                rel->image.src_access = cur.access;
                rel->image.old_layout = cur.layout;
                rel->image.dst_stages = need->stages;
                rel->image.dst_access = need->access;
                rel->image.new_layout = need->layout;
                rel->image.base_mip = m;
                rel->image.mip_count = run;
                rel->image.layer_count = NPH_GPU_ALL_LAYERS;
                rel->image.src_queue = (NphGpuQueueType)cur.queue;
                rel->image.dst_queue = pass->queue;
                acq->is_image = 1;
                acq->image = rel->image;
                transition = true;
            } else {
                GPU_LOG_ERROR("render graph '%s': imported image '%s' is owned by another queue; "
                              "its first use in the graph must be on the owning queue",
                              g->name, r->image_desc.name != NULL ? r->image_desc.name : "");
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        } else if (cross_queue ? need_layout : (need_layout || writes || uncovered)) {
            RgBarrier *b = push_barrier(g, pass, false);
            if (b == NULL) {
                return NPH_RESULT_FULL;
            }
            b->is_image = 1;
            NphGpuImageBarrier *ib = &b->image;
            ib->image = r->image;
            if (cross_queue) {
                ib->src_stages = (u32)NPH_GPU_STAGE_NONE;
                ib->src_access = (u32)NPH_GPU_ACCESS_NONE;
            } else if (need_layout || writes) {
                // After the last write and every read since.
                const u32 src = cur.stages | rs;
                ib->src_stages = src == 0U ? (u32)NPH_GPU_STAGE_ALL : src;
                ib->src_access = cur.access;
            } else {
                // A read after the last write: wait for that write only.
                ib->src_stages = cur.stages;
                ib->src_access = cur.access;
            }
            ib->old_layout = cur.layout;
            ib->dst_stages = need->stages;
            ib->dst_access = need->access;
            ib->new_layout = need->layout;
            ib->base_mip = m;
            ib->mip_count = run;
            ib->layer_count = NPH_GPU_ALL_LAYERS;
            ib->src_queue = pass->queue;
            ib->dst_queue = pass->queue;
            transition = need_layout;
        }
        for (u32 i = 0; i < run; ++i) {
            NphGpuMipState *st = &r->state[m + i];
            st->layout = need->layout;
            st->queue = (u32)pass->queue;
            if (writes) {
                st->stages = need->stages;
                st->access = need->access;
                r->read_stages[m + i] = 0U;
                r->read_access[m + i] = 0U;
            } else if (transition) {
                // A layout or owner change is a write ordered before this
                // pass's stages; later uses at other stages chain on them.
                st->stages = need->stages;
                st->access = (u32)NPH_GPU_ACCESS_NONE;
                r->read_stages[m + i] = need->stages;
                r->read_access[m + i] = need->access;
            } else if (cross_queue) {
                // The semaphore completed and made visible everything before.
                st->stages = (u32)NPH_GPU_STAGE_NONE;
                st->access = (u32)NPH_GPU_ACCESS_NONE;
                r->read_stages[m + i] = need->stages;
                r->read_access[m + i] = need->access;
            } else {
                r->read_stages[m + i] |= need->stages;
                r->read_access[m + i] |= need->access;
            }
        }
        m += run;
    }
    r->first_use_done = 1;
    return NPH_RESULT_OK;
}

static NphResult transition_buffer(NphGpuRenderGraph *g, NphGpuRgPass *pass, RgResource *r,
                                   const RequiredState *need)
{
    NphGpuMipState *cur = &r->state[0];
    u32 *rs = &r->read_stages[0];
    u32 *ra = &r->read_access[0];
    const bool first_use = !r->imported && !r->first_use_done;
    const bool cross_queue = g->dev->queues[cur->queue] != g->dev->queues[pass->queue];
    const bool writes = access_writes(need->access);
    // A read at a stage or of a kind the last write was not yet made
    // visible to (the counters a compute pass wrote, read next as
    // indirect arguments or by a copy).
    const bool uncovered =
        access_writes(cur->access) && ((need->stages & ~*rs) != 0U || (need->access & ~*ra) != 0U);
    if (first_use || (!cross_queue && (writes || uncovered))) {
        RgBarrier *b = push_barrier(g, pass, false);
        if (b == NULL) {
            return NPH_RESULT_FULL;
        }
        b->is_image = 0;
        b->buffer.buffer = r->buffer;
        if (first_use) {
            first_use_source(r, &b->buffer.src_stages, &b->buffer.src_access);
        } else if (writes) {
            // After the last write and every read since.
            const u32 src = cur->stages | *rs;
            b->buffer.src_stages = src == 0U ? (u32)NPH_GPU_STAGE_ALL : src;
            b->buffer.src_access = cur->access;
        } else {
            b->buffer.src_stages = cur->stages;
            b->buffer.src_access = cur->access;
        }
        b->buffer.dst_stages = need->stages;
        b->buffer.dst_access = need->access;
        b->buffer.size = NPH_GPU_WHOLE_SIZE;
    }
    if (writes) {
        cur->stages = need->stages;
        cur->access = need->access;
        *rs = 0U;
        *ra = 0U;
    } else if (cross_queue || first_use) {
        // Ordered by a semaphore (see transition_image), or a transient's
        // first use: nothing earlier is pending.
        cur->stages = (u32)NPH_GPU_STAGE_NONE;
        cur->access = (u32)NPH_GPU_ACCESS_NONE;
        *rs = need->stages;
        *ra = need->access;
    } else {
        *rs |= need->stages;
        *ra |= need->access;
    }
    cur->queue = (u32)pass->queue;
    r->first_use_done = 1;
    return NPH_RESULT_OK;
}

static NphResult compute_barriers(NphGpuRenderGraph *g)
{
    for (u32 i = 0; i < g->resource_count; ++i) {
        g->resources[i].first_use_done = 0;
        g->resources[i].present_transition_done = 0;
    }
    for (u32 o = 0; o < g->order_count; ++o) {
        NphGpuRgPass *pass = &g->passes[g->order[o]];
        RgBatch *batch = &g->batches[pass->batch];
        for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
            const RgUse *use = &g->uses[u];
            RgResource *r = &g->resources[use->resource];
            // Alias predecessors: the final state of everything that shared
            // the block before this resource's lifetime began.
            if (!r->imported && !r->first_use_done && r->block != RG_INVALID) {
                const RgBlock *block = &g->blocks[r->block];
                for (u32 mi = 0; mi < block->member_count; ++mi) {
                    const RgBlockMember *mem = &block->members[mi];
                    const RgResource *other = &g->resources[mem->resource];
                    if (other == r || other->last_pass >= r->first_pass) {
                        continue;
                    }
                    if (mem->offset < r->offset + r->req.size &&
                        r->offset < mem->offset + mem->size) {
                        const u32 mips = other->is_image ? other->image_desc.mip_count : 1U;
                        for (u32 mp = 0; mp < mips; ++mp) {
                            r->alias_src_stages |= other->state[mp].stages | other->read_stages[mp];
                            r->alias_src_access |= other->state[mp].access;
                        }
                    }
                }
            }
            // Cross-queue dependency: the batch waits for the batch that
            // last touched the resource on another queue, or, for an
            // imported resource entering the graph, for everything that
            // queue has been given so far (the previous frame's readers).
            const u32 prev_queue = r->state[0].queue;
            if (!r->first_use_done && r->imported &&
                g->dev->queues[prev_queue] != g->dev->queues[pass->queue]) {
                batch->wait_submitted[prev_queue] = 1;
            }
            if (r->first_use_done && prev_queue != (u32)pass->queue) {
                for (u32 o2 = o; o2-- > 0U;) {
                    const NphGpuRgPass *prev = &g->passes[g->order[o2]];
                    if ((u32)prev->queue == prev_queue) {
                        if (batch->depends_on[prev_queue] == RG_INVALID ||
                            batch->depends_on[prev_queue] < prev->batch) {
                            batch->depends_on[prev_queue] = prev->batch;
                        }
                        break;
                    }
                }
            }
            const RequiredState need = required_state(g, pass, r, use->use);
            if (r->is_image) {
                NPH_TRY(transition_image(g, pass, r, use->base_mip, use->mip_count, &need));
            } else {
                NPH_TRY(transition_buffer(g, pass, r, &need));
            }
        }
    }
    // Present: the final transition after the last pass that touches the
    // image, which must run on the graphics queue.
    if (g->present_resource != RG_INVALID) {
        RgResource *r = &g->resources[g->present_resource];
        if (r->last_pass == RG_INVALID) {
            GPU_LOG_ERROR("render graph '%s': the presented image is not used by any pass",
                          g->name);
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        NphGpuRgPass *last = &g->passes[g->order[r->last_pass]];
        NphGpuRgPass *first = &g->passes[g->order[r->first_pass]];
        if (last->queue != NPH_GPU_QUEUE_GRAPHICS || first->queue != NPH_GPU_QUEUE_GRAPHICS) {
            GPU_LOG_ERROR("render graph '%s': the presented image must be first and last used "
                          "on the graphics queue",
                          g->name);
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        const RequiredState need = required_state(g, last, r, NPH_GPU_RG_USE_PRESENT);
        RgBarrier *b = push_barrier(g, last, true);
        if (b == NULL) {
            return NPH_RESULT_FULL;
        }
        b->is_image = 1;
        b->image.image = r->image;
        const u32 src = r->state[0].stages | r->read_stages[0];
        b->image.src_stages = src == 0U ? (u32)NPH_GPU_STAGE_ALL : src;
        b->image.src_access = r->state[0].access;
        b->image.old_layout = r->state[0].layout;
        b->image.dst_stages = need.stages;
        b->image.dst_access = need.access;
        b->image.new_layout = need.layout;
        b->image.mip_count = NPH_GPU_ALL_MIPS;
        b->image.layer_count = NPH_GPU_ALL_LAYERS;
        b->image.src_queue = NPH_GPU_QUEUE_GRAPHICS;
        b->image.dst_queue = NPH_GPU_QUEUE_GRAPHICS;
        r->state[0].layout = need.layout;
        r->state[0].stages = need.stages;
        r->state[0].access = need.access;
        r->read_stages[0] = 0U;
        r->read_access[0] = 0U;
        g->batches[first->batch].wait_acquire = 1;
        g->batches[last->batch].signal_present = 1;
    }
    return NPH_RESULT_OK;
}

static NphResult compile_graph(NphGpuRenderGraph *g)
{
    NPH_ASSERT(g != NULL && !g->executing);
    g->compiled = 0;
    g->barrier_count = 0U;
    for (u32 p = 0; p < g->pass_count; ++p) {
        NphGpuRgPass *pass = &g->passes[p];
        pass->pre_head = RG_INVALID;
        pass->pre_count = 0U;
        pass->post_head = RG_INVALID;
        pass->post_count = 0U;
    }
    g->stats.passes = g->pass_count;
    cull(g);
    schedule(g);
    for (u32 p = 0; p < g->pass_count; ++p) {
        if (g->passes[p].culled) {
            ++g->stats.culled;
        } else if (g->passes[p].queue == NPH_GPU_QUEUE_COMPUTE) {
            ++g->stats.async_passes;
        }
    }
    NPH_TRY(pack_transients(g));
    NPH_TRY(realise_transients(g));
    NPH_TRY(compute_barriers(g));
    g->compiled = 1;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

static void record_barrier_list(NphGpuRenderGraph *g, NphGpuCmd *cmd, u32 head)
{
    NphGpuImageBarrier images[32];
    NphGpuBufferBarrier buffers[32];
    u32 ni = 0U;
    u32 nb = 0U;
    for (u32 b = head; b != RG_INVALID; b = g->barriers[b].next) {
        const RgBarrier *rb = &g->barriers[b];
        if (rb->is_image) {
            images[ni++] = rb->image;
        } else {
            buffers[nb++] = rb->buffer;
        }
        if (ni == 32U || nb == 32U) {
            NphGpuBarrierDesc desc;
            memset(&desc, 0, sizeof desc);
            desc.images = images;
            desc.image_count = ni;
            desc.buffers = buffers;
            desc.buffer_count = nb;
            nph_gpu_cmd_barrier(cmd, &desc);
            ni = 0U;
            nb = 0U;
        }
    }
    if (ni != 0U || nb != 0U) {
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = images;
        desc.image_count = ni;
        desc.buffers = buffers;
        desc.buffer_count = nb;
        nph_gpu_cmd_barrier(cmd, &desc);
    }
}

// Bytes of mips [base_mip, base_mip + mip_count) of a resource (all
// layers), or the whole size of a buffer.
static u64 use_range_bytes(const RgResource *r, u32 base_mip, u32 mip_count)
{
    if (!r->is_image) {
        return r->buffer_desc.size;
    }
    const NphGpuImageDesc *d = &r->image_desc;
    const u32 mips = d->mip_count != 0U ? d->mip_count : 1U;
    const u32 end =
        mip_count == NPH_GPU_ALL_MIPS || base_mip + mip_count > mips ? mips : base_mip + mip_count;
    const u32 layers = d->layer_count != 0U ? d->layer_count : 1U;
    const u32 samples = d->samples != 0U ? d->samples : 1U;
    u64 bytes = 0U;
    for (u32 m = base_mip; m < end; ++m) {
        bytes += nph_gpu_format_level_bytes(d->format, nph_gpu_mip_extent(d->width, m),
                                            nph_gpu_mip_extent(d->height, m),
                                            nph_gpu_mip_extent(d->depth, m)) *
                 layers * samples;
    }
    return bytes;
}

static const RgAttachment *pass_attachment(const NphGpuRgPass *pass, const RgUse *use)
{
    if (use->use == NPH_GPU_RG_USE_DEPTH_ATTACHMENT || use->use == NPH_GPU_RG_USE_DEPTH_READ_ONLY) {
        return pass->has_depth && pass->depth.resource == use->resource ? &pass->depth : NULL;
    }
    for (u32 i = 0; i < pass->color_count; ++i) {
        if (pass->colors[i].resource == use->resource) {
            return &pass->colors[i];
        }
    }
    return NULL;
}

// The declared footprint of a pass (NphGpuRgPassTiming.read_bytes and
// write_bytes): every use's range counted once as read and/or written.
static void pass_declared_bytes(const NphGpuRenderGraph *g, const NphGpuRgPass *pass, u64 *out_read,
                                u64 *out_write)
{
    u64 read = 0U;
    u64 write = 0U;
    for (u32 u = pass->use_head; u != RG_INVALID; u = g->uses[u].next) {
        const RgUse *use = &g->uses[u];
        const RgResource *r = &g->resources[use->resource];
        const u64 bytes = use_range_bytes(r, use->base_mip, use->mip_count);
        switch (use->use) {
        case NPH_GPU_RG_USE_COLOR_ATTACHMENT:
        case NPH_GPU_RG_USE_DEPTH_ATTACHMENT:
        case NPH_GPU_RG_USE_DEPTH_READ_ONLY: {
            const RgAttachment *a = pass_attachment(pass, use);
            const bool load = a == NULL || a->desc.load == NPH_GPU_LOAD_OP_LOAD;
            const bool store = use->use != NPH_GPU_RG_USE_DEPTH_READ_ONLY &&
                               (a == NULL || a->desc.store == NPH_GPU_STORE_OP_STORE);
            read += load ? bytes : 0U;
            write += store ? bytes : 0U;
            break;
        }
        case NPH_GPU_RG_USE_PRESENT:
            break;
        case NPH_GPU_RG_USE_STORAGE_WRITE:
        case NPH_GPU_RG_USE_SHADER_WRITE:
        case NPH_GPU_RG_USE_TRANSFER_DST:
            write += bytes;
            break;
        default:
            read += bytes;
            break;
        }
    }
    *out_read = read;
    *out_write = write;
}

// Records one pass into `cmd`, inside a debug label of its name when it
// shares the command buffer (the command buffer carries the name otherwise).
static void record_pass(NphGpuRenderGraph *g, NphGpuRgPass *pass, NphGpuCmd *cmd, bool label)
{
    NPH_PROFILE_BEGIN(pass->desc.name); // pass names are static strings
    if (label) {
        nph_gpu_cmd_begin_label(cmd, pass->desc.name, 0.4F, 0.6F, 0.9F);
    }
    pass->ts_begin = nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
    record_barrier_list(g, cmd, pass->pre_head);
    const bool rendering =
        pass->desc.type == NPH_GPU_RG_PASS_GRAPHICS && (pass->color_count != 0U || pass->has_depth);
    if (rendering) {
        NphGpuRenderingDesc rd;
        memset(&rd, 0, sizeof rd);
        for (u32 i = 0; i < pass->color_count; ++i) {
            const RgAttachment *a = &pass->colors[i];
            rd.colors[i].image = g->resources[a->resource].image;
            rd.colors[i].mip = a->desc.mip;
            rd.colors[i].base_layer = a->desc.base_layer;
            rd.colors[i].layer_count = a->desc.layer_count;
            rd.colors[i].load = a->desc.load;
            rd.colors[i].store = a->desc.store;
            rd.colors[i].clear = a->desc.clear;
        }
        rd.color_count = pass->color_count;
        if (pass->has_depth) {
            rd.depth.image = g->resources[pass->depth.resource].image;
            rd.depth.mip = pass->depth.desc.mip;
            rd.depth.base_layer = pass->depth.desc.base_layer;
            rd.depth.layer_count = pass->depth.desc.layer_count;
            rd.depth.load = pass->depth.desc.load;
            rd.depth.store = pass->depth.desc.store;
            rd.depth.clear = pass->depth.desc.clear;
            rd.depth_read_only = pass->depth_read_only;
        }
        nph_gpu_cmd_begin_rendering(cmd, &rd);
    }
    if (pass->desc.execute != NULL) {
        pass->desc.execute(g, cmd, pass->desc.user);
    }
    if (rendering) {
        nph_gpu_cmd_end_rendering(cmd);
    }
    record_barrier_list(g, cmd, pass->post_head);
    pass->ts_end = nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
    if (label) {
        nph_gpu_cmd_end_label(cmd);
    }
    NPH_PROFILE_END();
}

static void record_group(RgGroup *group)
{
    NphGpuRenderGraph *g = group->graph;
    const NphGpuRgPass *first = &g->passes[g->order[group->first]];
    group->cmd = NULL;
    group->result = nph_gpu_cmd_begin(g->dev, first->queue, first->desc.name, &group->cmd);
    if (group->result != NPH_RESULT_OK) {
        return;
    }
    for (u32 i = 0; i < group->count; ++i) {
        record_pass(g, &g->passes[g->order[group->first + i]], group->cmd, group->count > 1U);
    }
    nph_gpu_cmd_end(group->cmd);
}

static void record_job(void *user)
{
    record_group((RgGroup *)user);
}

// Splits each batch's passes into consecutive groups, one command buffer
// each: as many as there are threads to record them (one without a job
// system), so a frame's command buffers stay bounded by its batches and
// threads however many passes it has (CLAUDE.md §10.5, "a constant number
// of command buffers"), and no thread exhausts its per-frame pool.
static void plan_groups(NphGpuRenderGraph *g)
{
    const u32 threads =
        g->jobs != NULL ? nph_max_u32(nph_job_system_worker_count(g->jobs), 1U) : 1U;
    g->group_count = 0U;
    for (u32 b = 0; b < g->batch_count; ++b) {
        RgBatch *batch = &g->batches[b];
        const u32 n = nph_min_u32(batch->pass_count, threads);
        batch->first_group = g->group_count;
        batch->group_count = n;
        if (n == 0U) {
            continue; // a batch always holds a pass; this keeps the division safe
        }
        const u32 base = batch->pass_count / n;
        const u32 extra = batch->pass_count % n;
        u32 first = batch->first_pass;
        for (u32 k = 0; k < n; ++k) {
            RgGroup *group = &g->groups[g->group_count++];
            group->graph = g;
            group->first = first;
            group->count = base + (k < extra ? 1U : 0U);
            group->cmd = NULL;
            group->result = NPH_RESULT_OK;
            first += group->count;
        }
    }
}

static NphResult execute_graph(NphGpuRenderGraph *g)
{
    NPH_ASSERT(g != NULL);
    if (!g->compiled) {
        GPU_LOG_ERROR("render graph '%s': execute without a successful compile", g->name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuDevice *dev = g->dev;
    g->executing = 1;
    g->stats.submits = 0U;
    g->stats.command_buffers = 0U;

    // Record every group, in parallel when a job system is available.
    plan_groups(g);
    if (g->jobs != NULL && g->group_count > 1U) {
        NphJobDecl decls[128];
        u32 done = 0U;
        while (done < g->group_count) {
            u32 n = 0U;
            while (done + n < g->group_count && n < 128U) {
                RgGroup *group = &g->groups[done + n];
                decls[n].fn = record_job;
                decls[n].user = group;
                decls[n].name = g->passes[g->order[group->first]].desc.name;
                ++n;
            }
            NphJobCounter counter = NPH_JOB_COUNTER_INIT;
            if (nph_job_run(g->jobs, decls, n, NPH_JOB_PRIORITY_HIGH, &counter) != NPH_RESULT_OK) {
                for (u32 i = 0; i < n; ++i) {
                    record_group(&g->groups[done + i]);
                }
            } else {
                nph_job_wait(g->jobs, &counter);
            }
            done += n;
        }
    } else {
        for (u32 i = 0; i < g->group_count; ++i) {
            record_group(&g->groups[i]);
        }
    }
    for (u32 i = 0; i < g->group_count; ++i) {
        if (g->groups[i].result != NPH_RESULT_OK) {
            g->executing = 0;
            return g->groups[i].result;
        }
    }

    // Submit the batches in order.
    const u32 slot = dev->frame_slot;
    g->timing_count[slot] = 0U;
    g->timing_frame[slot] = dev->frame_index;
    NphResult result = NPH_RESULT_OK;
    for (u32 b = 0; b < g->batch_count && result == NPH_RESULT_OK; ++b) {
        RgBatch *batch = &g->batches[b];
        NphGpuCmd *cmds[128];
        u32 n = 0U;
        for (u32 i = 0; i < batch->group_count && n < 128U; ++i) {
            cmds[n++] = g->groups[batch->first_group + i].cmd;
        }
        for (u32 i = 0; i < batch->pass_count; ++i) {
            const NphGpuRgPass *pass = &g->passes[g->order[batch->first_pass + i]];
            RgTiming *t = &g->timings[slot][g->timing_count[slot]++];
            t->name = pass->desc.name;
            t->ts_begin = pass->ts_begin;
            t->ts_end = pass->ts_end;
            t->queue = pass->queue;
            pass_declared_bytes(g, pass, &t->read_bytes, &t->write_bytes);
        }
        NphGpuTimelineOp waits[2U * NPH_GPU_QUEUE_COUNT + 1U];
        u32 wait_count = 0U;
        for (u32 q = 0; q < NPH_GPU_QUEUE_COUNT; ++q) {
            if (dev->queues[q] == dev->queues[batch->queue]) {
                continue; // the queue orders its own work
            }
            u64 value = 0U;
            if (batch->depends_on[q] != RG_INVALID && batch->depends_on[q] < b) {
                value = g->batches[batch->depends_on[q]].value;
            }
            if (batch->wait_submitted[q]) {
                value = nph_max_u64(value, nph_gpu_queue_submitted_value(dev, (NphGpuQueueType)q));
            }
            if (value != 0U) {
                waits[wait_count].timeline = nph_gpu_queue_timeline(dev, (NphGpuQueueType)q);
                waits[wait_count].value = value;
                waits[wait_count].stages = (u32)NPH_GPU_STAGE_ALL;
                ++wait_count;
            }
        }
        // Compute batches also wait for the frame's uploads, which the
        // graphics queue orders implicitly.
        if (batch->queue != NPH_GPU_QUEUE_GRAPHICS && dev->upload.last_flush_value != 0U &&
            dev->queues[NPH_GPU_QUEUE_TRANSFER] != dev->queues[batch->queue]) {
            waits[wait_count].timeline = nph_gpu_queue_timeline(dev, NPH_GPU_QUEUE_TRANSFER);
            waits[wait_count].value = dev->upload.last_flush_value;
            waits[wait_count].stages = (u32)NPH_GPU_STAGE_ALL;
            ++wait_count;
        }
        NphGpuSubmitDesc submit;
        memset(&submit, 0, sizeof submit);
        submit.queue = batch->queue;
        submit.cmds = cmds;
        submit.cmd_count = n;
        submit.waits = waits;
        submit.wait_count = wait_count;
        if (g->present_swapchain != NULL && (batch->wait_acquire || batch->signal_present)) {
            submit.swapchain = g->present_swapchain;
            submit.swapchain_ops =
                (batch->wait_acquire ? (u32)NPH_GPU_SWAPCHAIN_WAIT_ACQUIRE : 0U) |
                (batch->signal_present ? (u32)NPH_GPU_SWAPCHAIN_SIGNAL_PRESENT : 0U);
        }
        result = nph_gpu_submit(dev, &submit, &batch->value);
        ++g->stats.submits;
        g->stats.command_buffers += n;
    }

    // Write the final states back to the imported resources.
    for (u32 i = 0; i < g->resource_count; ++i) {
        RgResource *r = &g->resources[i];
        if (!r->imported || r->first_pass == RG_INVALID) {
            continue;
        }
        // The device keeps one (stages, access) per mip: everything a later
        // barrier must wait for, the last write and the reads since.
        NphGpuMipState final_state[NPH_GPU_MAX_MIPS];
        memcpy(final_state, r->state, sizeof final_state);
        for (u32 m = 0; m < NPH_GPU_MAX_MIPS; ++m) {
            final_state[m].stages |= r->read_stages[m];
            final_state[m].access |= r->read_access[m];
        }
        if (r->is_image) {
            NphGpuImageSlot *islot = gpu_image(dev, r->image);
            if (islot != NULL) {
                memcpy(islot->state, final_state, sizeof islot->state);
            }
        } else {
            NphGpuBufferSlot *bslot = gpu_buffer(dev, r->buffer);
            if (bslot != NULL) {
                bslot->state = final_state[0];
            }
        }
    }
    g->executing = 0;
    g->compiled = 0; // a graph is executed once per declaration
    return result;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

NphGpuImage nph_gpu_rg_image(const NphGpuRenderGraph *g, NphGpuRgImage image)
{
    const u32 index = image.value - 1U;
    if (image.value == 0U || index >= g->resource_count || !g->resources[index].is_image) {
        return NPH_GPU_IMAGE_NULL;
    }
    return g->resources[index].image;
}

NphGpuBuffer nph_gpu_rg_buffer(const NphGpuRenderGraph *g, NphGpuRgBuffer buffer)
{
    const u32 index = buffer.value - 1U;
    if (buffer.value == 0U || index >= g->resource_count || g->resources[index].is_image) {
        return NPH_GPU_BUFFER_NULL;
    }
    return g->resources[index].buffer;
}

NphGpuDevice *nph_gpu_rg_device(NphGpuRenderGraph *g)
{
    return g->dev;
}

NphGpuRgStats nph_gpu_rg_stats(const NphGpuRenderGraph *g)
{
    return g->stats;
}

bool nph_gpu_rg_pass_is_culled(const NphGpuRenderGraph *g, const NphGpuRgPass *pass)
{
    NPH_UNUSED(g);
    return pass != NULL && pass->culled;
}

u32 nph_gpu_rg_timings(const NphGpuRenderGraph *g, NphGpuRgPassTiming *out, u32 cap,
                       u64 *out_frame_index)
{
    const NphGpuDevice *dev = g->dev;
    u64 frame = 0U;
    const u64 *ticks = NULL;
    u32 tick_count = 0U;
    if (!nph_gpu_frame_timestamps(dev, &frame, &ticks, &tick_count)) {
        return 0U;
    }
    const u32 slot = dev->frame_slot;
    if (g->timing_frame[slot] != frame) {
        return 0U;
    }
    const f64 period = (f64)dev->info.limits.timestamp_period_ns;
    // The earliest beginning of the frame's passes, which begin_ns counts
    // from.
    u64 first = NPH_U64_MAX;
    for (u32 i = 0; i < g->timing_count[slot]; ++i) {
        const RgTiming *t = &g->timings[slot][i];
        if (t->ts_begin < tick_count && ticks[t->ts_begin] < first) {
            first = ticks[t->ts_begin];
        }
    }
    u32 n = 0U;
    for (u32 i = 0; i < g->timing_count[slot] && n < cap; ++i) {
        const RgTiming *t = &g->timings[slot][i];
        out[n].name = t->name;
        out[n].queue = t->queue;
        out[n].read_bytes = t->read_bytes;
        out[n].write_bytes = t->write_bytes;
        out[n].gpu_ns = 0U;
        out[n].begin_ns = 0U;
        if (t->ts_begin < tick_count && t->ts_end < tick_count &&
            ticks[t->ts_end] >= ticks[t->ts_begin]) {
            out[n].gpu_ns = (u64)((f64)(ticks[t->ts_end] - ticks[t->ts_begin]) * period);
            out[n].begin_ns = (u64)((f64)(ticks[t->ts_begin] - first) * period);
        }
        ++n;
    }
    if (out_frame_index != NULL) {
        *out_frame_index = frame;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Profiled entry points
// ---------------------------------------------------------------------------

NphResult nph_gpu_rg_compile(NphGpuRenderGraph *g)
{
    NPH_PROFILE_BEGIN("gpu.rg.compile");
    const NphResult r = compile_graph(g);
    NPH_PROFILE_END();
    return r;
}

NphResult nph_gpu_rg_execute(NphGpuRenderGraph *g)
{
    NPH_PROFILE_BEGIN("gpu.rg.execute");
    const NphResult r = execute_graph(g);
    NPH_PROFILE_END();
    return r;
}

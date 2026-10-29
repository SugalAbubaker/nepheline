// SPDX-License-Identifier: Apache-2.0
// A world partition's cells' runs of instance slots (ADR-0064): each cell's
// members and its proxy's instances, and the instances of no cell, kept as
// runs in one GPU buffer as instances come and go, for the cell culls to
// read a cell's instances from where its cell is kept.
//
// The runs live in ranges of RenderVis.member_ranges, mirrored on the CPU
// (member_entries). A slot made goes at its run's end, the run moving to a
// range twice the size where it is full (the old range given back after the
// frames in flight); a slot destroyed takes its run's last in its place, the
// moved slot's place (RenderInstance.run_entry) following. A frame uploads
// the runs that changed, whole, and their cells' records (NphVisCellRun):
// each change is O(1) amortized, and a scene loaded cell by cell fills each
// run once.
#include <nepheline/core/assert.h>
#include <nepheline/gpu/upload.h>

#include "render_internal.h"

#include <string.h>

// A run's first range: a cell of the landscape holds about 130 members.
#define RUN_FIRST_CAPACITY 16U

// The run instance `inst` belongs to, and its cell's slot (NPH_U32_MAX: the
// run of no cell).
static RenderRun *run_of(NphRenderer *r, const RenderInstance *inst, u32 *cell_slot)
{
    if (inst->cell == 0U) {
        *cell_slot = NPH_U32_MAX;
        return &r->vis.loose;
    }
    *cell_slot = inst->cell - 1U;
    RenderCell *c = &r->cells[inst->cell - 1U];
    return inst->proxy ? &c->proxies_run : &c->members_run;
}

static void mark(RenderVis *v, u32 cell_slot)
{
    if (cell_slot == NPH_U32_MAX) {
        v->loose_dirty = 1;
    } else if (v->run_dirty[cell_slot] == 0U) {
        v->run_dirty[cell_slot] = 1U;
        v->runs_changed[v->runs_changed_count++] = cell_slot;
    }
}

// Room for one more slot in `run`: a first range, or one twice the size with
// the entries moved where it is full.
static NphResult run_room(NphRenderer *r, RenderRun *run)
{
    RenderVis *v = &r->vis;
    if (run->count < run->capacity) {
        return NPH_RESULT_OK;
    }
    const u32 capacity = run->capacity == 0U ? RUN_FIRST_CAPACITY : run->capacity * 2U;
    NphRange range = NPH_RANGE_NULL;
    NPH_TRY(nph_range_alloc(&v->member_ranges, capacity, &range));
    if (run->capacity != 0U) {
        memcpy(v->member_entries + range.offset, v->member_entries + run->range.offset,
               (size_t)run->count * sizeof(u32));
        render_vis_member_range_free_later(r, run->range);
    }
    run->range = range;
    run->capacity = capacity;
    return NPH_RESULT_OK;
}

NphResult render_vis_run_add(NphRenderer *r, u32 slot)
{
    RenderVis *v = &r->vis;
    if (!v->supported) {
        return NPH_RESULT_OK;
    }
    RenderInstance *inst = &r->instances[slot];
    u32 cell_slot = NPH_U32_MAX;
    RenderRun *run = run_of(r, inst, &cell_slot);
    NPH_TRY(run_room(r, run));
    inst->run_entry = run->count;
    v->member_entries[run->range.offset + run->count] = slot;
    run->count += 1U;
    mark(v, cell_slot);
    return NPH_RESULT_OK;
}

void render_vis_run_remove(NphRenderer *r, u32 slot)
{
    RenderVis *v = &r->vis;
    if (!v->supported) {
        return;
    }
    const RenderInstance *inst = &r->instances[slot];
    u32 cell_slot = NPH_U32_MAX;
    RenderRun *run = run_of(r, inst, &cell_slot);
    NPH_ASSERT(run->count != 0U && inst->run_entry < run->count &&
               v->member_entries[run->range.offset + inst->run_entry] == slot);
    const u32 last = v->member_entries[run->range.offset + run->count - 1U];
    v->member_entries[run->range.offset + inst->run_entry] = last;
    r->instances[last].run_entry = inst->run_entry;
    run->count -= 1U;
    mark(v, cell_slot);
}

void render_vis_cell_runs_release(NphRenderer *r, u32 cell_slot)
{
    RenderVis *v = &r->vis;
    RenderCell *c = &r->cells[cell_slot];
    NPH_ASSERT_MSG(c->members_run.count == 0U && c->proxies_run.count == 0U,
                   "a cell's runs released while instances are in them");
    if (c->members_run.capacity != 0U) {
        render_vis_member_range_free_later(r, c->members_run.range);
    }
    if (c->proxies_run.capacity != 0U) {
        render_vis_member_range_free_later(r, c->proxies_run.range);
    }
    memset(&c->members_run, 0, sizeof c->members_run);
    memset(&c->proxies_run, 0, sizeof c->proxies_run);
    // Its record goes up empty: a cell made in the slot later starts so.
    if (v->supported) {
        mark(v, cell_slot);
    }
}

// Run `run`'s entries to the member buffer.
static NphResult run_upload(NphRenderer *r, const RenderRun *run)
{
    RenderVis *v = &r->vis;
    if (run->count == 0U) {
        return NPH_RESULT_OK;
    }
    return nph_gpu_upload_buffer(r->dev, v->cell_members, (u64)run->range.offset * sizeof(u32),
                                 v->member_entries + run->range.offset,
                                 (u64)run->count * sizeof(u32));
}

NphResult render_vis_runs_upload(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    if (!v->supported) {
        return NPH_RESULT_OK;
    }
    NphResult res = NPH_RESULT_OK;
    const bool any = v->runs_changed_count != 0U || v->loose_dirty;
    for (u32 i = 0; i < v->runs_changed_count && res == NPH_RESULT_OK; ++i) {
        const u32 cell_slot = v->runs_changed[i];
        const RenderCell *c = &r->cells[cell_slot];
        res = run_upload(r, &c->members_run);
        if (res == NPH_RESULT_OK) {
            res = run_upload(r, &c->proxies_run);
        }
        if (res == NPH_RESULT_OK) {
            const NphVisCellRun rec = {
                c->members_run.count != 0U ? c->members_run.range.offset : 0U, c->members_run.count,
                c->proxies_run.count != 0U ? c->proxies_run.range.offset : 0U,
                c->proxies_run.count};
            res = nph_gpu_upload_buffer(r->dev, v->cell_runs, (u64)cell_slot * sizeof rec, &rec,
                                        sizeof rec);
        }
        v->run_dirty[cell_slot] = 0U;
    }
    v->runs_changed_count = 0U;
    if (res == NPH_RESULT_OK && v->loose_dirty) {
        res = run_upload(r, &v->loose);
        v->loose_dirty = 0;
    }
    if (res == NPH_RESULT_OK && any) {
        r->pending_uploads = 1;
    }
    return res;
}

NphResult render_vis_runs_create(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    const NphRendererDesc *d = &r->desc;
    NphAllocator *a = r->alloc;
    // Every run at most twice its count and at least its first range, each
    // cell's two, and the range classes' rounding (an eighth at most) on top.
    const u64 entries = 2ULL * d->max_instances + 2ULL * RUN_FIRST_CAPACITY * d->max_cells;
    const u64 capacity = entries + entries / 8U + RUN_FIRST_CAPACITY;
    if (capacity > (u64)NPH_U32_MAX / sizeof(u32)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    v->member_capacity = (u32)capacity;
    // A cell's two runs and the run of no cell live at once, and as many
    // moving as change in a frame's flight: room for twice that.
    NPH_TRY(
        nph_range_alloc_init(&v->member_ranges, a, v->member_capacity, 4U * d->max_cells + 64U));
    v->member_entries = NPH_ALLOC_ARRAY_ZEROED(a, u32, v->member_capacity);
    v->run_dirty = NPH_ALLOC_ARRAY_ZEROED(a, u8, d->max_cells);
    v->runs_changed = NPH_ALLOC_ARRAY(a, u32, d->max_cells);
    if (v->member_entries == NULL || v->run_dirty == NULL || v->runs_changed == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = (u64)v->member_capacity * sizeof(u32);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC; // read back by the tests
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "vis cell members";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &v->cell_members));
    bd.size = (u64)nph_max_u32(d->max_cells, 1U) * sizeof(NphVisCellRun);
    bd.name = "vis cell runs";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &v->cell_runs));
    // Every instance is in one run and listed once a frame at most.
    bd.size = (u64)d->max_instances * sizeof(u32);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    bd.name = "vis cell work";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &v->cell_work));
    bd.size = (u64)nph_max_u32(d->max_cells, 1U) * 16U;
    bd.name = "vis cells listed";
    NPH_TRY(nph_gpu_buffer_create(r->dev, &bd, &v->cell_listed));
    return NPH_RESULT_OK;
}

void render_vis_runs_destroy(NphRenderer *r)
{
    RenderVis *v = &r->vis;
    NphAllocator *a = r->alloc;
    nph_gpu_buffer_destroy(r->dev, v->cell_members);
    nph_gpu_buffer_destroy(r->dev, v->cell_runs);
    nph_gpu_buffer_destroy(r->dev, v->cell_work);
    nph_gpu_buffer_destroy(r->dev, v->cell_listed);
    nph_range_alloc_destroy(&v->member_ranges);
    if (v->member_entries != NULL) {
        NPH_FREE_ARRAY(a, u32, v->member_entries, v->member_capacity);
    }
    if (v->run_dirty != NULL) {
        NPH_FREE_ARRAY(a, u8, v->run_dirty, r->desc.max_cells);
    }
    if (v->runs_changed != NULL) {
        NPH_FREE_ARRAY(a, u32, v->runs_changed, r->desc.max_cells);
    }
}

// SPDX-License-Identifier: Apache-2.0
// The many lights (render/renderer.h NphRenderManyLight; docs/render.md "Many
// lights"; docs/adr/0073-many-lights.md).
//
// A light's reach is split by a smooth partition of unity at its near range:
// inside, pixels evaluate it themselves, finding it through a hash of the world's
// cells; beyond, the radiance cache's update samples the lights through a tree in
// proportion to what they may deliver, with a shadow ray each, and adds their light
// to the cells' irradiance. Neither depends on how many lights there are, nor on how
// many are in view.
//
// The set is rebuilt whole in the first frame after a light changes: on the main
// thread, and in new buffers, so that the frames in flight keep reading the old ones
// (destroying them waits for those frames). These lights are for what rarely moves.
#include <nepheline/core/log.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/time.h>
#include <nepheline/render/light_tree.h>

#include "render_internal.h"

#include <math.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderManyLightData) == 32U,
                  "NphRenderManyLightData matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderManyView) == 64U, "NphRenderManyView matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderLightTreeNode) == 6U * sizeof(NphVec4),
                  "a tree node is the six vectors many_lights.glsl reads");

// The hash table has this many entries per light (rounded up to a power of two), and
// each light lies in at most eight cells: few entries hold the lights of two cells.
#define MANY_TABLE_PER_LIGHT 16U
#define MANY_TABLE_MIN       1024U
#define MANY_MAX_COUNT       ((1U << (32U - NPH_RENDER_MANY_COUNT_SHIFT)) - 1U)
#define MANY_MAX_FIRST       ((u32)NPH_RENDER_MANY_FIRST_MASK)

NphResult render_many_create(NphRenderer *r)
{
    RenderMany *m = &r->many;
    const u32 cap = r->desc.max_many_lights;
    if (cap == 0U) {
        return NPH_RESULT_OK;
    }
    m->lights = NPH_ALLOC_ARRAY_ZEROED(r->alloc, RenderManyLight, cap);
    if (m->lights == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return NPH_RESULT_OK;
}

static void destroy_buffers(NphRenderer *r)
{
    RenderMany *m = &r->many;
    nph_gpu_buffer_destroy(r->dev, m->records);
    nph_gpu_buffer_destroy(r->dev, m->nodes);
    nph_gpu_buffer_destroy(r->dev, m->table);
    nph_gpu_buffer_destroy(r->dev, m->indices);
    m->records = NPH_GPU_BUFFER_NULL;
    m->nodes = NPH_GPU_BUFFER_NULL;
    m->table = NPH_GPU_BUFFER_NULL;
    m->indices = NPH_GPU_BUFFER_NULL;
}

void render_many_destroy(NphRenderer *r)
{
    RenderMany *m = &r->many;
    if (r->desc.max_many_lights == 0U) {
        return;
    }
    destroy_buffers(r);
    if (m->lights != NULL) {
        NPH_FREE_ARRAY(r->alloc, RenderManyLight, m->lights, r->desc.max_many_lights);
    }
    m->lights = NULL;
}

// ---------------------------------------------------------------------------
// The lights
// ---------------------------------------------------------------------------

static NphResult validate(const NphRenderManyLightDesc *d)
{
    const bool finite = nph_f64_is_finite(d->position.x) && nph_f64_is_finite(d->position.y) &&
                        nph_f64_is_finite(d->position.z) && nph_f32_is_finite(d->intensity) &&
                        nph_f32_is_finite(d->near_range) && nph_f32_is_finite(d->range) &&
                        nph_f32_is_finite(d->color.x) && nph_f32_is_finite(d->color.y) &&
                        nph_f32_is_finite(d->color.z);
    if (!finite || d->intensity < 0.0F || d->near_range <= 0.0F || d->range < 0.0F ||
        d->color.x < 0.0F || d->color.y < 0.0F || d->color.z < 0.0F ||
        (d->range > 0.0F && d->range < d->near_range)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return NPH_RESULT_OK;
}

NphResult nph_render_many_light_create(NphRenderer *r, const NphRenderManyLightDesc *desc,
                                       NphRenderManyLight *out)
{
    NPH_ASSERT(r != NULL && desc != NULL && out != NULL);
    *out = NPH_RENDER_MANY_LIGHT_NULL;
    NPH_TRY(validate(desc));
    RenderMany *m = &r->many;
    if (r->desc.max_many_lights == 0U) {
        return NPH_RESULT_FULL;
    }
    NphHandle h = NPH_HANDLE_NULL;
    if (nph_handle_pool_alloc(&m->pool, &h) != NPH_RESULT_OK) {
        return NPH_RESULT_FULL;
    }
    const u32 slot = nph_handle_pool_index(&m->pool, h);
    m->lights[slot].live = 1;
    m->lights[slot].desc = *desc;
    m->dirty = 1;
    out->value = h.value;
    return NPH_RESULT_OK;
}

static u32 slot_of(const NphRenderer *r, NphRenderManyLight light)
{
    return r->desc.max_many_lights == 0U ? NPH_U32_MAX : render_slot_of(&r->many.pool, light.value);
}

NphResult nph_render_many_light_update(NphRenderer *r, NphRenderManyLight light,
                                       const NphRenderManyLightDesc *desc)
{
    NPH_ASSERT(r != NULL && desc != NULL);
    const u32 slot = slot_of(r, light);
    if (slot == NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(validate(desc));
    r->many.lights[slot].desc = *desc;
    r->many.dirty = 1;
    return NPH_RESULT_OK;
}

NphResult nph_render_many_light_desc(const NphRenderer *r, NphRenderManyLight light,
                                     NphRenderManyLightDesc *out)
{
    NPH_ASSERT(r != NULL && out != NULL);
    const u32 slot = slot_of(r, light);
    if (slot == NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    *out = r->many.lights[slot].desc;
    return NPH_RESULT_OK;
}

void nph_render_many_light_destroy(NphRenderer *r, NphRenderManyLight light)
{
    NPH_ASSERT(r != NULL);
    if (light.value == 0U) {
        return;
    }
    const u32 slot = slot_of(r, light);
    NPH_ASSERT_MSG(slot != NPH_U32_MAX, "stale many-light handle");
    if (slot == NPH_U32_MAX) {
        return;
    }
    RenderMany *m = &r->many;
    memset(&m->lights[slot], 0, sizeof m->lights[slot]);
    const NphHandle h = {light.value};
    (void)nph_handle_pool_release(&m->pool, h);
    m->dirty = 1;
}

bool nph_render_many_light_is_valid(const NphRenderer *r, NphRenderManyLight light)
{
    NPH_ASSERT(r != NULL);
    return slot_of(r, light) != NPH_U32_MAX;
}

// ---------------------------------------------------------------------------
// The set: the records, the tree, and the near lights' table
// ---------------------------------------------------------------------------

static u32 next_power_of_two(u32 v)
{
    u32 p = 1U;
    while (p < v) {
        p <<= 1U;
    }
    return p;
}

// The table entry of the cell at world-relative `at`'s coordinates, and the range of
// cells a sphere covers along an axis.
static i32 cell_of(f64 at, f64 cells_per_metre)
{
    return (i32)floor(at * cells_per_metre);
}

u32 render_many_table_entry(i32 x, i32 y, i32 z, u32 mask)
{
    return NPH_RENDER_MANY_HASH((u32)x, (u32)y, (u32)z) & mask;
}

// Calls `visit` once per table entry the sphere's cells hash to: two of its cells
// that share an entry must not list it twice, or a pixel would light with it twice.
typedef struct EntryWalk {
    u32 entries[8];
    u32 count;
} EntryWalk;

static void sphere_entries(NphVec3 p, f32 radius, f32 cells_per_metre, u32 mask, EntryWalk *out)
{
    const f64 cpm = (f64)cells_per_metre;
    const i32 lo[3] = {cell_of((f64)p.x - (f64)radius, cpm), cell_of((f64)p.y - (f64)radius, cpm),
                       cell_of((f64)p.z - (f64)radius, cpm)};
    const i32 hi[3] = {cell_of((f64)p.x + (f64)radius, cpm), cell_of((f64)p.y + (f64)radius, cpm),
                       cell_of((f64)p.z + (f64)radius, cpm)};
    out->count = 0U;
    for (i32 z = lo[2]; z <= hi[2]; ++z) {
        for (i32 y = lo[1]; y <= hi[1]; ++y) {
            for (i32 x = lo[0]; x <= hi[0]; ++x) {
                const u32 entry = render_many_table_entry(x, y, z, mask);
                bool seen = false;
                for (u32 i = 0; i < out->count; ++i) {
                    seen = seen || out->entries[i] == entry;
                }
                // A cell is twice the greatest near range across, so a sphere meets
                // at most two along an axis.
                if (!seen && out->count < 8U) {
                    out->entries[out->count++] = entry;
                }
            }
        }
    }
}

static NphResult make_buffer(NphRenderer *r, const void *data, u64 size, const char *name,
                             NphGpuBuffer *out)
{
    NphGpuBufferDesc d;
    memset(&d, 0, sizeof d);
    d.size = size != 0U ? size : 16U;
    d.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    d.memory = NPH_GPU_MEMORY_DEVICE;
    d.name = name;
    NPH_TRY(nph_gpu_buffer_create(r->dev, &d, out));
    return size != 0U ? nph_gpu_upload_buffer(r->dev, *out, 0U, data, size) : NPH_RESULT_OK;
}

// What a rebuild allocates, freed together.
typedef struct ManyBuild {
    NphAllocator *alloc;
    u32 live;
    u32 table_size;
    u64 index_cap;
    NphDVec3 *positions;
    f32 *powers;
    NphRenderManyLightData *records;
    u32 *table;
    u32 *cursors; // the next place in each entry's list, as the lists fill
    u32 *indices;
    NphRenderLightTree tree;
} ManyBuild;

static void build_free(ManyBuild *b)
{
    if (b->positions != NULL) {
        NPH_FREE_ARRAY(b->alloc, NphDVec3, b->positions, b->live);
    }
    if (b->powers != NULL) {
        NPH_FREE_ARRAY(b->alloc, f32, b->powers, b->live);
    }
    if (b->records != NULL) {
        NPH_FREE_ARRAY(b->alloc, NphRenderManyLightData, b->records, b->live);
    }
    if (b->table != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->table, b->table_size);
    }
    if (b->cursors != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->cursors, b->table_size);
    }
    if (b->indices != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->indices, b->index_cap);
    }
    nph_render_light_tree_free(&b->tree);
}

// Fills the table from the lights' cells: how many each entry holds, then where in
// `indices` each begins, then the lists.
static void fill_table(ManyBuild *b, f32 cells_per_metre, u32 *out_index_count, u32 *out_dropped)
{
    const u32 mask = b->table_size - 1U;
    memset(b->cursors, 0, (u64)b->table_size * sizeof(u32));
    EntryWalk walk;
    for (u32 i = 0; i < b->live; ++i) {
        const NphRenderManyLightData *l = &b->records[i];
        const NphVec3 at = nph_vec3(l->position.x, l->position.y, l->position.z);
        sphere_entries(at, 1.0F / nph_f32_sqrt(l->position.w), cells_per_metre, mask, &walk);
        for (u32 e = 0; e < walk.count; ++e) {
            b->cursors[walk.entries[e]] += 1U;
        }
    }
    u32 total = 0U;
    u32 dropped = 0U;
    for (u32 e = 0; e < b->table_size; ++e) {
        const u32 wanted = b->cursors[e];
        const u32 n = nph_min_u32(wanted, MANY_MAX_COUNT);
        dropped += wanted - n;
        b->table[e] = total | (n << NPH_RENDER_MANY_COUNT_SHIFT);
        b->cursors[e] = total;
        total += n;
    }
    for (u32 i = 0; i < b->live; ++i) {
        const NphRenderManyLightData *l = &b->records[i];
        const NphVec3 at = nph_vec3(l->position.x, l->position.y, l->position.z);
        sphere_entries(at, 1.0F / nph_f32_sqrt(l->position.w), cells_per_metre, mask, &walk);
        for (u32 e = 0; e < walk.count; ++e) {
            const u32 entry = walk.entries[e];
            const u32 first = b->table[entry] & MANY_MAX_FIRST;
            const u32 n = b->table[entry] >> NPH_RENDER_MANY_COUNT_SHIFT;
            if (b->cursors[entry] - first < n) {
                b->indices[b->cursors[entry]++] = i;
            }
        }
    }
    *out_index_count = total;
    *out_dropped = dropped;
}

static NphResult rebuild(NphRenderer *r)
{
    RenderMany *m = &r->many;
    const u64 began = nph_time_now_ns();
    m->dirty = 0;
    destroy_buffers(r);
    m->count = 0U;
    ManyBuild b;
    memset(&b, 0, sizeof b);
    b.alloc = r->alloc;
    b.live = nph_handle_pool_live(&m->pool);
    if (b.live == 0U) {
        return NPH_RESULT_OK;
    }
    b.positions = NPH_ALLOC_ARRAY(b.alloc, NphDVec3, b.live);
    b.powers = NPH_ALLOC_ARRAY(b.alloc, f32, b.live);
    b.records = NPH_ALLOC_ARRAY(b.alloc, NphRenderManyLightData, b.live);
    b.table_size = next_power_of_two(nph_max_u32(MANY_TABLE_MIN, b.live * MANY_TABLE_PER_LIGHT));
    b.index_cap = (u64)b.live * 8U;
    b.table = NPH_ALLOC_ARRAY(b.alloc, u32, b.table_size);
    b.cursors = NPH_ALLOC_ARRAY(b.alloc, u32, b.table_size);
    b.indices = NPH_ALLOC_ARRAY(b.alloc, u32, b.index_cap);
    NphResult res = NPH_RESULT_OK;
    if (b.positions == NULL || b.powers == NULL || b.records == NULL || b.table == NULL ||
        b.cursors == NULL || b.indices == NULL) {
        res = NPH_RESULT_OUT_OF_MEMORY;
    }
    f32 near_range = 0.0F;
    if (res == NPH_RESULT_OK) {
        u32 at = 0U;
        for (u32 s = 0; s < m->pool.bump; ++s) {
            const RenderManyLight *l = &m->lights[s];
            if (!l->live) {
                continue;
            }
            b.positions[at] = l->desc.position;
            const NphVec3 c = nph_vec3_scale(l->desc.color, l->desc.intensity);
            // What it may deliver at a metre, as the eye weighs it (Rec. 709).
            b.powers[at] = 0.2126F * c.x + 0.7152F * c.y + 0.0722F * c.z;
            near_range = nph_f32_max(near_range, l->desc.near_range);
            ++at;
        }
        NphRenderLightTreeInput in;
        memset(&in, 0, sizeof in);
        in.positions = b.positions;
        in.powers = b.powers;
        in.count = b.live;
        in.nearest = near_range;
        res = nph_render_light_tree_build(&in, b.alloc, &b.tree);
    }
    if (res == NPH_RESULT_OK && b.tree.node_count == 0U) {
        // No light has any power: nothing to draw.
        build_free(&b);
        return NPH_RESULT_OK;
    }
    u32 index_count = 0U;
    u32 dropped = 0U;
    const f32 cells_per_metre = 0.5F / nph_f32_max(near_range, 1e-3F);
    if (res == NPH_RESULT_OK) {
        u32 at = 0U;
        for (u32 s = 0; s < m->pool.bump; ++s) {
            const RenderManyLight *l = &m->lights[s];
            if (!l->live) {
                continue;
            }
            const NphRenderManyLightDesc *d = &l->desc;
            const NphVec3 c = nph_vec3_scale(d->color, d->intensity);
            NphRenderManyLightData *o = &b.records[at];
            o->position = nph_vec4(
                (f32)(d->position.x - b.tree.origin.x), (f32)(d->position.y - b.tree.origin.y),
                (f32)(d->position.z - b.tree.origin.z), 1.0F / (d->near_range * d->near_range));
            o->color =
                nph_vec4(c.x, c.y, c.z, d->range > 0.0F ? 1.0F / (d->range * d->range) : 0.0F);
            ++at;
        }
        fill_table(&b, cells_per_metre, &index_count, &dropped);
        if (dropped != 0U) {
            NPH_LOG_WARN(&NPH_LOG_RENDER,
                         "%s: %u many lights over a table entry's %u: their near fields go unlit",
                         r->name, dropped, MANY_MAX_COUNT);
        }
        NPH_ASSERT(index_count <= MANY_MAX_FIRST);
    }
    if (res == NPH_RESULT_OK) {
        res = make_buffer(r, b.records, (u64)b.live * sizeof(NphRenderManyLightData),
                          "many light records", &m->records);
    }
    if (res == NPH_RESULT_OK) {
        res = make_buffer(r, b.tree.nodes, (u64)b.tree.node_count * sizeof(NphRenderLightTreeNode),
                          "many light tree", &m->nodes);
    }
    if (res == NPH_RESULT_OK) {
        res =
            make_buffer(r, b.table, (u64)b.table_size * sizeof(u32), "many light table", &m->table);
    }
    if (res == NPH_RESULT_OK) {
        res = make_buffer(r, b.indices, (u64)index_count * sizeof(u32), "many light indices",
                          &m->indices);
    }
    if (res == NPH_RESULT_OK) {
        m->count = b.live;
        m->origin = b.tree.origin;
        m->near_range = near_range;
        m->cells_per_metre = cells_per_metre;
        m->table_mask = b.table_size - 1U;
        m->bytes = (u64)b.live * sizeof(NphRenderManyLightData) +
                   (u64)b.tree.node_count * sizeof(NphRenderLightTreeNode) +
                   (u64)b.table_size * sizeof(u32) + (u64)index_count * sizeof(u32);
        NPH_LOG_INFO(&NPH_LOG_RENDER,
                     "%s: %u many lights: near range %.2f m, %u tree nodes, %u table entries, %.2f "
                     "lights a cell's list on average, %.2f MB, built in %.2f ms",
                     r->name, b.live, (f64)near_range, b.tree.node_count, b.table_size,
                     (f64)index_count / (f64)b.table_size, (f64)m->bytes / (1024.0 * 1024.0),
                     (f64)(nph_time_now_ns() - began) / 1e6);
    } else {
        destroy_buffers(r);
    }
    build_free(&b);
    return res;
}

// ---------------------------------------------------------------------------
// A frame
// ---------------------------------------------------------------------------

NphResult render_many_frame_begin(NphRenderer *r, const NphRenderView *view, bool production)
{
    RenderMany *m = &r->many;
    m->frame_active = 0;
    if (r->desc.max_many_lights == 0U) {
        return NPH_RESULT_OK;
    }
    if (m->dirty) {
        NPH_TRY(rebuild(r));
    }
    if (!production || m->count == 0U || view->disable_many_lights) {
        return NPH_RESULT_OK;
    }
    NphRenderManyView *v = render_frame_at(r, r->layout.many_view_offset);
    memset(v, 0, sizeof *v);
    v->records = nph_gpu_buffer_address(r->dev, m->records);
    v->nodes = nph_gpu_buffer_address(r->dev, m->nodes);
    v->table = nph_gpu_buffer_address(r->dev, m->table);
    v->indices = nph_gpu_buffer_address(r->dev, m->indices);
    const NphVec3 to_origin = nph_dvec3_relative(view->camera.position, m->origin);
    v->to_origin = nph_vec4(to_origin.x, to_origin.y, to_origin.z, m->cells_per_metre);
    v->table_mask = m->table_mask;
    v->count = m->count;
    v->near2 = m->near_range * m->near_range;
    m->frame_active = 1;
    return NPH_RESULT_OK;
}

u64 render_many_view_address(const NphRenderer *r)
{
    return r->many.frame_active ? r->frame_addresses[r->frame_slot] + r->layout.many_view_offset
                                : 0U;
}

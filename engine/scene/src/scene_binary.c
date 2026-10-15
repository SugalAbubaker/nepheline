// SPDX-License-Identifier: Apache-2.0
// The binary scene format: a flat snapshot grouped by archetype, columns
// stored contiguously, loaded with one batch spawn and one copy per run of
// rows (scene_io.h; docs/scene.md "Scene files";
// docs/adr/0021-scene-serialization.md).
//
//     header  magic 'NSCB', version
//     HEAD    u32 entity_count, u32 flags
//     COMP    u32 count; per component: str name, u64 layout hash, u32 size
//     ARCH*   u32 flags (bit 0: hierarchy), u32 component_count,
//             u32 component[component_count] (COMP indices, ascending),
//             u32 entity_count, u32 id[entity_count], then 16-aligned, per
//             component with data: entity_count * size bytes
//     STRS    u32 count; str each: the text of the string IDs in the data
//     HIER    u32 count; (u32 child, u32 parent) pairs in file order
//     trailer CRC-32C
//
// Column data is canonical: padding and the bytes after a TEXT terminator
// are zero, TRANSIENT fields hold their defaults, and ENTITY fields hold the
// file id plus one (0: no entity), so equal worlds give equal files.

#include <nepheline/core/assert.h>
#include <nepheline/scene/transform.h>

#include "scene_io_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG(a, b, c, d) ((u32)(a) | ((u32)(b) << 8U) | ((u32)(c) << 16U) | ((u32)(d) << 24U))
#define TAG_HEAD        TAG('H', 'E', 'A', 'D')
#define TAG_COMP        TAG('C', 'O', 'M', 'P')
#define TAG_ARCH        TAG('A', 'R', 'C', 'H')
#define TAG_STRS        TAG('S', 'T', 'R', 'S')
#define TAG_HIER        TAG('H', 'I', 'E', 'R')

#define ARCH_FLAG_HIERARCHY 1U
#define COLUMN_ALIGN        16U

typedef union ValueBuffer {
    u8 bytes[NPH_ECS_MAX_COMPONENT_SIZE];
    u64 align_u64;
    f64 align_f64;
} ValueBuffer;

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

typedef struct BinSaver {
    const NphWorld *w;
    NphSaveSet set;
    NphAllocator *scratch;
    NphBinWriter *out;
    u32 comp_index[NPH_ECS_MAX_COMPONENTS]; // world id -> COMP index, NPH_U32_MAX when unused
    u32 comp_count;
    NphHashMap strings; // string IDs written (u64 -> nothing)
} BinSaver;

// The canonical bytes of a component value (see the file comment).
static void canonical(const BinSaver *s, NphComponentId id, const u8 *src, u8 *dst)
{
    const NphComponentDesc *d = s->w->components[id].desc;
    memset(dst, 0, d->size);
    for (u32 i = 0; i < d->field_count; ++i) {
        const NphFieldDesc *f = &d->fields[i];
        if ((f->flags & NPH_FIELD_FLAG_TRANSIENT) != 0U) {
            if (d->defaults != NULL) {
                memcpy(dst + f->offset, (const u8 *)d->defaults + f->offset, f->size);
            }
            continue;
        }
        if (f->kind == NPH_FIELD_ENTITY) {
            NphEntity e;
            memcpy(&e, src + f->offset, sizeof e);
            const u32 fid = nph_saveset_id_(&s->set, s->w, e);
            const u32 v = fid != NPH_SAVE_NONE ? fid + 1U : 0U;
            memcpy(dst + f->offset, &v, sizeof v);
        } else if (f->kind == NPH_FIELD_TEXT) {
            NphText t;
            memcpy(&t, src + f->offset, sizeof t);
            const NphStr text = nph_text_str(&t);
            memcpy(dst + f->offset, text.data,
                   (size_t)nph_min_u64(text.len, NPH_TEXT_CAPACITY - 1U));
        } else {
            memcpy(dst + f->offset, src + f->offset, f->size);
        }
    }
}

// Records the string IDs of a value for the STRS section.
static NphResult note_strings(BinSaver *s, NphComponentId id, const u8 *value)
{
    const NphComponentDesc *d = s->w->components[id].desc;
    for (u32 i = 0; i < d->field_count; ++i) {
        const NphFieldDesc *f = &d->fields[i];
        if (f->kind != NPH_FIELD_STR_ID || (f->flags & NPH_FIELD_FLAG_TRANSIENT) != 0U) {
            continue;
        }
        NphStrId sid;
        memcpy(&sid, value + f->offset, sizeof sid);
        if (!nph_str_id_is_null(sid) && nph_str_table_lookup(&s->w->strings, sid).data != NULL) {
            NPH_TRY(nph_hashmap_set_u64(&s->strings, sid.value, NULL));
        }
    }
    return NPH_RESULT_OK;
}

static int compare_u64(const void *a, const void *b)
{
    u64 x;
    u64 y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static bool saved_column(const NphWorld *w, NphComponentId id)
{
    return nph_scene_component_saved_(w->components[id].desc);
}

static NphResult write_archetype(BinSaver *s, const NphArchetype *a, const u32 *ids, u32 n)
{
    NphBinWriter *o = s->out;
    NphBinSection sec;
    NPH_TRY(nph_bin_write_section_begin(o, TAG_ARCH, &sec));
    const bool hier = a->column_of[NPH_COMPONENT_ID_HIERARCHY] != NPH_NO_COLUMN;
    NPH_TRY(nph_bin_write_u32(o, hier ? ARCH_FLAG_HIERARCHY : 0U));
    u32 count = 0U;
    for (u32 k = 0; k < a->component_count; ++k) {
        count += saved_column(s->w, a->components[k]) ? 1U : 0U;
    }
    NPH_TRY(nph_bin_write_u32(o, count));
    for (u32 k = 0; k < a->component_count; ++k) {
        if (saved_column(s->w, a->components[k])) {
            NPH_TRY(nph_bin_write_u32(o, s->comp_index[a->components[k]]));
        }
    }
    NPH_TRY(nph_bin_write_u32(o, n));
    for (u32 i = 0; i < n; ++i) {
        NPH_TRY(nph_bin_write_u32(o, ids[i]));
    }
    for (u32 k = 0; k < a->component_count; ++k) {
        const NphComponentId id = a->components[k];
        const u32 size = s->w->components[id].desc->size;
        if (!saved_column(s->w, id) || size == 0U) {
            continue;
        }
        NPH_TRY(nph_bin_write_align(o, COLUMN_ALIGN));
        NPH_TRY(nph_bin_writer_reserve(o, nph_bin_writer_len(o) + (u64)n * size));
        for (u32 i = 0; i < n; ++i) {
            const u8 *src = (const u8 *)nph_world_get(s->w, s->set.order[ids[i]], id);
            ValueBuffer buffer;
            canonical(s, id, src, buffer.bytes);
            NPH_TRY(note_strings(s, id, buffer.bytes));
            NPH_TRY(nph_bin_write_bytes(o, buffer.bytes, size));
        }
    }
    nph_bin_write_section_end(o, &sec);
    return NPH_RESULT_OK;
}

static NphResult save_body(BinSaver *s)
{
    const NphWorld *w = s->w;
    NphBinWriter *o = s->out;
    NPH_TRY(nph_bin_write_header(o, NPH_SCENE_BINARY_MAGIC, NPH_SCENE_BINARY_VERSION));
    NphBinSection sec;
    NPH_TRY(nph_bin_write_section_begin(o, TAG_HEAD, &sec));
    NPH_TRY(nph_bin_write_u32(o, s->set.count));
    NPH_TRY(nph_bin_write_u32(o, 0U));
    nph_bin_write_section_end(o, &sec);

    // Group the saved entities by archetype (a counting sort keeps file
    // order within each group), and number the components that appear.
    const u32 na = w->archetype_count;
    u32 *start = NPH_ALLOC_ARRAY_ZEROED(s->scratch, u32, (u64)na + 1U);
    u32 *grouped = NPH_ALLOC_ARRAY(s->scratch, u32, nph_max_u32(s->set.count, 1U));
    NphResult r = start != NULL && grouped != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 i = 0; i < s->set.count; ++i) {
            start[w->records[nph_world_entity_index(w, s->set.order[i])].chunk->archetype->index +
                  1U] += 1U;
        }
        for (u32 a = 0; a < na; ++a) {
            start[a + 1U] += start[a];
        }
        for (u32 i = 0; i < s->set.count; ++i) {
            const u32 a =
                w->records[nph_world_entity_index(w, s->set.order[i])].chunk->archetype->index;
            grouped[start[a]++] = i;
        }
        // `start` now holds the ends; the groups run [end(a-1), end(a)).
        for (u32 id = 0; id < NPH_ECS_MAX_COMPONENTS; ++id) {
            s->comp_index[id] = NPH_U32_MAX;
        }
        u32 begin = 0U;
        for (u32 a = 0; a < na; ++a) {
            if (start[a] == begin) {
                continue;
            }
            const NphArchetype *arch = w->archetypes[a];
            for (u32 k = 0; k < arch->component_count; ++k) {
                const NphComponentId id = arch->components[k];
                if (saved_column(w, id) && s->comp_index[id] == NPH_U32_MAX) {
                    s->comp_index[id] = 0U; // numbered below, in id order
                }
            }
            begin = start[a];
        }
        for (u32 id = 0; id < w->component_count; ++id) {
            if (s->comp_index[id] != NPH_U32_MAX) {
                s->comp_index[id] = s->comp_count++;
            }
        }
        r = nph_bin_write_section_begin(o, TAG_COMP, &sec);
        r = r == NPH_RESULT_OK ? nph_bin_write_u32(o, s->comp_count) : r;
        for (u32 id = 0; r == NPH_RESULT_OK && id < w->component_count; ++id) {
            if (s->comp_index[id] == NPH_U32_MAX) {
                continue;
            }
            const NphComponentInfo *info = &w->components[id];
            r = nph_bin_write_str(o, nph_str_from_cstr(info->desc->name));
            r = r == NPH_RESULT_OK ? nph_bin_write_u64(o, info->layout_hash) : r;
            r = r == NPH_RESULT_OK ? nph_bin_write_u32(o, info->desc->size) : r;
        }
        if (r == NPH_RESULT_OK) {
            nph_bin_write_section_end(o, &sec);
        }
        begin = 0U;
        for (u32 a = 0; r == NPH_RESULT_OK && a < na; ++a) {
            if (start[a] > begin) {
                r = write_archetype(s, w->archetypes[a], grouped + begin, start[a] - begin);
            }
            begin = start[a];
        }
    }
    if (start != NULL) {
        NPH_FREE_ARRAY(s->scratch, u32, start, (u64)na + 1U);
    }
    if (grouped != NULL) {
        NPH_FREE_ARRAY(s->scratch, u32, grouped, nph_max_u32(s->set.count, 1U));
    }
    NPH_TRY(r);

    // The texts of the string IDs, sorted by ID so the file does not depend
    // on the order they were met in.
    const u32 nstr = nph_hashmap_len(&s->strings);
    u64 *sids = NPH_ALLOC_ARRAY(s->scratch, u64, nph_max_u32(nstr, 1U));
    if (sids == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphHashMapIter it = nph_hashmap_iter(&s->strings);
    const void *key;
    void *val;
    u32 k = 0U;
    while (nph_hashmap_next(&it, &key, &val)) {
        memcpy(&sids[k++], key, sizeof sids[0]);
    }
    qsort(sids, nstr, sizeof sids[0], compare_u64);
    r = nph_bin_write_section_begin(o, TAG_STRS, &sec);
    r = r == NPH_RESULT_OK ? nph_bin_write_u32(o, nstr) : r;
    for (u32 i = 0; r == NPH_RESULT_OK && i < nstr; ++i) {
        const NphStrId sid = {sids[i]};
        r = nph_bin_write_str(o, nph_str_table_lookup(&w->strings, sid));
    }
    NPH_FREE_ARRAY(s->scratch, u64, sids, nph_max_u32(nstr, 1U));
    NPH_TRY(r);
    nph_bin_write_section_end(o, &sec);

    NPH_TRY(nph_bin_write_section_begin(o, TAG_HIER, &sec));
    const u64 count_at = nph_bin_writer_len(o);
    NPH_TRY(nph_bin_write_u32(o, 0U));
    u32 pairs = 0U;
    for (u32 i = 0; i < s->set.count; ++i) {
        const u32 parent = nph_saveset_id_(&s->set, w, nph_world_parent(w, s->set.order[i]));
        if (parent != NPH_SAVE_NONE) {
            NPH_TRY(nph_bin_write_u32(o, i));
            NPH_TRY(nph_bin_write_u32(o, parent));
            pairs += 1U;
        }
    }
    nph_bin_patch_u32(o, count_at, pairs);
    nph_bin_write_section_end(o, &sec);
    return nph_bin_write_checksum(o);
}

NphResult nph_scene_save_binary(const NphWorld *w, const NphSceneSaveDesc *desc, NphBinWriter *out,
                                NphSceneResult *result)
{
    NPH_ASSERT(w != NULL && out != NULL);
    if (result != NULL) {
        memset(result, 0, sizeof *result);
    }
    NphSceneSaveDesc d;
    memset(&d, 0, sizeof d);
    if (desc != NULL) {
        d = *desc;
    }
    NphAllocator *scratch = d.scratch != NULL ? d.scratch : nph_allocator_system();
    BinSaver *s = NPH_ALLOC_TYPE(scratch, BinSaver);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    s->w = w;
    s->scratch = scratch;
    s->out = out;
    const NphHashMapDesc md = nph_hashmap_desc_u64(0U);
    NphResult r = nph_hashmap_init(&s->strings, scratch, &md);
    if (r == NPH_RESULT_OK) {
        r = nph_saveset_build_(w, d.entities, d.entities != NULL ? d.entity_count : 0U, scratch,
                               &s->set);
        if (r == NPH_RESULT_OK) {
            const u64 start = nph_bin_writer_len(out);
            r = save_body(s);
            if (r != NPH_RESULT_OK) {
                out->len = start;
            } else if (result != NULL) {
                result->entity_count = s->set.count;
            }
            nph_saveset_free_(&s->set);
        }
        nph_hashmap_free(&s->strings);
    }
    if (r != NPH_RESULT_OK) {
        nph_scene_error_(result, "saving failed: %s", nph_result_string(r));
    }
    NPH_FREE_TYPE(scratch, BinSaver, s);
    return r;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

typedef struct GroupIn {
    u32 flags;
    u32 component_count;
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS]; // world ids
    u32 entity_count;
    const u8 *ids_data;                        // entity_count u32 file ids
    const u8 *columns[NPH_ECS_MAX_COMPONENTS]; // per component with data, else NULL
} GroupIn;

typedef struct BinLoader {
    NphWorld *w;
    NphAllocator *scratch;
    NphSceneResult *result;
    u32 n; // entities in the file
    NphEntity *created;
    u8 *assigned;                                 // per file id: seen in a group
    NphComponentId table[NPH_ECS_MAX_COMPONENTS]; // COMP index -> world id
    u32 table_count;
    bool have_table;
    GroupIn *groups;
    u32 group_count;
    u32 group_cap;
    const u8 *hier;
    u32 hier_count;
    bool reserved; // `created` holds reserved handles
} BinLoader;

NPH_PRINTF_LIKE(3, 4)
static NphResult bfail(BinLoader *l, NphResult r, const char *fmt, ...)
{
    if (l->result != NULL) {
        va_list args;
        va_start(args, fmt);
        (void)vsnprintf(l->result->error, sizeof l->result->error, fmt, args);
        va_end(args);
    }
    return r;
}

static u32 read_u32_at(const u8 *p, u32 i)
{
    u32 v;
    memcpy(&v, p + 4U * (u64)i, sizeof v);
    return v;
}

static NphResult parse_comp(BinLoader *l, NphBinReader *r)
{
    u32 count;
    NPH_TRY(nph_bin_read_u32(r, &count));
    if (count > NPH_ECS_MAX_COMPONENTS || l->have_table) {
        return bfail(l, NPH_RESULT_CORRUPT, "malformed component table");
    }
    for (u32 i = 0; i < count; ++i) {
        NphStr name;
        u64 layout;
        u32 size;
        if (nph_bin_read_str(r, &name) != NPH_RESULT_OK ||
            nph_bin_read_u64(r, &layout) != NPH_RESULT_OK ||
            nph_bin_read_u32(r, &size) != NPH_RESULT_OK) {
            return bfail(l, NPH_RESULT_CORRUPT, "truncated component table");
        }
        const NphComponentId id = nph_world_find_component(l->w, name);
        if (id == NPH_COMPONENT_NONE) {
            return bfail(l, NPH_RESULT_NOT_FOUND, "the world has no component '%.*s'",
                         NPH_STR_ARG(nph_str_prefix(name, 64U)));
        }
        const NphComponentInfo *info = &l->w->components[id];
        if (!nph_scene_component_saved_(info->desc)) {
            return bfail(l, NPH_RESULT_CORRUPT, "component '%s' cannot appear in a scene",
                         info->desc->name);
        }
        if (info->layout_hash != layout || info->desc->size != size) {
            return bfail(l, NPH_RESULT_VERSION_MISMATCH,
                         "component '%s' was cooked with another layout; recook the scene",
                         info->desc->name);
        }
        for (u32 k = 0; k < i; ++k) {
            if (l->table[k] == id) {
                return bfail(l, NPH_RESULT_CORRUPT, "component '%s' is listed twice",
                             info->desc->name);
            }
        }
        l->table[i] = id;
    }
    l->table_count = count;
    l->have_table = true;
    return NPH_RESULT_OK;
}

static NphResult parse_arch(BinLoader *l, NphBinReader *r)
{
    if (!l->have_table) {
        return bfail(l, NPH_RESULT_CORRUPT, "an archetype before the component table");
    }
    if (l->group_count == l->group_cap) {
        const u32 cap = l->group_cap == 0U ? 8U : l->group_cap * 2U;
        GroupIn *g = (GroupIn *)NPH_REALLOC(l->scratch, l->groups, sizeof(GroupIn) * l->group_cap,
                                            sizeof(GroupIn) * cap, NPH_ALIGNOF(GroupIn));
        if (g == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        l->groups = g;
        l->group_cap = cap;
    }
    GroupIn *g = &l->groups[l->group_count];
    memset(g, 0, sizeof *g);
    if (nph_bin_read_u32(r, &g->flags) != NPH_RESULT_OK ||
        nph_bin_read_u32(r, &g->component_count) != NPH_RESULT_OK ||
        g->component_count > l->table_count || (g->flags & ~ARCH_FLAG_HIERARCHY) != 0U) {
        return bfail(l, NPH_RESULT_CORRUPT, "malformed archetype");
    }
    u32 prev = NPH_U32_MAX;
    for (u32 k = 0; k < g->component_count; ++k) {
        u32 index;
        if (nph_bin_read_u32(r, &index) != NPH_RESULT_OK || index >= l->table_count ||
            (prev != NPH_U32_MAX && index <= prev)) {
            return bfail(l, NPH_RESULT_CORRUPT, "malformed archetype component list");
        }
        prev = index;
        g->ids[k] = l->table[index];
    }
    if (nph_bin_read_u32(r, &g->entity_count) != NPH_RESULT_OK || g->entity_count == 0U ||
        g->entity_count > l->n || nph_bin_reader_remaining(r) / 4U < g->entity_count) {
        return bfail(l, NPH_RESULT_CORRUPT, "malformed archetype entity list");
    }
    g->ids_data = r->data + r->pos;
    NPH_TRY(nph_bin_read_skip(r, 4U * (u64)g->entity_count));
    for (u32 i = 0; i < g->entity_count; ++i) {
        const u32 fid = read_u32_at(g->ids_data, i);
        if (fid >= l->n || l->assigned[fid] != 0U) {
            return bfail(l, NPH_RESULT_CORRUPT, "entity id %u is out of range or repeated", fid);
        }
        l->assigned[fid] = 1U;
    }
    for (u32 k = 0; k < g->component_count; ++k) {
        const u64 size = l->w->components[g->ids[k]].desc->size;
        if (size == 0U) {
            continue;
        }
        if (nph_bin_read_align(r, COLUMN_ALIGN) != NPH_RESULT_OK ||
            nph_bin_reader_remaining(r) / size < g->entity_count) {
            return bfail(l, NPH_RESULT_CORRUPT, "truncated column");
        }
        g->columns[k] = r->data + r->pos;
        NPH_TRY(nph_bin_read_skip(r, size * g->entity_count));
    }
    l->group_count += 1U;
    return NPH_RESULT_OK;
}

static NphResult parse_strs(BinLoader *l, NphBinReader *r)
{
    u32 count;
    NPH_TRY(nph_bin_read_u32(r, &count));
    for (u32 i = 0; i < count; ++i) {
        NphStr s;
        if (nph_bin_read_str(r, &s) != NPH_RESULT_OK) {
            return bfail(l, NPH_RESULT_CORRUPT, "truncated string table");
        }
        if (!nph_utf8_is_valid(s)) {
            return bfail(l, NPH_RESULT_CORRUPT, "a string is not UTF-8");
        }
        NphStrId id;
        const NphResult res = nph_str_table_intern(&l->w->strings, s, &id);
        if (res != NPH_RESULT_OK) {
            return bfail(l, res, "string table: %s", nph_result_string(res));
        }
    }
    return NPH_RESULT_OK;
}

static NphResult parse_hier(BinLoader *l, NphBinReader *r)
{
    u32 count;
    NPH_TRY(nph_bin_read_u32(r, &count));
    if (nph_bin_reader_remaining(r) / 8U < count) {
        return bfail(l, NPH_RESULT_CORRUPT, "truncated hierarchy");
    }
    l->hier = r->data + r->pos;
    l->hier_count = count;
    for (u32 i = 0; i < count; ++i) {
        const u32 child = read_u32_at(l->hier, 2U * i);
        const u32 parent = read_u32_at(l->hier, 2U * i + 1U);
        if (child >= l->n || parent >= l->n || child == parent) {
            return bfail(l, NPH_RESULT_CORRUPT, "malformed hierarchy pair");
        }
    }
    return nph_bin_read_skip(r, 8U * (u64)count);
}

static NphEntity map_file_entity(void *user, NphEntity e)
{
    const BinLoader *l = (const BinLoader *)user;
    // Validated before the remap: 0 or a file id + 1.
    return e.value == 0U ? NPH_ENTITY_NULL : l->created[e.value - 1U];
}

// A TEXT field as the text loader accepts it: terminated within the
// capacity, zero after the terminator, valid UTF-8.
static bool text_valid(const u8 *p)
{
    u32 len = 0U;
    while (len < NPH_TEXT_CAPACITY && p[len] != 0U) {
        ++len;
    }
    if (len == NPH_TEXT_CAPACITY) {
        return false;
    }
    for (u32 i = len; i < NPH_TEXT_CAPACITY; ++i) {
        if (p[i] != 0U) {
            return false;
        }
    }
    return nph_utf8_is_valid(nph_str((const char *)p, len));
}

static bool text_fields_valid(const NphComponentDesc *d, const u8 *column, u32 count)
{
    for (u32 k = 0; k < d->field_count; ++k) {
        if (d->fields[k].kind != NPH_FIELD_TEXT) {
            continue;
        }
        for (u32 i = 0; i < count; ++i) {
            if (!text_valid(column + (u64)i * d->size + d->fields[k].offset)) {
                return false;
            }
        }
    }
    return true;
}

// Checks every ENTITY field of a column before anything is copied.
static bool entity_fields_valid(const BinLoader *l, const NphComponentInfo *info, const u8 *column,
                                u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        for (u32 k = 0; k < info->entity_field_count; ++k) {
            u32 v;
            memcpy(&v, column + (u64)i * info->desc->size + info->entity_field_offsets[k],
                   sizeof v);
            if (v > l->n) {
                return false;
            }
        }
    }
    return true;
}

// Creates every entity: handles are reserved in file order first, so the
// loaded world's slots follow the file (and a text save of it matches one
// of the world that was saved), then each group is placed in its
// archetype with one batch.
static NphResult spawn_groups(BinLoader *l)
{
    NphWorld *w = l->w;
    for (u32 gi = 0; gi < l->group_count; ++gi) {
        const GroupIn *g = &l->groups[gi];
        for (u32 k = 0; k < g->component_count; ++k) {
            const NphComponentInfo *info = &w->components[g->ids[k]];
            if (g->columns[k] != NULL && info->entity_field_count > 0U &&
                !entity_fields_valid(l, info, g->columns[k], g->entity_count)) {
                return bfail(l, NPH_RESULT_CORRUPT, "an entity reference is out of range");
            }
            if (g->columns[k] != NULL &&
                !text_fields_valid(info->desc, g->columns[k], g->entity_count)) {
                return bfail(l, NPH_RESULT_CORRUPT, "a text field is not canonical UTF-8");
            }
        }
    }
    NphResult res = nph_world_reserve_(w, l->n, l->created);
    if (res != NPH_RESULT_OK) {
        memset(l->created, 0, sizeof(NphEntity) * (u64)nph_max_u32(l->n, 1U));
        return bfail(l, res, "reserving %u entities: %s", l->n, nph_result_string(res));
    }
    l->reserved = true;
    NphEntity *batch = NULL;
    u32 batch_cap = 0U;
    for (u32 gi = 0; res == NPH_RESULT_OK && gi < l->group_count; ++gi) {
        const GroupIn *g = &l->groups[gi];
        NphComponentId ids[NPH_ECS_MAX_COMPONENTS + 1U];
        memcpy(ids, g->ids, sizeof(NphComponentId) * g->component_count);
        u32 count = g->component_count;
        if ((g->flags & ARCH_FLAG_HIERARCHY) != 0U) {
            ids[count++] = NPH_COMPONENT_ID_HIERARCHY;
        }
        if (g->entity_count > batch_cap) {
            if (batch != NULL) {
                NPH_FREE_ARRAY(l->scratch, NphEntity, batch, batch_cap);
            }
            batch = NPH_ALLOC_ARRAY(l->scratch, NphEntity, g->entity_count);
            batch_cap = batch != NULL ? g->entity_count : 0U;
            if (batch == NULL) {
                res = NPH_RESULT_OUT_OF_MEMORY;
                break;
            }
        }
        for (u32 i = 0; i < g->entity_count; ++i) {
            batch[i] = l->created[read_u32_at(g->ids_data, i)];
        }
        res = nph_world_place_(w, ids, count, g->entity_count, batch);
        if (res != NPH_RESULT_OK) {
            res = bfail(l, res, "placing %u entities: %s", g->entity_count, nph_result_string(res));
            break;
        }
    }
    if (batch != NULL) {
        NPH_FREE_ARRAY(l->scratch, NphEntity, batch, batch_cap);
    }
    return res;
}

// Copies the columns of every group into the spawned rows. Rows of one
// group are consecutive within each chunk (a batch spawn fills them in
// order), so the copy goes by runs.
static void fill_groups(BinLoader *l)
{
    NphWorld *w = l->w;
    const u32 tick = nph_world_change_tick_(w);
    for (u32 gi = 0; gi < l->group_count; ++gi) {
        const GroupIn *g = &l->groups[gi];
        u32 i = 0U;
        while (i < g->entity_count) {
            const NphEntityRecord *first =
                nph_world_record_(w, l->created[read_u32_at(g->ids_data, i)]);
            u32 run = 1U;
            while (i + run < g->entity_count) {
                const NphEntityRecord *next =
                    nph_world_record_(w, l->created[read_u32_at(g->ids_data, i + run)]);
                if (next->chunk != first->chunk || next->row != first->row + run) {
                    break;
                }
                run += 1U;
            }
            NphChunk *c = first->chunk;
            for (u32 k = 0; k < g->component_count; ++k) {
                if (g->columns[k] == NULL) {
                    continue;
                }
                const NphComponentInfo *info = &w->components[g->ids[k]];
                const u32 col = c->archetype->column_of[g->ids[k]];
                const u64 size = info->desc->size;
                u8 *dst = nph_chunk_cell(c, col, first->row);
                memcpy(dst, g->columns[k] + (u64)i * size, (size_t)(size * run));
                if (info->entity_field_count > 0U) {
                    for (u32 j = 0; j < run; ++j) {
                        nph_component_remap_entities_(info, dst + (u64)j * size, map_file_entity,
                                                      l);
                    }
                }
                nph_chunk_stamp(c, col, tick);
            }
            i += run;
        }
    }
}

static NphResult load_body(BinLoader *l, const void *data, u64 size)
{
    u64 payload = 0U;
    if (nph_bin_verify_checksum(data, size, &payload) != NPH_RESULT_OK) {
        return bfail(l, NPH_RESULT_CORRUPT, "the checksum does not match");
    }
    NphBinReader r = nph_bin_reader(data, payload);
    u32 version = 0U;
    const NphResult hr = nph_bin_read_header(&r, NPH_SCENE_BINARY_MAGIC, NPH_SCENE_BINARY_VERSION,
                                             NPH_SCENE_BINARY_VERSION, &version);
    if (hr == NPH_RESULT_VERSION_MISMATCH) {
        return bfail(l, hr, "binary scene version %u, expected %u", version,
                     NPH_SCENE_BINARY_VERSION);
    }
    if (hr != NPH_RESULT_OK) {
        return bfail(l, NPH_RESULT_CORRUPT, "not a binary scene");
    }
    bool have_head = false;
    while (!nph_bin_reader_at_end(&r)) {
        u32 tag;
        u32 len;
        if (nph_bin_read_section(&r, &tag, &len) != NPH_RESULT_OK) {
            return bfail(l, NPH_RESULT_CORRUPT, "malformed section");
        }
        NphBinReader sr = nph_bin_reader(r.data, r.pos + len);
        sr.pos = r.pos;
        NphResult res = NPH_RESULT_OK;
        if (tag == TAG_HEAD) {
            u32 flags;
            if (have_head || nph_bin_read_u32(&sr, &l->n) != NPH_RESULT_OK ||
                nph_bin_read_u32(&sr, &flags) != NPH_RESULT_OK || flags != 0U) {
                return bfail(l, NPH_RESULT_CORRUPT, "malformed header section");
            }
            if ((u64)nph_world_entity_count(l->w) + l->n > nph_world_entity_capacity(l->w)) {
                return bfail(l, NPH_RESULT_FULL, "%u entities do not fit the world", l->n);
            }
            l->created = NPH_ALLOC_ARRAY_ZEROED(l->scratch, NphEntity, nph_max_u32(l->n, 1U));
            l->assigned = NPH_ALLOC_ARRAY_ZEROED(l->scratch, u8, nph_max_u32(l->n, 1U));
            if (l->created == NULL || l->assigned == NULL) {
                return NPH_RESULT_OUT_OF_MEMORY;
            }
            have_head = true;
        } else if (!have_head) {
            return bfail(l, NPH_RESULT_CORRUPT, "the header section must come first");
        } else if (tag == TAG_COMP) {
            res = parse_comp(l, &sr);
        } else if (tag == TAG_ARCH) {
            res = parse_arch(l, &sr);
        } else if (tag == TAG_STRS) {
            res = parse_strs(l, &sr);
        } else if (tag == TAG_HIER) {
            res = l->hier != NULL ? bfail(l, NPH_RESULT_CORRUPT, "two hierarchy sections")
                                  : parse_hier(l, &sr);
        }
        // Unknown sections are skipped: newer writers, older readers.
        if (res != NPH_RESULT_OK) {
            return res == NPH_RESULT_END_OF_STREAM
                       ? bfail(l, NPH_RESULT_CORRUPT, "truncated section")
                       : res;
        }
        NPH_TRY(nph_bin_read_skip(&r, len));
    }
    if (!have_head) {
        return bfail(l, NPH_RESULT_CORRUPT, "no header section");
    }
    for (u32 i = 0; i < l->n; ++i) {
        if (l->assigned[i] == 0U) {
            return bfail(l, NPH_RESULT_CORRUPT, "entity %u belongs to no archetype", i);
        }
    }
    NPH_TRY(spawn_groups(l));
    fill_groups(l);
    for (u32 i = 0; i < l->hier_count; ++i) {
        const u32 child = read_u32_at(l->hier, 2U * i);
        const u32 parent = read_u32_at(l->hier, 2U * i + 1U);
        const NphResult res = nph_world_set_parent(l->w, l->created[child], l->created[parent]);
        if (res != NPH_RESULT_OK) {
            return bfail(l, res == NPH_RESULT_INVALID_ARGUMENT ? NPH_RESULT_CORRUPT : res,
                         "hierarchy pair %u: %s", i, nph_result_string(res));
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_scene_load_binary(NphWorld *w, const void *data, u64 size,
                                const NphSceneLoadDesc *desc, NphSceneResult *result)
{
    NPH_ASSERT(w != NULL && (data != NULL || size == 0U));
    if (result != NULL) {
        memset(result, 0, sizeof *result);
    }
    NphSceneLoadDesc d;
    memset(&d, 0, sizeof d);
    if (desc != NULL) {
        d = *desc;
    }
    BinLoader *l =
        NPH_ALLOC_TYPE(d.scratch != NULL ? d.scratch : nph_allocator_system(), BinLoader);
    if (l == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    l->w = w;
    l->scratch = d.scratch != NULL ? d.scratch : nph_allocator_system();
    l->result = result;
    const NphResult r = load_body(l, data, size);
    if (r != NPH_RESULT_OK) {
        if (result != NULL && result->error[0] == '\0') {
            nph_scene_error_(result, "loading failed: %s", nph_result_string(r));
        }
        for (u32 i = l->n; l->reserved && i > 0U; --i) {
            if (nph_world_is_alive(w, l->created[i - 1U])) {
                NPH_IGNORE_RESULT(nph_world_despawn(w, l->created[i - 1U]));
            }
        }
        if (l->reserved) {
            nph_world_unreserve_(w, l->created, l->n);
        }
    } else {
        if (d.out_entities != NULL && l->n > 0U) {
            memcpy(d.out_entities, l->created,
                   sizeof(NphEntity) * nph_min_u32(l->n, d.out_capacity));
        }
        if (result != NULL) {
            result->entity_count = l->n;
        }
    }
    NphAllocator *scratch = l->scratch;
    if (l->created != NULL) {
        NPH_FREE_ARRAY(scratch, NphEntity, l->created, nph_max_u32(l->n, 1U));
    }
    if (l->assigned != NULL) {
        NPH_FREE_ARRAY(scratch, u8, l->assigned, nph_max_u32(l->n, 1U));
    }
    if (l->groups != NULL) {
        NPH_FREE_ARRAY(scratch, GroupIn, l->groups, l->group_cap);
    }
    NPH_FREE_TYPE(scratch, BinLoader, l);
    return r;
}

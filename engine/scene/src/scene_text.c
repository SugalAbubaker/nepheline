// SPDX-License-Identifier: Apache-2.0
// The text scene format: JSON written from and read into component
// descriptors, with prefab instances as differences (scene_io.h;
// docs/scene.md "Scene files"; docs/adr/0021-scene-serialization.md).

#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>
#include <nepheline/scene/transform.h>

#include "scene_io_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Values are staged in a buffer aligned for any field.
typedef union ValueBuffer {
    u8 bytes[NPH_ECS_MAX_COMPONENT_SIZE];
    u64 align_u64;
    f64 align_f64;
} ValueBuffer;

#define ALL_FIELDS NPH_U64_MAX

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------

// Component ids in name order: the order components are written in, so a
// file does not depend on registration order.
static u32 sorted_components(const NphWorld *w, NphComponentId *out)
{
    const u32 n = w->component_count;
    for (u32 i = 0; i < n; ++i) {
        out[i] = i;
    }
    for (u32 i = 1; i < n; ++i) {
        const NphComponentId id = out[i];
        u32 j = i;
        while (j > 0U &&
               strcmp(w->components[out[j - 1U]].desc->name, w->components[id].desc->name) > 0) {
            out[j] = out[j - 1U];
            --j;
        }
        out[j] = id;
    }
    return n;
}

// The rank of `e` among the live entities of a prefab's world (its
// instance index), or NPH_SAVE_NONE.
static u32 prefab_rank(const NphWorld *pw, NphEntity e)
{
    if (!nph_world_is_alive(pw, e)) {
        return NPH_SAVE_NONE;
    }
    const u32 slot = nph_world_entity_index(pw, e);
    u32 rank = 0U;
    for (u32 i = 0; i < slot; ++i) {
        rank += pw->records[i].chunk != NULL ? 1U : 0U;
    }
    return rank;
}

// Components a file names directly: saved ones except PrefabInstance,
// which the "prefab" member carries.
static bool listed_component(const NphWorld *w, NphComponentId id)
{
    return id != NPH_COMPONENT_ID_PREFAB_INSTANCE &&
           nph_scene_component_saved_(w->components[id].desc);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

typedef struct Saver {
    const NphWorld *w;
    NphSaveSet set;
    NphJsonWriter jw;
    NphPrefabResolveFn resolve;
    void *resolve_user;
    NphComponentId sorted[NPH_ECS_MAX_COMPONENTS];
    u32 sorted_count;
} Saver;

static NphResult write_float(NphJsonWriter *jw, f64 v, bool single)
{
    if (isnan(v)) {
        return nph_json_writer_str(jw, NPH_STR("nan"));
    }
    if (isinf(v)) {
        return nph_json_writer_str(jw, v > 0.0 ? NPH_STR("inf") : NPH_STR("-inf"));
    }
    return single ? nph_json_writer_f32(jw, (f32)v) : nph_json_writer_f64(jw, v);
}

static NphResult write_f32s(NphJsonWriter *jw, const u8 *p, u32 count)
{
    NPH_TRY(nph_json_writer_begin_array(jw));
    for (u32 i = 0; i < count; ++i) {
        f32 v;
        memcpy(&v, p + 4U * i, sizeof v);
        NPH_TRY(write_float(jw, (f64)v, true));
    }
    return nph_json_writer_end_array(jw);
}

static NphResult write_str_id(const Saver *s, NphJsonWriter *jw, NphStrId id)
{
    if (nph_str_id_is_null(id)) {
        return nph_json_writer_null(jw);
    }
    const NphStr text = nph_str_table_lookup(&s->w->strings, id);
    if (text.data != NULL) {
        return nph_json_writer_str(jw, text);
    }
    // An ID whose text the world does not know: the raw hash.
    char buffer[24];
    const int n = snprintf(buffer, sizeof buffer, "%016llx", (unsigned long long)id.value);
    NPH_TRY(nph_json_writer_begin_object(jw));
    NPH_TRY(nph_json_writer_key(jw, NPH_STR("hash")));
    NPH_TRY(nph_json_writer_str(jw, nph_str(buffer, (u64)n)));
    return nph_json_writer_end_object(jw);
}

static NphResult write_value(Saver *s, const NphFieldDesc *f, const u8 *p)
{
    NphJsonWriter *jw = &s->jw;
    switch ((NphFieldKind)f->kind) {
    case NPH_FIELD_BOOL: {
        b32 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_bool(jw, v != 0);
    }
    case NPH_FIELD_U8:
        return nph_json_writer_i64(jw, (i64)*p);
    case NPH_FIELD_U16: {
        u16 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_i64(jw, (i64)v);
    }
    case NPH_FIELD_U32: {
        u32 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_i64(jw, (i64)v);
    }
    case NPH_FIELD_U64: {
        u64 v;
        memcpy(&v, p, sizeof v);
        if (v <= (u64)NPH_I64_MAX) {
            return nph_json_writer_i64(jw, (i64)v);
        }
        // Beyond what a JSON integer reader holds: decimal text.
        char buffer[24];
        const int n = snprintf(buffer, sizeof buffer, "%llu", (unsigned long long)v);
        return nph_json_writer_str(jw, nph_str(buffer, (u64)n));
    }
    case NPH_FIELD_I8: {
        i8 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_i64(jw, (i64)v);
    }
    case NPH_FIELD_I16: {
        i16 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_i64(jw, (i64)v);
    }
    case NPH_FIELD_I32: {
        i32 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_i64(jw, (i64)v);
    }
    case NPH_FIELD_I64: {
        i64 v;
        memcpy(&v, p, sizeof v);
        return nph_json_writer_i64(jw, v);
    }
    case NPH_FIELD_F32: {
        f32 v;
        memcpy(&v, p, sizeof v);
        return write_float(jw, (f64)v, true);
    }
    case NPH_FIELD_F64: {
        f64 v;
        memcpy(&v, p, sizeof v);
        return write_float(jw, v, false);
    }
    case NPH_FIELD_VEC2:
        return write_f32s(jw, p, 2U);
    case NPH_FIELD_VEC3:
        return write_f32s(jw, p, 3U);
    case NPH_FIELD_VEC4:
    case NPH_FIELD_QUAT:
        return write_f32s(jw, p, 4U);
    case NPH_FIELD_DVEC3: {
        NPH_TRY(nph_json_writer_begin_array(jw));
        for (u32 i = 0; i < 3U; ++i) {
            f64 v;
            memcpy(&v, p + 8U * i, sizeof v);
            NPH_TRY(write_float(jw, v, false));
        }
        return nph_json_writer_end_array(jw);
    }
    case NPH_FIELD_ENTITY: {
        NphEntity e;
        memcpy(&e, p, sizeof e);
        const u32 id = nph_saveset_id_(&s->set, s->w, e);
        return id != NPH_SAVE_NONE ? nph_json_writer_i64(jw, (i64)id) : nph_json_writer_null(jw);
    }
    case NPH_FIELD_STR_ID: {
        NphStrId id;
        memcpy(&id, p, sizeof id);
        return write_str_id(s, jw, id);
    }
    case NPH_FIELD_TEXT: {
        NphText t;
        memcpy(&t, p, sizeof t);
        return nph_json_writer_str(jw, nph_text_str(&t));
    }
    case NPH_FIELD_KIND_COUNT:
        break;
    }
    NPH_UNREACHABLE();
    return NPH_RESULT_CORRUPT;
}

// Writes `"Name": {field: value, ...}`: the saved fields whose bit is set in
// `mask` (ALL_FIELDS for a whole component).
static NphResult write_component(Saver *s, NphComponentId id, const u8 *value, u64 mask)
{
    const NphComponentDesc *d = s->w->components[id].desc;
    NPH_TRY(nph_json_writer_key(&s->jw, nph_str_from_cstr(d->name)));
    NPH_TRY(nph_json_writer_begin_object(&s->jw));
    for (u32 i = 0; i < d->field_count; ++i) {
        const NphFieldDesc *f = &d->fields[i];
        if ((f->flags & NPH_FIELD_FLAG_TRANSIENT) != 0U || ((mask >> i) & 1U) == 0U) {
            continue;
        }
        NPH_TRY(nph_json_writer_key(&s->jw, nph_str_from_cstr(f->name)));
        NPH_TRY(write_value(s, f, value + f->offset));
    }
    return nph_json_writer_end_object(&s->jw);
}

static NphResult write_full(Saver *s, NphEntity e)
{
    bool any = false;
    for (u32 k = 0; k < s->sorted_count && !any; ++k) {
        any = listed_component(s->w, s->sorted[k]) && nph_world_has(s->w, e, s->sorted[k]);
    }
    if (!any) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(nph_json_writer_key(&s->jw, NPH_STR("components")));
    NPH_TRY(nph_json_writer_begin_object(&s->jw));
    for (u32 k = 0; k < s->sorted_count; ++k) {
        const NphComponentId id = s->sorted[k];
        if (!listed_component(s->w, id) || !nph_world_has(s->w, e, id)) {
            continue;
        }
        static const u8 no_data = 0U;
        const u8 *value = (const u8 *)nph_world_get(s->w, e, id);
        NPH_TRY(write_component(s, id, value != NULL ? value : &no_data, ALL_FIELDS));
    }
    return nph_json_writer_end_object(&s->jw);
}

typedef struct InstanceCtx {
    const NphWorld *pw; // the prefab's world
    NphEntity pe;       // the prefab entity this instance entity came from
    NphEntity root;     // the instance's root (world entity)
} InstanceCtx;

// The instance index of `e` when it belongs to the instance rooted at
// `root`, else NPH_SAVE_NONE.
static u32 instance_index(const NphWorld *w, NphEntity e, NphEntity root)
{
    const NphPrefabInstance *pi =
        NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_PREFAB_INSTANCE, NphPrefabInstance);
    return pi != NULL && nph_entity_equals(pi->root, root) ? pi->index : NPH_SAVE_NONE;
}

// Bit i set when field i of the instance's value differs from the prefab's.
// ENTITY fields compare by position inside the instance.
static u64 diff_fields(const NphWorld *w, const InstanceCtx *ic, NphComponentId id, const u8 *inst,
                       const u8 *pref)
{
    const NphComponentDesc *d = w->components[id].desc;
    u64 mask = 0U;
    for (u32 i = 0; i < d->field_count; ++i) {
        const NphFieldDesc *f = &d->fields[i];
        if ((f->flags & NPH_FIELD_FLAG_TRANSIENT) != 0U) {
            continue;
        }
        bool same;
        if (f->kind == NPH_FIELD_ENTITY) {
            NphEntity ie;
            NphEntity pe;
            memcpy(&ie, inst + f->offset, sizeof ie);
            memcpy(&pe, pref + f->offset, sizeof pe);
            const u32 ii = instance_index(w, ie, ic->root);
            const u32 pi = prefab_rank(ic->pw, pe);
            if (ii != NPH_SAVE_NONE || pi != NPH_SAVE_NONE) {
                same = ii == pi;
            } else {
                same = !nph_world_is_alive(w, ie);
            }
        } else if (f->kind == NPH_FIELD_TEXT) {
            NphText a;
            NphText b;
            memcpy(&a, inst + f->offset, sizeof a);
            memcpy(&b, pref + f->offset, sizeof b);
            same = nph_str_equals(nph_text_str(&a), nph_text_str(&b));
        } else {
            same = memcmp(inst + f->offset, pref + f->offset, f->size) == 0;
        }
        if (!same) {
            mask |= (u64)1U << i;
        }
    }
    return mask;
}

typedef enum Section {
    SECTION_ADDED,
    SECTION_OVERRIDES,
    SECTION_REMOVED,
} Section;

// Whether component `id` goes into `section` for this instance entity, and
// for overrides the mask of differing fields.
static bool in_section(const Saver *s, const InstanceCtx *ic, NphEntity e, NphComponentId id,
                       Section section, u64 *out_mask)
{
    if (!listed_component(s->w, id)) {
        return false;
    }
    const bool in_inst = nph_world_has(s->w, e, id);
    const bool in_pref = id < ic->pw->component_count && nph_world_has(ic->pw, ic->pe, id);
    switch (section) {
    case SECTION_ADDED:
        return in_inst && !in_pref;
    case SECTION_REMOVED:
        return in_pref && !in_inst;
    case SECTION_OVERRIDES: {
        if (!in_inst || !in_pref || s->w->components[id].desc->size == 0U) {
            return false;
        }
        const u8 *iv = (const u8 *)nph_world_get(s->w, e, id);
        const u8 *pv = (const u8 *)nph_world_get(ic->pw, ic->pe, id);
        *out_mask = diff_fields(s->w, ic, id, iv, pv);
        return *out_mask != 0U;
    }
    }
    return false;
}

static NphResult write_instance_sections(Saver *s, const InstanceCtx *ic, NphEntity e)
{
    static const char *const keys[3] = {"components", "overrides", "removed"};
    for (u32 sec = 0; sec < 3U; ++sec) {
        const Section section = (Section)sec;
        u64 mask = 0U;
        bool any = false;
        for (u32 k = 0; k < s->sorted_count && !any; ++k) {
            any = in_section(s, ic, e, s->sorted[k], section, &mask);
        }
        if (!any) {
            continue;
        }
        NPH_TRY(nph_json_writer_key(&s->jw, nph_str_from_cstr(keys[sec])));
        if (section == SECTION_REMOVED) {
            NPH_TRY(nph_json_writer_begin_array(&s->jw));
        } else {
            NPH_TRY(nph_json_writer_begin_object(&s->jw));
        }
        for (u32 k = 0; k < s->sorted_count; ++k) {
            const NphComponentId id = s->sorted[k];
            if (!in_section(s, ic, e, id, section, &mask)) {
                continue;
            }
            const NphComponentDesc *d = s->w->components[id].desc;
            if (section == SECTION_REMOVED) {
                NPH_TRY(nph_json_writer_str(&s->jw, nph_str_from_cstr(d->name)));
                continue;
            }
            static const u8 no_data = 0U;
            const u8 *value = (const u8 *)nph_world_get(s->w, e, id);
            NPH_TRY(write_component(s, id, value != NULL ? value : &no_data,
                                    section == SECTION_ADDED ? ALL_FIELDS : mask));
        }
        if (section == SECTION_REMOVED) {
            NPH_TRY(nph_json_writer_end_array(&s->jw));
        } else {
            NPH_TRY(nph_json_writer_end_object(&s->jw));
        }
    }
    return NPH_RESULT_OK;
}

static NphResult write_entity(Saver *s, u32 id)
{
    const NphWorld *w = s->w;
    NphJsonWriter *jw = &s->jw;
    const NphEntity e = s->set.order[id];
    NPH_TRY(nph_json_writer_begin_object(jw));
    NPH_TRY(nph_json_writer_key(jw, NPH_STR("id")));
    NPH_TRY(nph_json_writer_i64(jw, (i64)id));
    const u32 parent = nph_saveset_id_(&s->set, w, nph_world_parent(w, e));
    if (parent != NPH_SAVE_NONE) {
        NPH_TRY(nph_json_writer_key(jw, NPH_STR("parent")));
        NPH_TRY(nph_json_writer_i64(jw, (i64)parent));
    }
    const NphPrefabInstance *pi =
        NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_PREFAB_INSTANCE, NphPrefabInstance);
    const u32 root = pi != NULL ? nph_saveset_id_(&s->set, w, pi->root) : NPH_SAVE_NONE;
    if (root == NPH_SAVE_NONE) {
        // Not an instance, or one whose root is not saved: in full.
        NPH_TRY(write_full(s, e));
        return nph_json_writer_end_object(jw);
    }

    const NphPrefab *prefab = s->resolve != NULL ? s->resolve(s->resolve_user, pi->prefab) : NULL;
    const NphEntity pe = prefab != NULL ? nph_prefab_entity(prefab, pi->index) : NPH_ENTITY_NULL;
    NPH_TRY(nph_json_writer_key(jw, NPH_STR("prefab")));
    NPH_TRY(nph_json_writer_begin_object(jw));
    NPH_TRY(nph_json_writer_key(jw, NPH_STR("name")));
    if (prefab != NULL) {
        NPH_TRY(nph_json_writer_str(jw, nph_prefab_name(prefab)));
    } else {
        NPH_TRY(write_str_id(s, jw, pi->prefab));
    }
    NPH_TRY(nph_json_writer_key(jw, NPH_STR("index")));
    NPH_TRY(nph_json_writer_i64(jw, (i64)pi->index));
    NPH_TRY(nph_json_writer_key(jw, NPH_STR("root")));
    NPH_TRY(nph_json_writer_i64(jw, (i64)root));
    if (nph_entity_is_null(pe)) {
        // The prefab is not available to diff against: the instance is
        // written in full and loads exactly as written, without the
        // prefab. A later save that has the prefab writes differences.
        NPH_TRY(nph_json_writer_key(jw, NPH_STR("full")));
        NPH_TRY(nph_json_writer_bool(jw, true));
    }
    NPH_TRY(nph_json_writer_end_object(jw));

    if (nph_entity_is_null(pe)) {
        NPH_TRY(write_full(s, e));
    } else {
        const InstanceCtx ic = {nph_prefab_world_const(prefab), pe, pi->root};
        NPH_TRY(write_instance_sections(s, &ic, e));
    }
    return nph_json_writer_end_object(jw);
}

NphResult nph_scene_save_text(const NphWorld *w, const NphSceneSaveDesc *desc, NphStrBuilder *out,
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
    // The saver holds the sorted id table; keep it off the stack of
    // callers with small stacks (fibers).
    Saver *s = NPH_ALLOC_TYPE(scratch, Saver);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    s->w = w;
    s->resolve = d.resolve;
    s->resolve_user = d.resolve_user;
    NphResult r = nph_saveset_build_(w, d.entities, d.entities != NULL ? d.entity_count : 0U,
                                     scratch, &s->set);
    if (r == NPH_RESULT_OK) {
        s->sorted_count = sorted_components(w, s->sorted);
        nph_json_writer_init(&s->jw, out, d.compact == 0);
        r = nph_json_writer_begin_object(&s->jw);
        r = r == NPH_RESULT_OK ? nph_json_writer_key(&s->jw, NPH_STR("nepheline_scene")) : r;
        r = r == NPH_RESULT_OK ? nph_json_writer_i64(&s->jw, (i64)NPH_SCENE_TEXT_VERSION) : r;
        r = r == NPH_RESULT_OK ? nph_json_writer_key(&s->jw, NPH_STR("entities")) : r;
        r = r == NPH_RESULT_OK ? nph_json_writer_begin_array(&s->jw) : r;
        for (u32 i = 0; r == NPH_RESULT_OK && i < s->set.count; ++i) {
            r = write_entity(s, i);
        }
        r = r == NPH_RESULT_OK ? nph_json_writer_end_array(&s->jw) : r;
        r = r == NPH_RESULT_OK ? nph_json_writer_end_object(&s->jw) : r;
        r = r == NPH_RESULT_OK ? nph_sb_append_char(out, '\n') : r;
        if (result != NULL) {
            result->entity_count = r == NPH_RESULT_OK ? s->set.count : 0U;
        }
        nph_saveset_free_(&s->set);
    }
    if (r != NPH_RESULT_OK) {
        nph_scene_error_(result, "saving failed: %s", nph_result_string(r));
    }
    NPH_FREE_TYPE(scratch, Saver, s);
    return r;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

typedef struct EntityIn {
    const NphJsonValue *obj;
    u32 id;
    u32 parent; // index, or NPH_SAVE_NONE
    u32 root;   // instance root index, or NPH_SAVE_NONE when not an instance
    u32 prefab_index;
    NphStrId prefab_id;
    NphStr prefab_name;      // the name as written (empty for a hash)
    const NphPrefab *prefab; // resolved; NULL for a full instance
    NphEntity pe;            // the prefab entity; null for a full instance
    bool full;               // an instance written in full
    bool has_children;
} EntityIn;

typedef struct Loader {
    NphWorld *w;
    NphAllocator *scratch;
    NphSceneResult *result;
    NphPrefabResolveFn resolve;
    void *resolve_user;
    NphJsonDoc doc;
    EntityIn *in;
    NphEntity *created;
    u32 n;
    NphHashMap ids;  // file id -> index
    NphHashMap inst; // (root index << 32 | prefab index) -> index
} Loader;

NPH_PRINTF_LIKE(3, 4)
static NphResult fail(Loader *l, NphResult r, const char *fmt, ...)
{
    if (l->result != NULL) {
        va_list args;
        va_start(args, fmt);
        (void)vsnprintf(l->result->error, sizeof l->result->error, fmt, args);
        va_end(args);
    }
    return r;
}

static u32 lookup_id(const Loader *l, i64 id)
{
    if (id < 0 || id > (i64)NPH_I32_MAX) {
        return NPH_SAVE_NONE;
    }
    const u32 *index = (const u32 *)nph_hashmap_find_u64(&l->ids, (u64)id);
    return index != NULL ? *index : NPH_SAVE_NONE;
}

static bool json_integer(const NphJsonValue *v, i64 *out)
{
    if (v == NULL || v->type != NPH_JSON_NUMBER || v->is_integer == 0U) {
        return false;
    }
    *out = v->integer;
    return true;
}

// A float field: a number, or "nan", "inf", "-inf".
static bool json_float(const NphJsonValue *v, f64 *out)
{
    if (v == NULL) {
        return false;
    }
    if (v->type == NPH_JSON_NUMBER) {
        *out = v->number;
        return true;
    }
    if (v->type == NPH_JSON_STRING) {
        if (nph_str_equals(v->string, NPH_STR("nan"))) {
            *out = (f64)NAN;
            return true;
        }
        if (nph_str_equals(v->string, NPH_STR("inf"))) {
            *out = (f64)INFINITY;
            return true;
        }
        if (nph_str_equals(v->string, NPH_STR("-inf"))) {
            *out = -(f64)INFINITY;
            return true;
        }
    }
    return false;
}

// Doubles at or beyond 2^128 - 2^103 (FLT_MAX plus half its ulp) round to
// infinity as floats; anything below rounds to a finite float, FLT_MAX at
// most (the shortest text of FLT_MAX, 3.4028235e+38, reads as a double
// slightly above it).
#define F32_ROUNDS_TO_INF 3.4028235677973366e38

static bool to_f32(f64 d, f32 *out)
{
    if (!isnan(d) && !isinf(d) && (d >= F32_ROUNDS_TO_INF || d <= -F32_ROUNDS_TO_INF)) {
        return false;
    }
    *out = (f32)d;
    return true;
}

static NphResult read_floats(const NphJsonValue *v, u32 count, bool single, u8 *dst)
{
    if (v == NULL || v->type != NPH_JSON_ARRAY) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    if (v->count != count) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    u32 i = 0U;
    NPH_JSON_FOREACH(item, v)
    {
        f64 d;
        if (!json_float(item, &d)) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        if (single) {
            f32 f;
            if (!to_f32(d, &f)) {
                return NPH_RESULT_OVERFLOW;
            }
            memcpy(dst + 4U * i, &f, sizeof f);
        } else {
            memcpy(dst + 8U * i, &d, sizeof d);
        }
        i += 1U;
    }
    return NPH_RESULT_OK;
}

static NphResult read_int(const NphJsonValue *v, i64 lo, i64 hi, i64 *out)
{
    if (!json_integer(v, out)) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    return *out >= lo && *out <= hi ? NPH_RESULT_OK : NPH_RESULT_OVERFLOW;
}

static bool parse_hex_u64(NphStr s, u64 *out)
{
    if (s.len == 0U || s.len > 16U) {
        return false;
    }
    u64 v = 0U;
    for (u64 i = 0; i < s.len; ++i) {
        const char c = s.data[i];
        u64 digit;
        if (c >= '0' && c <= '9') {
            digit = (u64)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (u64)(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = (u64)(c - 'A') + 10U;
        } else {
            return false;
        }
        v = (v << 4U) | digit;
    }
    *out = v;
    return true;
}

static NphResult read_value(Loader *l, const NphFieldDesc *f, const NphJsonValue *v, u8 *dst)
{
    i64 iv = 0;
    switch ((NphFieldKind)f->kind) {
    case NPH_FIELD_BOOL: {
        if (v->type != NPH_JSON_BOOL) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        const b32 b = v->boolean != 0U ? 1 : 0;
        memcpy(dst, &b, sizeof b);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_U8:
        NPH_TRY(read_int(v, 0, NPH_U8_MAX, &iv));
        *dst = (u8)iv;
        return NPH_RESULT_OK;
    case NPH_FIELD_U16: {
        NPH_TRY(read_int(v, 0, NPH_U16_MAX, &iv));
        const u16 x = (u16)iv;
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_U32: {
        NPH_TRY(read_int(v, 0, (i64)NPH_U32_MAX, &iv));
        const u32 x = (u32)iv;
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_U64: {
        u64 x = 0U;
        if (v->type == NPH_JSON_STRING) {
            if (!nph_str_parse_u64(v->string, &x)) {
                return NPH_RESULT_TYPE_MISMATCH;
            }
        } else {
            NPH_TRY(read_int(v, 0, NPH_I64_MAX, &iv));
            x = (u64)iv;
        }
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_I8: {
        NPH_TRY(read_int(v, NPH_I8_MIN, NPH_I8_MAX, &iv));
        const i8 x = (i8)iv;
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_I16: {
        NPH_TRY(read_int(v, NPH_I16_MIN, NPH_I16_MAX, &iv));
        const i16 x = (i16)iv;
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_I32: {
        NPH_TRY(read_int(v, NPH_I32_MIN, NPH_I32_MAX, &iv));
        const i32 x = (i32)iv;
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_I64:
        NPH_TRY(read_int(v, NPH_I64_MIN, NPH_I64_MAX, &iv));
        memcpy(dst, &iv, sizeof iv);
        return NPH_RESULT_OK;
    case NPH_FIELD_F32: {
        f64 d;
        if (!json_float(v, &d)) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        f32 x;
        if (!to_f32(d, &x)) {
            return NPH_RESULT_OVERFLOW;
        }
        memcpy(dst, &x, sizeof x);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_F64: {
        f64 d;
        if (!json_float(v, &d)) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        memcpy(dst, &d, sizeof d);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_VEC2:
        return read_floats(v, 2U, true, dst);
    case NPH_FIELD_VEC3:
        return read_floats(v, 3U, true, dst);
    case NPH_FIELD_VEC4:
    case NPH_FIELD_QUAT:
        return read_floats(v, 4U, true, dst);
    case NPH_FIELD_DVEC3:
        return read_floats(v, 3U, false, dst);
    case NPH_FIELD_ENTITY: {
        NphEntity e = NPH_ENTITY_NULL;
        if (v->type != NPH_JSON_NULL) {
            if (!json_integer(v, &iv)) {
                return NPH_RESULT_TYPE_MISMATCH;
            }
            const u32 index = lookup_id(l, iv);
            if (index == NPH_SAVE_NONE) {
                return NPH_RESULT_NOT_FOUND;
            }
            e = l->created[index];
        }
        memcpy(dst, &e, sizeof e);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_STR_ID: {
        NphStrId id = NPH_STR_ID_NULL;
        if (v->type == NPH_JSON_STRING) {
            NPH_TRY(nph_str_table_intern(&l->w->strings, v->string, &id));
        } else if (v->type == NPH_JSON_OBJECT) {
            const NphJsonValue *hash = nph_json_object_get(v, NPH_STR("hash"));
            if (hash == NULL || hash->type != NPH_JSON_STRING ||
                !parse_hex_u64(hash->string, &id.value)) {
                return NPH_RESULT_TYPE_MISMATCH;
            }
        } else if (v->type != NPH_JSON_NULL) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        memcpy(dst, &id, sizeof id);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_TEXT: {
        if (v->type != NPH_JSON_STRING) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        if (v->string.len > NPH_TEXT_CAPACITY - 1U) {
            return NPH_RESULT_OVERFLOW;
        }
        if (memchr(v->string.data, 0, (size_t)v->string.len) != NULL ||
            !nph_utf8_is_valid(v->string)) {
            return NPH_RESULT_TYPE_MISMATCH;
        }
        NphText t;
        NPH_VERIFY(nph_text_set(&t, v->string));
        memcpy(dst, &t, sizeof t);
        return NPH_RESULT_OK;
    }
    case NPH_FIELD_KIND_COUNT:
        break;
    }
    NPH_UNREACHABLE();
    return NPH_RESULT_CORRUPT;
}

// Applies the members of `obj` to the component value in `buffer`.
static NphResult read_fields(Loader *l, u32 index, NphComponentId id, const NphJsonValue *obj,
                             u8 *buffer)
{
    const NphComponentDesc *d = l->w->components[id].desc;
    if (obj->type != NPH_JSON_OBJECT) {
        return fail(l, NPH_RESULT_TYPE_MISMATCH, "entity %u: component '%s' is not an object",
                    l->in[index].id, d->name);
    }
    NPH_JSON_FOREACH(member, obj)
    {
        const NphFieldDesc *f = nph_component_find_field(d, member->key);
        if (f == NULL) {
            // A field that no longer exists: its value is dropped, and the
            // next save no longer writes it (docs/adr/0021).
            NPH_LOG_WARN(&NPH_LOG_SCENE, "entity %u: component '%s' has no field '%.*s'; ignored",
                         l->in[index].id, d->name, NPH_STR_ARG(member->key));
            continue;
        }
        if ((f->flags & NPH_FIELD_FLAG_TRANSIENT) != 0U) {
            continue;
        }
        const NphResult r = read_value(l, f, member, buffer + f->offset);
        if (r != NPH_RESULT_OK) {
            return fail(l, r, "entity %u: %s.%s: %s", l->in[index].id, d->name, f->name,
                        r == NPH_RESULT_TYPE_MISMATCH ? "the value has the wrong type"
                        : r == NPH_RESULT_OVERFLOW    ? "the value is out of range"
                        : r == NPH_RESULT_NOT_FOUND   ? "no entity has that id"
                                                      : nph_result_string(r));
        }
    }
    return NPH_RESULT_OK;
}

static NphResult parse_entities(Loader *l, const NphJsonValue *list)
{
    l->n = list->count;
    l->in = NPH_ALLOC_ARRAY_ZEROED(l->scratch, EntityIn, nph_max_u32(l->n, 1U));
    l->created = NPH_ALLOC_ARRAY_ZEROED(l->scratch, NphEntity, nph_max_u32(l->n, 1U));
    if (l->in == NULL || l->created == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NPH_TRY(nph_hashmap_reserve(&l->ids, l->n));
    u32 i = 0U;
    NPH_JSON_FOREACH(obj, list)
    {
        EntityIn *in = &l->in[i];
        in->obj = obj;
        in->parent = NPH_SAVE_NONE;
        in->root = NPH_SAVE_NONE;
        i64 id = 0;
        if (obj->type != NPH_JSON_OBJECT ||
            !json_integer(nph_json_object_get(obj, NPH_STR("id")), &id) || id < 0 ||
            id > (i64)NPH_I32_MAX) {
            return fail(l, NPH_RESULT_PARSE_ERROR,
                        "entity #%u: not an object with an \"id\" in [0, 2^31)", i);
        }
        in->id = (u32)id;
        if (nph_hashmap_insert_u64(&l->ids, (u64)id, &i) != NPH_RESULT_OK) {
            return fail(l, NPH_RESULT_PARSE_ERROR, "entity #%u: id %u is used twice", i, in->id);
        }
        i += 1U;
    }
    // Parents and prefab references, now that every id is known.
    for (i = 0; i < l->n; ++i) {
        EntityIn *in = &l->in[i];
        const NphJsonValue *parent = nph_json_object_get(in->obj, NPH_STR("parent"));
        if (parent != NULL) {
            i64 pid = 0;
            const u32 p = json_integer(parent, &pid) ? lookup_id(l, pid) : NPH_SAVE_NONE;
            if (p == NPH_SAVE_NONE || p == i) {
                return fail(l, NPH_RESULT_NOT_FOUND,
                            "entity %u: \"parent\" is not another entity's id", in->id);
            }
            in->parent = p;
            l->in[p].has_children = true;
        }
        const NphJsonValue *prefab = nph_json_object_get(in->obj, NPH_STR("prefab"));
        if (prefab == NULL) {
            continue;
        }
        const NphJsonValue *name = nph_json_object_get(prefab, NPH_STR("name"));
        i64 index = 0;
        i64 root = 0;
        if (prefab->type != NPH_JSON_OBJECT || name == NULL ||
            !json_integer(nph_json_object_get(prefab, NPH_STR("index")), &index) || index < 0 ||
            index > (i64)NPH_I32_MAX ||
            !json_integer(nph_json_object_get(prefab, NPH_STR("root")), &root)) {
            return fail(l, NPH_RESULT_PARSE_ERROR,
                        "entity %u: \"prefab\" needs a name, an index, and a root", in->id);
        }
        in->root = lookup_id(l, root);
        if (in->root == NPH_SAVE_NONE) {
            return fail(l, NPH_RESULT_NOT_FOUND, "entity %u: the prefab root is not an entity's id",
                        in->id);
        }
        NphStrId pid = NPH_STR_ID_NULL;
        if (name->type == NPH_JSON_STRING) {
            pid = nph_str_id(name->string);
            in->prefab_name = name->string;
        } else {
            const NphJsonValue *hash = nph_json_object_get(name, NPH_STR("hash"));
            if (hash == NULL || hash->type != NPH_JSON_STRING ||
                !parse_hex_u64(hash->string, &pid.value)) {
                return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: malformed prefab name", in->id);
            }
        }
        in->prefab_id = pid;
        in->prefab_index = (u32)index;
        const NphJsonValue *full = nph_json_object_get(prefab, NPH_STR("full"));
        if (full != NULL) {
            if (full->type != NPH_JSON_BOOL) {
                return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: \"full\" is not a boolean",
                            in->id);
            }
            in->full = full->boolean != 0U;
        }
        if (in->full) {
            continue; // loads as written; the prefab is not needed
        }
        in->prefab = l->resolve != NULL ? l->resolve(l->resolve_user, pid) : NULL;
        if (in->prefab == NULL) {
            const NphStr shown =
                name->type == NPH_JSON_STRING ? name->string : NPH_STR("(by hash)");
            return fail(l, NPH_RESULT_NOT_FOUND, "entity %u: prefab '%.*s' not found", in->id,
                        NPH_STR_ARG(shown));
        }
        in->pe = nph_prefab_entity(in->prefab, in->prefab_index);
        if (nph_entity_is_null(in->pe)) {
            return fail(l, NPH_RESULT_NOT_FOUND, "entity %u: prefab '%.*s' has no entity %u",
                        in->id, NPH_STR_ARG(nph_prefab_name(in->prefab)), in->prefab_index);
        }
    }
    return NPH_RESULT_OK;
}

// The component set of entity `i`, and spawning it.
static NphResult spawn_entity(Loader *l, u32 i)
{
    EntityIn *in = &l->in[i];
    NphWorld *w = l->w;
    NphComponentMask mask;
    memset(&mask, 0, sizeof mask);
    if (in->root != NPH_SAVE_NONE) {
        nph_mask_set(&mask, NPH_COMPONENT_ID_PREFAB_INSTANCE);
    }
    if (in->prefab != NULL) {
        const NphWorld *pw = nph_prefab_world_const(in->prefab);
        NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
        const u32 n = nph_world_entity_components(pw, in->pe, ids, NPH_ECS_MAX_COMPONENTS);
        for (u32 k = 0; k < n; ++k) {
            if (ids[k] != NPH_COMPONENT_ID_HIERARCHY) {
                nph_mask_set(&mask, ids[k]);
            }
        }
        const NphJsonValue *removed = nph_json_object_get(in->obj, NPH_STR("removed"));
        if (removed != NULL) {
            if (removed->type != NPH_JSON_ARRAY) {
                return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: \"removed\" is not an array",
                            in->id);
            }
            NPH_JSON_FOREACH(item, removed)
            {
                const NphComponentId id = item->type == NPH_JSON_STRING
                                              ? nph_world_find_component(w, item->string)
                                              : NPH_COMPONENT_NONE;
                if (id == NPH_COMPONENT_NONE || !listed_component(w, id)) {
                    return fail(l, NPH_RESULT_NOT_FOUND,
                                "entity %u: \"removed\" names an unknown component", in->id);
                }
                nph_mask_clear(&mask, id);
            }
        }
    } else {
        // Differences need the prefab they differ from.
        static const char *const instance_only[2] = {"removed", "overrides"};
        for (u32 k = 0; k < 2U; ++k) {
            if (nph_json_object_get(in->obj, nph_str_from_cstr(instance_only[k])) != NULL) {
                return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: \"%s\" without \"prefab\"",
                            in->id, instance_only[k]);
            }
        }
    }
    const NphJsonValue *comps = nph_json_object_get(in->obj, NPH_STR("components"));
    if (comps != NULL) {
        if (comps->type != NPH_JSON_OBJECT) {
            return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: \"components\" is not an object",
                        in->id);
        }
        NPH_JSON_FOREACH(member, comps)
        {
            const NphComponentId id = nph_world_find_component(w, member->key);
            if (id == NPH_COMPONENT_NONE) {
                return fail(l, NPH_RESULT_NOT_FOUND, "entity %u: unknown component '%.*s'", in->id,
                            NPH_STR_ARG(member->key));
            }
            // Hierarchy and PrefabInstance are written as "parent" and
            // "prefab"; transient components (WorldTransform) may be listed.
            if ((w->components[id].desc->flags & NPH_COMPONENT_FLAG_INTERNAL) != 0U ||
                id == NPH_COMPONENT_ID_PREFAB_INSTANCE) {
                return fail(l, NPH_RESULT_PARSE_ERROR,
                            "entity %u: component '%.*s' cannot be listed (it is written as "
                            "\"parent\" or \"prefab\")",
                            in->id, NPH_STR_ARG(member->key));
            }
            nph_mask_set(&mask, id);
        }
    }
    const NphJsonValue *overrides = nph_json_object_get(in->obj, NPH_STR("overrides"));
    if (overrides != NULL) {
        if (overrides->type != NPH_JSON_OBJECT) {
            return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: \"overrides\" is not an object",
                        in->id);
        }
        NPH_JSON_FOREACH(member, overrides)
        {
            const NphComponentId id = nph_world_find_component(w, member->key);
            if (id == NPH_COMPONENT_NONE || !listed_component(w, id) || !nph_mask_test(&mask, id)) {
                return fail(l, NPH_RESULT_PARSE_ERROR,
                            "entity %u: override of '%.*s', which the instance does not have",
                            in->id, NPH_STR_ARG(member->key));
            }
        }
    }
    if (in->parent != NPH_SAVE_NONE || in->has_children) {
        nph_mask_set(&mask, NPH_COMPONENT_ID_HIERARCHY);
    }
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    u32 count = 0U;
    for (u32 id = 0; id < w->component_count; ++id) {
        if (nph_mask_test(&mask, id)) {
            ids[count++] = id;
        }
    }
    const NphResult r = nph_world_spawn(w, ids, count, &l->created[i]);
    if (r != NPH_RESULT_OK) {
        return fail(l, r, "entity %u: %s", in->id, nph_result_string(r));
    }
    return NPH_RESULT_OK;
}

typedef struct InstanceRemap {
    const Loader *l;
    const NphWorld *pw;
    u32 root;
} InstanceRemap;

// A prefab entity referenced from a prefab value -> the entity of the
// same index in this instance, or null when the instance lacks it.
static NphEntity remap_instance(void *user, NphEntity e)
{
    const InstanceRemap *m = (const InstanceRemap *)user;
    const u32 rank = prefab_rank(m->pw, e);
    if (rank == NPH_SAVE_NONE) {
        return NPH_ENTITY_NULL;
    }
    const u64 key = ((u64)m->root << 32U) | rank;
    const u32 *index = (const u32 *)nph_hashmap_find_u64(&m->l->inst, key);
    return index != NULL ? m->l->created[*index] : NPH_ENTITY_NULL;
}

static NphResult fill_entity(Loader *l, u32 i)
{
    EntityIn *in = &l->in[i];
    NphWorld *w = l->w;
    NphEntityRecord *rec = nph_world_record_(w, l->created[i]);
    if (in->prefab != NULL) {
        const NphWorld *pw = nph_prefab_world_const(in->prefab);
        const NphEntityRecord *src = nph_world_record_(pw, in->pe);
        const NphArchetype *sa = src->chunk->archetype;
        InstanceRemap remap = {l, pw, in->root};
        for (u32 col = 0; col < sa->column_count; ++col) {
            const NphComponentId id = sa->column_ids[col];
            if (id == NPH_COMPONENT_ID_HIERARCHY || id == NPH_COMPONENT_ID_PREFAB_INSTANCE ||
                !nph_world_has(w, l->created[i], id)) {
                continue;
            }
            void *cell = nph_world_cell_mut_(w, rec, id);
            memcpy(cell, nph_chunk_cell(src->chunk, col, src->row), sa->column_sizes[col]);
            nph_component_remap_entities_(&w->components[id], cell, remap_instance, &remap);
        }
    }
    if (in->root != NPH_SAVE_NONE) {
        NphPrefabInstance *pi =
            (NphPrefabInstance *)nph_world_cell_mut_(w, rec, NPH_COMPONENT_ID_PREFAB_INSTANCE);
        pi->prefab = in->prefab_id;
        if (in->prefab_name.len > 0U) {
            NPH_TRY(nph_str_table_intern(&w->strings, in->prefab_name, &pi->prefab));
        }
        pi->index = in->prefab_index;
        pi->root = l->created[in->root];
    }
    const NphJsonValue *comps = nph_json_object_get(in->obj, NPH_STR("components"));
    if (comps != NULL) {
        NPH_JSON_FOREACH(member, comps)
        {
            const NphComponentId id = nph_world_find_component(w, member->key);
            const NphComponentDesc *d = w->components[id].desc;
            if (d->size == 0U) {
                continue;
            }
            ValueBuffer buffer;
            nph_component_init_default(d, buffer.bytes);
            NPH_TRY(read_fields(l, i, id, member, buffer.bytes));
            memcpy(nph_world_cell_mut_(w, rec, id), buffer.bytes, d->size);
        }
    }
    const NphJsonValue *overrides = nph_json_object_get(in->obj, NPH_STR("overrides"));
    if (overrides != NULL) {
        NPH_JSON_FOREACH(member, overrides)
        {
            const NphComponentId id = nph_world_find_component(w, member->key);
            const NphComponentDesc *d = w->components[id].desc;
            if (d->size == 0U) {
                continue;
            }
            ValueBuffer buffer;
            void *cell = nph_world_cell_mut_(w, rec, id);
            memcpy(buffer.bytes, cell, d->size);
            NPH_TRY(read_fields(l, i, id, member, buffer.bytes));
            memcpy(cell, buffer.bytes, d->size);
        }
    }
    return NPH_RESULT_OK;
}

static NphResult load(Loader *l, NphStr text)
{
    NphWorld *w = l->w;
    const NphResult parsed = nph_json_parse(text, l->scratch, &l->doc);
    if (parsed == NPH_RESULT_PARSE_ERROR) {
        return fail(l, parsed, "line %u, column %u: %s", l->doc.error_line, l->doc.error_column,
                    l->doc.error_message);
    }
    NPH_TRY(parsed);
    const NphJsonValue *root = l->doc.root;
    i64 version = 0;
    if (root == NULL || root->type != NPH_JSON_OBJECT ||
        !json_integer(nph_json_object_get(root, NPH_STR("nepheline_scene")), &version)) {
        return fail(l, NPH_RESULT_PARSE_ERROR, "not a scene: no \"nepheline_scene\" version");
    }
    if (version != (i64)NPH_SCENE_TEXT_VERSION) {
        return fail(l, NPH_RESULT_VERSION_MISMATCH, "scene format version %lld, expected %u",
                    (long long)version, NPH_SCENE_TEXT_VERSION);
    }
    const NphJsonValue *list = nph_json_object_get(root, NPH_STR("entities"));
    if (list == NULL || list->type != NPH_JSON_ARRAY) {
        return fail(l, NPH_RESULT_PARSE_ERROR, "\"entities\" is missing or not an array");
    }
    NPH_TRY(parse_entities(l, list));
    if ((u64)nph_world_entity_count(w) + l->n > nph_world_entity_capacity(w)) {
        return fail(l, NPH_RESULT_FULL, "%u entities do not fit the world", l->n);
    }
    for (u32 i = 0; i < l->n; ++i) {
        NPH_TRY(spawn_entity(l, i));
    }
    for (u32 i = 0; i < l->n; ++i) {
        if (l->in[i].root == NPH_SAVE_NONE) {
            continue;
        }
        const u64 key = ((u64)l->in[i].root << 32U) | l->in[i].prefab_index;
        if (nph_hashmap_insert_u64(&l->inst, key, &i) != NPH_RESULT_OK) {
            return fail(l, NPH_RESULT_PARSE_ERROR,
                        "entity %u: another entity of the same instance has index %u", l->in[i].id,
                        l->in[i].prefab_index);
        }
    }
    for (u32 i = 0; i < l->n; ++i) {
        NPH_TRY(fill_entity(l, i));
    }
    // The hierarchy, in file order: children are attached in the order the
    // file lists them, which is the order they were saved in.
    for (u32 i = 0; i < l->n; ++i) {
        if (l->in[i].parent == NPH_SAVE_NONE) {
            continue;
        }
        const NphResult r = nph_world_set_parent(w, l->created[i], l->created[l->in[i].parent]);
        if (r == NPH_RESULT_INVALID_ARGUMENT) {
            return fail(l, NPH_RESULT_PARSE_ERROR, "entity %u: the parents form a cycle",
                        l->in[i].id);
        }
        if (r != NPH_RESULT_OK) {
            return fail(l, r, "entity %u: %s", l->in[i].id,
                        r == NPH_RESULT_OVERFLOW ? "the hierarchy is too deep"
                                                 : nph_result_string(r));
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_scene_load_text(NphWorld *w, NphStr text, const NphSceneLoadDesc *desc,
                              NphSceneResult *result)
{
    NPH_ASSERT(w != NULL);
    if (result != NULL) {
        memset(result, 0, sizeof *result);
    }
    NphSceneLoadDesc d;
    memset(&d, 0, sizeof d);
    if (desc != NULL) {
        d = *desc;
    }
    Loader l;
    memset(&l, 0, sizeof l);
    l.w = w;
    l.scratch = d.scratch != NULL ? d.scratch : nph_allocator_system();
    nph_json_doc_init(&l.doc, l.scratch);
    l.result = result;
    l.resolve = d.resolve;
    l.resolve_user = d.resolve_user;
    const NphHashMapDesc map = nph_hashmap_desc_u64(sizeof(u32));
    NphResult r = nph_hashmap_init(&l.ids, l.scratch, &map);
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&l.inst, l.scratch, &map);
    }
    if (r == NPH_RESULT_OK) {
        r = load(&l, text);
    }
    if (r != NPH_RESULT_OK) {
        if (result != NULL && result->error[0] == '\0') {
            nph_scene_error_(result, "loading failed: %s", nph_result_string(r));
        }
        // All or nothing.
        for (u32 i = l.n; i > 0U; --i) {
            if (!nph_entity_is_null(l.created[i - 1U])) {
                NPH_IGNORE_RESULT(nph_world_despawn(w, l.created[i - 1U]));
            }
        }
    } else {
        if (d.out_entities != NULL) {
            const u32 n = nph_min_u32(l.n, d.out_capacity);
            if (n > 0U) {
                memcpy(d.out_entities, l.created, sizeof(NphEntity) * n);
            }
        }
        if (result != NULL) {
            result->entity_count = l.n;
        }
    }
    nph_json_doc_free(&l.doc);
    nph_hashmap_free(&l.ids);
    nph_hashmap_free(&l.inst);
    if (l.in != NULL) {
        NPH_FREE_ARRAY(l.scratch, EntityIn, l.in, nph_max_u32(l.n, 1U));
    }
    if (l.created != NULL) {
        NPH_FREE_ARRAY(l.scratch, NphEntity, l.created, nph_max_u32(l.n, 1U));
    }
    return r;
}

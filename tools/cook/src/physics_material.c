// SPDX-License-Identifier: Apache-2.0
// Physics material sources (cook/physics_material.h; ADR-0108).
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>

#include <cook/physics_material.h>
#include <stdio.h>
#include <string.h>

#define SAVE_PATH_MAX 4096U

static const char *const k_combine_names[NPH_PHYSICS_MATERIAL_COMBINE_COUNT] = {
    "default", "average", "minimum", "multiply", "maximum"};

void cook_physics_material_source_default(CookPhysicsMaterialSource *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->friction = 0.6F;
    out->restitution = 0.0F;
    out->density = 1000.0F;
    out->friction_combine = (u32)NPH_PHYSICS_MATERIAL_COMBINE_DEFAULT;
    out->restitution_combine = (u32)NPH_PHYSICS_MATERIAL_COMBINE_DEFAULT;
}

const char *cook_physics_combine_name(u32 mode)
{
    return mode < (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT ? k_combine_names[mode] : NULL;
}

static bool in_range(f32 x, f32 lo, f32 hi)
{
    return x >= lo && x <= hi; // false for NaN
}

NphResult cook_physics_material_source_validate(const CookPhysicsMaterialSource *m)
{
    NPH_ASSERT(m != NULL);
    if (!in_range(m->friction, 0.0F, NPH_PHYSICS_MATERIAL_MAX_FRICTION) ||
        !in_range(m->restitution, 0.0F, 1.0F) ||
        !in_range(m->density, 0.0F, NPH_PHYSICS_MATERIAL_MAX_DENSITY) ||
        m->friction_combine >= (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT ||
        m->restitution_combine >= (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT) {
        return NPH_RESULT_CORRUPT;
    }
    return NPH_RESULT_OK;
}

bool cook_physics_material_source_equal(const CookPhysicsMaterialSource *a,
                                        const CookPhysicsMaterialSource *b)
{
    NPH_ASSERT(a != NULL && b != NULL);
    return a->friction == b->friction && a->restitution == b->restitution &&
           a->density == b->density && a->friction_combine == b->friction_combine &&
           a->restitution_combine == b->restitution_combine;
}

void cook_physics_material_source_header(const CookPhysicsMaterialSource *m,
                                         NphPhysicsMaterialHeader *out)
{
    NPH_ASSERT(m != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->friction = m->friction;
    out->restitution = m->restitution;
    out->density = m->density;
    out->friction_combine = m->friction_combine;
    out->restitution_combine = m->restitution_combine;
}

// ---------------------------------------------------------------------------
// The `.npmat` file
// ---------------------------------------------------------------------------

typedef enum Key {
    KEY_NPMAT = 0,
    KEY_FRICTION,
    KEY_RESTITUTION,
    KEY_DENSITY,
    KEY_FRICTION_COMBINE,
    KEY_RESTITUTION_COMBINE,
    KEY_COUNT
} Key;

static const char *const k_keys[KEY_COUNT] = {"npmat",   "friction",         "restitution",
                                              "density", "friction_combine", "restitution_combine"};

// The index of `name` among `names`, `count` when it is none.
static u32 name_index(NphStr name, const char *const *names, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        if (nph_str_equals(name, nph_str_from_cstr(names[i]))) {
            return i;
        }
    }
    return count;
}

static NphResult read_number(const NphJsonValue *v, f32 lo, f32 hi, f32 *out)
{
    if (v == NULL || v->type != NPH_JSON_NUMBER || !(v->number >= (f64)lo) ||
        !(v->number <= (f64)hi)) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out = (f32)v->number; // in range still: the bounds are f32s, and rounding keeps order
    return NPH_RESULT_OK;
}

static NphResult read_combine(const NphJsonValue *v, u32 *out)
{
    NphStr s;
    if (nph_json_as_str(v, &s) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const u32 i = name_index(s, k_combine_names, (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT);
    if (i == (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out = i;
    return NPH_RESULT_OK;
}

// One member of the document, by its key.
static NphResult read_member(CookPhysicsMaterialSource *m, Key key, const NphJsonValue *v)
{
    switch (key) {
    case KEY_NPMAT:
        return NPH_RESULT_OK; // read first
    case KEY_FRICTION:
        return read_number(v, 0.0F, NPH_PHYSICS_MATERIAL_MAX_FRICTION, &m->friction);
    case KEY_RESTITUTION:
        return read_number(v, 0.0F, 1.0F, &m->restitution);
    case KEY_DENSITY:
        return read_number(v, 0.0F, NPH_PHYSICS_MATERIAL_MAX_DENSITY, &m->density);
    case KEY_FRICTION_COMBINE:
        return read_combine(v, &m->friction_combine);
    case KEY_RESTITUTION_COMBINE:
        return read_combine(v, &m->restitution_combine);
    case KEY_COUNT:
        break;
    }
    return NPH_RESULT_PARSE_ERROR;
}

static NphResult read_document(CookPhysicsMaterialSource *m, const NphJsonValue *root)
{
    if (root == NULL || root->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    // The version first: another version's keys are not this one's to check.
    i64 version = 0;
    if (nph_json_as_i64(nph_json_object_get(root, NPH_STR("npmat")), &version) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    if (version != (i64)COOK_PHYSICS_MATERIAL_SOURCE_VERSION) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    u32 seen = 0U;
    NPH_JSON_FOREACH(e, root)
    {
        const u32 k = name_index(e->key, k_keys, KEY_COUNT);
        if (k == KEY_COUNT || (seen & (1U << k)) != 0U) {
            return NPH_RESULT_PARSE_ERROR;
        }
        seen |= 1U << k;
        NPH_TRY(read_member(m, (Key)k, e));
    }
    return NPH_RESULT_OK;
}

NphResult cook_physics_material_source_read(NphAllocator *alloc, NphStr text,
                                            CookPhysicsMaterialSource *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    cook_physics_material_source_default(out);
    NphJsonDoc doc;
    NphResult r = nph_json_parse(text, alloc, &doc);
    if (r == NPH_RESULT_OK) {
        r = read_document(out, doc.root);
    } else if (r != NPH_RESULT_OUT_OF_MEMORY) {
        r = NPH_RESULT_PARSE_ERROR;
    }
    nph_json_doc_free(&doc);
    if (r != NPH_RESULT_OK) {
        cook_physics_material_source_default(out);
    }
    return r;
}

NphResult cook_physics_material_source_load(NphAllocator *alloc, const char *path,
                                            CookPhysicsMaterialSource *out)
{
    NPH_ASSERT(alloc != NULL && path != NULL && out != NULL);
    cook_physics_material_source_default(out);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, alloc, &data, &size));
    const NphResult r =
        cook_physics_material_source_read(alloc, nph_str((const char *)data, size), out);
    nph_file_free_all(alloc, data, size);
    return r;
}

static NphResult write_key_number(NphJsonWriter *w, Key key, f32 v)
{
    NPH_TRY(nph_json_writer_key(w, nph_str_from_cstr(k_keys[key])));
    // -0 reads back as 0: written as 0, so the file is the same either way.
    return nph_json_writer_f32_shortest(w, v == 0.0F ? 0.0F : v);
}

static NphResult write_key_combine(NphJsonWriter *w, Key key, u32 mode)
{
    NPH_TRY(nph_json_writer_key(w, nph_str_from_cstr(k_keys[key])));
    return nph_json_writer_str(w, nph_str_from_cstr(k_combine_names[mode]));
}

static NphResult write_document(const CookPhysicsMaterialSource *m, NphJsonWriter *w)
{
    NPH_TRY(nph_json_writer_begin_object(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("npmat")));
    NPH_TRY(nph_json_writer_i64(w, (i64)COOK_PHYSICS_MATERIAL_SOURCE_VERSION));
    NPH_TRY(write_key_number(w, KEY_FRICTION, m->friction));
    NPH_TRY(write_key_number(w, KEY_RESTITUTION, m->restitution));
    NPH_TRY(write_key_number(w, KEY_DENSITY, m->density));
    NPH_TRY(write_key_combine(w, KEY_FRICTION_COMBINE, m->friction_combine));
    NPH_TRY(write_key_combine(w, KEY_RESTITUTION_COMBINE, m->restitution_combine));
    NPH_TRY(nph_json_writer_end_object(w));
    return nph_sb_append_cstr(w->out, "\n");
}

NphResult cook_physics_material_source_write(const CookPhysicsMaterialSource *m, NphStrBuilder *out)
{
    NPH_ASSERT(m != NULL && out != NULL);
    NPH_TRY(cook_physics_material_source_validate(m));
    NphJsonWriter w;
    nph_json_writer_init(&w, out, true);
    return write_document(m, &w);
}

NphResult cook_physics_material_source_save(const CookPhysicsMaterialSource *m, NphAllocator *alloc,
                                            const char *path)
{
    NPH_ASSERT(m != NULL && alloc != NULL && path != NULL);
    char temp[SAVE_PATH_MAX];
    const int n = snprintf(temp, sizeof temp, "%s.part", path);
    if (n < 0 || (u64)n >= sizeof temp) {
        return NPH_RESULT_OVERFLOW;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    NphResult r = cook_physics_material_source_write(m, &sb);
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(temp, sb.data, sb.len);
    }
    nph_sb_free(&sb);
    if (r == NPH_RESULT_OK) {
        r = nph_fs_rename(temp, path);
    }
    if (r != NPH_RESULT_OK) {
        (void)nph_fs_remove_file(temp);
    }
    return r;
}

// SPDX-License-Identifier: Apache-2.0
// Material sources (cook/material.h; ADR-0105).
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>

#include <cook/cooker.h>
#include <cook/material.h>
#include <stdio.h>
#include <string.h>

#define SAVE_PATH_MAX 4096U

static const char *const k_slot_names[NPH_MATERIAL_TEXTURE_COUNT] = {
    "base_color", "metallic_roughness", "normal", "occlusion", "emissive"};
static const char *const k_alpha_names[] = {"opaque", "mask", "blend"};
static const char *const k_wrap_names[] = {"repeat", "clamp", "mirror"};
static const char *const k_filter_names[] = {"linear", "nearest"};

void cook_material_source_default(CookMaterialSource *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->base_color = nph_vec4(0.8F, 0.8F, 0.8F, 1.0F);
    out->metallic = 0.0F;
    out->roughness = 0.5F;
    out->emissive = nph_vec3_zero();
    out->emissive_strength = 1.0F;
    out->normal_scale = 1.0F;
    out->occlusion_strength = 1.0F;
    out->alpha = (u32)NPH_MATERIAL_ALPHA_OPAQUE;
    out->alpha_cutoff = 0.5F;
    out->tiling = nph_vec2(1.0F, 1.0F);
    out->rotation = 0.0F;
    out->offset = nph_vec2(0.0F, 0.0F);
    // Every slot unused, repeating (NPH_MATERIAL_WRAP_REPEAT is 0) and filtered linearly.
}

bool cook_material_image_key_valid(NphStr key)
{
    if (key.data == NULL || key.len == 0U || key.len >= COOK_MATERIAL_IMAGE_MAX) {
        return false;
    }
    u64 start = 0U;
    for (u64 i = 0U; i <= key.len; ++i) {
        char ch = '/'; // past the end: the last part ends there
        if (i < key.len) {
            ch = key.data[i];
        }
        if ((unsigned char)ch < 0x20U || ch == '\\' || ch == ':') {
            return false;
        }
        if (ch != '/') {
            continue;
        }
        // A part: not empty (no leading, trailing or doubled '/'), not "." or "..".
        const u64 n = i - start;
        if (n == 0U || (n == 1U && key.data[start] == '.') ||
            (n == 2U && key.data[start] == '.' && key.data[start + 1U] == '.')) {
            return false;
        }
        start = i + 1U;
    }
    char path[COOK_MATERIAL_IMAGE_MAX];
    memcpy(path, key.data, (size_t)key.len);
    path[key.len] = '\0';
    return cooker_source_kind(path) == COOKER_SOURCE_IMAGE;
}

static bool in_range(f32 x, f32 lo, f32 hi)
{
    return x >= lo && x <= hi; // false for NaN
}

static bool tiling_ok(f32 t)
{
    const f32 size = t < 0.0F ? -t : t;
    return in_range(size, COOK_MATERIAL_MIN_TILING, COOK_MATERIAL_MAX_TILING);
}

// The length of a slot's image key, or COOK_MATERIAL_IMAGE_MAX when it has no NUL.
static u64 image_length(const CookMaterialTexture *t)
{
    const char *nul = (const char *)memchr(t->image, '\0', sizeof t->image);
    return nul != NULL ? (u64)(nul - t->image) : (u64)COOK_MATERIAL_IMAGE_MAX;
}

// The source's numbers, in one order: what validation bounds and equality compares.
#define SCALARS 18U

static void scalars_of(const CookMaterialSource *m, f32 out[SCALARS])
{
    const f32 v[SCALARS] = {m->base_color.x,      m->base_color.y, m->base_color.z,
                            m->base_color.w,      m->metallic,     m->roughness,
                            m->emissive.x,        m->emissive.y,   m->emissive.z,
                            m->emissive_strength, m->normal_scale, m->occlusion_strength,
                            m->alpha_cutoff,      m->tiling.x,     m->tiling.y,
                            m->rotation,          m->offset.x,     m->offset.y};
    memcpy(out, v, sizeof v);
}

NphResult cook_material_source_validate(const CookMaterialSource *m)
{
    NPH_ASSERT(m != NULL);
    f32 s[SCALARS];
    scalars_of(m, s);
    // Colours, metallic, roughness, occlusion and the cutoff are 0..1.
    static const u32 unit[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 11U, 12U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(unit); ++i) {
        if (!in_range(s[unit[i]], 0.0F, 1.0F)) {
            return NPH_RESULT_CORRUPT;
        }
    }
    if (!in_range(m->emissive_strength, 0.0F, COOK_MATERIAL_MAX_EMISSIVE) ||
        !in_range(m->normal_scale, -COOK_MATERIAL_MAX_NORMAL, COOK_MATERIAL_MAX_NORMAL) ||
        !tiling_ok(m->tiling.x) || !tiling_ok(m->tiling.y) ||
        !in_range(m->rotation, -COOK_MATERIAL_MAX_ROTATION, COOK_MATERIAL_MAX_ROTATION) ||
        !in_range(m->offset.x, -COOK_MATERIAL_MAX_OFFSET, COOK_MATERIAL_MAX_OFFSET) ||
        !in_range(m->offset.y, -COOK_MATERIAL_MAX_OFFSET, COOK_MATERIAL_MAX_OFFSET) ||
        m->alpha > (u32)NPH_MATERIAL_ALPHA_BLEND || m->double_sided < 0 || m->double_sided > 1 ||
        m->unlit < 0 || m->unlit > 1) {
        return NPH_RESULT_CORRUPT;
    }
    for (u32 slot = 0; slot < NPH_MATERIAL_TEXTURE_COUNT; ++slot) {
        const CookMaterialTexture *t = &m->textures[slot];
        if (t->wrap_u > (u8)NPH_MATERIAL_WRAP_MIRROR || t->wrap_v > (u8)NPH_MATERIAL_WRAP_MIRROR ||
            t->nearest > 1U || t->reserved != 0U) {
            return NPH_RESULT_CORRUPT;
        }
        const u64 len = image_length(t);
        if (len == (u64)COOK_MATERIAL_IMAGE_MAX ||
            (len != 0U && !cook_material_image_key_valid(nph_str(t->image, len)))) {
            return NPH_RESULT_CORRUPT;
        }
    }
    return NPH_RESULT_OK;
}

bool cook_material_source_equal(const CookMaterialSource *a, const CookMaterialSource *b)
{
    NPH_ASSERT(a != NULL && b != NULL);
    f32 sa[SCALARS];
    f32 sb[SCALARS];
    scalars_of(a, sa);
    scalars_of(b, sb);
    for (u32 i = 0; i < SCALARS; ++i) {
        if (sa[i] != sb[i]) {
            return false;
        }
    }
    if (a->alpha != b->alpha || a->double_sided != b->double_sided || a->unlit != b->unlit) {
        return false;
    }
    for (u32 slot = 0; slot < NPH_MATERIAL_TEXTURE_COUNT; ++slot) {
        const CookMaterialTexture *ta = &a->textures[slot];
        const CookMaterialTexture *tb = &b->textures[slot];
        const u64 la = image_length(ta);
        if (la != image_length(tb) || memcmp(ta->image, tb->image, (size_t)la) != 0) {
            return false;
        }
        // An unused slot's wrap and filter are not part of the material.
        if (la != 0U &&
            (ta->wrap_u != tb->wrap_u || ta->wrap_v != tb->wrap_v || ta->nearest != tb->nearest)) {
            return false;
        }
    }
    return true;
}

CookTextureUsage cook_material_slot_usage(u32 slot)
{
    switch (slot) {
    case NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS:
        return COOK_TEXTURE_LINEAR;
    case NPH_MATERIAL_TEXTURE_NORMAL:
        return COOK_TEXTURE_NORMAL;
    case NPH_MATERIAL_TEXTURE_OCCLUSION:
        return COOK_TEXTURE_MASK;
    default: // base colour, emissive
        return COOK_TEXTURE_COLOR;
    }
}

NphMaterialUvTransform cook_material_source_uv(const CookMaterialSource *m)
{
    NPH_ASSERT(m != NULL);
    // Unturned exactly, so that an untiled material is the identity bit for bit.
    f32 c = 1.0F;
    f32 s = 0.0F;
    if (m->rotation != 0.0F) {
        const f32 r = m->rotation * (NPH_PI / 180.0F);
        c = nph_f32_cos(r);
        s = nph_f32_sin(r);
    }
    // The glTF importer's composition (gltf.c uv_transform_of): u' = c sx u + s sy v + ox,
    // v' = -s sx u + c sy v + oy.
    const NphMaterialUvTransform t = {c * m->tiling.x,  s * m->tiling.y, m->offset.x,
                                      -s * m->tiling.x, c * m->tiling.y, m->offset.y};
    return t;
}

void cook_material_source_header(const CookMaterialSource *m,
                                 const NphGuid textures[NPH_MATERIAL_TEXTURE_COUNT],
                                 NphMaterialHeader *out)
{
    NPH_ASSERT(m != NULL && textures != NULL && out != NULL);
    nph_material_header_default(out);
    out->base_color_factor = m->base_color;
    out->emissive_factor = m->emissive;
    out->emissive_strength = m->emissive_strength;
    out->metallic_factor = m->metallic;
    out->roughness_factor = m->roughness;
    out->alpha_cutoff = m->alpha_cutoff;
    out->alpha_mode = m->alpha;
    out->flags = (m->double_sided != 0 ? (u32)NPH_MATERIAL_FLAG_DOUBLE_SIDED : 0U) |
                 (m->unlit != 0 ? (u32)NPH_MATERIAL_FLAG_UNLIT : 0U);
    out->alpha_key = 0U;
    for (u32 slot = 0; slot < NPH_MATERIAL_TEXTURE_COUNT; ++slot) {
        const CookMaterialTexture *t = &m->textures[slot];
        NphMaterialTextureRef *ref = &out->textures[slot];
        ref->texture = t->image[0] != '\0' ? textures[slot] : NPH_GUID_NULL;
        ref->wrap_u = t->wrap_u;
        ref->wrap_v = t->wrap_v;
        ref->nearest = t->nearest;
    }
    out->textures[NPH_MATERIAL_TEXTURE_NORMAL].scale = m->normal_scale;
    out->textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale = m->occlusion_strength;
}

// ---------------------------------------------------------------------------
// The `.nmat` file
// ---------------------------------------------------------------------------

typedef enum Key {
    KEY_NMAT = 0,
    KEY_BASE_COLOR,
    KEY_METALLIC,
    KEY_ROUGHNESS,
    KEY_EMISSIVE,
    KEY_EMISSIVE_STRENGTH,
    KEY_NORMAL_SCALE,
    KEY_OCCLUSION_STRENGTH,
    KEY_ALPHA,
    KEY_ALPHA_CUTOFF,
    KEY_DOUBLE_SIDED,
    KEY_UNLIT,
    KEY_TILING,
    KEY_ROTATION,
    KEY_OFFSET,
    KEY_TEXTURES,
    KEY_COUNT
} Key;

static const char *const k_keys[KEY_COUNT] = {
    "nmat",     "base_color",        "metallic",     "roughness",
    "emissive", "emissive_strength", "normal_scale", "occlusion_strength",
    "alpha",    "alpha_cutoff",      "double_sided", "unlit",
    "tiling",   "rotation",          "offset",       "textures"};

static const char *const k_texture_keys[] = {"image", "wrap", "filter"};

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
    *out = (f32)v->number;
    return NPH_RESULT_OK;
}

static NphResult read_numbers(const NphJsonValue *v, u32 n, f32 lo, f32 hi, f32 *out)
{
    if (v == NULL || v->type != NPH_JSON_ARRAY || v->count != n) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u32 i = 0U;
    NPH_JSON_FOREACH(e, v)
    {
        NPH_TRY(read_number(e, lo, hi, &out[i]));
        i += 1U;
    }
    return NPH_RESULT_OK;
}

static NphResult read_bool(const NphJsonValue *v, b32 *out)
{
    bool b = false;
    if (nph_json_as_bool(v, &b) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out = b ? 1 : 0;
    return NPH_RESULT_OK;
}

static NphResult read_name(const NphJsonValue *v, const char *const *names, u32 count, u32 *out)
{
    NphStr s;
    if (nph_json_as_str(v, &s) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const u32 i = name_index(s, names, count);
    if (i == count) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out = i;
    return NPH_RESULT_OK;
}

static NphResult read_wrap(CookMaterialTexture *t, const NphJsonValue *v)
{
    if (v->type != NPH_JSON_ARRAY || v->count != 2U) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u32 wrap[2] = {0U, 0U};
    u32 i = 0U;
    NPH_JSON_FOREACH(e, v)
    {
        NPH_TRY(read_name(e, k_wrap_names, (u32)NPH_ARRAY_COUNT(k_wrap_names), &wrap[i]));
        i += 1U;
    }
    t->wrap_u = (u8)wrap[0];
    t->wrap_v = (u8)wrap[1];
    return NPH_RESULT_OK;
}

static NphResult read_texture(CookMaterialTexture *t, const NphJsonValue *v)
{
    if (v->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const u32 count = (u32)NPH_ARRAY_COUNT(k_texture_keys);
    u32 seen = 0U;
    NPH_JSON_FOREACH(e, v)
    {
        const u32 k = name_index(e->key, k_texture_keys, count);
        if (k == count || (seen & (1U << k)) != 0U) {
            return NPH_RESULT_PARSE_ERROR;
        }
        seen |= 1U << k;
        if (k == 0U) {
            NphStr s;
            if (nph_json_as_str(e, &s) != NPH_RESULT_OK || !cook_material_image_key_valid(s)) {
                return NPH_RESULT_PARSE_ERROR;
            }
            memcpy(t->image, s.data, (size_t)s.len);
            t->image[s.len] = '\0';
        } else if (k == 1U) {
            NPH_TRY(read_wrap(t, e));
        } else {
            u32 filter = 0U;
            NPH_TRY(read_name(e, k_filter_names, (u32)NPH_ARRAY_COUNT(k_filter_names), &filter));
            t->nearest = (u8)filter;
        }
    }
    return (seen & 1U) != 0U ? NPH_RESULT_OK : NPH_RESULT_PARSE_ERROR; // the image is required
}

static NphResult read_textures(CookMaterialSource *m, const NphJsonValue *v)
{
    if (v->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u32 seen = 0U;
    NPH_JSON_FOREACH(e, v)
    {
        const u32 slot = name_index(e->key, k_slot_names, NPH_MATERIAL_TEXTURE_COUNT);
        if (slot == NPH_MATERIAL_TEXTURE_COUNT || (seen & (1U << slot)) != 0U) {
            return NPH_RESULT_PARSE_ERROR;
        }
        seen |= 1U << slot;
        NPH_TRY(read_texture(&m->textures[slot], e));
    }
    return NPH_RESULT_OK;
}

static NphResult read_tiling(CookMaterialSource *m, const NphJsonValue *v)
{
    f32 t[2] = {0.0F, 0.0F};
    NPH_TRY(read_numbers(v, 2U, -COOK_MATERIAL_MAX_TILING, COOK_MATERIAL_MAX_TILING, t));
    if (!tiling_ok(t[0]) || !tiling_ok(t[1])) {
        return NPH_RESULT_PARSE_ERROR;
    }
    m->tiling = nph_vec2(t[0], t[1]);
    return NPH_RESULT_OK;
}

// One member of the document, by its key.
static NphResult read_member(CookMaterialSource *m, Key key, const NphJsonValue *v)
{
    f32 n[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    switch (key) {
    case KEY_NMAT:
        return NPH_RESULT_OK; // read first
    case KEY_BASE_COLOR:
        NPH_TRY(read_numbers(v, 4U, 0.0F, 1.0F, n));
        m->base_color = nph_vec4(n[0], n[1], n[2], n[3]);
        return NPH_RESULT_OK;
    case KEY_METALLIC:
        return read_number(v, 0.0F, 1.0F, &m->metallic);
    case KEY_ROUGHNESS:
        return read_number(v, 0.0F, 1.0F, &m->roughness);
    case KEY_EMISSIVE:
        NPH_TRY(read_numbers(v, 3U, 0.0F, 1.0F, n));
        m->emissive = nph_vec3(n[0], n[1], n[2]);
        return NPH_RESULT_OK;
    case KEY_EMISSIVE_STRENGTH:
        return read_number(v, 0.0F, COOK_MATERIAL_MAX_EMISSIVE, &m->emissive_strength);
    case KEY_NORMAL_SCALE:
        return read_number(v, -COOK_MATERIAL_MAX_NORMAL, COOK_MATERIAL_MAX_NORMAL,
                           &m->normal_scale);
    case KEY_OCCLUSION_STRENGTH:
        return read_number(v, 0.0F, 1.0F, &m->occlusion_strength);
    case KEY_ALPHA:
        return read_name(v, k_alpha_names, (u32)NPH_ARRAY_COUNT(k_alpha_names), &m->alpha);
    case KEY_ALPHA_CUTOFF:
        return read_number(v, 0.0F, 1.0F, &m->alpha_cutoff);
    case KEY_DOUBLE_SIDED:
        return read_bool(v, &m->double_sided);
    case KEY_UNLIT:
        return read_bool(v, &m->unlit);
    case KEY_TILING:
        return read_tiling(m, v);
    case KEY_ROTATION:
        return read_number(v, -COOK_MATERIAL_MAX_ROTATION, COOK_MATERIAL_MAX_ROTATION,
                           &m->rotation);
    case KEY_OFFSET:
        NPH_TRY(read_numbers(v, 2U, -COOK_MATERIAL_MAX_OFFSET, COOK_MATERIAL_MAX_OFFSET, n));
        m->offset = nph_vec2(n[0], n[1]);
        return NPH_RESULT_OK;
    case KEY_TEXTURES:
        return read_textures(m, v);
    case KEY_COUNT:
        break;
    }
    return NPH_RESULT_PARSE_ERROR;
}

static NphResult read_document(CookMaterialSource *m, const NphJsonValue *root)
{
    if (root == NULL || root->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    // The version first: another version's keys are not this one's to check.
    i64 version = 0;
    if (nph_json_as_i64(nph_json_object_get(root, NPH_STR("nmat")), &version) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    if (version != (i64)COOK_MATERIAL_SOURCE_VERSION) {
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

NphResult cook_material_source_read(NphAllocator *alloc, NphStr text, CookMaterialSource *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    cook_material_source_default(out);
    NphJsonDoc doc;
    NphResult r = nph_json_parse(text, alloc, &doc);
    if (r == NPH_RESULT_OK) {
        r = read_document(out, doc.root);
    } else if (r != NPH_RESULT_OUT_OF_MEMORY) {
        r = NPH_RESULT_PARSE_ERROR;
    }
    nph_json_doc_free(&doc);
    if (r != NPH_RESULT_OK) {
        cook_material_source_default(out);
    }
    return r;
}

NphResult cook_material_source_load(NphAllocator *alloc, const char *path, CookMaterialSource *out)
{
    NPH_ASSERT(alloc != NULL && path != NULL && out != NULL);
    cook_material_source_default(out);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, alloc, &data, &size));
    const NphResult r = cook_material_source_read(alloc, nph_str((const char *)data, size), out);
    nph_file_free_all(alloc, data, size);
    return r;
}

static NphResult write_number(NphJsonWriter *w, f32 v)
{
    // -0 reads back as 0: written as 0, so the file is the same either way.
    return nph_json_writer_f32_shortest(w, v == 0.0F ? 0.0F : v);
}

static NphResult write_numbers(NphJsonWriter *w, const f32 *v, u32 n)
{
    NPH_TRY(nph_json_writer_begin_array_inline(w));
    for (u32 i = 0; i < n; ++i) {
        NPH_TRY(write_number(w, v[i]));
    }
    return nph_json_writer_end_array(w);
}

static NphResult write_key_number(NphJsonWriter *w, Key key, f32 v)
{
    NPH_TRY(nph_json_writer_key(w, nph_str_from_cstr(k_keys[key])));
    return write_number(w, v);
}

static NphResult write_key_numbers(NphJsonWriter *w, Key key, const f32 *v, u32 n)
{
    NPH_TRY(nph_json_writer_key(w, nph_str_from_cstr(k_keys[key])));
    return write_numbers(w, v, n);
}

static NphResult write_texture(NphJsonWriter *w, const CookMaterialTexture *t)
{
    NPH_TRY(nph_json_writer_begin_object(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("image")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(t->image)));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("wrap")));
    NPH_TRY(nph_json_writer_begin_array_inline(w));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(k_wrap_names[t->wrap_u])));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(k_wrap_names[t->wrap_v])));
    NPH_TRY(nph_json_writer_end_array(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("filter")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(k_filter_names[t->nearest])));
    return nph_json_writer_end_object(w);
}

static NphResult write_document(const CookMaterialSource *m, NphJsonWriter *w)
{
    NPH_TRY(nph_json_writer_begin_object(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("nmat")));
    NPH_TRY(nph_json_writer_i64(w, (i64)COOK_MATERIAL_SOURCE_VERSION));
    const f32 base[4] = {m->base_color.x, m->base_color.y, m->base_color.z, m->base_color.w};
    NPH_TRY(write_key_numbers(w, KEY_BASE_COLOR, base, 4U));
    NPH_TRY(write_key_number(w, KEY_METALLIC, m->metallic));
    NPH_TRY(write_key_number(w, KEY_ROUGHNESS, m->roughness));
    const f32 emissive[3] = {m->emissive.x, m->emissive.y, m->emissive.z};
    NPH_TRY(write_key_numbers(w, KEY_EMISSIVE, emissive, 3U));
    NPH_TRY(write_key_number(w, KEY_EMISSIVE_STRENGTH, m->emissive_strength));
    NPH_TRY(write_key_number(w, KEY_NORMAL_SCALE, m->normal_scale));
    NPH_TRY(write_key_number(w, KEY_OCCLUSION_STRENGTH, m->occlusion_strength));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("alpha")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(k_alpha_names[m->alpha])));
    NPH_TRY(write_key_number(w, KEY_ALPHA_CUTOFF, m->alpha_cutoff));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("double_sided")));
    NPH_TRY(nph_json_writer_bool(w, m->double_sided != 0));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("unlit")));
    NPH_TRY(nph_json_writer_bool(w, m->unlit != 0));
    const f32 tiling[2] = {m->tiling.x, m->tiling.y};
    NPH_TRY(write_key_numbers(w, KEY_TILING, tiling, 2U));
    NPH_TRY(write_key_number(w, KEY_ROTATION, m->rotation));
    const f32 offset[2] = {m->offset.x, m->offset.y};
    NPH_TRY(write_key_numbers(w, KEY_OFFSET, offset, 2U));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("textures")));
    NPH_TRY(nph_json_writer_begin_object(w));
    for (u32 slot = 0; slot < NPH_MATERIAL_TEXTURE_COUNT; ++slot) {
        if (m->textures[slot].image[0] == '\0') {
            continue;
        }
        NPH_TRY(nph_json_writer_key(w, nph_str_from_cstr(k_slot_names[slot])));
        NPH_TRY(write_texture(w, &m->textures[slot]));
    }
    NPH_TRY(nph_json_writer_end_object(w));
    NPH_TRY(nph_json_writer_end_object(w));
    return nph_sb_append_cstr(w->out, "\n");
}

NphResult cook_material_source_write(const CookMaterialSource *m, NphStrBuilder *out)
{
    NPH_ASSERT(m != NULL && out != NULL);
    NPH_TRY(cook_material_source_validate(m));
    NphJsonWriter w;
    nph_json_writer_init(&w, out, true);
    return write_document(m, &w);
}

NphResult cook_material_source_save(const CookMaterialSource *m, NphAllocator *alloc,
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
    NphResult r = cook_material_source_write(m, &sb);
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

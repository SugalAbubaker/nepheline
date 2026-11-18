// SPDX-License-Identifier: Apache-2.0
// Import settings for images (cook/texture_import.h; ADR-0105).
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>

#include <cook/texture_import.h>
#include <stdio.h>
#include <string.h>

#define SAVE_PATH_MAX 4096U

static const char *const k_usage_names[] = {"color", "linear", "normal", "mask", "hdr"};
static const char *const k_green_names[] = {"up", "down"};

typedef enum Key {
    KEY_TEXTURE = 0,
    KEY_USAGE,
    KEY_MAX_EXTENT,
    KEY_UNCOMPRESSED,
    KEY_MIPS,
    KEY_NORMAL_GREEN,
    KEY_COUNT
} Key;

static const char *const k_keys[KEY_COUNT] = {"texture",      "usage", "max_extent",
                                              "uncompressed", "mips",  "normal_green"};

void cook_texture_import_default(CookTextureImport *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->usage = COOK_TEXTURE_COLOR;
}

bool cook_texture_import_equal(const CookTextureImport *a, const CookTextureImport *b)
{
    NPH_ASSERT(a != NULL && b != NULL);
    // What is not set is not part of the settings.
    return a->has_usage == b->has_usage && (a->has_usage == 0 || a->usage == b->usage) &&
           a->has_max_extent == b->has_max_extent &&
           (a->has_max_extent == 0 || a->max_extent == b->max_extent) &&
           a->has_uncompressed == b->has_uncompressed &&
           (a->has_uncompressed == 0 || a->uncompressed == b->uncompressed) &&
           a->has_mips == b->has_mips && (a->has_mips == 0 || a->mips == b->mips) &&
           a->green_down == b->green_down;
}

CookTextureSettings cook_texture_import_apply(const CookTextureImport *s,
                                              const CookTextureSettings *base, bool alone)
{
    NPH_ASSERT(s != NULL && base != NULL);
    CookTextureSettings out = *base;
    if (alone && s->has_usage != 0) {
        out.usage = s->usage;
    }
    if (s->has_max_extent != 0) {
        out.max_extent = s->max_extent;
    }
    if (s->has_uncompressed != 0) {
        out.uncompressed = s->uncompressed;
    }
    if (s->has_mips != 0) {
        out.no_mips = s->mips != 0 ? 0 : 1;
    }
    return out;
}

static bool valid(const CookTextureImport *s)
{
    return s->has_usage >= 0 && s->has_usage <= 1 && (u32)s->usage <= (u32)COOK_TEXTURE_HDR &&
           s->has_max_extent >= 0 && s->has_max_extent <= 1 &&
           s->max_extent <= COOK_TEXTURE_IMPORT_MAX_EXTENT && s->has_uncompressed >= 0 &&
           s->has_uncompressed <= 1 && s->uncompressed >= 0 && s->uncompressed <= 1 &&
           s->has_mips >= 0 && s->has_mips <= 1 && s->mips >= 0 && s->mips <= 1 &&
           s->green_down >= 0 && s->green_down <= 1;
}

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

static NphResult read_bool(const NphJsonValue *v, b32 *out)
{
    bool b = false;
    if (nph_json_as_bool(v, &b) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out = b ? 1 : 0;
    return NPH_RESULT_OK;
}

static NphResult read_member(CookTextureImport *s, Key key, const NphJsonValue *v)
{
    u32 name = 0U;
    i64 extent = 0;
    switch (key) {
    case KEY_TEXTURE:
        return NPH_RESULT_OK; // read first
    case KEY_USAGE:
        NPH_TRY(read_name(v, k_usage_names, (u32)NPH_ARRAY_COUNT(k_usage_names), &name));
        s->has_usage = 1;
        s->usage = (CookTextureUsage)name;
        return NPH_RESULT_OK;
    case KEY_MAX_EXTENT:
        if (nph_json_as_i64(v, &extent) != NPH_RESULT_OK || extent < 0 ||
            extent > (i64)COOK_TEXTURE_IMPORT_MAX_EXTENT) {
            return NPH_RESULT_PARSE_ERROR;
        }
        s->has_max_extent = 1;
        s->max_extent = (u32)extent;
        return NPH_RESULT_OK;
    case KEY_UNCOMPRESSED:
        s->has_uncompressed = 1;
        return read_bool(v, &s->uncompressed);
    case KEY_MIPS:
        s->has_mips = 1;
        return read_bool(v, &s->mips);
    case KEY_NORMAL_GREEN:
        NPH_TRY(read_name(v, k_green_names, (u32)NPH_ARRAY_COUNT(k_green_names), &name));
        s->green_down = name == 1U ? 1 : 0;
        return NPH_RESULT_OK;
    case KEY_COUNT:
        break;
    }
    return NPH_RESULT_PARSE_ERROR;
}

static NphResult read_document(CookTextureImport *s, const NphJsonValue *root)
{
    if (root == NULL || root->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    i64 version = 0;
    if (nph_json_as_i64(nph_json_object_get(root, NPH_STR("texture")), &version) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    if (version != (i64)COOK_TEXTURE_IMPORT_VERSION) {
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
        NPH_TRY(read_member(s, (Key)k, e));
    }
    return NPH_RESULT_OK;
}

NphResult cook_texture_import_read(NphAllocator *alloc, NphStr text, CookTextureImport *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    cook_texture_import_default(out);
    NphJsonDoc doc;
    NphResult r = nph_json_parse(text, alloc, &doc);
    if (r == NPH_RESULT_OK) {
        r = read_document(out, doc.root);
    } else if (r != NPH_RESULT_OUT_OF_MEMORY) {
        r = NPH_RESULT_PARSE_ERROR;
    }
    nph_json_doc_free(&doc);
    if (r != NPH_RESULT_OK) {
        cook_texture_import_default(out);
    }
    return r;
}

NphResult cook_texture_import_write(const CookTextureImport *s, NphStrBuilder *out)
{
    NPH_ASSERT(s != NULL && out != NULL);
    if (!valid(s)) {
        return NPH_RESULT_CORRUPT;
    }
    NphJsonWriter w;
    nph_json_writer_init(&w, out, true);
    NPH_TRY(nph_json_writer_begin_object(&w));
    NPH_TRY(nph_json_writer_key(&w, NPH_STR("texture")));
    NPH_TRY(nph_json_writer_i64(&w, (i64)COOK_TEXTURE_IMPORT_VERSION));
    if (s->has_usage != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("usage")));
        NPH_TRY(nph_json_writer_str(&w, nph_str_from_cstr(k_usage_names[s->usage])));
    }
    if (s->has_max_extent != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("max_extent")));
        NPH_TRY(nph_json_writer_i64(&w, (i64)s->max_extent));
    }
    if (s->has_uncompressed != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("uncompressed")));
        NPH_TRY(nph_json_writer_bool(&w, s->uncompressed != 0));
    }
    if (s->has_mips != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("mips")));
        NPH_TRY(nph_json_writer_bool(&w, s->mips != 0));
    }
    if (s->green_down != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("normal_green")));
        NPH_TRY(nph_json_writer_str(&w, NPH_STR("down")));
    }
    NPH_TRY(nph_json_writer_end_object(&w));
    return nph_sb_append_cstr(out, "\n");
}

NphResult cook_texture_import_save(const CookTextureImport *s, NphAllocator *alloc,
                                   const char *path)
{
    NPH_ASSERT(s != NULL && alloc != NULL && path != NULL);
    char temp[SAVE_PATH_MAX];
    const int n = snprintf(temp, sizeof temp, "%s.part", path);
    if (n < 0 || (u64)n >= sizeof temp) {
        return NPH_RESULT_OVERFLOW;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    NphResult r = cook_texture_import_write(s, &sb);
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

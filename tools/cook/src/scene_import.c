// SPDX-License-Identifier: Apache-2.0
// Import settings for scenes (cook/scene_import.h; ADR-0107).
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>

#include <cook/scene_import.h>
#include <stdio.h>
#include <string.h>

#define SAVE_PATH_MAX 4096U

static const char *const k_collision_names[COOK_COLLISION_KIND_COUNT] = {"none", "mesh", "hull"};

typedef enum Key {
    KEY_SCENE = 0,
    KEY_COLLISION,
    KEY_HULL_VERTICES,
    KEY_LODS,
    KEY_VOXELS,
    KEY_TEXTURE_MAX_EXTENT,
    KEY_TEXTURE_UNCOMPRESSED,
    KEY_TEXTURE_MIPS,
    KEY_COUNT
} Key;

static const char *const k_keys[KEY_COUNT] = {
    "scene",  "collision",          "hull_vertices",        "lods",
    "voxels", "texture_max_extent", "texture_uncompressed", "texture_mips"};

void cook_scene_import_default(CookSceneImport *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->collision = COOK_COLLISION_NONE;
    out->hull_vertices = COOK_SCENE_HULL_VERTICES_DEFAULT;
}

// Two settings of one key: the same when neither is set, or both are, to the same value.
static bool same(b32 has_a, u32 a, b32 has_b, u32 b)
{
    return has_a == has_b && (has_a == 0 || a == b);
}

bool cook_scene_import_equal(const CookSceneImport *a, const CookSceneImport *b)
{
    NPH_ASSERT(a != NULL && b != NULL);
    return same(a->has_collision, (u32)a->collision, b->has_collision, (u32)b->collision) &&
           same(a->has_hull_vertices, a->hull_vertices, b->has_hull_vertices, b->hull_vertices) &&
           same(a->has_lods, (u32)a->lods, b->has_lods, (u32)b->lods) &&
           same(a->has_voxels, (u32)a->voxels, b->has_voxels, (u32)b->voxels) &&
           same(a->has_texture_max_extent, a->texture_max_extent, b->has_texture_max_extent,
                b->texture_max_extent) &&
           same(a->has_texture_uncompressed, (u32)a->texture_uncompressed,
                b->has_texture_uncompressed, (u32)b->texture_uncompressed) &&
           same(a->has_texture_mips, (u32)a->texture_mips, b->has_texture_mips,
                (u32)b->texture_mips);
}

CookMeshSettings cook_scene_import_mesh(const CookSceneImport *s, const CookMeshSettings *base)
{
    NPH_ASSERT(s != NULL && base != NULL);
    CookMeshSettings out = *base;
    if (s->has_lods != 0) {
        out.no_lods = s->lods != 0 ? 0 : 1;
    }
    if (s->has_voxels != 0) {
        out.voxels = s->voxels != 0 ? 1 : 0;
    }
    return out;
}

CookTextureSettings cook_scene_import_texture(const CookSceneImport *s,
                                              const CookTextureSettings *base)
{
    NPH_ASSERT(s != NULL && base != NULL);
    CookTextureSettings out = *base;
    if (s->has_texture_max_extent != 0) {
        out.max_extent = s->texture_max_extent;
    }
    if (s->has_texture_uncompressed != 0) {
        out.uncompressed = s->texture_uncompressed != 0 ? 1 : 0;
    }
    if (s->has_texture_mips != 0) {
        out.no_mips = s->texture_mips != 0 ? 0 : 1;
    }
    return out;
}

CookCollisionKind cook_scene_import_collision(const CookSceneImport *s)
{
    NPH_ASSERT(s != NULL);
    return s->has_collision != 0 ? s->collision : COOK_COLLISION_NONE;
}

u32 cook_scene_import_hull_vertices(const CookSceneImport *s)
{
    NPH_ASSERT(s != NULL);
    return s->has_hull_vertices != 0 ? s->hull_vertices : COOK_SCENE_HULL_VERTICES_DEFAULT;
}

const char *cook_collision_kind_name(CookCollisionKind kind)
{
    return (u32)kind < (u32)COOK_COLLISION_KIND_COUNT ? k_collision_names[kind] : "none";
}

bool cook_collision_kind_parse(NphStr name, CookCollisionKind *out)
{
    NPH_ASSERT(out != NULL);
    for (u32 i = 0; i < (u32)COOK_COLLISION_KIND_COUNT; ++i) {
        if (nph_str_equals(name, nph_str_from_cstr(k_collision_names[i]))) {
            *out = (CookCollisionKind)i;
            return true;
        }
    }
    return false;
}

static bool flag_ok(b32 v)
{
    return v == 0 || v == 1;
}

static bool valid(const CookSceneImport *s)
{
    return flag_ok(s->has_collision) && (u32)s->collision < (u32)COOK_COLLISION_KIND_COUNT &&
           flag_ok(s->has_hull_vertices) &&
           (s->has_hull_vertices == 0 || (s->hull_vertices >= COOK_SCENE_HULL_VERTICES_MIN &&
                                          s->hull_vertices <= COOK_SCENE_HULL_VERTICES_MAX)) &&
           flag_ok(s->has_lods) && flag_ok(s->lods) && flag_ok(s->has_voxels) &&
           flag_ok(s->voxels) && flag_ok(s->has_texture_max_extent) &&
           s->texture_max_extent <= COOK_SCENE_IMPORT_MAX_EXTENT &&
           flag_ok(s->has_texture_uncompressed) && flag_ok(s->texture_uncompressed) &&
           flag_ok(s->has_texture_mips) && flag_ok(s->texture_mips);
}

static u32 key_index(NphStr name)
{
    for (u32 i = 0; i < KEY_COUNT; ++i) {
        if (nph_str_equals(name, nph_str_from_cstr(k_keys[i]))) {
            return i;
        }
    }
    return KEY_COUNT;
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

static NphResult read_u32(const NphJsonValue *v, u32 lo, u32 hi, u32 *out)
{
    i64 n = 0;
    if (nph_json_as_i64(v, &n) != NPH_RESULT_OK || n < (i64)lo || n > (i64)hi) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out = (u32)n;
    return NPH_RESULT_OK;
}

static NphResult read_member(CookSceneImport *s, Key key, const NphJsonValue *v)
{
    NphStr name;
    switch (key) {
    case KEY_SCENE:
        return NPH_RESULT_OK; // read first
    case KEY_COLLISION:
        if (nph_json_as_str(v, &name) != NPH_RESULT_OK ||
            !cook_collision_kind_parse(name, &s->collision)) {
            return NPH_RESULT_PARSE_ERROR;
        }
        s->has_collision = 1;
        return NPH_RESULT_OK;
    case KEY_HULL_VERTICES:
        s->has_hull_vertices = 1;
        return read_u32(v, COOK_SCENE_HULL_VERTICES_MIN, COOK_SCENE_HULL_VERTICES_MAX,
                        &s->hull_vertices);
    case KEY_LODS:
        s->has_lods = 1;
        return read_bool(v, &s->lods);
    case KEY_VOXELS:
        s->has_voxels = 1;
        return read_bool(v, &s->voxels);
    case KEY_TEXTURE_MAX_EXTENT:
        s->has_texture_max_extent = 1;
        return read_u32(v, 0U, COOK_SCENE_IMPORT_MAX_EXTENT, &s->texture_max_extent);
    case KEY_TEXTURE_UNCOMPRESSED:
        s->has_texture_uncompressed = 1;
        return read_bool(v, &s->texture_uncompressed);
    case KEY_TEXTURE_MIPS:
        s->has_texture_mips = 1;
        return read_bool(v, &s->texture_mips);
    case KEY_COUNT:
        break;
    }
    return NPH_RESULT_PARSE_ERROR;
}

static NphResult read_document(CookSceneImport *s, const NphJsonValue *root)
{
    if (root == NULL || root->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    i64 version = 0;
    if (nph_json_as_i64(nph_json_object_get(root, NPH_STR("scene")), &version) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    if (version != (i64)COOK_SCENE_IMPORT_VERSION) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    u32 seen = 0U;
    NPH_JSON_FOREACH(e, root)
    {
        const u32 k = key_index(e->key);
        if (k == KEY_COUNT || (seen & (1U << k)) != 0U) {
            return NPH_RESULT_PARSE_ERROR;
        }
        seen |= 1U << k;
        NPH_TRY(read_member(s, (Key)k, e));
    }
    return NPH_RESULT_OK;
}

NphResult cook_scene_import_read(NphAllocator *alloc, NphStr text, CookSceneImport *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    cook_scene_import_default(out);
    NphJsonDoc doc;
    NphResult r = nph_json_parse(text, alloc, &doc);
    if (r == NPH_RESULT_OK) {
        r = read_document(out, doc.root);
    } else if (r != NPH_RESULT_OUT_OF_MEMORY) {
        r = NPH_RESULT_PARSE_ERROR;
    }
    nph_json_doc_free(&doc);
    if (r != NPH_RESULT_OK) {
        cook_scene_import_default(out);
    }
    return r;
}

NphResult cook_scene_import_write(const CookSceneImport *s, NphStrBuilder *out)
{
    NPH_ASSERT(s != NULL && out != NULL);
    if (!valid(s)) {
        return NPH_RESULT_CORRUPT;
    }
    NphJsonWriter w;
    nph_json_writer_init(&w, out, true);
    NPH_TRY(nph_json_writer_begin_object(&w));
    NPH_TRY(nph_json_writer_key(&w, NPH_STR("scene")));
    NPH_TRY(nph_json_writer_i64(&w, (i64)COOK_SCENE_IMPORT_VERSION));
    if (s->has_collision != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("collision")));
        NPH_TRY(nph_json_writer_str(&w, nph_str_from_cstr(cook_collision_kind_name(s->collision))));
    }
    if (s->has_hull_vertices != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("hull_vertices")));
        NPH_TRY(nph_json_writer_i64(&w, (i64)s->hull_vertices));
    }
    if (s->has_lods != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("lods")));
        NPH_TRY(nph_json_writer_bool(&w, s->lods != 0));
    }
    if (s->has_voxels != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("voxels")));
        NPH_TRY(nph_json_writer_bool(&w, s->voxels != 0));
    }
    if (s->has_texture_max_extent != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("texture_max_extent")));
        NPH_TRY(nph_json_writer_i64(&w, (i64)s->texture_max_extent));
    }
    if (s->has_texture_uncompressed != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("texture_uncompressed")));
        NPH_TRY(nph_json_writer_bool(&w, s->texture_uncompressed != 0));
    }
    if (s->has_texture_mips != 0) {
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("texture_mips")));
        NPH_TRY(nph_json_writer_bool(&w, s->texture_mips != 0));
    }
    NPH_TRY(nph_json_writer_end_object(&w));
    return nph_sb_append_cstr(out, "\n");
}

NphResult cook_scene_import_save(const CookSceneImport *s, NphAllocator *alloc, const char *path)
{
    NPH_ASSERT(s != NULL && alloc != NULL && path != NULL);
    char temp[SAVE_PATH_MAX];
    const int n = snprintf(temp, sizeof temp, "%s.part", path);
    if (n < 0 || (u64)n >= sizeof temp) {
        return NPH_RESULT_OVERFLOW;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    NphResult r = cook_scene_import_write(s, &sb);
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

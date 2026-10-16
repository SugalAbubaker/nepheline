// SPDX-License-Identifier: Apache-2.0
// The cooker (cooker.h): source keys, output paths, the incremental
// decisions, the glTF and image drivers, and packing.
#include <nepheline/asset/scene.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>

#include <cook/cooker.h>
#include <cook/gltf.h>
#include <cook/image.h>
#include <cook/polymesh.h>
#include <stdio.h>
#include <string.h>

typedef NPH_ARRAY(NphGuid) GuidArray;

struct Cooker {
    NphAllocator *alloc;
    NphJobSystem *jobs;
    char out_dir[COOKER_PATH_MAX];
    char root[COOKER_PATH_MAX]; // absolute and canonical (canonical_path)
    char db_path[COOKER_PATH_MAX + 32U];
    CookDb db;
    CookTextureSettings texture;
    CookMeshSettings mesh;
    b32 force;
    CookerStats stats;
    void (*on_cooked)(void *user, NphGuid guid, u32 type);
    void *on_cooked_user;
};

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

NphResult cooker_canonical_path(const char *in, char *out, u64 cap)
{
    NPH_ASSERT(in != NULL && out != NULL);
    char joined[COOKER_PATH_MAX * 2U];
    if (nph_path_is_absolute(nph_str_from_cstr(in))) {
        const u64 n = strlen(in);
        if (n >= sizeof joined) {
            return NPH_RESULT_OVERFLOW;
        }
        memcpy(joined, in, (size_t)n + 1U);
    } else {
        char cwd[COOKER_PATH_MAX];
        NPH_TRY(nph_fs_current_directory(cwd, sizeof cwd, NULL));
        NPH_TRY(nph_path_join(joined, sizeof joined, nph_str_from_cstr(cwd), nph_str_from_cstr(in),
                              NULL));
    }
    nph_path_normalize_separators(joined);
    // The root: "X:" on Windows, "//server/share" for UNC, "" for POSIX "/".
    u64 pos = 0U;
    u64 w = 0U;
    if (joined[0] != '\0' && joined[1] == ':') {
        pos = 2U;
    } else if (joined[0] == '/' && joined[1] == '/') {
        // //server/share
        u64 slashes = 0U;
        pos = 2U;
        while (joined[pos] != '\0' && slashes < 2U) {
            if (joined[pos] == '/') {
                ++slashes;
                if (slashes == 2U) {
                    break;
                }
            }
            ++pos;
        }
    }
    if (pos + 1U > cap) {
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(out, joined, (size_t)pos);
    w = pos;
    const u64 root_len = pos;
    // The components.
    while (joined[pos] != '\0') {
        while (joined[pos] == '/') {
            ++pos;
        }
        const u64 start = pos;
        while (joined[pos] != '\0' && joined[pos] != '/') {
            ++pos;
        }
        const u64 len = pos - start;
        if (len == 0U || (len == 1U && joined[start] == '.')) {
            continue;
        }
        if (len == 2U && joined[start] == '.' && joined[start + 1U] == '.') {
            while (w > root_len && out[w - 1U] != '/') {
                --w;
            }
            if (w > root_len) {
                --w; // the separator before the dropped component
            }
            continue;
        }
        if (w + 1U + len + 1U > cap) {
            return NPH_RESULT_OVERFLOW;
        }
        out[w++] = '/';
        memcpy(out + w, joined + start, (size_t)len);
        w += len;
    }
    if (w == root_len) {
        if (w + 2U > cap) {
            return NPH_RESULT_OVERFLOW;
        }
        out[w++] = '/'; // the root itself
    }
    out[w] = '\0';
    return NPH_RESULT_OK;
}

static bool chars_equal(char a, char b)
{
#if NPH_PLATFORM_WINDOWS
    // Windows file names compare without case; keys keep the spelling given.
    return nph_ascii_to_lower(a) == nph_ascii_to_lower(b);
#else
    return a == b;
#endif
}

NphResult cooker_relative_path(const char *root, const char *path, char *out, u64 cap)
{
    NPH_ASSERT(root != NULL && path != NULL && out != NULL && cap != 0U);
    const u64 rn = strlen(root);
    for (u64 i = 0; i < rn; ++i) {
        // A path shorter than the root ends inside this loop.
        if (path[i] == '\0' || !chars_equal(path[i], root[i])) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    u64 start = rn;
    // The root "X:/" or "/" ends in a separator; any other root does not,
    // and the path continues with one or ends.
    if (path[rn] != '\0' && (rn == 0U || root[rn - 1U] != '/')) {
        if (path[rn] != '/') {
            return NPH_RESULT_INVALID_ARGUMENT; // "/a/bc" is not under "/a/b"
        }
        start = rn + 1U;
    }
    const u64 n = strlen(path + start);
    if (n + 1U > cap) {
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(out, path + start, (size_t)n + 1U);
    return NPH_RESULT_OK;
}

NphResult cooker_source_key(const Cooker *c, const char *path, char *out, u64 cap)
{
    NPH_ASSERT(c != NULL && path != NULL && out != NULL && cap != 0U);
    char abs[COOKER_PATH_MAX];
    NPH_TRY(cooker_canonical_path(path, abs, sizeof abs));
    NPH_TRY(cooker_relative_path(c->root, abs, out, cap));
    // The root itself is no source.
    return out[0] != '\0' ? NPH_RESULT_OK : NPH_RESULT_INVALID_ARGUMENT;
}

// Keeps letters, digits, '-', '_' and '.'; everything else becomes '_'.
static void sanitize(const char *in, u64 n, char *out, u64 cap)
{
    u64 w = 0U;
    for (u64 i = 0; i < n && w + 1U < cap; ++i) {
        const char ch = in[i];
        const bool ok =
            nph_ascii_is_alpha(ch) || nph_ascii_is_digit(ch) || ch == '-' || ch == '_' || ch == '.';
        out[w++] = (char)(ok ? ch : '_');
    }
    if (w == 0U && cap >= 2U) {
        out[w++] = '_';
    }
    out[w] = '\0';
}

// The output directory of a source: its key, each component sanitised,
// the extension's '.' made '_' ("models/crate.gltf" -> "models/crate_gltf").
static void output_prefix(const char *key, char *out, u64 cap)
{
    u64 w = 0U;
    const char *p = key;
    out[0] = '\0';
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        const u64 len = slash != NULL ? (u64)(slash - p) : strlen(p);
        char comp[256];
        sanitize(p, nph_min_u64(len, sizeof comp - 1U), comp, sizeof comp);
        if (slash == NULL) {
            char *dot = strrchr(comp, '.');
            if (dot != NULL && dot != comp) {
                *dot = '_';
            }
        }
        const u64 cn = strlen(comp);
        if (w + cn + 2U > cap) {
            break; // overlong keys are cut (the GUID suffix keeps them apart)
        }
        if (w != 0U) {
            out[w++] = '/';
        }
        memcpy(out + w, comp, (size_t)cn);
        w += cn;
        out[w] = '\0';
        if (slash == NULL) {
            break;
        }
        p = slash + 1;
    }
}

static NphGuid asset_guid(const char *key, const char *sub)
{
    char text[COOKER_PATH_MAX + 300U];
    snprintf(text, sizeof text, "%s#%s", key, sub);
    return nph_guid_from_cstr(text);
}

// The output path of an asset: "<prefix>/<file>.nph", with a GUID suffix
// when another asset already owns that path.
static void output_path(const Cooker *c, const char *key, const char *sub, const char *file,
                        NphGuid guid, char *out, u64 cap)
{
    char prefix[COOKER_PATH_MAX];
    output_prefix(key, prefix, sizeof prefix);
    char name[200];
    sanitize(file, strlen(file), name, sizeof name);
    snprintf(out, (size_t)cap, "%s/%s.nph", prefix, name);
    const CookDbEntry *owner = cook_db_find_output(&c->db, nph_str_from_cstr(out));
    if (owner != NULL && (strcmp(owner->source, key) != 0 || strcmp(owner->sub, sub) != 0)) {
        snprintf(out, (size_t)cap, "%s/%s_%08x.nph", prefix, name, (u32)(guid.hi >> 32U));
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult cooker_create(const CookerDesc *desc, Cooker **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->out_dir == NULL || desc->out_dir[0] == '\0') {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Cooker *c = NPH_ALLOC_TYPE(desc->alloc, Cooker);
    if (c == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(c, 0, sizeof *c);
    c->alloc = desc->alloc;
    c->jobs = desc->jobs;
    c->force = desc->force;
    c->texture = desc->texture;
    c->mesh = desc->mesh;
    c->on_cooked = desc->on_cooked;
    c->on_cooked_user = desc->on_cooked_user;
    NphResult r = cooker_canonical_path(desc->out_dir, c->out_dir, sizeof c->out_dir);
    if (r == NPH_RESULT_OK) {
        r = cooker_canonical_path(desc->source_root != NULL ? desc->source_root : ".", c->root,
                                  sizeof c->root);
    }
    if (r == NPH_RESULT_OK && (u64)snprintf(c->db_path, sizeof c->db_path, "%s/%s", c->out_dir,
                                            COOK_DB_FILE_NAME) >= sizeof c->db_path) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(desc->alloc, Cooker, c);
        return r == NPH_RESULT_OVERFLOW ? NPH_RESULT_INVALID_ARGUMENT : r;
    }
    r = nph_fs_create_directories(c->out_dir);
    bool db_ready = false;
    if (r == NPH_RESULT_OK) {
        r = cook_db_init(&c->db, c->alloc);
        db_ready = r == NPH_RESULT_OK;
    }
    if (r == NPH_RESULT_OK) {
        const NphResult lr = cook_db_load(&c->db, c->db_path);
        if (lr != NPH_RESULT_OK) {
            // The database leaves itself empty: everything recooks.
            NPH_LOG_WARN(&NPH_LOG_COOK, "%s: %s; starting with an empty database", c->db_path,
                         nph_result_string(lr));
            c->db.dirty = 1;
        }
    }
    if (r != NPH_RESULT_OK) {
        if (db_ready) {
            cook_db_free(&c->db);
        }
        NPH_FREE_TYPE(desc->alloc, Cooker, c);
        return r;
    }
    *out = c;
    return NPH_RESULT_OK;
}

void cooker_destroy(Cooker *c)
{
    if (c == NULL) {
        return;
    }
    const NphResult r = cook_db_save(&c->db, c->db_path, false);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: cannot save the database (%s)", c->db_path,
                      nph_result_string(r));
    }
    cook_db_free(&c->db);
    NPH_FREE_TYPE(c->alloc, Cooker, c);
}

CookerStats cooker_stats(const Cooker *c)
{
    NPH_ASSERT(c != NULL);
    return c->stats;
}

const CookDb *cooker_db(const Cooker *c)
{
    NPH_ASSERT(c != NULL);
    return &c->db;
}

// ---------------------------------------------------------------------------
// The incremental decisions
// ---------------------------------------------------------------------------

// The settings hash: everything that changes the output for the same
// source, so a settings change recooks.
// The payload format version of each type the cooker writes (a table: the
// versions are all 1 today, and a switch over equal constants is a clone).
static u32 type_version(u32 type)
{
    static const u32 versions[][2] = {
        {NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION},
        {NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION},
        {NPH_ASSET_TYPE_MATERIAL, NPH_MATERIAL_TYPE_VERSION},
        {NPH_ASSET_TYPE_SCENE, NPH_SCENE_TYPE_VERSION},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(versions); ++i) {
        if (versions[i][0] == type) {
            return versions[i][1];
        }
    }
    return 0U;
}

static u64 settings_hash(const Cooker *c, u32 type)
{
    u64 h = nph_hash_mix_u64(COOK_VERSION);
    h = nph_hash_combine(h, type);
    h = nph_hash_combine(h, NPH_ASSET_FORMAT_VERSION);
    h = nph_hash_combine(h, type_version(type));
    if (type == NPH_ASSET_TYPE_TEXTURE) {
        CookTextureSettings t = c->texture;
        t.usage = COOK_TEXTURE_COLOR; // the usage is folded in per texture
        h = nph_hash_combine(h, nph_hash64(&t, sizeof t, 1U));
    } else if (type == NPH_ASSET_TYPE_MESH) {
        h = nph_hash_combine(h, nph_hash64(&c->mesh, sizeof c->mesh, 2U));
    }
    return h;
}

// True when the database holds a current entry whose output exists.
static bool is_current(const Cooker *c, const char *key, const char *sub, u64 content_hash,
                       u64 shash)
{
    if (c->force) {
        return false;
    }
    const CookDbEntry *e = cook_db_find(&c->db, nph_str_from_cstr(key), nph_str_from_cstr(sub));
    if (!cook_db_entry_is_current(e, content_hash, shash)) {
        return false;
    }
    char full[COOKER_PATH_MAX * 2U];
    snprintf(full, sizeof full, "%s/%s", c->out_dir, e->output);
    return nph_fs_exists(full);
}

// Writes a finished asset to its output path (creating directories) and
// records it in the database.
static NphResult emit(Cooker *c, NphAssetWriter *w, const char *key, const char *sub,
                      const char *relative, u32 type, u64 content_hash, u64 shash,
                      const NphGuid *deps, u32 dep_count)
{
    char full[COOKER_PATH_MAX * 2U];
    if ((u64)snprintf(full, sizeof full, "%s/%s", c->out_dir, relative) >= sizeof full) {
        return NPH_RESULT_OVERFLOW;
    }
    char dir[COOKER_PATH_MAX * 2U];
    const NphStr d = nph_path_dirname(nph_str_from_cstr(full));
    memcpy(dir, d.data, (size_t)d.len);
    dir[d.len] = '\0';
    NPH_TRY(nph_fs_create_directories(dir));
    NPH_TRY(nph_asset_writer_write_file(w, full));
    // An asset of this key that moved to another output leaves its old file.
    const CookDbEntry *previous =
        cook_db_find(&c->db, nph_str_from_cstr(key), nph_str_from_cstr(sub));
    if (previous != NULL && strcmp(previous->output, relative) != 0) {
        char old[COOKER_PATH_MAX * 2U];
        snprintf(old, sizeof old, "%s/%s", c->out_dir, previous->output);
        NPH_IGNORE_RESULT(nph_fs_remove_file(old));
    }
    NPH_TRY(cook_db_set(&c->db, nph_str_from_cstr(key), nph_str_from_cstr(sub),
                        nph_str_from_cstr(relative), w->header.guid, type, content_hash, shash,
                        deps, dep_count));
    c->stats.cooked += 1U;
    if (c->on_cooked != NULL) {
        c->on_cooked(c->on_cooked_user, w->header.guid, type);
    }
    NPH_LOG_INFO(&NPH_LOG_COOK, "cooked %s", relative);
    return NPH_RESULT_OK;
}

static bool guid_in(const GuidArray *list, NphGuid g)
{
    NPH_ARRAY_FOREACH (NphGuid, it, list) {
        if (nph_guid_equals(*it, g)) {
            return true;
        }
    }
    return false;
}

// Drops the database entries of `key` that this cook did not produce, and
// their files: a mesh deleted from a glTF must not linger in the pack.
static void remove_stale(Cooker *c, const char *key, const GuidArray *produced)
{
    for (u32 i = 0; i < cook_db_count(&c->db);) {
        const CookDbEntry *e = cook_db_entry(&c->db, i);
        if (strcmp(e->source, key) != 0 || guid_in(produced, e->guid)) {
            ++i;
            continue;
        }
        char full[COOKER_PATH_MAX * 2U];
        snprintf(full, sizeof full, "%s/%s", c->out_dir, e->output);
        NPH_IGNORE_RESULT(nph_fs_remove_file(full));
        NPH_LOG_INFO(&NPH_LOG_COOK, "removed %s (no longer in %s)", e->output, key);
        char sub[256];
        snprintf(sub, sizeof sub, "%s", e->sub);
        NPH_IGNORE_RESULT(cook_db_remove(&c->db, nph_str_from_cstr(key), nph_str_from_cstr(sub)));
        c->stats.removed += 1U;
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static const char *const usage_names[] = {"color", "linear", "normal", "mask", "hdr"};

// Cooks encoded image bytes as a texture asset under `sub`.
static NphResult cook_texture_bytes(Cooker *c, const char *key, const char *sub, const char *file,
                                    const void *bytes, u64 size, CookTextureUsage usage,
                                    u64 content_hash, NphGuid *out_guid)
{
    CookTextureSettings settings = c->texture;
    settings.usage = usage;
    const u64 shash = nph_hash_combine(settings_hash(c, NPH_ASSET_TYPE_TEXTURE), (u64)usage);
    *out_guid = asset_guid(key, sub);
    if (is_current(c, key, sub, content_hash, shash)) {
        c->stats.skipped += 1U;
        return NPH_RESULT_OK;
    }
    CookImage image;
    NphResult r = cook_image_decode(c->alloc, bytes, size, &image);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK,
                      "%s: %s: not an image the cooker reads (PNG, JPEG, TGA, "
                      "BMP, Radiance HDR): %s",
                      key, sub[0] != '\0' ? sub : file, nph_result_string(r));
        return r;
    }
    if (usage == COOK_TEXTURE_HDR && image.format != COOK_IMAGE_RGBA32F) {
        // An LDR image asked to be HDR: promote it (the bytes are linear).
        CookImage promoted;
        r = cook_image_to_f32(c->alloc, &image, false, &promoted);
        if (r == NPH_RESULT_OK) {
            cook_image_free(&image);
            image = promoted;
        }
    } else if (usage != COOK_TEXTURE_HDR && image.format == COOK_IMAGE_RGBA32F) {
        CookImage ldr;
        r = cook_image_to_rgba8(c->alloc, &image, usage == COOK_TEXTURE_COLOR, &ldr);
        if (r == NPH_RESULT_OK) {
            cook_image_free(&image);
            image = ldr;
        }
    }
    NphAssetWriter w;
    nph_asset_writer_init(&w, c->alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION, *out_guid,
                          content_hash);
    if (r == NPH_RESULT_OK) {
        r = cook_texture(c->alloc, &image, &settings, c->jobs, &w);
    }
    if (r == NPH_RESULT_OK) {
        char relative[COOKER_PATH_MAX];
        output_path(c, key, sub, file, *out_guid, relative, sizeof relative);
        r = emit(c, &w, key, sub, relative, NPH_ASSET_TYPE_TEXTURE, content_hash, shash, NULL, 0U);
    }
    nph_asset_writer_free(&w);
    cook_image_free(&image);
    return r;
}

NphResult cooker_cook_image(Cooker *c, const char *source_path, CookTextureUsage usage)
{
    NPH_ASSERT(c != NULL && source_path != NULL);
    if ((u32)usage >= NPH_ARRAY_COUNT(usage_names)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    char key[COOKER_PATH_MAX];
    NphResult r = cooker_source_key(c, source_path, key, sizeof key);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: not under the source root %s", source_path, c->root);
        c->stats.failed += 1U;
        return r;
    }
    void *bytes = NULL;
    u64 size = 0U;
    r = nph_file_read_all(source_path, c->alloc, &bytes, &size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", source_path, nph_result_string(r));
        c->stats.failed += 1U;
        return r;
    }
    const u64 content_hash = nph_hash_combine(nph_hash64(bytes, size, NPH_HASH_DEFAULT_SEED), size);
    // A standalone image keys each usage as its own sub-asset.
    char sub[32];
    snprintf(sub, sizeof sub, "%s", usage_names[usage]);
    char file[48];
    snprintf(file, sizeof file, "tex_%s", usage_names[usage]);
    NphGuid guid;
    r = cook_texture_bytes(c, key, sub, file, bytes, size, usage, content_hash, &guid);
    nph_file_free_all(c->alloc, bytes, size);
    if (r != NPH_RESULT_OK) {
        c->stats.failed += 1U;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Editable meshes
// ---------------------------------------------------------------------------

NphResult cooker_cook_mesh(Cooker *c, const char *source_path)
{
    NPH_ASSERT(c != NULL && source_path != NULL);
    char key[COOKER_PATH_MAX];
    NphResult r = cooker_source_key(c, source_path, key, sizeof key);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: not under the source root %s", source_path, c->root);
        c->stats.failed += 1U;
        return r;
    }
    void *bytes = NULL;
    u64 size = 0U;
    r = nph_file_read_all(source_path, c->alloc, &bytes, &size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", source_path, nph_result_string(r));
        c->stats.failed += 1U;
        return r;
    }
    const u64 content_hash = nph_hash_combine(nph_hash64(bytes, size, NPH_HASH_DEFAULT_SEED), size);
    const u64 shash = settings_hash(c, NPH_ASSET_TYPE_MESH);
    const NphGuid guid = asset_guid(key, "mesh");
    if (is_current(c, key, "mesh", content_hash, shash)) {
        c->stats.skipped += 1U;
        nph_file_free_all(c->alloc, bytes, size);
        return NPH_RESULT_OK;
    }
    CookPolyMesh poly;
    r = cook_polymesh_read(c->alloc, nph_str((const char *)bytes, size), &poly);
    nph_file_free_all(c->alloc, bytes, size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: not an editable mesh: %s", key, nph_result_string(r));
    }
    const NphStr stem = nph_path_stem(nph_str_from_cstr(key));
    char name[COOK_MESH_NAME_MAX];
    snprintf(name, sizeof name, "%.*s", (int)stem.len, stem.data);
    CookMesh mesh;
    cook_mesh_init(&mesh, c->alloc);
    if (r == NPH_RESULT_OK) {
        cook_mesh_free(&mesh);
        r = cook_polymesh_to_mesh(&poly, name, &mesh);
    }
    if (r == NPH_RESULT_OK && mesh.submeshes.len == 0U) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: has no faces", key);
        r = NPH_RESULT_INVALID_ARGUMENT;
    }
    if (r == NPH_RESULT_OK) {
        NphAssetWriter w;
        nph_asset_writer_init(&w, c->alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION, guid,
                              content_hash);
        CookMeshStats st;
        r = cook_mesh_process(&mesh, &c->mesh, &w, &st);
        if (r == NPH_RESULT_OK) {
            char relative[COOKER_PATH_MAX];
            output_path(c, key, "mesh", "mesh", guid, relative, sizeof relative);
            r = emit(c, &w, key, "mesh", relative, NPH_ASSET_TYPE_MESH, content_hash, shash, NULL,
                     0U);
        } else {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", key, nph_result_string(r));
        }
        nph_asset_writer_free(&w);
    }
    cook_mesh_free(&mesh);
    cook_polymesh_free(&poly);
    if (r != NPH_RESULT_OK) {
        c->stats.failed += 1U;
    }
    return r;
}

// ---------------------------------------------------------------------------
// glTF scenes
// ---------------------------------------------------------------------------

typedef struct ImageUse {
    i32 image;
    CookTextureUsage usage;
    NphGuid guid;
} ImageUse;

// The usage a material slot implies for its texture.
static CookTextureUsage slot_usage(u32 slot)
{
    switch (slot) {
    case NPH_MATERIAL_TEXTURE_BASE_COLOR:
    case NPH_MATERIAL_TEXTURE_EMISSIVE:
        return COOK_TEXTURE_COLOR;
    case NPH_MATERIAL_TEXTURE_NORMAL:
        return COOK_TEXTURE_NORMAL;
    case NPH_MATERIAL_TEXTURE_OCCLUSION:
        return COOK_TEXTURE_MASK;
    default:
        return COOK_TEXTURE_LINEAR;
    }
}

// Textures: one per (image, usage) pair the materials reference. An
// occlusion map that is the metallic-roughness image (glTF's packed
// "ORM" textures: occlusion in R, roughness in G, metallic in B) shares
// the BC7 texture instead of becoming a second BC4 one: the renderer
// reads R either way.
static NphResult cook_scene_textures(Cooker *c, const char *key, CookGltfScene *scene,
                                     GuidArray *produced)
{
    const u32 max_uses = nph_max_u32(scene->material_count * NPH_MATERIAL_TEXTURE_COUNT, 1U);
    ImageUse *uses = NPH_ALLOC_ARRAY_ZEROED(c->alloc, ImageUse, max_uses);
    if (uses == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 use_count = 0U;
    NphResult r = NPH_RESULT_OK;
    for (u32 m = 0; m < scene->material_count && r == NPH_RESULT_OK; ++m) {
        CookGltfMaterial *mat = &scene->materials[m];
        for (u32 slot = 0; slot < NPH_MATERIAL_TEXTURE_COUNT && r == NPH_RESULT_OK; ++slot) {
            const i32 image = mat->image[slot];
            if (image < 0 || (u32)image >= scene->image_count) {
                continue;
            }
            const CookGltfImage *img = &scene->images[image];
            CookTextureUsage usage = slot_usage(slot);
            if (slot == NPH_MATERIAL_TEXTURE_OCCLUSION &&
                mat->image[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS] == image) {
                usage = COOK_TEXTURE_LINEAR;
            }
            if (cook_image_data_is_hdr(img->data, img->size)) {
                usage = COOK_TEXTURE_HDR;
            }
            ImageUse *use = NULL;
            for (u32 u = 0; u < use_count; ++u) {
                if (uses[u].image == image && uses[u].usage == usage) {
                    use = &uses[u];
                    break;
                }
            }
            if (use == NULL) {
                use = &uses[use_count++];
                use->image = image;
                use->usage = usage;
                char sub[64];
                snprintf(sub, sizeof sub, "image/%d/%s", image, usage_names[usage]);
                char file[COOK_GLTF_NAME_MAX + 64U];
                snprintf(file, sizeof file, "tex_%d_%s_%s", image, img->name, usage_names[usage]);
                // A texture depends on its image's bytes, not the document.
                r = cook_texture_bytes(c, key, sub, file, img->data, img->size, usage,
                                       nph_hash_combine(img->hash, img->size), &use->guid);
                if (r == NPH_RESULT_OK) {
                    r = NPH_ARRAY_PUSH(produced, use->guid);
                }
            }
            if (r == NPH_RESULT_OK) {
                mat->header.textures[slot].texture = use->guid;
            }
        }
    }
    NPH_FREE_ARRAY(c->alloc, ImageUse, uses, max_uses);
    return r;
}

static NphResult cook_material(Cooker *c, const char *key, const char *sub, const char *file,
                               const NphMaterialHeader *h, u64 content_hash, NphGuid guid)
{
    const u64 shash = settings_hash(c, NPH_ASSET_TYPE_MATERIAL);
    if (is_current(c, key, sub, content_hash, shash)) {
        c->stats.skipped += 1U;
        return NPH_RESULT_OK;
    }
    NphAssetWriter w;
    nph_asset_writer_init(&w, c->alloc, NPH_ASSET_TYPE_MATERIAL, NPH_MATERIAL_TYPE_VERSION, guid,
                          content_hash);
    NphResult r = nph_asset_writer_add_blob(&w, NPH_MATERIAL_TAG_HEADER, 0U, h, sizeof *h,
                                            NPH_ASSET_CODEC_NONE, 0U);
    if (r == NPH_RESULT_OK) {
        NphGuid deps[NPH_MATERIAL_TEXTURE_COUNT];
        u32 dep_count = 0U;
        for (u32 slot = 0; slot < NPH_MATERIAL_TEXTURE_COUNT; ++slot) {
            const NphGuid t = h->textures[slot].texture;
            bool seen = nph_guid_is_null(t);
            for (u32 d = 0; d < dep_count && !seen; ++d) {
                seen = nph_guid_equals(deps[d], t);
            }
            if (!seen) {
                deps[dep_count++] = t;
            }
        }
        char relative[COOKER_PATH_MAX];
        output_path(c, key, sub, file, guid, relative, sizeof relative);
        r = emit(c, &w, key, sub, relative, NPH_ASSET_TYPE_MATERIAL, content_hash, shash, deps,
                 dep_count);
    }
    nph_asset_writer_free(&w);
    return r;
}

static NphResult cook_scene_asset(Cooker *c, const char *key, const CookGltfScene *scene,
                                  const NphGuid *mesh_guids, const NphGuid *material_guids,
                                  NphGuid default_material, const GuidArray *deps,
                                  NphGuid scene_guid)
{
    const u64 shash = settings_hash(c, NPH_ASSET_TYPE_SCENE);
    if (is_current(c, key, "scene", scene->content_hash, shash)) {
        c->stats.skipped += 1U;
        return NPH_RESULT_OK;
    }
    NphAllocator *alloc = c->alloc;
    const u32 n = scene->node_count;
    NphSceneNode *nodes = NPH_ALLOC_ARRAY_ZEROED(alloc, NphSceneNode, nph_max_u32(n, 1U));
    NphSceneMeshInstance *inst =
        NPH_ALLOC_ARRAY_ZEROED(alloc, NphSceneMeshInstance, nph_max_u32(n, 1U));
    u32 *last_child = NPH_ALLOC_ARRAY(alloc, u32, nph_max_u32(n, 1U));
    NphStrBuilder strings;
    nph_sb_init(&strings, alloc);
    GuidArray guid_list;
    NPH_ARRAY_INIT(&guid_list, alloc);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_SCENE, NPH_SCENE_TYPE_VERSION, scene_guid,
                          scene->content_hash);
    NphResult r = nodes != NULL && inst != NULL && last_child != NULL ? NPH_RESULT_OK
                                                                      : NPH_RESULT_OUT_OF_MEMORY;
    u32 inst_count = 0U;
    for (u32 i = 0; i < n && r == NPH_RESULT_OK; ++i) {
        const CookGltfNode *gn = &scene->nodes[i];
        NphSceneNode *node = &nodes[i];
        node->translation = gn->translation;
        node->rotation = gn->rotation;
        node->scale = gn->scale;
        node->parent = gn->parent;
        node->first_child = NPH_SCENE_NO_NODE;
        node->next_sibling = NPH_SCENE_NO_NODE;
        last_child[i] = NPH_SCENE_NO_NODE;
        node->name = (u32)strings.len;
        r = nph_sb_append_cstr(&strings, gn->name);
        if (r == NPH_RESULT_OK) {
            r = nph_sb_append_char(&strings, '\0');
        }
        if (gn->parent != NPH_SCENE_NO_NODE) {
            // Siblings in document order: append after the parent's last child.
            NPH_ASSERT(gn->parent < i);
            if (last_child[gn->parent] == NPH_SCENE_NO_NODE) {
                nodes[gn->parent].first_child = i;
            } else {
                nodes[last_child[gn->parent]].next_sibling = i;
            }
            last_child[gn->parent] = i;
        }
        if (r == NPH_RESULT_OK && gn->mesh >= 0 && (u32)gn->mesh < scene->mesh_count &&
            scene->meshes[gn->mesh].mesh.submeshes.len != 0U) {
            const CookGltfMesh *gm = &scene->meshes[gn->mesh];
            NphSceneMeshInstance *mi = &inst[inst_count++];
            mi->mesh = mesh_guids[gn->mesh];
            mi->node = i;
            mi->material_offset = guid_list.len;
            mi->material_count = gm->material_count;
            for (u32 s = 0; s < gm->material_count && r == NPH_RESULT_OK; ++s) {
                const i32 mat = gm->materials[s];
                const NphGuid g = mat >= 0 && (u32)mat < scene->material_count ? material_guids[mat]
                                                                               : default_material;
                r = NPH_ARRAY_PUSH(&guid_list, g);
            }
        }
    }
    NphSceneHeader h;
    memset(&h, 0, sizeof h);
    h.node_count = n;
    h.instance_count = inst_count;
    h.material_count = guid_list.len;
    h.camera_count = scene->camera_count;
    h.light_count = scene->light_count;
    h.string_bytes = (u32)strings.len;
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_HEADER, 0U, &h, sizeof h,
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_NODES, 0U, nodes, (u64)n * sizeof *nodes,
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_INSTANCES, 0U, inst,
                                      (u64)inst_count * sizeof *inst, NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_MATERIALS, 0U, guid_list.data,
                                      NPH_ARRAY_BYTES(&guid_list), NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_CAMERAS, 0U, scene->cameras,
                                      (u64)scene->camera_count * sizeof(NphSceneCamera),
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_LIGHTS, 0U, scene->lights,
                                      (u64)scene->light_count * sizeof(NphSceneLight),
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_SCENE_TAG_STRINGS, 0U, strings.data, strings.len,
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        char relative[COOKER_PATH_MAX];
        output_path(c, key, "scene", "scene", scene_guid, relative, sizeof relative);
        r = emit(c, &w, key, "scene", relative, NPH_ASSET_TYPE_SCENE, scene->content_hash, shash,
                 deps->data, deps->len);
    }
    nph_asset_writer_free(&w);
    NPH_ARRAY_FREE(&guid_list);
    nph_sb_free(&strings);
    if (nodes != NULL) {
        NPH_FREE_ARRAY(alloc, NphSceneNode, nodes, nph_max_u32(n, 1U));
    }
    if (inst != NULL) {
        NPH_FREE_ARRAY(alloc, NphSceneMeshInstance, inst, nph_max_u32(n, 1U));
    }
    if (last_child != NULL) {
        NPH_FREE_ARRAY(alloc, u32, last_child, nph_max_u32(n, 1U));
    }
    return r;
}

NphResult cooker_cook_gltf(Cooker *c, const char *source_path)
{
    NPH_ASSERT(c != NULL && source_path != NULL);
    char key[COOKER_PATH_MAX];
    NphResult r = cooker_source_key(c, source_path, key, sizeof key);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: not under the source root %s", source_path, c->root);
        c->stats.failed += 1U;
        return r;
    }
    CookGltfScene scene;
    r = cook_gltf_load(c->alloc, source_path, &scene);
    if (r != NPH_RESULT_OK) {
        c->stats.failed += 1U;
        return r;
    }
    NphAllocator *alloc = c->alloc;
    GuidArray produced; // every asset this cook stands for, cooked or current
    NPH_ARRAY_INIT(&produced, alloc);
    GuidArray scene_deps;
    NPH_ARRAY_INIT(&scene_deps, alloc);
    NphGuid *material_guids =
        NPH_ALLOC_ARRAY_ZEROED(alloc, NphGuid, nph_max_u32(scene.material_count, 1U));
    NphGuid *mesh_guids = NPH_ALLOC_ARRAY_ZEROED(alloc, NphGuid, nph_max_u32(scene.mesh_count, 1U));
    if (material_guids == NULL || mesh_guids == NULL) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    }
    if (r == NPH_RESULT_OK) {
        r = cook_scene_textures(c, key, &scene, &produced);
    }
    // Materials, and the default one for primitives without a material.
    for (u32 m = 0; m < scene.material_count && r == NPH_RESULT_OK; ++m) {
        const CookGltfMaterial *mat = &scene.materials[m];
        char sub[64];
        snprintf(sub, sizeof sub, "material/%u", m);
        char file[COOK_GLTF_NAME_MAX + 32U];
        snprintf(file, sizeof file, "material_%u_%s", m, mat->name);
        material_guids[m] = asset_guid(key, sub);
        r = cook_material(c, key, sub, file, &mat->header, scene.content_hash, material_guids[m]);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&produced, material_guids[m]);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&scene_deps, material_guids[m]);
        }
    }
    NphGuid default_material = NPH_GUID_NULL;
    bool needs_default = false;
    for (u32 m = 0; m < scene.mesh_count; ++m) {
        for (u32 s = 0; s < scene.meshes[m].material_count; ++s) {
            needs_default = needs_default || scene.meshes[m].materials[s] < 0;
        }
    }
    if (needs_default && r == NPH_RESULT_OK) {
        NphMaterialHeader h;
        nph_material_header_default(&h);
        default_material = asset_guid(key, "material/default");
        r = cook_material(c, key, "material/default", "material_default", &h, scene.content_hash,
                          default_material);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&produced, default_material);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&scene_deps, default_material);
        }
    }
    // Meshes.
    const u64 mesh_shash = settings_hash(c, NPH_ASSET_TYPE_MESH);
    for (u32 m = 0; m < scene.mesh_count && r == NPH_RESULT_OK; ++m) {
        CookGltfMesh *gm = &scene.meshes[m];
        if (gm->mesh.submeshes.len == 0U) {
            continue; // nothing drawable: no asset, and no instance refers to it
        }
        char sub[64];
        snprintf(sub, sizeof sub, "mesh/%u", m);
        mesh_guids[m] = asset_guid(key, sub);
        r = NPH_ARRAY_PUSH(&produced, mesh_guids[m]);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&scene_deps, mesh_guids[m]);
        }
        if (r != NPH_RESULT_OK || is_current(c, key, sub, scene.content_hash, mesh_shash)) {
            c->stats.skipped += r == NPH_RESULT_OK ? 1U : 0U;
            continue;
        }
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION, mesh_guids[m],
                              scene.content_hash);
        CookMeshStats st;
        r = cook_mesh_process(&gm->mesh, &c->mesh, &w, &st);
        if (r == NPH_RESULT_OK) {
            NPH_LOG_INFO(
                &NPH_LOG_COOK,
                "mesh %s: %u -> %u vertices, %u -> %u triangles, %u meshlets, ACMR %.2f -> %.2f",
                gm->mesh.name, st.source_vertices, st.vertices, st.source_triangles, st.triangles,
                st.meshlets, (f64)st.acmr_before, (f64)st.acmr_after);
            char file[COOK_MESH_NAME_MAX + 32U];
            snprintf(file, sizeof file, "mesh_%u_%s", m, gm->mesh.name);
            char relative[COOKER_PATH_MAX];
            output_path(c, key, sub, file, mesh_guids[m], relative, sizeof relative);
            r = emit(c, &w, key, sub, relative, NPH_ASSET_TYPE_MESH, scene.content_hash, mesh_shash,
                     NULL, 0U);
        } else {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "mesh %s: %s", gm->mesh.name, nph_result_string(r));
        }
        nph_asset_writer_free(&w);
    }
    // The scene.
    const NphGuid scene_guid = asset_guid(key, "scene");
    if (r == NPH_RESULT_OK) {
        r = cook_scene_asset(c, key, &scene, mesh_guids, material_guids, default_material,
                             &scene_deps, scene_guid);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH(&produced, scene_guid);
    }
    if (r == NPH_RESULT_OK) {
        remove_stale(c, key, &produced);
    }
    if (mesh_guids != NULL) {
        NPH_FREE_ARRAY(alloc, NphGuid, mesh_guids, nph_max_u32(scene.mesh_count, 1U));
    }
    if (material_guids != NULL) {
        NPH_FREE_ARRAY(alloc, NphGuid, material_guids, nph_max_u32(scene.material_count, 1U));
    }
    NPH_ARRAY_FREE(&scene_deps);
    NPH_ARRAY_FREE(&produced);
    if (scene.warnings != 0U) {
        NPH_LOG_WARN(&NPH_LOG_COOK, "%s: %u features skipped (see above)", key, scene.warnings);
    }
    cook_gltf_free(&scene);
    if (r != NPH_RESULT_OK) {
        c->stats.failed += 1U;
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", source_path, nph_result_string(r));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Files and directories
// ---------------------------------------------------------------------------

static bool is_scene_file(NphStr ext)
{
    return nph_str_equals_ignore_case(ext, NPH_STR(".gltf")) ||
           nph_str_equals_ignore_case(ext, NPH_STR(".glb"));
}

CookerSourceKind cooker_source_kind(const char *path)
{
    NPH_ASSERT(path != NULL);
    static const char *const images[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"};
    const NphStr ext = nph_path_extension(nph_str_from_cstr(path));
    if (is_scene_file(ext)) {
        return COOKER_SOURCE_SCENE;
    }
    if (nph_str_equals_ignore_case(ext, NPH_STR(".nmesh"))) {
        return COOKER_SOURCE_MESH;
    }
    for (u32 i = 0; i < NPH_ARRAY_COUNT(images); ++i) {
        if (nph_str_equals_ignore_case(ext, nph_str_from_cstr(images[i]))) {
            return COOKER_SOURCE_IMAGE;
        }
    }
    return COOKER_SOURCE_NONE;
}

NphResult cooker_cook_file(Cooker *c, const char *source_path)
{
    NPH_ASSERT(c != NULL && source_path != NULL);
    NphFileInfo info;
    if (nph_fs_stat(source_path, &info) == NPH_RESULT_OK && info.is_directory) {
        return cooker_cook_directory(c, source_path);
    }
    switch (cooker_source_kind(source_path)) {
    case COOKER_SOURCE_SCENE:
        return cooker_cook_gltf(c, source_path);
    case COOKER_SOURCE_MESH:
        return cooker_cook_mesh(c, source_path);
    case COOKER_SOURCE_IMAGE: {
        const NphStr ext = nph_path_extension(nph_str_from_cstr(source_path));
        return cooker_cook_image(
            c, source_path,
            nph_str_equals_ignore_case(ext, NPH_STR(".hdr")) ? COOK_TEXTURE_HDR : c->texture.usage);
    }
    case COOKER_SOURCE_NONE:
        break;
    }
    NPH_LOG_WARN(&NPH_LOG_COOK, "%s: no cooker for this file type", source_path);
    return NPH_RESULT_UNSUPPORTED;
}

typedef NPH_ARRAY(NphDirEntry) DirEntryArray;

static int compare_entries(const NphDirEntry *a, const NphDirEntry *b)
{
    return strcmp(a->name, b->name);
}

// The entries of one directory sorted by name, so the cook (and the logs)
// are in the same order on every file system.
static NphResult list_sorted(const char *dir, DirEntryArray *out)
{
    NphDirIterator it;
    NPH_TRY(nph_dir_open(dir, &it));
    NphDirEntry e;
    NphResult r = NPH_RESULT_OK;
    while (r == NPH_RESULT_OK && nph_dir_next(&it, &e)) {
        r = NPH_ARRAY_PUSH(out, e);
    }
    nph_dir_close(&it);
    for (u32 i = 1; i < out->len; ++i) { // insertion sort: directories are small
        const NphDirEntry key = out->data[i];
        u32 j = i;
        while (j > 0U && compare_entries(&key, &out->data[j - 1U]) < 0) {
            out->data[j] = out->data[j - 1U];
            --j;
        }
        out->data[j] = key;
    }
    return r;
}

static NphResult cook_tree(Cooker *c, const char *dir, u32 depth, NphResult *first_error)
{
    if (depth > 64U) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: directories nested too deep", dir);
        return NPH_RESULT_OVERFLOW;
    }
    DirEntryArray entries;
    NPH_ARRAY_INIT(&entries, c->alloc);
    NphResult r = list_sorted(dir, &entries);
    for (u32 i = 0; i < entries.len && r == NPH_RESULT_OK; ++i) {
        const NphDirEntry *e = &entries.data[i];
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) {
            continue;
        }
        char path[COOKER_PATH_MAX];
        if ((u64)snprintf(path, sizeof path, "%s/%s", dir, e->name) >= sizeof path) {
            continue;
        }
        if (e->is_directory) {
            r = cook_tree(c, path, depth + 1U, first_error);
            continue;
        }
        const CookerSourceKind kind = cooker_source_kind(e->name);
        if (kind != COOKER_SOURCE_SCENE && kind != COOKER_SOURCE_MESH) {
            continue;
        }
        const NphResult fr =
            kind == COOKER_SOURCE_SCENE ? cooker_cook_gltf(c, path) : cooker_cook_mesh(c, path);
        if (fr != NPH_RESULT_OK && *first_error == NPH_RESULT_OK) {
            *first_error = fr; // keep going: one bad file does not stop the rest
        }
    }
    NPH_ARRAY_FREE(&entries);
    return r;
}

NphResult cooker_cook_directory(Cooker *c, const char *dir)
{
    NPH_ASSERT(c != NULL && dir != NULL);
    NphResult first_error = NPH_RESULT_OK;
    const NphResult r = cook_tree(c, dir, 0U, &first_error);
    return r != NPH_RESULT_OK ? r : first_error;
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

NphResult cooker_pack(Cooker *c, const char *pack_path)
{
    NPH_ASSERT(c != NULL && pack_path != NULL);
    NPH_TRY(cook_db_save(&c->db, c->db_path, false));
    NphPackWriter w;
    NPH_TRY(nph_pack_writer_begin(&w, c->alloc, pack_path, 0U));
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; i < cook_db_count(&c->db) && r == NPH_RESULT_OK; ++i) {
        const CookDbEntry *e = cook_db_entry(&c->db, i);
        char full[COOKER_PATH_MAX * 2U];
        snprintf(full, sizeof full, "%s/%s", c->out_dir, e->output);
        r = nph_pack_writer_add_file(&w, nph_str_from_cstr(e->output), e->guid, e->type, full);
        if (r == NPH_RESULT_NOT_FOUND) {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: output missing (cook %s again)", e->output,
                          e->source);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_pack_writer_finish(&w);
    } else {
        nph_pack_writer_abort(&w);
    }
    if (r == NPH_RESULT_OK) {
        NPH_LOG_INFO(&NPH_LOG_COOK, "packed %u assets into %s", cook_db_count(&c->db), pack_path);
    }
    return r;
}

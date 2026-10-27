// SPDX-License-Identifier: Apache-2.0
// The cooker (cooker.h): source keys, output paths, the incremental
// decisions, the glTF and image drivers, and packing.
#include <nepheline/asset/scene.h>
#include <nepheline/asset/sound.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include <cook/cooker.h>
#include <cook/gltf.h>
#include <cook/image.h>
#include <cook/polymesh.h>
#include <cook/voxel.h>
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
    CookSoundSettings sound;
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
    c->sound = desc->sound;
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
        {NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION},
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
        h = nph_hash_combine(h, COOK_MESH_VERSION);
        h = nph_hash_combine(h, nph_hash64(&c->mesh, sizeof c->mesh, 2U));
    } else if (type == NPH_ASSET_TYPE_SOUND) {
        // The cooker's defaults; a source's own settings are in its content
        // hash (cooker_cook_sound).
        h = nph_hash_combine(h, nph_hash64(&c->sound, sizeof c->sound, 3U));
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
// Writes texture `sub` of `key` from a decoded image (RGBA8, or RGBA32F
// for HDR), whatever the database says: the callers ask it first.
static NphResult write_texture(Cooker *c, const char *key, const char *sub, const char *file,
                               const CookImage *image, CookTextureUsage usage, u64 content_hash,
                               u64 shash, NphGuid guid)
{
    CookTextureSettings settings = c->texture;
    settings.usage = usage;
    NphAssetWriter w;
    nph_asset_writer_init(&w, c->alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION, guid,
                          content_hash);
    NphResult r = cook_texture(c->alloc, image, &settings, c->jobs, &w);
    if (r == NPH_RESULT_OK) {
        char relative[COOKER_PATH_MAX];
        output_path(c, key, sub, file, guid, relative, sizeof relative);
        r = emit(c, &w, key, sub, relative, NPH_ASSET_TYPE_TEXTURE, content_hash, shash, NULL, 0U);
    }
    nph_asset_writer_free(&w);
    return r;
}

static u64 texture_settings_hash(const Cooker *c, CookTextureUsage usage)
{
    return nph_hash_combine(settings_hash(c, NPH_ASSET_TYPE_TEXTURE), (u64)usage);
}

static NphResult cook_texture_bytes(Cooker *c, const char *key, const char *sub, const char *file,
                                    const void *bytes, u64 size, CookTextureUsage usage,
                                    u64 content_hash, NphGuid *out_guid)
{
    const u64 shash = texture_settings_hash(c, usage);
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
                      "BMP, Radiance HDR, DDS): %s",
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
    if (r == NPH_RESULT_OK) {
        r = write_texture(c, key, sub, file, &image, usage, content_hash, shash, *out_guid);
    }
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
    // HDR texels (a Radiance file, a BC6H DDS) take the HDR pipeline
    // whatever usage was asked for, as a scene's images do.
    if (cook_image_data_is_hdr(bytes, size)) {
        usage = COOK_TEXTURE_HDR;
    }
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
        r = cook_mesh_process(&mesh, &c->mesh, NULL, &w, &st);
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
// Sounds
// ---------------------------------------------------------------------------

static const char *sound_codec_name(NphSoundCodec codec)
{
    switch (codec) {
    case NPH_SOUND_CODEC_PCM:
        return "PCM";
    case NPH_SOUND_CODEC_ADPCM:
        return "ADPCM";
    case NPH_SOUND_CODEC_LOSSLESS:
        return "lossless";
    }
    return "?";
}

// The settings file beside a source (sound.h): its settings over `inout`,
// and its bytes into `*inout_hash`. Nothing when there is none.
static NphResult read_import_settings(Cooker *c, const char *source_path, CookSoundSettings *inout,
                                      u64 *inout_hash)
{
    char path[COOKER_PATH_MAX + 16U];
    if ((u64)snprintf(path, sizeof path, "%s.import.json", source_path) >= sizeof path) {
        return NPH_RESULT_OVERFLOW;
    }
    if (!nph_fs_exists(path)) {
        return NPH_RESULT_OK;
    }
    void *json = NULL;
    u64 size = 0U;
    NphResult r = nph_file_read_all(path, c->alloc, &json, &size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", path, nph_result_string(r));
        return r;
    }
    *inout_hash = nph_hash_combine(*inout_hash, nph_hash64(json, size, NPH_HASH_DEFAULT_SEED));
    *inout_hash = nph_hash_combine(*inout_hash, size);
    r = cook_sound_settings_parse(c->alloc, json, size, inout);
    nph_file_free_all(c->alloc, json, size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", path,
                      r == NPH_RESULT_PARSE_ERROR ? "not a JSON object"
                                                  : "an unknown key or value (docs/cook.md)");
    }
    return r;
}

// True when rounding the 64-bit float samples to 32 bits changes one.
static bool rounding_changes(const NphWavInfo *wav)
{
    const u64 n = wav->frames * wav->channels;
    for (u64 i = 0U; i < n; ++i) {
        const f64 x = nph_wav_sample_float(wav, i / wav->channels, (u32)(i % wav->channels));
        if ((f64)(f32)x != x) {
            return true;
        }
    }
    return false;
}

NphResult cooker_cook_sound(Cooker *c, const char *source_path)
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
    u64 content_hash = nph_hash_combine(nph_hash64(bytes, size, NPH_HASH_DEFAULT_SEED), size);
    CookSoundSettings settings = c->sound;
    r = read_import_settings(c, source_path, &settings, &content_hash);
    const u64 shash = settings_hash(c, NPH_ASSET_TYPE_SOUND);
    const NphGuid guid = asset_guid(key, "sound");
    if (r == NPH_RESULT_OK && is_current(c, key, "sound", content_hash, shash)) {
        c->stats.skipped += 1U;
        nph_file_free_all(c->alloc, bytes, size);
        return NPH_RESULT_OK;
    }
    NphWavInfo wav;
    memset(&wav, 0, sizeof wav);
    if (r == NPH_RESULT_OK) {
        r = nph_wav_parse(bytes, size, &wav);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: not a WAV file the cooker reads: %s", key,
                          nph_result_string(r));
        }
    }
    CookSoundPlan plan;
    memset(&plan, 0, sizeof plan);
    if (r == NPH_RESULT_OK) {
        r = cook_sound_plan(&wav, &settings, &plan);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_COOK,
                          "%s: cannot be cooked so (ADPCM streamed, too long to keep in memory, "
                          "or a sample that is not a number)",
                          key);
        }
    }
    if (r == NPH_RESULT_OK) {
        const bool asked_lossless = settings.codec == COOK_SOUND_CODEC_LOSSLESS ||
                                    (settings.codec == COOK_SOUND_CODEC_AUTO && plan.stream);
        if (asked_lossless && plan.codec == NPH_SOUND_CODEC_PCM) {
            NPH_LOG_INFO(&NPH_LOG_COOK,
                         "%s: kept as %u-bit PCM: the lossless codec cannot hold its samples "
                         "exactly",
                         key, plan.bits);
        }
        if (wav.encoding == NPH_WAV_FLOAT64 && plan.codec == NPH_SOUND_CODEC_PCM &&
            rounding_changes(&wav)) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "%s: its 64-bit float samples are rounded to 32 bits", key);
        }
        NphAssetWriter w;
        nph_asset_writer_init(&w, c->alloc, NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION, guid,
                              content_hash);
        r = cook_sound(c->alloc, &wav, &settings, &w);
        if (r == NPH_RESULT_OK) {
            NPH_LOG_INFO(&NPH_LOG_COOK, "%s: %.2f s, %u channels at %u Hz: %s %u-bit, %s", key,
                         (f64)wav.frames / (f64)wav.sample_rate, wav.channels, wav.sample_rate,
                         sound_codec_name(plan.codec), plan.bits,
                         plan.stream ? "streamed" : "in memory");
            char relative[COOKER_PATH_MAX];
            output_path(c, key, "sound", "sound", guid, relative, sizeof relative);
            r = emit(c, &w, key, "sound", relative, NPH_ASSET_TYPE_SOUND, content_hash, shash, NULL,
                     0U);
        } else {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", key, nph_result_string(r));
        }
        nph_asset_writer_free(&w);
    }
    nph_file_free_all(c->alloc, bytes, size);
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

// A specular-glossiness material's textures (cook/specgloss.h): one pair
// per (diffuse image, specular-glossiness image, factors).
typedef struct SpecGlossUse {
    i32 images[2];
    u64 factors_hash;
    NphGuid base;
    NphGuid mr;
} SpecGlossUse;

// Decodes image `index` of the scene for the conversion (8-bit texels), or
// leaves `out` empty for -1.
static NphResult decode_specgloss_image(Cooker *c, const char *key, const CookGltfScene *scene,
                                        i32 index, CookImage *out)
{
    memset(out, 0, sizeof *out);
    if (index < 0) {
        return NPH_RESULT_OK;
    }
    const CookGltfImage *img = &scene->images[index];
    NphResult r = cook_image_decode(c->alloc, img->data, img->size, out);
    if (r == NPH_RESULT_OK && out->format != COOK_IMAGE_RGBA8) {
        cook_image_free(out);
        r = NPH_RESULT_UNSUPPORTED;
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK,
                      "%s: image %s: not an 8-bit image the specular-glossiness conversion "
                      "reads: %s",
                      key, img->name, nph_result_string(r));
    }
    return r;
}

// The base colour and metallic-roughness textures of a specular-glossiness
// material, made from its two images and its factors (one per distinct
// trio among the scene's materials).
static NphResult cook_specgloss(Cooker *c, const char *key, const CookGltfScene *scene,
                                const CookGltfMaterial *mat, SpecGlossUse *use)
{
    const i32 d = mat->specgloss_image[0];
    const i32 s = mat->specgloss_image[1];
    char sub_base[96];
    char sub_mr[96];
    snprintf(sub_base, sizeof sub_base, "specgloss/%d/%d/%016llx/base", d, s,
             (unsigned long long)use->factors_hash);
    snprintf(sub_mr, sizeof sub_mr, "specgloss/%d/%d/%016llx/mr", d, s,
             (unsigned long long)use->factors_hash);
    char file_base[64];
    char file_mr[64];
    snprintf(file_base, sizeof file_base, "tex_sg_%d_%d_%08x_base", d, s,
             (unsigned)(use->factors_hash & 0xFFFFFFFFU));
    snprintf(file_mr, sizeof file_mr, "tex_sg_%d_%d_%08x_mr", d, s,
             (unsigned)(use->factors_hash & 0xFFFFFFFFU));
    // They depend on both images' bytes and the factors.
    const u64 content = nph_hash_combine(
        nph_hash_combine(d >= 0 ? scene->images[d].hash : 0U, s >= 0 ? scene->images[s].hash : 0U),
        use->factors_hash);
    const u64 shash_base = texture_settings_hash(c, COOK_TEXTURE_COLOR);
    const u64 shash_mr = texture_settings_hash(c, COOK_TEXTURE_LINEAR);
    use->base = asset_guid(key, sub_base);
    use->mr = asset_guid(key, sub_mr);
    if (is_current(c, key, sub_base, content, shash_base) &&
        is_current(c, key, sub_mr, content, shash_mr)) {
        c->stats.skipped += 2U;
        return NPH_RESULT_OK;
    }
    CookImage diffuse;
    CookImage specgloss;
    NphResult r = decode_specgloss_image(c, key, scene, d, &diffuse);
    if (r == NPH_RESULT_OK) {
        r = decode_specgloss_image(c, key, scene, s, &specgloss);
        if (r != NPH_RESULT_OK) {
            cook_image_free(&diffuse);
            return r;
        }
    } else {
        return r;
    }
    CookImage base;
    CookImage mr;
    r = cook_specgloss_textures(c->alloc, d >= 0 ? &diffuse : NULL, s >= 0 ? &specgloss : NULL,
                                &mat->specgloss_factors, &base, &mr);
    cook_image_free(&diffuse);
    cook_image_free(&specgloss);
    if (r == NPH_RESULT_OK) {
        r = write_texture(c, key, sub_base, file_base, &base, COOK_TEXTURE_COLOR, content,
                          shash_base, use->base);
        if (r == NPH_RESULT_OK) {
            r = write_texture(c, key, sub_mr, file_mr, &mr, COOK_TEXTURE_LINEAR, content, shash_mr,
                              use->mr);
        }
        cook_image_free(&base);
        cook_image_free(&mr);
    }
    return r;
}

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
    if (r != NPH_RESULT_OK) {
        return r;
    }
    // Specular-glossiness materials: their base colour and
    // metallic-roughness textures, made from two images each.
    SpecGlossUse *sg_uses =
        NPH_ALLOC_ARRAY_ZEROED(c->alloc, SpecGlossUse, nph_max_u32(scene->material_count, 1U));
    if (sg_uses == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 sg_count = 0U;
    for (u32 m = 0; m < scene->material_count && r == NPH_RESULT_OK; ++m) {
        CookGltfMaterial *mat = &scene->materials[m];
        if (!mat->specgloss) {
            continue;
        }
        const u64 fh = nph_hash64(&mat->specgloss_factors, sizeof mat->specgloss_factors,
                                  NPH_HASH_DEFAULT_SEED);
        SpecGlossUse *use = NULL;
        for (u32 u = 0; u < sg_count; ++u) {
            if (sg_uses[u].images[0] == mat->specgloss_image[0] &&
                sg_uses[u].images[1] == mat->specgloss_image[1] && sg_uses[u].factors_hash == fh) {
                use = &sg_uses[u];
                break;
            }
        }
        if (use == NULL) {
            use = &sg_uses[sg_count++];
            use->images[0] = mat->specgloss_image[0];
            use->images[1] = mat->specgloss_image[1];
            use->factors_hash = fh;
            r = cook_specgloss(c, key, scene, mat, use);
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(produced, use->base);
            }
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(produced, use->mr);
            }
        }
        if (r == NPH_RESULT_OK) {
            mat->header.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture = use->base;
            mat->header.textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].texture = use->mr;
        }
    }
    NPH_FREE_ARRAY(c->alloc, SpecGlossUse, sg_uses, nph_max_u32(scene->material_count, 1U));
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

// ---------------------------------------------------------------------------
// Voxel materials (cook/voxel.h; ADR-0061)
// ---------------------------------------------------------------------------

// The scene's images as the voxelizer reads them, each decoded the first
// time a mesh that cooks needs it and kept until the scene is done: per
// image its linear and its sRGB chain, per specular-glossiness material the
// converted pair, and per material (then the default one) its voxel
// material.
typedef struct VoxelImages {
    Cooker *c;
    const char *key;
    const CookGltfScene *scene;
    NphAllocator *alloc;
    CookVoxelImage *chains; // per image: linear, then sRGB
    u8 *tried;              // per image: bit 0 linear tried, bit 1 sRGB
    CookVoxelImage *sg;     // per material: base, then metallic-roughness
    u8 *sg_tried;
    CookVoxelMaterial *materials; // per material, then the default
    u8 *ready;
    u32 image_count;
    u32 material_count;
} VoxelImages;

static NphResult voxel_images_init(VoxelImages *v, Cooker *c, const char *key,
                                   const CookGltfScene *scene)
{
    memset(v, 0, sizeof *v);
    v->c = c;
    v->key = key;
    v->scene = scene;
    v->alloc = c->alloc;
    v->image_count = scene->image_count;
    v->material_count = scene->material_count;
    const u32 ni = nph_max_u32(v->image_count, 1U);
    const u32 nm = v->material_count + 1U;
    v->chains = NPH_ALLOC_ARRAY_ZEROED(v->alloc, CookVoxelImage, (u64)ni * 2U);
    v->tried = NPH_ALLOC_ARRAY_ZEROED(v->alloc, u8, ni);
    v->sg = NPH_ALLOC_ARRAY_ZEROED(v->alloc, CookVoxelImage, (u64)nm * 2U);
    v->sg_tried = NPH_ALLOC_ARRAY_ZEROED(v->alloc, u8, nm);
    v->materials = NPH_ALLOC_ARRAY_ZEROED(v->alloc, CookVoxelMaterial, nm);
    v->ready = NPH_ALLOC_ARRAY_ZEROED(v->alloc, u8, nm);
    return v->chains != NULL && v->tried != NULL && v->sg != NULL && v->sg_tried != NULL &&
                   v->materials != NULL && v->ready != NULL
               ? NPH_RESULT_OK
               : NPH_RESULT_OUT_OF_MEMORY;
}

static void voxel_images_free(VoxelImages *v)
{
    const u32 ni = nph_max_u32(v->image_count, 1U);
    const u32 nm = v->material_count + 1U;
    if (v->chains != NULL) {
        for (u32 i = 0; i < ni * 2U; ++i) {
            cook_voxel_image_free(&v->chains[i]);
        }
        NPH_FREE_ARRAY(v->alloc, CookVoxelImage, v->chains, (u64)ni * 2U);
    }
    if (v->sg != NULL) {
        for (u32 i = 0; i < nm * 2U; ++i) {
            cook_voxel_image_free(&v->sg[i]);
        }
        NPH_FREE_ARRAY(v->alloc, CookVoxelImage, v->sg, (u64)nm * 2U);
    }
    if (v->tried != NULL) {
        NPH_FREE_ARRAY(v->alloc, u8, v->tried, ni);
    }
    if (v->sg_tried != NULL) {
        NPH_FREE_ARRAY(v->alloc, u8, v->sg_tried, nm);
    }
    if (v->materials != NULL) {
        NPH_FREE_ARRAY(v->alloc, CookVoxelMaterial, v->materials, nm);
    }
    if (v->ready != NULL) {
        NPH_FREE_ARRAY(v->alloc, u8, v->ready, nm);
    }
    memset(v, 0, sizeof *v);
}

// Image `index`'s chain (sRGB-encoded for a base colour), or NULL for none
// or one that does not decode (then the material samples its factors alone,
// with a warning).
static const CookVoxelImage *voxel_image(VoxelImages *v, i32 index, bool srgb)
{
    if (index < 0 || (u32)index >= v->image_count) {
        return NULL;
    }
    CookVoxelImage *chain = &v->chains[(u64)index * 2U + (srgb ? 1U : 0U)];
    const u8 bit = srgb ? 2U : 1U;
    if ((v->tried[index] & bit) == 0U) {
        v->tried[index] |= bit;
        const CookGltfImage *img = &v->scene->images[index];
        CookImage image;
        NphResult r = cook_image_decode(v->alloc, img->data, img->size, &image);
        if (r == NPH_RESULT_OK) {
            r = cook_voxel_image_init(chain, v->alloc, &image, srgb);
            cook_image_free(&image);
        }
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "%s: image %s: not read for voxels (%s): its factors alone",
                         v->key, img->name, nph_result_string(r));
        }
    }
    return chain->texels != NULL ? chain : NULL;
}

// A specular-glossiness material's converted pair (cook/specgloss.h), as
// its textures are made: base colour, then metallic-roughness.
static void voxel_specgloss(VoxelImages *v, u32 m, const CookGltfMaterial *mat)
{
    if (v->sg_tried[m] != 0U) {
        return;
    }
    v->sg_tried[m] = 1U;
    CookImage images[2];
    memset(images, 0, sizeof images);
    NphResult r = NPH_RESULT_OK;
    for (u32 k = 0; k < 2U && r == NPH_RESULT_OK; ++k) {
        const i32 index = mat->specgloss_image[k];
        if (index >= 0 && (u32)index < v->image_count) {
            const CookGltfImage *img = &v->scene->images[index];
            r = cook_image_decode(v->alloc, img->data, img->size, &images[k]);
            if (r == NPH_RESULT_OK && images[k].format != COOK_IMAGE_RGBA8) {
                r = NPH_RESULT_UNSUPPORTED;
            }
        }
    }
    CookImage base;
    CookImage mr;
    memset(&base, 0, sizeof base);
    memset(&mr, 0, sizeof mr);
    if (r == NPH_RESULT_OK) {
        r = cook_specgloss_textures(v->alloc, images[0].pixels != NULL ? &images[0] : NULL,
                                    images[1].pixels != NULL ? &images[1] : NULL,
                                    &mat->specgloss_factors, &base, &mr);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_voxel_image_init(&v->sg[(u64)m * 2U], v->alloc, &base, true);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_voxel_image_init(&v->sg[(u64)m * 2U + 1U], v->alloc, &mr, false);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&NPH_LOG_COOK,
                     "%s: material %s: its specular-glossiness images not read for voxels (%s)",
                     v->key, mat->name, nph_result_string(r));
    }
    cook_image_free(&images[0]);
    cook_image_free(&images[1]);
    cook_image_free(&base);
    cook_image_free(&mr);
}

static CookVoxelSlot voxel_slot(const CookVoxelImage *image, const NphMaterialHeader *h, u32 slot)
{
    CookVoxelSlot s;
    s.image = image;
    s.wrap_u = h->textures[slot].wrap_u;
    s.wrap_v = h->textures[slot].wrap_v;
    return s;
}

// glTF material `m`'s voxel material (-1: the default material's).
static const CookVoxelMaterial *voxel_material(VoxelImages *v, i32 m)
{
    const u32 at = m >= 0 && (u32)m < v->material_count ? (u32)m : v->material_count;
    CookVoxelMaterial *out = &v->materials[at];
    if (v->ready[at] != 0U) {
        return out;
    }
    v->ready[at] = 1U;
    if (at == v->material_count) {
        NphMaterialHeader h;
        nph_material_header_default(&h);
        cook_voxel_material_init(out, &h);
        return out;
    }
    const CookGltfMaterial *mat = &v->scene->materials[at];
    const NphMaterialHeader *h = &mat->header;
    cook_voxel_material_init(out, h);
    if (out->surface == NPH_RENDER_VOXEL_SURFACE_NONE) {
        return out; // its images are never read
    }
    const i32 *image = mat->image;
    if (mat->specgloss) {
        voxel_specgloss(v, at, mat);
        const CookVoxelImage *base = &v->sg[(u64)at * 2U];
        const CookVoxelImage *mr = &v->sg[(u64)at * 2U + 1U];
        out->base_color_image =
            voxel_slot(base->texels != NULL ? base : NULL, h, NPH_MATERIAL_TEXTURE_BASE_COLOR);
        out->metallic_roughness_image =
            voxel_slot(mr->texels != NULL ? mr : NULL, h, NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS);
    } else {
        out->base_color_image =
            voxel_slot(voxel_image(v, image[NPH_MATERIAL_TEXTURE_BASE_COLOR], true), h,
                       NPH_MATERIAL_TEXTURE_BASE_COLOR);
        out->metallic_roughness_image =
            voxel_slot(voxel_image(v, image[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS], false), h,
                       NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS);
    }
    out->normal_image = voxel_slot(voxel_image(v, image[NPH_MATERIAL_TEXTURE_NORMAL], false), h,
                                   NPH_MATERIAL_TEXTURE_NORMAL);
    out->occlusion_image = voxel_slot(voxel_image(v, image[NPH_MATERIAL_TEXTURE_OCCLUSION], false),
                                      h, NPH_MATERIAL_TEXTURE_OCCLUSION);
    return out;
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
    // Meshes, with their materials for voxels where the settings ask.
    const u64 mesh_shash = settings_hash(c, NPH_ASSET_TYPE_MESH);
    const bool with_voxels = c->mesh.voxels != 0;
    VoxelImages voxel_images;
    memset(&voxel_images, 0, sizeof voxel_images);
    if (r == NPH_RESULT_OK && with_voxels) {
        r = voxel_images_init(&voxel_images, c, key, &scene);
    }
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
        memset(&st, 0, sizeof st);
        CookVoxelMaterial *slots = NULL;
        CookVoxelMaterials materials;
        memset(&materials, 0, sizeof materials);
        if (with_voxels) {
            slots = NPH_ALLOC_ARRAY_ZEROED(alloc, CookVoxelMaterial,
                                           nph_max_u32(gm->material_count, 1U));
            if (slots == NULL) {
                r = NPH_RESULT_OUT_OF_MEMORY;
            }
            for (u32 s = 0; s < gm->material_count && slots != NULL; ++s) {
                slots[s] = *voxel_material(&voxel_images, gm->materials[s]);
            }
            materials.slots = slots;
            materials.slot_count = gm->material_count;
        }
        if (r == NPH_RESULT_OK) {
            r = cook_mesh_process(&gm->mesh, &c->mesh, slots != NULL ? &materials : NULL, &w, &st);
        }
        if (slots != NULL) {
            NPH_FREE_ARRAY(alloc, CookVoxelMaterial, slots, nph_max_u32(gm->material_count, 1U));
        }
        if (r == NPH_RESULT_OK) {
            NPH_LOG_INFO(
                &NPH_LOG_COOK,
                "mesh %s: %u -> %u vertices, %u -> %u triangles, %u meshlets, %u levels (the "
                "coarsest %u triangles in %u clusters; %u voxels in %u bricks, %u clusters), "
                "ACMR %.2f -> %.2f",
                gm->mesh.name, st.source_vertices, st.vertices, st.source_triangles, st.triangles,
                st.meshlets, st.lod_levels, st.lod_coarsest_triangles, st.lod_coarsest_clusters,
                st.voxels, st.voxel_bricks, st.voxel_clusters, (f64)st.acmr_before,
                (f64)st.acmr_after);
            // A line of its own: a log message holds NPH_LOG_MESSAGE_MAX bytes.
            NPH_LOG_INFO(&NPH_LOG_COOK,
                         "mesh %s: %.2f s (prepare %.2f, reorder %.2f, meshlets %.2f, levels %.2f, "
                         "compression %.2f)",
                         gm->mesh.name, nph_time_ns_to_s(st.total_ns),
                         nph_time_ns_to_s(st.prepare_ns), nph_time_ns_to_s(st.optimize_ns),
                         nph_time_ns_to_s(st.meshlets_ns), nph_time_ns_to_s(st.lods_ns),
                         nph_time_ns_to_s(st.blobs_ns));
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
    voxel_images_free(&voxel_images);
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
    static const char *const images[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds"};
    const NphStr ext = nph_path_extension(nph_str_from_cstr(path));
    if (is_scene_file(ext)) {
        return COOKER_SOURCE_SCENE;
    }
    if (nph_str_equals_ignore_case(ext, NPH_STR(".nmesh"))) {
        return COOKER_SOURCE_MESH;
    }
    if (nph_str_equals_ignore_case(ext, NPH_STR(".wav"))) {
        return COOKER_SOURCE_SOUND;
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
    case COOKER_SOURCE_SOUND:
        return cooker_cook_sound(c, source_path);
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
        if (kind != COOKER_SOURCE_SCENE && kind != COOKER_SOURCE_MESH &&
            kind != COOKER_SOURCE_SOUND) {
            continue;
        }
        const NphResult fr = kind == COOKER_SOURCE_SCENE  ? cooker_cook_gltf(c, path)
                             : kind == COOKER_SOURCE_MESH ? cooker_cook_mesh(c, path)
                                                          : cooker_cook_sound(c, path);
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

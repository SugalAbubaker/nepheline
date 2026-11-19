// SPDX-License-Identifier: Apache-2.0
// The project: the import thread, its requests, and the snapshot of the
// cooker's database that the resolver reads (project.h).
#include "project.h"

#include <nepheline/asset/vfs.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/json.h>
#include <nepheline/core/string.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "editor_log.h"

#include <cook/cooker.h>
#include <cook/database.h>
#include <cook/gltf.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Directory copies stop this deep (a link cycle would not end otherwise).
#define TREE_DEPTH_MAX 32U
#define STATUS_MAX     256U

typedef enum RequestKind {
    REQUEST_IMPORT = 0, // copy what lies outside the sources, then cook
    REQUEST_COOK,       // cook one source of the project
} RequestKind;

typedef struct Request {
    u32 id;
    u32 kind;
    u64 tag;
    b32 force;
    char folder[EDITOR_PROJECT_PATH_MAX]; // imports: where outside paths go, normalised
    char *paths[EDITOR_IMPORT_PATHS_MAX]; // owned (a cook: the key)
    u32 path_count;
} Request;

// Where a copied source came from (watch.json beside the database; ADR-0107).
typedef struct Origin {
    char *key;  // the source's, relative to the sources (owned)
    char *path; // what was copied, canonical (owned)
    b32 copied; // copied in since the watch last synced: what it is now is not a change
} Origin;

typedef NPH_ARRAY(Origin) OriginArray;

// A file the watcher looks at, and what it saw of it last.
typedef struct WatchFile {
    char *path; // owned
    u64 size;
    u64 modified_ns;
    b32 exists;
    b32 at_origin; // the origin's, or a file beside it: a change imports the origin again
    b32 pending;   // it changed, and no cook has taken the change yet
} WatchFile;

typedef NPH_ARRAY(WatchFile) WatchFileArray;

// A source of the database, with its settings, the files a scene names, and its origin's.
typedef struct WatchSource {
    char *key;    // owned
    char *origin; // the origin its files were made with, NULL for none (owned)
    WatchFileArray files;
    u64 changed_ns;  // when a change was last seen; 0 when none waits
    b32 from_origin; // a change at the origin waits
    b32 in_project;  // a change in the sources waits
    b32 seen;        // in the database at the last sync
    b32 due;         // settled at this look: to take in
} WatchSource;

typedef NPH_ARRAY(WatchSource) WatchSourceArray;

// What the watch saw of a file when the last session ended (watch.json): the baseline its first
// look compares against, so what changed while the editor was closed is a change.
typedef struct Seen {
    char *path; // owned
    u64 size;
    u64 modified_ns;
    u32 exists; // 0, 1, or WATCH_PENDING
} Seen;

typedef NPH_ARRAY(Seen) SeenArray;

// The database as the last request left it, and its sources.
typedef struct Snapshot {
    CookDb db;
    NphHashMap sources; // nph_str_hash(source) -> u32 index of one of its entries
} Snapshot;

struct EditorProject {
    NphAllocator *alloc;
    NphJobSystem *jobs;
    char assets[EDITOR_PROJECT_PATH_MAX]; // canonical
    char cooked[EDITOR_PROJECT_PATH_MAX];
    char db_path[EDITOR_PROJECT_PATH_MAX + 32U];
    NphThread thread;

    NphMutex lock;      // everything below
    NphCondVar changed; // a request queued or finished, or quit
    b32 quit;
    Request queue[EDITOR_PROJECT_REQUESTS];
    u32 queue_head;
    u32 queue_count;
    b32 running;
    EditorImportResult done[EDITOR_PROJECT_REQUESTS];
    u32 done_head;
    u32 done_count;
    u32 next_id;
    char status[STATUS_MAX];
    Snapshot *snapshot; // NULL without a database
    u64 generation;

    // The import thread's alone (no lock; ADR-0107): where copied sources came from, and what
    // the watcher saw last of the files it watches.
    u64 watch_interval_ns; // 0: no watching
    u64 watch_settle_ns;
    char watch_path[EDITOR_PROJECT_PATH_MAX + 32U];
    OriginArray origins;
    b32 watch_dirty;
    WatchSourceArray watch;
    SeenArray seen;      // the last session's baseline, until the first sync takes it
    NphHashMap seen_map; // nph_str_hash(path) -> u32 index into seen
};

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------

static void snapshot_free(NphAllocator *alloc, Snapshot *s)
{
    if (s == NULL) {
        return;
    }
    nph_hashmap_free(&s->sources);
    cook_db_free(&s->db);
    NPH_FREE_TYPE(alloc, Snapshot, s);
}

// The database on disk, or NULL when there is none or it does not load
// (logged; the previous snapshot then stays).
static Snapshot *snapshot_load(EditorProject *p)
{
    if (!nph_fs_exists(p->db_path)) {
        return NULL;
    }
    Snapshot *s = NPH_ALLOC_TYPE(p->alloc, Snapshot);
    if (s == NULL) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: out of memory reading %s", p->db_path);
        return NULL;
    }
    NphResult r = cook_db_init(&s->db, p->alloc);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(p->alloc, Snapshot, s);
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: %s", nph_result_string(r));
        return NULL;
    }
    const NphHashMapDesc d = nph_hashmap_desc_u64(sizeof(u32));
    r = nph_hashmap_init(&s->sources, p->alloc, &d);
    if (r == NPH_RESULT_OK) {
        r = cook_db_load(&s->db, p->db_path);
    }
    for (u32 i = 0; i < cook_db_count(&s->db) && r == NPH_RESULT_OK; ++i) {
        const u64 h = nph_str_hash(nph_str_from_cstr(cook_db_entry(&s->db, i)->source));
        if (!nph_hashmap_contains_u64(&s->sources, h)) {
            r = nph_hashmap_insert_u64(&s->sources, h, &i);
        }
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: %s: %s", p->db_path, nph_result_string(r));
        snapshot_free(p->alloc, s);
        return NULL;
    }
    return s;
}

// An entry of `key`'s source, or NULL. Under the lock.
static const CookDbEntry *snapshot_source(const Snapshot *s, const char *key)
{
    if (s == NULL) {
        return NULL;
    }
    const NphStr k = nph_str_from_cstr(key);
    const u32 *index = (const u32 *)nph_hashmap_find_u64(&s->sources, nph_str_hash(k));
    if (index != NULL && strcmp(cook_db_entry(&s->db, *index)->source, key) == 0) {
        return cook_db_entry(&s->db, *index);
    }
    // Two sources with one hash: the map kept the first, so look further.
    for (u32 i = 0; index != NULL && i < cook_db_count(&s->db); ++i) {
        if (strcmp(cook_db_entry(&s->db, i)->source, key) == 0) {
            return cook_db_entry(&s->db, i);
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// The import thread's work
// ---------------------------------------------------------------------------

NPH_PRINTF_LIKE(2, 3)
static void set_status(EditorProject *p, const char *fmt, ...)
{
    char text[STATUS_MAX];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    nph_mutex_lock(&p->lock);
    memcpy(p->status, text, sizeof text);
    nph_mutex_unlock(&p->lock);
}

static NphResult join(char *out, u64 cap, const char *a, const char *b)
{
    if (b[0] == '\0') {
        const u64 n = strlen(a);
        if (n + 1U > cap) {
            return NPH_RESULT_OVERFLOW;
        }
        memcpy(out, a, (size_t)n + 1U);
        return NPH_RESULT_OK;
    }
    return nph_path_join(out, cap, nph_str_from_cstr(a), nph_str_from_cstr(b), NULL);
}

// ---------------------------------------------------------------------------
// What the watch keeps between sessions (watch.json beside the database; ADR-0107): where each
// copied source came from, and what it saw last of every file it watches
// ---------------------------------------------------------------------------

#define WATCH_FILE_NAME "watch.json"
#define WATCH_VERSION   1
#define WATCH_PENDING   2U // a file's `exists` in watch.json: it changed, and no cook took it

static char *dup_cstr(NphAllocator *alloc, const char *s)
{
    const u64 n = strlen(s) + 1U;
    char *out = NPH_ALLOC_ARRAY(alloc, char, n);
    if (out != NULL) {
        memcpy(out, s, (size_t)n);
    }
    return out;
}

static void free_cstr(NphAllocator *alloc, char *s)
{
    if (s != NULL) {
        NPH_FREE_ARRAY(alloc, char, s, strlen(s) + 1U);
    }
}

static Origin *origin_find(EditorProject *p, const char *key)
{
    NPH_ARRAY_FOREACH (Origin, o, &p->origins) {
        if (strcmp(o->key, key) == 0) {
            return o;
        }
    }
    return NULL;
}

// Sets `key`'s origin to `path` (both copied); marks the origins to save when it changed and
// `dirty`.
static NphResult origin_set(EditorProject *p, const char *key, const char *path, bool dirty)
{
    Origin *o = origin_find(p, key);
    if (o != NULL && strcmp(o->path, path) == 0) {
        return NPH_RESULT_OK;
    }
    char *copy = dup_cstr(p->alloc, path);
    if (copy == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (o != NULL) {
        free_cstr(p->alloc, o->path);
        o->path = copy;
    } else {
        Origin fresh;
        memset(&fresh, 0, sizeof fresh);
        fresh.key = dup_cstr(p->alloc, key);
        fresh.path = copy;
        NphResult r =
            fresh.key != NULL ? NPH_ARRAY_PUSH(&p->origins, fresh) : NPH_RESULT_OUT_OF_MEMORY;
        if (r != NPH_RESULT_OK) {
            free_cstr(p->alloc, fresh.key);
            free_cstr(p->alloc, copy);
            return r;
        }
    }
    p->watch_dirty = p->watch_dirty != 0 || dirty ? 1 : 0;
    return NPH_RESULT_OK;
}

// Records that the source at `to` (within the sources) was copied from `from`. A failure only
// costs the watching, so it is logged, not returned.
static void origin_note(EditorProject *p, const char *from, const char *to)
{
    char key[EDITOR_PROJECT_PATH_MAX];
    char origin[EDITOR_PROJECT_PATH_MAX];
    NphResult r = cooker_relative_path(p->assets, to, key, sizeof key);
    if (r == NPH_RESULT_OK) {
        r = cooker_canonical_path(from, origin, sizeof origin);
    }
    if (r == NPH_RESULT_OK) {
        r = origin_set(p, key, origin, true);
    }
    Origin *o = r == NPH_RESULT_OK ? origin_find(p, key) : NULL;
    if (o != NULL) {
        o->copied = 1; // the copy is not a change for the watch to take
    } else {
        NPH_LOG_WARN(&LOG_EDITOR, "assets: where %s came from is not kept (%s)", to,
                     nph_result_string(r));
    }
}

static NphResult seen_add(EditorProject *p, const char *file, u64 size, u64 modified_ns, u32 exists)
{
    Seen e;
    memset(&e, 0, sizeof e);
    e.path = dup_cstr(p->alloc, file);
    e.size = size;
    e.modified_ns = modified_ns;
    e.exists = exists;
    const u32 index = p->seen.len;
    NphResult r = e.path != NULL ? NPH_ARRAY_PUSH(&p->seen, e) : NPH_RESULT_OUT_OF_MEMORY;
    if (r != NPH_RESULT_OK) {
        free_cstr(p->alloc, e.path);
        return r;
    }
    r = nph_hashmap_insert_u64(&p->seen_map, nph_str_hash(nph_str_from_cstr(file)), &index);
    return r == NPH_RESULT_ALREADY_EXISTS ? NPH_RESULT_OK : r; // the first of a path is it
}

static const Seen *seen_find(const EditorProject *p, const char *file)
{
    const u32 *index =
        (const u32 *)nph_hashmap_find_u64(&p->seen_map, nph_str_hash(nph_str_from_cstr(file)));
    return index != NULL && strcmp(p->seen.data[*index].path, file) == 0 ? &p->seen.data[*index]
                                                                         : NULL;
}

static void seen_free(EditorProject *p)
{
    NPH_ARRAY_FOREACH (Seen, e, &p->seen) {
        free_cstr(p->alloc, e->path);
    }
    NPH_ARRAY_CLEAR(&p->seen);
    nph_hashmap_clear(&p->seen_map);
}

static void origins_free(EditorProject *p)
{
    NPH_ARRAY_FOREACH (Origin, o, &p->origins) {
        free_cstr(p->alloc, o->key);
        free_cstr(p->alloc, o->path);
    }
    NPH_ARRAY_FREE(&p->origins);
}

// What an earlier session kept: {"watch": 1, "origins": {"<key>": "<path>", ...}, "seen":
// {"<path>": [size, time, exists], ...}}. A file that does not read is warned about: its
// sources are then re-imported only when they change again, or are dropped again.
static void watch_load(EditorProject *p)
{
    if (!nph_fs_exists(p->watch_path)) {
        return;
    }
    void *text = NULL;
    u64 size = 0U;
    NphResult r = nph_file_read_all(p->watch_path, p->alloc, &text, &size);
    NphJsonDoc doc;
    memset(&doc, 0, sizeof doc);
    const NphJsonValue *files = NULL;
    if (r == NPH_RESULT_OK) {
        r = nph_json_parse(nph_str((const char *)text, size), p->alloc, &doc);
    }
    i64 version = 0;
    const NphJsonValue *seen = NULL;
    if (r == NPH_RESULT_OK && doc.root != NULL && doc.root->type == NPH_JSON_OBJECT &&
        nph_json_as_i64(nph_json_object_get(doc.root, NPH_STR("watch")), &version) ==
            NPH_RESULT_OK &&
        version == WATCH_VERSION) {
        files = nph_json_object_get(doc.root, NPH_STR("origins"));
        seen = nph_json_object_get(doc.root, NPH_STR("seen"));
    }
    if (seen != NULL && seen->type == NPH_JSON_OBJECT) {
        NPH_JSON_FOREACH(e, seen)
        {
            i64 v[3] = {0, 0, 0};
            bool ok = e->type == NPH_JSON_ARRAY && e->count == 3U;
            for (u32 k = 0; k < 3U && ok; ++k) {
                ok = nph_json_as_i64(nph_json_array_at(e, k), &v[k]) == NPH_RESULT_OK && v[k] >= 0;
            }
            if (ok && v[2] <= (i64)WATCH_PENDING &&
                seen_add(p, e->key.data, (u64)v[0], (u64)v[1], (u32)v[2]) != NPH_RESULT_OK) {
                break;
            }
        }
    }
    if (files != NULL && files->type == NPH_JSON_OBJECT) {
        NPH_JSON_FOREACH(e, files)
        {
            NphStr path;
            char key[EDITOR_PROJECT_PATH_MAX];
            char origin[EDITOR_PROJECT_PATH_MAX];
            if (nph_json_as_str(e, &path) != NPH_RESULT_OK || e->key.len >= sizeof key ||
                path.len >= sizeof origin) {
                continue;
            }
            memcpy(key, e->key.data, (size_t)e->key.len);
            key[e->key.len] = '\0';
            memcpy(origin, path.data, (size_t)path.len);
            origin[path.len] = '\0';
            if (origin_set(p, key, origin, false) != NPH_RESULT_OK) {
                break;
            }
        }
    } else {
        NPH_LOG_WARN(&LOG_EDITOR,
                     "assets: %s is not what the editor's watch keeps; sources are re-imported "
                     "only when they change again",
                     p->watch_path);
    }
    nph_json_doc_free(&doc);
    if (text != NULL) {
        nph_file_free_all(p->alloc, text, size);
    }
}

// One file's state as watch.json keeps it: "<path>": [size, time, exists].
static NphResult seen_write(NphJsonWriter *w, const char *file, u64 size, u64 modified_ns,
                            u32 exists)
{
    NPH_TRY(nph_json_writer_key(w, nph_str_from_cstr(file)));
    NPH_TRY(nph_json_writer_begin_array_inline(w));
    NPH_TRY(nph_json_writer_i64(w, (i64)size));
    NPH_TRY(nph_json_writer_i64(w, (i64)modified_ns));
    NPH_TRY(nph_json_writer_i64(w, (i64)exists));
    return nph_json_writer_end_array(w);
}

// Writes what the watch keeps over watch.json whole (a ".part" file renamed over it).
static void watch_save(EditorProject *p)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, p->alloc);
    NphJsonWriter w;
    nph_json_writer_init(&w, &sb, true);
    NphResult r = nph_json_writer_begin_object(&w);
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("watch"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_i64(&w, WATCH_VERSION);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("origins"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_begin_object(&w);
    }
    for (u32 i = 0; i < p->origins.len && r == NPH_RESULT_OK; ++i) {
        r = nph_json_writer_key(&w, nph_str_from_cstr(p->origins.data[i].key));
        if (r == NPH_RESULT_OK) {
            r = nph_json_writer_str(&w, nph_str_from_cstr(p->origins.data[i].path));
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_end_object(&w);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("seen"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_begin_object(&w);
    }
    if (p->watch_interval_ns == 0U) {
        // Nothing watched this session: the last one's baseline stays.
        for (u32 i = 0; i < p->seen.len && r == NPH_RESULT_OK; ++i) {
            const Seen *e = &p->seen.data[i];
            r = seen_write(&w, e->path, e->size, e->modified_ns, e->exists);
        }
    }
    for (u32 i = 0; i < p->watch.len && r == NPH_RESULT_OK; ++i) {
        NPH_ARRAY_FOREACH (WatchFile, f, &p->watch.data[i].files) {
            r = seen_write(&w, f->path, f->size, f->modified_ns,
                           f->pending != 0 ? WATCH_PENDING : (u32)f->exists);
            if (r != NPH_RESULT_OK) {
                break;
            }
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_end_object(&w);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_end_object(&w);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_sb_append_cstr(&sb, "\n");
    }
    char temp[EDITOR_PROJECT_PATH_MAX + 48U];
    if (r == NPH_RESULT_OK &&
        (u64)snprintf(temp, sizeof temp, "%s.part", p->watch_path) >= sizeof temp) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r == NPH_RESULT_OK) {
        r = nph_fs_create_directories(p->cooked);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(temp, sb.data, sb.len);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_fs_rename(temp, p->watch_path);
    }
    nph_sb_free(&sb);
    if (r == NPH_RESULT_OK) {
        p->watch_dirty = 0;
    } else {
        NPH_LOG_WARN(&LOG_EDITOR, "assets: %s: %s", p->watch_path, nph_result_string(r));
    }
}

static NphResult copy_tree(EditorProject *p, const char *from, const char *to, u32 depth)
{
    if (depth >= TREE_DEPTH_MAX) {
        NPH_LOG_ERROR(&LOG_EDITOR, "import: %s is nested too deeply", from);
        return NPH_RESULT_OVERFLOW;
    }
    NPH_TRY(nph_fs_create_directories(to));
    NphDirIterator it;
    NPH_TRY(nph_dir_open(from, &it));
    NphResult r = NPH_RESULT_OK;
    NphDirEntry entry;
    while (r == NPH_RESULT_OK && nph_dir_next(&it, &entry)) {
        char src[EDITOR_PROJECT_PATH_MAX];
        char dst[EDITOR_PROJECT_PATH_MAX];
        r = join(src, sizeof src, from, entry.name);
        if (r == NPH_RESULT_OK) {
            r = join(dst, sizeof dst, to, entry.name);
        }
        if (r == NPH_RESULT_OK) {
            r = entry.is_directory ? copy_tree(p, src, dst, depth + 1U)
                                   : nph_fs_copy_file(src, dst);
            if (r == NPH_RESULT_OK && !entry.is_directory) {
                origin_note(p, src, dst);
            }
        }
    }
    nph_dir_close(&it);
    return r;
}

typedef struct DependencyCopy {
    char from_dir[EDITOR_PROJECT_PATH_MAX];
    char to_dir[EDITOR_PROJECT_PATH_MAX];
    NphResult result;
} DependencyCopy;

// One file a document names, copied to the same place beside its copy.
static void copy_dependency(void *user, const char *relative_path)
{
    DependencyCopy *d = (DependencyCopy *)user;
    if (d->result != NPH_RESULT_OK) {
        return;
    }
    char src[EDITOR_PROJECT_PATH_MAX];
    char dst[EDITOR_PROJECT_PATH_MAX];
    NphResult r = join(src, sizeof src, d->from_dir, relative_path);
    if (r == NPH_RESULT_OK) {
        r = join(dst, sizeof dst, d->to_dir, relative_path);
    }
    if (r == NPH_RESULT_OK) {
        const NphStr dir = nph_path_dirname(nph_str_from_cstr(dst));
        char parent[EDITOR_PROJECT_PATH_MAX];
        memcpy(parent, dir.data, (size_t)dir.len);
        parent[dir.len] = '\0';
        r = nph_fs_create_directories(parent);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_fs_copy_file(src, dst);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "import: cannot copy %s (%s)", src, nph_result_string(r));
        d->result = r;
    }
}

static void dirname_of(const char *path, char *out, u64 cap)
{
    const NphStr dir = nph_path_dirname(nph_str_from_cstr(path));
    const u64 n = dir.len < cap ? dir.len : cap - 1U;
    memcpy(out, dir.data, (size_t)n);
    out[n] = '\0';
}

// Copies a file, and a scene with the files it names.
static NphResult copy_source(EditorProject *p, const char *from, const char *to)
{
    NPH_TRY(nph_fs_copy_file(from, to));
    origin_note(p, from, to);
    const CookerSourceKind kind = cooker_source_kind(from);
    if (kind == COOKER_SOURCE_SOUND || kind == COOKER_SOURCE_IMAGE || kind == COOKER_SOURCE_SCENE) {
        // A sound's, an image's or a scene's settings file comes with it (cook/sound.h,
        // cook/texture_import.h, cook/scene_import.h); one already in the project stays when
        // the file comes without.
        char settings_from[EDITOR_PROJECT_PATH_MAX + 16U];
        char settings_to[EDITOR_PROJECT_PATH_MAX + 16U];
        if ((u64)snprintf(settings_from, sizeof settings_from, "%s.import.json", from) >=
                sizeof settings_from ||
            (u64)snprintf(settings_to, sizeof settings_to, "%s.import.json", to) >=
                sizeof settings_to) {
            return NPH_RESULT_OVERFLOW;
        }
        if (nph_fs_exists(settings_from)) {
            NPH_TRY(nph_fs_copy_file(settings_from, settings_to));
        }
    }
    if (kind != COOKER_SOURCE_SCENE) {
        return NPH_RESULT_OK;
    }
    DependencyCopy d;
    memset(&d, 0, sizeof d);
    dirname_of(from, d.from_dir, sizeof d.from_dir);
    dirname_of(to, d.to_dir, sizeof d.to_dir);
    const NphResult r = cook_gltf_files(p->alloc, from, copy_dependency, &d);
    return r != NPH_RESULT_OK ? r : d.result;
}

// Records the asset `sub` cooked from `source` in a result's list.
static void note(Cooker *c, const char *source, const char *sub, NphGuid *guids,
                 char (*names)[EDITOR_IMPORT_NAME_MAX], u32 *count)
{
    char key[EDITOR_PROJECT_PATH_MAX];
    if (*count == EDITOR_IMPORT_SCENES_MAX ||
        cooker_source_key(c, source, key, sizeof key) != NPH_RESULT_OK) {
        return;
    }
    const CookDbEntry *e =
        cook_db_find(cooker_db(c), nph_str_from_cstr(key), nph_str_from_cstr(sub));
    if (e == NULL) {
        return;
    }
    const NphStr stem = nph_path_stem(nph_str_from_cstr(source));
    const u64 n = stem.len < EDITOR_IMPORT_NAME_MAX ? stem.len : EDITOR_IMPORT_NAME_MAX - 1U;
    guids[*count] = e->guid;
    memcpy(names[*count], stem.data, (size_t)n);
    names[*count][n] = '\0';
    ++*count;
}

// Cooks a source of the project: a directory's scenes, editable meshes, and
// sounds, a scene, an editable mesh, a sound, or an image (unless images
// are only along for a scene). Anything else was only copied.
static NphResult cook_source(EditorProject *p, Cooker *c, const char *source, bool is_directory,
                             bool images_alone, EditorImportResult *res)
{
    const NphStr name = nph_path_basename(nph_str_from_cstr(source));
    set_status(p, "Cooking %.*s", (int)name.len, name.data);
    if (is_directory) {
        return cooker_cook_directory(c, source);
    }
    switch (cooker_source_kind(source)) {
    case COOKER_SOURCE_SCENE: {
        const NphResult r = cooker_cook_gltf(c, source);
        if (r == NPH_RESULT_OK) {
            note(c, source, "scene", res->scenes, res->scene_names, &res->scene_count);
        }
        return r;
    }
    case COOKER_SOURCE_MESH: {
        const NphResult r = cooker_cook_mesh(c, source);
        if (r == NPH_RESULT_OK) {
            note(c, source, "mesh", res->meshes, res->mesh_names, &res->mesh_count);
        }
        return r;
    }
    case COOKER_SOURCE_IMAGE:
        return images_alone ? cooker_cook_file(c, source) : NPH_RESULT_OK;
    case COOKER_SOURCE_SOUND:
        // No scene names a sound: it cooks whenever it comes in.
        return cooker_cook_sound(c, source);
    case COOKER_SOURCE_MATERIAL:
        // A material cooks whenever it comes in or changes, with its images;
        // what it wrote is reloaded with the rest (`note_written`).
        return cooker_cook_material(c, source);
    case COOKER_SOURCE_NONE:
        break;
    }
    return NPH_RESULT_OK;
}

// Brings one path of an import into the sources and cooks it.
static NphResult import_path(EditorProject *p, Cooker *c, const Request *req, const char *path,
                             bool images_alone, EditorImportResult *res)
{
    char abs[EDITOR_PROJECT_PATH_MAX];
    NphResult r = cooker_canonical_path(path, abs, sizeof abs);
    NphFileInfo info;
    memset(&info, 0, sizeof info);
    if (r == NPH_RESULT_OK) {
        r = nph_fs_stat(abs, &info);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "import: %s: %s", path, nph_result_string(r));
        return r;
    }
    char rel[EDITOR_PROJECT_PATH_MAX];
    char source[EDITOR_PROJECT_PATH_MAX];
    if (cooker_relative_path(p->assets, abs, rel, sizeof rel) == NPH_RESULT_OK) {
        // Already a source: cooked where it is.
        memcpy(source, abs, strlen(abs) + 1U);
    } else if (info.is_directory &&
               cooker_relative_path(abs, p->assets, rel, sizeof rel) == NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "import: %s holds the project's sources; not copied", abs);
        return NPH_RESULT_INVALID_ARGUMENT;
    } else {
        char dir[EDITOR_PROJECT_PATH_MAX];
        const NphStr name = nph_path_basename(nph_str_from_cstr(abs));
        char file[EDITOR_PROJECT_PATH_MAX];
        memcpy(file, name.data, (size_t)name.len);
        file[name.len] = '\0';
        r = join(dir, sizeof dir, p->assets, req->folder);
        if (r == NPH_RESULT_OK) {
            r = join(source, sizeof source, dir, file);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_fs_create_directories(dir);
        }
        if (r == NPH_RESULT_OK) {
            set_status(p, "Copying %s", file);
            r = info.is_directory ? copy_tree(p, abs, source, 0U) : copy_source(p, abs, source);
        }
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_EDITOR, "import: copying %s: %s", abs, nph_result_string(r));
            return r;
        }
    }
    return cook_source(p, c, source, info.is_directory, images_alone, res);
}

static NphResult cook_key(EditorProject *p, Cooker *c, const char *key, EditorImportResult *res)
{
    char source[EDITOR_PROJECT_PATH_MAX];
    NPH_TRY(join(source, sizeof source, p->assets, key));
    NphFileInfo info;
    const NphResult r = nph_fs_stat(source, &info);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "cook: %s: %s", key, nph_result_string(r));
        return r;
    }
    if (!info.is_directory && cooker_source_kind(source) == COOKER_SOURCE_NONE) {
        NPH_LOG_ERROR(&LOG_EDITOR, "cook: %s is neither a scene nor an image", key);
        return NPH_RESULT_UNSUPPORTED;
    }
    return cook_source(p, c, source, info.is_directory, true, res);
}

typedef NPH_ARRAY(NphGuid) GuidArray;

typedef struct Written {
    GuidArray guids;
    NphResult result; // the first failure to note one
} Written;

static void note_written(void *user, NphGuid guid, u32 type)
{
    NPH_UNUSED(type);
    Written *w = (Written *)user;
    const NphResult r = NPH_ARRAY_PUSH(&w->guids, guid);
    if (r != NPH_RESULT_OK && w->result == NPH_RESULT_OK) {
        w->result = r;
    }
}

// The written GUIDs, moved into the result in a block of their own.
static NphResult hand_over(EditorProject *p, Written *w, EditorImportResult *res)
{
    NphResult r = w->result;
    if (r == NPH_RESULT_OK && w->guids.len != 0U) {
        res->written = NPH_ALLOC_ARRAY(p->alloc, NphGuid, w->guids.len);
        if (res->written == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        } else {
            memcpy(res->written, w->guids.data, sizeof(NphGuid) * (u64)w->guids.len);
            res->written_count = w->guids.len;
        }
    }
    NPH_ARRAY_FREE(&w->guids);
    if (r != NPH_RESULT_OK) {
        // The cook happened; only the viewport will not know to reload.
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: keeping what to reload: %s", nph_result_string(r));
    }
    return r;
}

static void run_request(EditorProject *p, const Request *req, EditorImportResult *res)
{
    memset(res, 0, sizeof *res);
    res->id = req->id;
    res->tag = req->tag;
    Written written;
    memset(&written, 0, sizeof written);
    NPH_ARRAY_INIT(&written.guids, p->alloc);
    CookerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = p->alloc;
    d.out_dir = p->cooked;
    d.source_root = p->assets;
    d.jobs = p->jobs;
    d.force = req->force;
    d.on_cooked = note_written;
    d.on_cooked_user = &written;
    cook_texture_settings_default(&d.texture);
    cook_mesh_settings_default(&d.mesh);
    Cooker *c = NULL;
    res->result = cooker_create(&d, &c);
    if (res->result != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: the cooker for %s: %s", p->cooked,
                      nph_result_string(res->result));
        NPH_ARRAY_FREE(&written.guids);
        return;
    }
    // A request holding a scene brings images along for it: the scene cooks
    // the ones it uses, with the usage each material slot needs.
    bool images_alone = true;
    for (u32 i = 0; i < req->path_count; ++i) {
        if (cooker_source_kind(req->paths[i]) == COOKER_SOURCE_SCENE) {
            images_alone = false;
        }
    }
    for (u32 i = 0; i < req->path_count; ++i) {
        const NphResult r = req->kind == REQUEST_IMPORT
                                ? import_path(p, c, req, req->paths[i], images_alone, res)
                                : cook_key(p, c, req->paths[i], res);
        if (r != NPH_RESULT_OK && res->result == NPH_RESULT_OK) {
            res->result = r;
        }
    }
    const CookerStats stats = cooker_stats(c);
    res->cooked = stats.cooked;
    res->skipped = stats.skipped;
    res->failed = stats.failed;
    cooker_destroy(c);                              // saves the database
    NPH_IGNORE_RESULT(hand_over(p, &written, res)); // logged
}

static void free_request(NphAllocator *alloc, Request *req)
{
    for (u32 i = 0; i < req->path_count; ++i) {
        NPH_FREE_ARRAY(alloc, char, req->paths[i], strlen(req->paths[i]) + 1U);
        req->paths[i] = NULL;
    }
    req->path_count = 0U;
}

// ---------------------------------------------------------------------------
// Watching (project.h; ADR-0107): the import thread's alone
// ---------------------------------------------------------------------------

// Takes a file's state in; true when it differs from what was seen last.
static bool watch_file_changed(WatchFile *f)
{
    NphFileInfo info;
    memset(&info, 0, sizeof info);
    const bool exists = nph_fs_stat(f->path, &info) == NPH_RESULT_OK && !info.is_directory;
    const u64 size = exists ? info.size : 0U;
    const u64 modified = exists ? info.modified_ns : 0U;
    const bool changed =
        (f->exists != 0) != exists || f->size != size || f->modified_ns != modified;
    f->exists = exists ? 1 : 0;
    f->size = size;
    f->modified_ns = modified;
    return changed;
}

static NphResult watch_add_file(EditorProject *p, WatchSource *s, const char *path, bool at_origin)
{
    WatchFile f;
    memset(&f, 0, sizeof f);
    f.path = dup_cstr(p->alloc, path);
    if (f.path == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    f.at_origin = at_origin ? 1 : 0;
    const Seen *was = seen_find(p, path);
    if (was != NULL) {
        // The last session's: a change since, made while the editor was closed, is a change.
        f.size = was->size;
        f.modified_ns = was->modified_ns;
        f.exists = was->exists == 1U ? 1 : 0;
        f.pending = was->exists == WATCH_PENDING ? 1 : 0;
    } else {
        (void)watch_file_changed(&f); // what it is now is the baseline
    }
    p->watch_dirty = 1;
    const NphResult r = NPH_ARRAY_PUSH(&s->files, f);
    if (r != NPH_RESULT_OK) {
        free_cstr(p->alloc, f.path);
    }
    return r;
}

typedef struct CompanionAdd {
    EditorProject *p;
    WatchSource *s;
    char dir[EDITOR_PROJECT_PATH_MAX];
    b32 at_origin;
    NphResult result;
} CompanionAdd;

static void watch_add_companion(void *user, const char *relative_path)
{
    CompanionAdd *a = (CompanionAdd *)user;
    char path[EDITOR_PROJECT_PATH_MAX];
    if (a->result == NPH_RESULT_OK) {
        a->result = join(path, sizeof path, a->dir, relative_path);
    }
    if (a->result == NPH_RESULT_OK) {
        a->result = watch_add_file(a->p, a->s, path, a->at_origin != 0);
    }
}

// A file and what comes with it: its settings and, for a scene, the files it names.
static NphResult watch_add_with_companions(EditorProject *p, WatchSource *s, const char *path,
                                           bool at_origin)
{
    NPH_TRY(watch_add_file(p, s, path, at_origin));
    char settings[EDITOR_PROJECT_PATH_MAX + 16U];
    if ((u64)snprintf(settings, sizeof settings, "%s.import.json", path) >= sizeof settings) {
        return NPH_RESULT_OVERFLOW;
    }
    NPH_TRY(watch_add_file(p, s, settings, at_origin));
    if (cooker_source_kind(path) != COOKER_SOURCE_SCENE || !nph_fs_exists(path)) {
        return NPH_RESULT_OK;
    }
    CompanionAdd a;
    memset(&a, 0, sizeof a);
    a.p = p;
    a.s = s;
    a.at_origin = at_origin ? 1 : 0;
    dirname_of(path, a.dir, sizeof a.dir);
    // A document that does not read names nothing to watch: its cook says what is wrong.
    if (cook_gltf_files(p->alloc, path, watch_add_companion, &a) != NPH_RESULT_OK) {
        return NPH_RESULT_OK;
    }
    return a.result;
}

static void watch_files_free(EditorProject *p, WatchSource *s)
{
    NPH_ARRAY_FOREACH (WatchFile, f, &s->files) {
        free_cstr(p->alloc, f->path);
    }
    NPH_ARRAY_CLEAR(&s->files);
}

static void watch_source_free(EditorProject *p, WatchSource *s)
{
    watch_files_free(p, s);
    NPH_ARRAY_FREE(&s->files);
    free_cstr(p->alloc, s->key);
    free_cstr(p->alloc, s->origin);
    memset(s, 0, sizeof *s);
}

// The files of `s` watched afresh, what they are now the baseline, and nothing waiting.
static NphResult watch_files_make(EditorProject *p, WatchSource *s)
{
    watch_files_free(p, s);
    free_cstr(p->alloc, s->origin);
    s->origin = NULL;
    s->changed_ns = 0U;
    s->from_origin = 0;
    s->in_project = 0;
    char source[EDITOR_PROJECT_PATH_MAX];
    NPH_TRY(join(source, sizeof source, p->assets, s->key));
    NPH_TRY(watch_add_with_companions(p, s, source, false));
    const Origin *o = origin_find(p, s->key);
    if (o == NULL) {
        return NPH_RESULT_OK;
    }
    s->origin = dup_cstr(p->alloc, o->path);
    if (s->origin == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return watch_add_with_companions(p, s, o->path, true);
}

static WatchSource *watch_find(EditorProject *p, const char *key)
{
    NPH_ARRAY_FOREACH (WatchSource, s, &p->watch) {
        if (strcmp(s->key, key) == 0) {
            return s;
        }
    }
    return NULL;
}

// The watched sources made the database's: new ones added, what they are now their baseline;
// gone ones dropped; one whose origin changed watched afresh.
static void watch_sync(EditorProject *p)
{
    if (p->watch_interval_ns == 0U) {
        return;
    }
    const Snapshot *snap = p->snapshot; // this thread alone swaps it: no lock to read it
    NPH_ARRAY_FOREACH (WatchSource, s, &p->watch) {
        s->seen = 0;
    }
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; snap != NULL && i < cook_db_count(&snap->db) && r == NPH_RESULT_OK; ++i) {
        const char *key = cook_db_entry(&snap->db, i)->source;
        WatchSource *s = watch_find(p, key);
        if (s == NULL) {
            WatchSource fresh;
            memset(&fresh, 0, sizeof fresh);
            NPH_ARRAY_INIT(&fresh.files, p->alloc);
            fresh.key = dup_cstr(p->alloc, key);
            r = fresh.key != NULL ? NPH_ARRAY_PUSH(&p->watch, fresh) : NPH_RESULT_OUT_OF_MEMORY;
            if (r != NPH_RESULT_OK) {
                free_cstr(p->alloc, fresh.key);
                break;
            }
            s = &p->watch.data[p->watch.len - 1U];
            r = watch_files_make(p, s);
        } else if (s->seen == 0) {
            Origin *o = origin_find(p, key);
            if ((o == NULL) != (s->origin == NULL) ||
                (o != NULL && (o->copied != 0 || strcmp(o->path, s->origin) != 0))) {
                r = watch_files_make(p, s); // copied in again, or from elsewhere, since
            }
        }
        s->seen = 1;
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: watching the sources: %s", nph_result_string(r));
        return; // the list stays as it was; the next sync tries again
    }
    NPH_ARRAY_FOREACH (Origin, o, &p->origins) {
        o->copied = 0;
    }
    u32 kept = 0U;
    for (u32 i = 0; i < p->watch.len; ++i) {
        WatchSource *s = &p->watch.data[i];
        if (s->seen != 0) {
            p->watch.data[kept++] = *s;
        } else {
            watch_source_free(p, s);
        }
    }
    p->watch.len = kept;
}

static void finish_request(EditorProject *p, EditorImportResult *res);

// Takes what changed of `s` in: its origin copied over it again where the origin changed, and
// it cooked again, as a request of the project's own (EDITOR_PROJECT_TAG_WATCH). False when its
// result has no room yet (the main thread has not taken the earlier ones): it then waits.
static bool watch_fire(EditorProject *p, WatchSource *s)
{
    nph_mutex_lock(&p->lock);
    const bool room = p->queue_count + p->done_count < EDITOR_PROJECT_REQUESTS;
    u32 id = 0U;
    if (room) {
        id = ++p->next_id;
        p->running = 1;
    }
    nph_mutex_unlock(&p->lock);
    if (!room) {
        return false;
    }
    char source[EDITOR_PROJECT_PATH_MAX];
    NphResult r = join(source, sizeof source, p->assets, s->key);
    bool cook = s->in_project != 0;
    if (r == NPH_RESULT_OK && s->from_origin != 0 && s->origin != NULL &&
        nph_fs_exists(s->origin)) {
        NPH_LOG_INFO(&LOG_EDITOR, "assets: %s changed where it came from (%s); importing it again",
                     s->key, s->origin);
        set_status(p, "Importing %s again", s->key);
        r = copy_source(p, s->origin, source);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_EDITOR, "import: copying %s: %s", s->origin, nph_result_string(r));
        }
        cook = true;
    } else if (cook) {
        NPH_LOG_INFO(&LOG_EDITOR, "assets: %s changed; cooking it again", s->key);
    }
    EditorImportResult res;
    memset(&res, 0, sizeof res);
    if (cook) {
        Request req;
        memset(&req, 0, sizeof req);
        req.id = id;
        req.kind = REQUEST_COOK;
        req.tag = EDITOR_PROJECT_TAG_WATCH;
        req.paths[0] = s->key; // not the request's: nothing to free
        req.path_count = 1U;
        run_request(p, &req, &res);
        if (r != NPH_RESULT_OK && res.result == NPH_RESULT_OK) {
            res.result = r;
        }
    }
    // Its files afresh: the copy and the cook are not changes to take again, and a scene may
    // name other files now.
    const NphResult made = watch_files_make(p, s);
    if (made != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: watching %s: %s", s->key, nph_result_string(made));
    }
    if (cook) {
        finish_request(p, &res);
    } else {
        nph_mutex_lock(&p->lock);
        p->running = 0;
        nph_condvar_broadcast(&p->changed);
        nph_mutex_unlock(&p->lock);
    }
    return true;
}

// One look at every watched file: a source whose files changed waits until they have held for
// the settle time, and is then taken in. Every file is looked at first: taking a source in syncs
// the list, which moves its entries.
static void watch_poll(EditorProject *p)
{
    const u64 now = nph_time_now_ns();
    u32 due = 0U;
    for (u32 i = 0; i < p->watch.len; ++i) {
        WatchSource *s = &p->watch.data[i];
        s->due = 0;
        bool changed = false;
        bool waiting = false;
        NPH_ARRAY_FOREACH (WatchFile, f, &s->files) {
            if (watch_file_changed(f)) {
                changed = true;
                f->pending = 1;
                p->watch_dirty = 1;
            }
            if (f->pending != 0) {
                waiting = true;
                if (f->at_origin != 0) {
                    s->from_origin = 1;
                } else {
                    s->in_project = 1;
                }
            }
        }
        if (changed || (waiting && s->changed_ns == 0U)) {
            s->changed_ns = now; // the settle time runs from the last change seen
        } else if (s->changed_ns != 0U && now - s->changed_ns >= p->watch_settle_ns) {
            s->due = 1;
            due += 1U;
        }
    }
    for (; due != 0U; --due) {
        WatchSource *s = NULL;
        for (u32 i = 0; i < p->watch.len && s == NULL; ++i) {
            s = p->watch.data[i].due != 0 ? &p->watch.data[i] : NULL;
        }
        if (s == NULL) {
            return; // it left the database with the last one's cook
        }
        s->due = 0;
        if (!watch_fire(p, s)) {
            return; // no room for its result yet: the next look takes it
        }
    }
}

// Takes in the database a request left, the origins and the watch to match, and hands its
// result over. The watch's baseline is taken before the result is, so a change made once the
// request is seen to be done is a change.
static void finish_request(EditorProject *p, EditorImportResult *res)
{
    Snapshot *fresh = snapshot_load(p);
    Snapshot *old = NULL;
    if (fresh != NULL) {
        nph_mutex_lock(&p->lock);
        old = p->snapshot;
        p->snapshot = fresh;
        nph_mutex_unlock(&p->lock);
    }
    if (p->watch_dirty != 0) {
        watch_save(p);
    }
    watch_sync(p);
    nph_mutex_lock(&p->lock);
    ++p->generation;
    const u32 slot = (p->done_head + p->done_count) % EDITOR_PROJECT_REQUESTS;
    memcpy(&p->done[slot], res, sizeof *res);
    ++p->done_count;
    p->running = 0;
    p->status[0] = '\0';
    nph_condvar_broadcast(&p->changed);
    nph_mutex_unlock(&p->lock);
    snapshot_free(p->alloc, old);
}

static void import_thread(void *user)
{
    EditorProject *p = (EditorProject *)user;
    watch_sync(p); // the last session's baseline where it kept one, else what is there now
    if (p->watch_interval_ns != 0U) {
        seen_free(p); // from now on what this session sees
    }
    u64 next_look = nph_time_now_ns() + p->watch_interval_ns;
    Request req;
    for (;;) {
        nph_mutex_lock(&p->lock);
        bool look = false;
        while (p->queue_count == 0U && !p->quit && !look) {
            if (p->watch_interval_ns == 0U) {
                nph_condvar_wait(&p->changed, &p->lock);
                continue;
            }
            const u64 now = nph_time_now_ns();
            if (now >= next_look) {
                look = true;
            } else {
                (void)nph_condvar_wait_timeout(&p->changed, &p->lock, next_look - now);
            }
        }
        if (p->quit) {
            nph_mutex_unlock(&p->lock);
            return;
        }
        if (p->queue_count == 0U) {
            nph_mutex_unlock(&p->lock);
            watch_poll(p);
            next_look = nph_time_now_ns() + p->watch_interval_ns;
            continue;
        }
        memcpy(&req, &p->queue[p->queue_head], sizeof req);
        p->queue_head = (p->queue_head + 1U) % EDITOR_PROJECT_REQUESTS;
        --p->queue_count;
        p->running = 1;
        nph_mutex_unlock(&p->lock);

        EditorImportResult res;
        run_request(p, &req, &res);
        free_request(p->alloc, &req);
        finish_request(p, &res);
    }
}

// ---------------------------------------------------------------------------
// The project
// ---------------------------------------------------------------------------

NphResult editor_project_create(const EditorProjectDesc *desc, EditorProject **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->assets_dir == NULL || desc->assets_dir[0] == '\0' ||
        desc->cooked_dir == NULL || desc->cooked_dir[0] == '\0') {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    EditorProject *p = NPH_ALLOC_TYPE(desc->alloc, EditorProject);
    if (p == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    p->alloc = desc->alloc;
    p->jobs = desc->jobs;
    p->watch_interval_ns = desc->watch_interval_ns;
    p->watch_settle_ns =
        desc->watch_settle_ns != 0U ? desc->watch_settle_ns : EDITOR_PROJECT_WATCH_SETTLE_NS;
    NPH_ARRAY_INIT(&p->origins, desc->alloc);
    NPH_ARRAY_INIT(&p->watch, desc->alloc);
    NPH_ARRAY_INIT(&p->seen, desc->alloc);
    const NphHashMapDesc seen_desc = nph_hashmap_desc_u64(sizeof(u32));
    NphResult r = nph_hashmap_init(&p->seen_map, desc->alloc, &seen_desc);
    if (r == NPH_RESULT_OK) {
        r = cooker_canonical_path(desc->assets_dir, p->assets, sizeof p->assets);
    }
    if (r == NPH_RESULT_OK) {
        r = cooker_canonical_path(desc->cooked_dir, p->cooked, sizeof p->cooked);
    }
    if (r == NPH_RESULT_OK && (u64)snprintf(p->db_path, sizeof p->db_path, "%s/%s", p->cooked,
                                            COOK_DB_FILE_NAME) >= sizeof p->db_path) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r == NPH_RESULT_OK && (u64)snprintf(p->watch_path, sizeof p->watch_path, "%s/%s", p->cooked,
                                            WATCH_FILE_NAME) >= sizeof p->watch_path) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(desc->alloc, EditorProject, p);
        return r == NPH_RESULT_OVERFLOW ? NPH_RESULT_INVALID_ARGUMENT : r;
    }
    p->snapshot = snapshot_load(p);
    watch_load(p);
    nph_mutex_init(&p->lock);
    nph_condvar_init(&p->changed);
    NphThreadDesc td;
    memset(&td, 0, sizeof td);
    td.fn = import_thread;
    td.user = p;
    td.name = "nph-import";
    r = nph_thread_create(&p->thread, &td);
    if (r != NPH_RESULT_OK) {
        nph_condvar_destroy(&p->changed);
        nph_mutex_destroy(&p->lock);
        snapshot_free(p->alloc, p->snapshot);
        origins_free(p);
        seen_free(p);
        NPH_ARRAY_FREE(&p->seen);
        nph_hashmap_free(&p->seen_map);
        NPH_FREE_TYPE(desc->alloc, EditorProject, p);
        return r;
    }
    // Cooking is background work: the frame's threads come first.
    NPH_IGNORE_RESULT(nph_thread_set_priority(&p->thread, NPH_THREAD_PRIORITY_LOW));
    *out = p;
    return NPH_RESULT_OK;
}

void editor_project_destroy(EditorProject *p)
{
    if (p == NULL) {
        return;
    }
    nph_mutex_lock(&p->lock);
    p->quit = 1;
    nph_condvar_broadcast(&p->changed);
    nph_mutex_unlock(&p->lock);
    nph_thread_join(&p->thread);
    for (u32 i = 0; i < p->queue_count; ++i) {
        free_request(p->alloc, &p->queue[(p->queue_head + i) % EDITOR_PROJECT_REQUESTS]);
    }
    for (u32 i = 0; i < p->done_count; ++i) {
        editor_project_result_free(p, &p->done[(p->done_head + i) % EDITOR_PROJECT_REQUESTS]);
    }
    nph_condvar_destroy(&p->changed);
    nph_mutex_destroy(&p->lock);
    snapshot_free(p->alloc, p->snapshot);
    if (p->watch_dirty != 0) {
        watch_save(p); // what this session saw, for the next one's first look
    }
    NPH_ARRAY_FOREACH (WatchSource, s, &p->watch) {
        watch_source_free(p, s);
    }
    NPH_ARRAY_FREE(&p->watch);
    origins_free(p);
    seen_free(p);
    NPH_ARRAY_FREE(&p->seen);
    nph_hashmap_free(&p->seen_map);
    NPH_FREE_TYPE(p->alloc, EditorProject, p);
}

static NphResult enqueue(EditorProject *p, u32 kind, const char *const *paths, u32 count,
                         const char *folder, bool force, u64 tag, u32 *out_id)
{
    if (paths == NULL || count == 0U || count > EDITOR_IMPORT_PATHS_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Request req;
    memset(&req, 0, sizeof req);
    req.kind = kind;
    req.tag = tag;
    req.force = force ? 1 : 0;
    // The folder is a path within the sources: normalised, never above them.
    if (folder != NULL && folder[0] != '\0') {
        if (nph_path_is_absolute(nph_str_from_cstr(folder)) ||
            nph_vfs_normalize_path(nph_str_from_cstr(folder), req.folder, sizeof req.folder,
                                   NULL) != NPH_RESULT_OK) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const u64 n = strlen(paths[i]);
        req.paths[i] = NPH_ALLOC_ARRAY(p->alloc, char, n + 1U);
        if (req.paths[i] == NULL) {
            free_request(p->alloc, &req);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        memcpy(req.paths[i], paths[i], (size_t)n + 1U);
        req.path_count = i + 1U;
    }
    nph_mutex_lock(&p->lock);
    const u32 outstanding = p->queue_count + (p->running ? 1U : 0U) + p->done_count;
    if (outstanding >= EDITOR_PROJECT_REQUESTS) {
        nph_mutex_unlock(&p->lock);
        free_request(p->alloc, &req);
        return NPH_RESULT_FULL;
    }
    req.id = ++p->next_id;
    memcpy(&p->queue[(p->queue_head + p->queue_count) % EDITOR_PROJECT_REQUESTS], &req, sizeof req);
    ++p->queue_count;
    nph_condvar_broadcast(&p->changed);
    nph_mutex_unlock(&p->lock);
    if (out_id != NULL) {
        *out_id = req.id;
    }
    return NPH_RESULT_OK;
}

NphResult editor_project_import(EditorProject *p, const char *const *paths, u32 count,
                                const char *folder, u64 tag, u32 *out_id)
{
    NPH_ASSERT(p != NULL);
    return enqueue(p, REQUEST_IMPORT, paths, count, folder, false, tag, out_id);
}

NphResult editor_project_cook(EditorProject *p, const char *key, bool force, u64 tag, u32 *out_id)
{
    NPH_ASSERT(p != NULL && key != NULL);
    char normalized[EDITOR_PROJECT_PATH_MAX];
    if (key[0] != '\0' && (nph_path_is_absolute(nph_str_from_cstr(key)) ||
                           nph_vfs_normalize_path(nph_str_from_cstr(key), normalized,
                                                  sizeof normalized, NULL) != NPH_RESULT_OK)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const char *paths[1] = {key[0] != '\0' ? normalized : ""};
    return enqueue(p, REQUEST_COOK, paths, 1U, NULL, force, tag, out_id);
}

bool editor_project_poll(EditorProject *p, EditorImportResult *out)
{
    NPH_ASSERT(p != NULL && out != NULL);
    nph_mutex_lock(&p->lock);
    const bool any = p->done_count != 0U;
    if (any) {
        memcpy(out, &p->done[p->done_head], sizeof *out);
        p->done_head = (p->done_head + 1U) % EDITOR_PROJECT_REQUESTS;
        --p->done_count;
    }
    nph_mutex_unlock(&p->lock);
    return any;
}

void editor_project_result_free(EditorProject *p, EditorImportResult *res)
{
    NPH_ASSERT(p != NULL && res != NULL);
    if (res->written != NULL) {
        NPH_FREE_ARRAY(p->alloc, NphGuid, res->written, res->written_count);
    }
    res->written = NULL;
    res->written_count = 0U;
}

u32 editor_project_pending(EditorProject *p)
{
    NPH_ASSERT(p != NULL);
    nph_mutex_lock(&p->lock);
    const u32 n = p->queue_count + (p->running ? 1U : 0U);
    nph_mutex_unlock(&p->lock);
    return n;
}

void editor_project_wait(EditorProject *p)
{
    NPH_ASSERT(p != NULL);
    nph_mutex_lock(&p->lock);
    while (p->queue_count != 0U || p->running) {
        nph_condvar_wait(&p->changed, &p->lock);
    }
    nph_mutex_unlock(&p->lock);
}

void editor_project_status(EditorProject *p, char *out, u64 cap)
{
    NPH_ASSERT(p != NULL && out != NULL && cap != 0U);
    nph_mutex_lock(&p->lock);
    (void)snprintf(out, (size_t)cap, "%s", p->status);
    nph_mutex_unlock(&p->lock);
}

// The asset `sub` of source `key` in the snapshot.
static bool find_asset(EditorProject *p, const char *key, NphStr sub, NphGuid *out)
{
    nph_mutex_lock(&p->lock);
    const CookDbEntry *e =
        p->snapshot != NULL ? cook_db_find(&p->snapshot->db, nph_str_from_cstr(key), sub) : NULL;
    if (e != NULL) {
        *out = e->guid;
    }
    nph_mutex_unlock(&p->lock);
    return e != NULL;
}

bool editor_project_scene_of(EditorProject *p, const char *key, NphGuid *out)
{
    NPH_ASSERT(p != NULL && key != NULL && out != NULL);
    return find_asset(p, key, NPH_STR("scene"), out);
}

bool editor_project_mesh_of(EditorProject *p, const char *key, NphGuid *out)
{
    NPH_ASSERT(p != NULL && key != NULL && out != NULL);
    return find_asset(p, key, NPH_STR("mesh"), out);
}

bool editor_project_source_of(EditorProject *p, NphGuid guid, char *out, u64 cap, char *out_sub,
                              u64 sub_cap)
{
    NPH_ASSERT(p != NULL && out != NULL && cap != 0U && out_sub != NULL && sub_cap != 0U);
    nph_mutex_lock(&p->lock);
    const CookDbEntry *e = p->snapshot != NULL ? cook_db_find_guid(&p->snapshot->db, guid) : NULL;
    bool found = false;
    if (e != NULL) {
        const int n = snprintf(out, (size_t)cap, "%s", e->source);
        const int m = snprintf(out_sub, (size_t)sub_cap, "%s", e->sub);
        found = n >= 0 && (u64)n < cap && m >= 0 && (u64)m < sub_cap;
    }
    nph_mutex_unlock(&p->lock);
    return found;
}

bool editor_project_is_cooked(EditorProject *p, const char *key)
{
    NPH_ASSERT(p != NULL && key != NULL);
    nph_mutex_lock(&p->lock);
    const bool cooked = snapshot_source(p->snapshot, key) != NULL;
    nph_mutex_unlock(&p->lock);
    return cooked;
}

bool editor_project_content_key(EditorProject *p, const char *key, const char *sub, u64 *out)
{
    NPH_ASSERT(p != NULL && key != NULL && sub != NULL && out != NULL);
    nph_mutex_lock(&p->lock);
    const CookDb *db = p->snapshot != NULL ? &p->snapshot->db : NULL;
    const CookDbEntry *e =
        db != NULL ? cook_db_find(db, nph_str_from_cstr(key), nph_str_from_cstr(sub)) : NULL;
    if (e != NULL) {
        u64 h = e->content_hash;
        for (u32 i = 0; i < e->dep_count; ++i) {
            const CookDbEntry *d = cook_db_find_guid(db, e->deps[i]);
            h = nph_hash_combine(h, d != NULL ? d->content_hash : 0U);
        }
        *out = h;
    }
    nph_mutex_unlock(&p->lock);
    return e != NULL;
}

u64 editor_project_generation(EditorProject *p)
{
    NPH_ASSERT(p != NULL);
    nph_mutex_lock(&p->lock);
    const u64 g = p->generation;
    nph_mutex_unlock(&p->lock);
    return g;
}

NphResult editor_project_resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len)
{
    EditorProject *p = (EditorProject *)user;
    NPH_ASSERT(p != NULL && buf != NULL && out_len != NULL);
    nph_mutex_lock(&p->lock);
    const CookDbEntry *e = p->snapshot != NULL ? cook_db_find_guid(&p->snapshot->db, guid) : NULL;
    NphResult r = NPH_RESULT_NOT_FOUND;
    if (e != NULL) {
        const u64 n = strlen(e->output);
        r = n + 1U <= cap ? NPH_RESULT_OK : NPH_RESULT_OVERFLOW;
        if (r == NPH_RESULT_OK) {
            memcpy(buf, e->output, (size_t)n + 1U);
            *out_len = n;
        }
    }
    nph_mutex_unlock(&p->lock);
    return r;
}

const char *editor_project_assets_dir(const EditorProject *p)
{
    NPH_ASSERT(p != NULL);
    return p->assets;
}

const char *editor_project_cooked_dir(const EditorProject *p)
{
    NPH_ASSERT(p != NULL);
    return p->cooked;
}

// SPDX-License-Identifier: Apache-2.0
// The project: the import thread, its requests, and the snapshot of the
// cooker's database that the resolver reads (project.h).
#include "project.h"

#include <nepheline/asset/vfs.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/string.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/thread.h>

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

static NphResult copy_tree(const char *from, const char *to, u32 depth)
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
            r = entry.is_directory ? copy_tree(src, dst, depth + 1U) : nph_fs_copy_file(src, dst);
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
    if (cooker_source_kind(from) != COOKER_SOURCE_SCENE) {
        return NPH_RESULT_OK;
    }
    DependencyCopy d;
    memset(&d, 0, sizeof d);
    dirname_of(from, d.from_dir, sizeof d.from_dir);
    dirname_of(to, d.to_dir, sizeof d.to_dir);
    const NphResult r = cook_gltf_files(p->alloc, from, copy_dependency, &d);
    return r != NPH_RESULT_OK ? r : d.result;
}

// Records the scene cooked from `source` in the result.
static void note_scene(Cooker *c, const char *source, EditorImportResult *res)
{
    char key[EDITOR_PROJECT_PATH_MAX];
    if (res->scene_count == EDITOR_IMPORT_SCENES_MAX ||
        cooker_source_key(c, source, key, sizeof key) != NPH_RESULT_OK) {
        return;
    }
    const CookDbEntry *e = cook_db_find(cooker_db(c), nph_str_from_cstr(key), NPH_STR("scene"));
    if (e == NULL) {
        return;
    }
    const NphStr stem = nph_path_stem(nph_str_from_cstr(source));
    const u64 n = stem.len < EDITOR_IMPORT_NAME_MAX ? stem.len : EDITOR_IMPORT_NAME_MAX - 1U;
    res->scenes[res->scene_count] = e->guid;
    memcpy(res->scene_names[res->scene_count], stem.data, (size_t)n);
    res->scene_names[res->scene_count][n] = '\0';
    ++res->scene_count;
}

// Cooks a source of the project: a directory's scenes, a scene, or an image
// (unless images are only along for a scene). Anything else was only copied.
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
            note_scene(c, source, res);
        }
        return r;
    }
    case COOKER_SOURCE_IMAGE:
        return images_alone ? cooker_cook_file(c, source) : NPH_RESULT_OK;
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
            r = info.is_directory ? copy_tree(abs, source, 0U) : copy_source(p, abs, source);
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

static void run_request(EditorProject *p, const Request *req, EditorImportResult *res)
{
    memset(res, 0, sizeof *res);
    res->id = req->id;
    res->tag = req->tag;
    CookerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = p->alloc;
    d.out_dir = p->cooked;
    d.source_root = p->assets;
    d.jobs = p->jobs;
    d.force = req->force;
    cook_texture_settings_default(&d.texture);
    cook_mesh_settings_default(&d.mesh);
    Cooker *c = NULL;
    res->result = cooker_create(&d, &c);
    if (res->result != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: the cooker for %s: %s", p->cooked,
                      nph_result_string(res->result));
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
    cooker_destroy(c); // saves the database
}

static void free_request(NphAllocator *alloc, Request *req)
{
    for (u32 i = 0; i < req->path_count; ++i) {
        NPH_FREE_ARRAY(alloc, char, req->paths[i], strlen(req->paths[i]) + 1U);
        req->paths[i] = NULL;
    }
    req->path_count = 0U;
}

static void import_thread(void *user)
{
    EditorProject *p = (EditorProject *)user;
    Request req;
    for (;;) {
        nph_mutex_lock(&p->lock);
        while (p->queue_count == 0U && !p->quit) {
            nph_condvar_wait(&p->changed, &p->lock);
        }
        if (p->quit) {
            nph_mutex_unlock(&p->lock);
            return;
        }
        memcpy(&req, &p->queue[p->queue_head], sizeof req);
        p->queue_head = (p->queue_head + 1U) % EDITOR_PROJECT_REQUESTS;
        --p->queue_count;
        p->running = 1;
        nph_mutex_unlock(&p->lock);

        EditorImportResult res;
        run_request(p, &req, &res);
        free_request(p->alloc, &req);
        Snapshot *fresh = snapshot_load(p);

        nph_mutex_lock(&p->lock);
        Snapshot *old = NULL;
        if (fresh != NULL) {
            old = p->snapshot;
            p->snapshot = fresh;
        }
        ++p->generation;
        const u32 slot = (p->done_head + p->done_count) % EDITOR_PROJECT_REQUESTS;
        memcpy(&p->done[slot], &res, sizeof res);
        ++p->done_count;
        p->running = 0;
        p->status[0] = '\0';
        nph_condvar_broadcast(&p->changed);
        nph_mutex_unlock(&p->lock);
        snapshot_free(p->alloc, old);
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
    NphResult r = cooker_canonical_path(desc->assets_dir, p->assets, sizeof p->assets);
    if (r == NPH_RESULT_OK) {
        r = cooker_canonical_path(desc->cooked_dir, p->cooked, sizeof p->cooked);
    }
    if (r == NPH_RESULT_OK && (u64)snprintf(p->db_path, sizeof p->db_path, "%s/%s", p->cooked,
                                            COOK_DB_FILE_NAME) >= sizeof p->db_path) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(desc->alloc, EditorProject, p);
        return r == NPH_RESULT_OVERFLOW ? NPH_RESULT_INVALID_ARGUMENT : r;
    }
    p->snapshot = snapshot_load(p);
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
    nph_condvar_destroy(&p->changed);
    nph_mutex_destroy(&p->lock);
    snapshot_free(p->alloc, p->snapshot);
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

bool editor_project_scene_of(EditorProject *p, const char *key, NphGuid *out)
{
    NPH_ASSERT(p != NULL && key != NULL && out != NULL);
    nph_mutex_lock(&p->lock);
    const CookDbEntry *e =
        p->snapshot != NULL
            ? cook_db_find(&p->snapshot->db, nph_str_from_cstr(key), NPH_STR("scene"))
            : NULL;
    if (e != NULL) {
        *out = e->guid;
    }
    nph_mutex_unlock(&p->lock);
    return e != NULL;
}

bool editor_project_is_cooked(EditorProject *p, const char *key)
{
    NPH_ASSERT(p != NULL && key != NULL);
    nph_mutex_lock(&p->lock);
    const bool cooked = snapshot_source(p->snapshot, key) != NULL;
    nph_mutex_unlock(&p->lock);
    return cooked;
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

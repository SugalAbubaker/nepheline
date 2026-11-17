// SPDX-License-Identifier: Apache-2.0
// Autosave, and the work given back after a crash (docs/editor.md "Autosave
// and recovery"; docs/adr/0104-autosave.md).
//
// Each running editor keeps a session directory under editor.autosave_dir,
// named `<seconds since 1970>-<pid>`, which holds:
//
//   lock          a lock that lasts as long as the process (platform/file.h):
//                 held while the editor runs, let go of by the system when it
//                 ends, however it ends;
//   session.json  the scene's file, when the session began and last
//                 autosaved, and the source of a mesh being edited;
//   scene.nscene  the edited world as a binary scene (scene_io.h: about a
//                 tenth of a microsecond an entity to write, a thirtieth of
//                 the text format's), whenever it has changes the last one
//                 does not hold and editor.autosave_interval seconds passed,
//                 and before Play; the one before it as scene.previous.nscene,
//                 should the newer hold what failed;
//   mesh.nmesh    the mesh being edited, while it has changes.
//
// A clean exit removes the directory. At attach, a session directory whose
// lock can be taken belongs to an editor that did not end cleanly, and its
// work is offered back: Recover, Discard, or Later.
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/scene/scene_io.h>

#include "editor_internal.h"

#include <stdio.h>
#include <string.h>

#define AUTOSAVE_SESSION_VERSION 1

// A file of a session's directory.
#define AUTOSAVE_FILE_MAX (EDITOR_PATH_MAX + 32U)

static void path_in(char *out, u64 cap, const char *dir, const char *name)
{
    (void)snprintf(out, (size_t)cap, "%s/%s", dir, name);
}

static void remove_quietly(const char *dir, const char *name)
{
    char path[AUTOSAVE_FILE_MAX];
    path_in(path, sizeof path, dir, name);
    if (nph_fs_exists(path)) {
        (void)nph_fs_remove_file(path);
    }
}

// ---------------------------------------------------------------------------
// The session's description
// ---------------------------------------------------------------------------

static NphResult write_session(Editor *ed)
{
    const EditorAutosave *as = &ed->autosave;
    NphStrBuilder sb;
    nph_sb_init(&sb, ed->alloc);
    NphJsonWriter w;
    nph_json_writer_init(&w, &sb, true);
    const char *mesh = as->has_mesh ? ed->mesh_edit.key : "";
    NphResult r = nph_json_writer_begin_object(&w);
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("nph_editor_session"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_i64(&w, AUTOSAVE_SESSION_VERSION);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("scene"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_str(&w, nph_str_from_cstr(ed->doc_path));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("has_scene"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_bool(&w, as->has_scene);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("mesh"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_str(&w, nph_str_from_cstr(mesh));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("started_ns"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_i64(&w, (i64)as->started_wall_ns);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("saved_ns"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_i64(&w, (i64)as->saved_wall_ns);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_end_object(&w);
    }
    if (r == NPH_RESULT_OK) {
        // Through a file renamed over it: never half a description.
        char path[AUTOSAVE_FILE_MAX];
        char tmp[AUTOSAVE_FILE_MAX];
        path_in(path, sizeof path, as->dir, "session.json");
        path_in(tmp, sizeof tmp, as->dir, "session.json.tmp");
        r = nph_file_write_all(tmp, sb.data, sb.len);
        if (r == NPH_RESULT_OK) {
            r = nph_fs_rename(tmp, path);
        }
    }
    nph_sb_free(&sb);
    return r;
}

typedef struct SessionInfo {
    char scene[EDITOR_PATH_MAX];
    char mesh[EDITOR_PROJECT_PATH_MAX];
    u64 saved_ns;
    bool has_scene;
} SessionInfo;

static void copy_string(const NphJsonValue *object, const char *key, char *out, u64 cap)
{
    out[0] = '\0';
    const NphJsonValue *v = nph_json_object_get(object, nph_str_from_cstr(key));
    NphStr s;
    if (v != NULL && nph_json_as_str(v, &s) == NPH_RESULT_OK && s.len < cap) {
        memcpy(out, s.data, (size_t)s.len);
        out[s.len] = '\0';
    }
}

static bool read_session(Editor *ed, const char *dir, SessionInfo *out)
{
    memset(out, 0, sizeof *out);
    char path[AUTOSAVE_FILE_MAX];
    path_in(path, sizeof path, dir, "session.json");
    void *data = NULL;
    u64 size = 0U;
    if (nph_file_read_all(path, ed->alloc, &data, &size) != NPH_RESULT_OK) {
        return false;
    }
    NphJsonDoc doc;
    const NphResult r = nph_json_parse(nph_str((const char *)data, size), ed->alloc, &doc);
    nph_file_free_all(ed->alloc, data, size);
    bool ok = false;
    if (r == NPH_RESULT_OK && doc.root != NULL &&
        nph_json_get_i64(doc.root, NPH_STR("nph_editor_session"), 0) == AUTOSAVE_SESSION_VERSION) {
        copy_string(doc.root, "scene", out->scene, sizeof out->scene);
        copy_string(doc.root, "mesh", out->mesh, sizeof out->mesh);
        out->has_scene = nph_json_get_bool(doc.root, NPH_STR("has_scene"), false);
        const i64 saved = nph_json_get_i64(doc.root, NPH_STR("saved_ns"), 0);
        out->saved_ns = saved > 0 ? (u64)saved : 0U;
        ok = true;
    }
    nph_json_doc_free(&doc); // a failed parse is freed too (json.h)
    return ok;
}

// ---------------------------------------------------------------------------
// Sessions that did not end cleanly
// ---------------------------------------------------------------------------

// Looks for the directories under `root` of sessions whose lock can be
// taken, and offers the newest one's work. A session that left nothing to
// recover is removed. It runs before this session's directory exists.
static void find_stale(Editor *ed, const char *root)
{
    NphDirIterator it;
    if (nph_dir_open(root, &it) != NPH_RESULT_OK) {
        return;
    }
    EditorRecovery *rc = &ed->recovery;
    NphDirEntry entry;
    while (nph_dir_next(&it, &entry)) {
        if (!entry.is_directory) {
            continue;
        }
        char dir[EDITOR_PATH_MAX];
        const int n = snprintf(dir, sizeof dir, "%s/%s", root, entry.name);
        if (n <= 0 || (u64)n >= sizeof dir) {
            continue;
        }
        char lock_path[AUTOSAVE_FILE_MAX];
        path_in(lock_path, sizeof lock_path, dir, "lock");
        if (!nph_fs_exists(lock_path)) {
            continue; // not a session's
        }
        NphFileLock lock;
        if (nph_file_lock_acquire(lock_path, &lock) != NPH_RESULT_OK) {
            continue; // a running editor's (BUSY)
        }
        SessionInfo info;
        const bool read = read_session(ed, dir, &info);
        char scene_path[AUTOSAVE_FILE_MAX];
        char mesh_path[AUTOSAVE_FILE_MAX];
        path_in(scene_path, sizeof scene_path, dir, "scene.nscene");
        path_in(mesh_path, sizeof mesh_path, dir, "mesh.nmesh");
        const bool has_scene = read && info.has_scene && nph_fs_exists(scene_path);
        const bool has_mesh = read && info.mesh[0] != '\0' && nph_fs_exists(mesh_path);
        if (!has_scene && !has_mesh) {
            // Nothing to give back: it ended before it had unsaved changes,
            // or after it saved them.
            nph_file_lock_release(&lock);
            (void)nph_fs_remove_tree(dir);
            continue;
        }
        if (rc->offered && info.saved_ns <= rc->saved_wall_ns) {
            nph_file_lock_release(&lock); // older: offered at a later start
            continue;
        }
        if (rc->offered) {
            nph_file_lock_release(&rc->lock); // a newer one comes first
        }
        rc->offered = true;
        rc->lock = lock;
        memcpy(rc->dir, dir, sizeof rc->dir);
        memcpy(rc->scene, info.scene, sizeof rc->scene);
        memcpy(rc->mesh_key, info.mesh, sizeof rc->mesh_key);
        rc->has_scene = has_scene;
        rc->has_mesh = has_mesh;
        rc->saved_wall_ns = info.saved_ns;
        rc->scene_newer = false;
        NphFileInfo fi;
        if (has_scene && info.scene[0] != '\0' && nph_fs_stat(info.scene, &fi) == NPH_RESULT_OK &&
            fi.modified_ns > info.saved_ns) {
            rc->scene_newer = true;
        }
    }
    nph_dir_close(&it);
}

// ---------------------------------------------------------------------------
// This session
// ---------------------------------------------------------------------------

static void close_session_dir(EditorAutosave *as, bool keep)
{
    nph_file_lock_release(&as->lock);
    if (!keep) {
        const NphResult r = nph_fs_remove_tree(as->dir);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_EDITOR, "autosave: %s stays: %s", as->dir, nph_result_string(r));
        }
    }
    as->dir[0] = '\0';
}

static int session_name(char *out, u64 cap, u64 seconds, u64 pid, u32 number)
{
    if (number == 0U) {
        return snprintf(out, (size_t)cap, "%llu-%llu", (unsigned long long)seconds,
                        (unsigned long long)pid);
    }
    return snprintf(out, (size_t)cap, "%llu-%llu-%u", (unsigned long long)seconds,
                    (unsigned long long)pid, number);
}

NphResult editor_autosave_session_name(const char *root, u64 seconds, u64 pid, char *out, u64 cap)
{
    NPH_ASSERT(root != NULL && out != NULL && cap > 0U);
    NphResult r = NPH_RESULT_BUSY; // every name held
    for (u32 number = 0U; number < 1000U; ++number) {
        const int n = session_name(out, cap, seconds, pid, number);
        char dir[EDITOR_PATH_MAX];
        const int m = snprintf(dir, sizeof dir, "%s/%s", root, out);
        if (n <= 0 || (u64)n >= cap || m <= 0 || (u64)m >= sizeof dir) {
            r = NPH_RESULT_OVERFLOW;
            break;
        }
        if (!nph_fs_exists(dir)) {
            return NPH_RESULT_OK;
        }
    }
    out[0] = '\0';
    return r;
}

NphResult editor_autosave_open(Editor *ed)
{
    NPH_ASSERT(ed != NULL && ed->history != NULL);
    EditorAutosave *as = &ed->autosave;
    memset(as, 0, sizeof *as);
    memset(&ed->recovery, 0, sizeof ed->recovery);
    const NphStr root_cvar = nph_cvar_get_string(&ed->cv_autosave_dir);
    if (root_cvar.len == 0U) {
        NPH_LOG_INFO(&LOG_EDITOR, "autosave is off (editor.autosave_dir is empty)");
        return NPH_RESULT_OK;
    }
    char root[EDITOR_PATH_MAX];
    if (root_cvar.len >= sizeof root) {
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(root, root_cvar.data, (size_t)root_cvar.len);
    root[root_cvar.len] = '\0';
    NPH_TRY(nph_fs_create_directories(root));

    as->started_wall_ns = nph_time_wall_ns();
    char name[64];
    NPH_TRY(editor_autosave_session_name(root, as->started_wall_ns / NPH_TIME_NS_PER_S,
                                         nph_platform_process_id(), name, sizeof name));
    find_stale(ed, root); // before this session's directory exists
    const int n = snprintf(as->dir, sizeof as->dir, "%s/%s", root, name);
    if (n <= 0 || (u64)n >= sizeof as->dir) {
        as->dir[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    char lock_path[AUTOSAVE_FILE_MAX];
    path_in(lock_path, sizeof lock_path, as->dir, "lock");
    NphResult r = nph_fs_create_directories(as->dir);
    if (r == NPH_RESULT_OK) {
        r = nph_file_lock_acquire(lock_path, &as->lock);
    }
    as->last_ns = nph_time_now_ns();
    as->saved_state = editor_history_state(ed->history);
    if (r == NPH_RESULT_OK) {
        r = write_session(ed);
    }
    if (r != NPH_RESULT_OK) {
        close_session_dir(as, false);
        return r;
    }
    const i64 interval = nph_cvar_get_int(&ed->cv_autosave_interval);
    if (interval > 0) {
        NPH_LOG_INFO(&LOG_EDITOR,
                     "autosave: unsaved changes every %lld s, and before Play, into %s",
                     (long long)interval, as->dir);
    } else {
        NPH_LOG_INFO(&LOG_EDITOR, "autosave: unsaved changes before Play, into %s", as->dir);
    }
    if (ed->recovery.offered) {
        NPH_LOG_WARN(&LOG_EDITOR, "an editor did not end normally: its work in %s is offered back",
                     ed->recovery.dir);
        if (ed->popup_request == EDITOR_POPUP_NONE) {
            ed->popup_request = EDITOR_POPUP_RECOVER;
        }
    }
    return NPH_RESULT_OK;
}

// True when the scene or the mesh being edited has changes the last
// autosave does not hold.
static bool work_unsaved(const Editor *ed)
{
    const EditorAutosave *as = &ed->autosave;
    const u64 state = editor_history_state(ed->history);
    const bool scene = state != ed->doc_saved_state && state != as->saved_state;
    const EditorMeshEdit *me = &ed->mesh_edit;
    const bool mesh = me->active && me->changed && me->revision != as->mesh_revision;
    return scene || mesh;
}

void editor_autosave_close(Editor *ed)
{
    EditorAutosave *as = &ed->autosave;
    EditorRecovery *rc = &ed->recovery;
    if (rc->offered) {
        // Unanswered: it waits for the next start.
        nph_file_lock_release(&rc->lock);
        rc->offered = false;
    }
    if (as->dir[0] == '\0') {
        return;
    }
    // An exit that nobody confirmed (the console's quit, a failure) with
    // unsaved changes keeps them for the next start, as a crash would.
    const bool unconfirmed = ed->history != NULL && editor_scene_dirty(ed) && !ed->quit_confirmed;
    if (unconfirmed && ed->engine != NULL) {
        NPH_IGNORE_RESULT(editor_autosave_now(ed, "exit")); // logged
    }
    const bool keep = unconfirmed && (as->has_scene || as->has_mesh);
    if (keep) {
        NPH_LOG_WARN(&LOG_EDITOR, "unsaved changes kept in %s for the next start", as->dir);
    }
    close_session_dir(as, keep);
}

// The edited world, as a binary scene: the one before it kept a generation.
static NphResult write_scene(Editor *ed, u32 *out_entities)
{
    const EditorAutosave *as = &ed->autosave;
    NphBinWriter bw;
    nph_bin_writer_init(&bw, ed->alloc);
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.scratch = ed->alloc;
    NphSceneResult res;
    memset(&res, 0, sizeof res);
    NphResult r = nph_scene_save_binary(nph_engine_edit_world(ed->engine), &sd, &bw, &res);
    char path[AUTOSAVE_FILE_MAX];
    char previous[AUTOSAVE_FILE_MAX];
    char tmp[AUTOSAVE_FILE_MAX];
    path_in(path, sizeof path, as->dir, "scene.nscene");
    path_in(previous, sizeof previous, as->dir, "scene.previous.nscene");
    path_in(tmp, sizeof tmp, as->dir, "scene.nscene.tmp");
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(tmp, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw));
    }
    if (r == NPH_RESULT_OK && nph_fs_exists(path)) {
        r = nph_fs_rename(path, previous);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_fs_rename(tmp, path);
    }
    nph_bin_writer_free(&bw);
    *out_entities = res.entity_count;
    return r;
}

NphResult editor_autosave_now(Editor *ed, const char *why)
{
    NPH_ASSERT(ed != NULL && why != NULL);
    EditorAutosave *as = &ed->autosave;
    if (as->dir[0] == '\0' || ed->engine == NULL || ed->history == NULL || !work_unsaved(ed)) {
        return NPH_RESULT_OK;
    }
    const u64 t0 = nph_time_now_ns();
    const u64 state = editor_history_state(ed->history);
    NphResult r = NPH_RESULT_OK;
    if (state != ed->doc_saved_state) {
        r = write_scene(ed, &as->last_entities);
        if (r == NPH_RESULT_OK) {
            as->saved_state = state;
            as->has_scene = true;
        }
    }
    const EditorMeshEdit *me = &ed->mesh_edit;
    if (r == NPH_RESULT_OK && me->active && me->changed) {
        char path[AUTOSAVE_FILE_MAX];
        path_in(path, sizeof path, as->dir, "mesh.nmesh");
        r = cook_polymesh_save(&me->mesh, path);
        if (r == NPH_RESULT_OK) {
            as->has_mesh = true;
            as->mesh_revision = me->revision;
        }
    }
    if (r == NPH_RESULT_OK) {
        as->saved_wall_ns = nph_time_wall_ns();
        r = write_session(ed);
    }
    as->last_ns = nph_time_now_ns(); // after a failure too: not again every frame
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "autosave (%s): %s", why, nph_result_string(r));
        return r;
    }
    ++as->count;
    as->last_ms = nph_time_ns_to_ms(as->last_ns - t0);
    NPH_LOG_DEBUG(&LOG_EDITOR, "autosaved (%s): %u entities%s in %.2f ms", why, as->last_entities,
                  as->has_mesh ? ", and the mesh being edited," : "", as->last_ms);
    return NPH_RESULT_OK;
}

void editor_autosave_update(Editor *ed, u64 now_ns)
{
    NPH_ASSERT(ed != NULL);
    const EditorAutosave *as = &ed->autosave;
    if (as->dir[0] == '\0' || ed->history == NULL || nph_engine_is_playing(ed->engine)) {
        return;
    }
    const i64 interval = nph_cvar_get_int(&ed->cv_autosave_interval);
    if (interval <= 0 || !work_unsaved(ed) ||
        now_ns - as->last_ns < (u64)interval * NPH_TIME_NS_PER_S) {
        return;
    }
    // Not in the middle of a change: a drag, or a field being typed in.
    if (ed->gizmo_dragging || (ed->imgui != NULL && igIsAnyItemActive())) {
        return;
    }
    NPH_IGNORE_RESULT(editor_autosave_now(ed, "interval")); // logged
}

void editor_autosave_settled(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorAutosave *as = &ed->autosave;
    if (as->dir[0] == '\0' || ed->history == NULL) {
        return;
    }
    if (as->has_scene) {
        remove_quietly(as->dir, "scene.nscene");
        remove_quietly(as->dir, "scene.previous.nscene");
        as->has_scene = false;
    }
    if (as->has_mesh && !ed->mesh_edit.active) {
        remove_quietly(as->dir, "mesh.nmesh");
        as->has_mesh = false;
    }
    as->saved_state = editor_history_state(ed->history);
    const NphResult r = write_session(ed);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "autosave: %s", nph_result_string(r));
    }
}

// ---------------------------------------------------------------------------
// The work offered back
// ---------------------------------------------------------------------------

NphResult editor_recovery_recover(Editor *ed)
{
    NPH_ASSERT(ed != NULL && ed->engine != NULL);
    EditorRecovery *rc = &ed->recovery;
    if (!rc->offered || nph_engine_is_playing(ed->engine)) {
        return NPH_RESULT_INVALID_STATE;
    }
    NphResult r = NPH_RESULT_OK;
    char path[AUTOSAVE_FILE_MAX];
    if (rc->has_mesh) {
        // The source first, cooked again: the scene that draws it then
        // shows it as it was being edited.
        path_in(path, sizeof path, rc->dir, "mesh.nmesh");
        char source[EDITOR_PROJECT_PATH_MAX * 2U];
        (void)snprintf(source, sizeof source, "%s/%s", editor_project_assets_dir(ed->project),
                       rc->mesh_key);
        r = nph_fs_copy_file(path, source);
        u32 id = 0U;
        if (r == NPH_RESULT_OK) {
            r = editor_project_cook(ed->project, rc->mesh_key, false, 0U, &id);
        }
    }
    if (r == NPH_RESULT_OK && rc->has_scene) {
        path_in(path, sizeof path, rc->dir, "scene.nscene");
        r = editor_open_scene(ed, path);
        if (r == NPH_RESULT_OK) {
            // Under the scene's own file, and unsaved: the file does not
            // hold it, and neither does this session's autosave yet.
            (void)snprintf(ed->doc_path, sizeof ed->doc_path, "%s", rc->scene);
            ed->doc_saved_state = ~0ULL;
            ed->autosave.saved_state = ~0ULL;
        }
    }
    nph_file_lock_release(&rc->lock);
    rc->offered = false;
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "recovering the work in %s: %s (it stays there)", rc->dir,
                      nph_result_string(r));
        return r;
    }
    NPH_LOG_INFO(&LOG_EDITOR, "recovered %s%s%s%s, not saved yet",
                 rc->has_scene ? "the scene " : "",
                 rc->has_scene ? (rc->scene[0] != '\0' ? rc->scene : "(untitled)") : "",
                 rc->has_mesh ? (rc->has_scene ? " and the mesh " : "the mesh ") : "",
                 rc->has_mesh ? rc->mesh_key : "");
    (void)nph_fs_remove_tree(rc->dir);
    // Into this session's autosave at once.
    NPH_IGNORE_RESULT(editor_autosave_now(ed, "recovered")); // logged
    return NPH_RESULT_OK;
}

void editor_recovery_discard(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorRecovery *rc = &ed->recovery;
    if (!rc->offered) {
        return;
    }
    nph_file_lock_release(&rc->lock);
    rc->offered = false;
    const NphResult r = nph_fs_remove_tree(rc->dir);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "discarding %s: %s", rc->dir, nph_result_string(r));
        return;
    }
    NPH_LOG_INFO(&LOG_EDITOR, "discarded the work in %s", rc->dir);
}

void editor_recovery_later(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorRecovery *rc = &ed->recovery;
    if (!rc->offered) {
        return;
    }
    nph_file_lock_release(&rc->lock);
    rc->offered = false;
    NPH_LOG_INFO(&LOG_EDITOR, "the work in %s waits for the next start", rc->dir);
}

// "5 minutes ago", for the popup.
static void describe_age(u64 seconds, char *out, u64 cap)
{
    if (seconds < 90U) {
        (void)snprintf(out, (size_t)cap, "%llu seconds ago", (unsigned long long)seconds);
    } else if (seconds < 90ULL * 60ULL) {
        (void)snprintf(out, (size_t)cap, "%llu minutes ago", (unsigned long long)(seconds / 60U));
    } else if (seconds < 36ULL * 3600ULL) {
        (void)snprintf(out, (size_t)cap, "%llu hours ago", (unsigned long long)(seconds / 3600U));
    } else {
        (void)snprintf(out, (size_t)cap, "%llu days ago", (unsigned long long)(seconds / 86400U));
    }
}

void editor_recovery_popup(Editor *ed)
{
    if (!igBeginPopupModal("Recover Work", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const EditorRecovery *rc = &ed->recovery;
    if (!rc->offered) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }
    const u64 now = nph_time_wall_ns();
    char when[64];
    describe_age(now > rc->saved_wall_ns ? (now - rc->saved_wall_ns) / NPH_TIME_NS_PER_S : 0U, when,
                 sizeof when);
    igTextUnformatted("The editor did not end normally last time.", NULL);
    igText("Its last autosave, %s, holds:", when);
    if (rc->has_scene) {
        igBulletText("the scene %s", rc->scene[0] != '\0' ? rc->scene : "(untitled)");
        if (rc->scene_newer) {
            igTextColored(ui_vec4(1.0F, 0.75F, 0.3F, 1.0F),
                          "    its file was saved after this autosave");
        }
    }
    if (rc->has_mesh) {
        igBulletText("changes to the mesh %s", rc->mesh_key);
    }
    const ImVec2_c button = ui_vec2(igGetFontSize() * 6.0F, 0.0F);
    if (igButton("Recover", button)) {
        igCloseCurrentPopup();
        NPH_IGNORE_RESULT(editor_recovery_recover(ed)); // logged
    }
    igSameLine(0.0F, -1.0F);
    if (igButton("Discard", button)) {
        igCloseCurrentPopup();
        editor_recovery_discard(ed);
    }
    igSameLine(0.0F, -1.0F);
    if (igButton("Later", button) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        igCloseCurrentPopup();
        editor_recovery_later(ed);
    }
    igEndPopup();
}

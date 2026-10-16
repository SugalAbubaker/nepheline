// SPDX-License-Identifier: Apache-2.0
// The project and the asset browser (docs/editor.md "Assets";
// docs/adr/0026-editor-import.md): imports from drops and from the browser,
// the scenes they cook placed in the edited world as undoable steps, and the
// panel showing the sources one folder at a time with what the cooker made
// of each file.
#include <nepheline/asset/loader.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/string.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/time.h>
#include <nepheline/runtime/scene_render.h>

#include "editor_internal.h"

#include <cook/cooker.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The folder is listed again this often while shown, to notice files that
// change outside the editor.
#define RELIST_NS (2ULL * NPH_TIME_NS_PER_S)

// ---------------------------------------------------------------------------
// The project
// ---------------------------------------------------------------------------

static bool mounted(const NphVfs *vfs, const char *dir)
{
    for (u32 i = 0; i < nph_vfs_mount_count(vfs); ++i) {
        const NphVfsMountInfo m = nph_vfs_mount_info(vfs, i);
        if (m.type == NPH_VFS_MOUNT_DIRECTORY && m.prefix.len == 0U &&
            nph_str_equals(m.backing, nph_str_from_cstr(dir))) {
            return true;
        }
    }
    return false;
}

NphResult editor_assets_open(Editor *ed)
{
    char assets[EDITOR_PROJECT_PATH_MAX];
    char cooked[EDITOR_PROJECT_PATH_MAX];
    const NphStr a = nph_cvar_get_string(&ed->cv_assets);
    const NphStr c = nph_cvar_get_string(&ed->cv_cooked);
    (void)snprintf(assets, sizeof assets, "%.*s", (int)a.len, a.data);
    (void)snprintf(cooked, sizeof cooked, "%.*s", (int)c.len, c.data);
    EditorProjectDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = ed->alloc;
    d.assets_dir = assets;
    d.cooked_dir = cooked;
    d.jobs = nph_engine_jobs(ed->engine);
    EditorProject *p = NULL;
    NphResult r = editor_project_create(&d, &p);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "the project (editor.assets=%s, editor.cooked=%s): %s", assets,
                      cooked, nph_result_string(r));
        return r;
    }
    // At the root after the packs, so files cooked here shadow packed ones
    // with the same path. The mount stays as long as the engine: what the
    // loader mapped from it may outlive the editor.
    NphVfs *vfs = nph_engine_vfs(ed->engine);
    const char *dir = editor_project_cooked_dir(p);
    if (!mounted(vfs, dir)) {
        r = nph_vfs_mount_directory(vfs, NPH_STR(""), dir, NULL);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_EDITOR, "mounting %s: %s", dir, nph_result_string(r));
            editor_project_destroy(p);
            return r;
        }
    }
    nph_mutex_lock(&ed->resolve_lock);
    ed->project = p;
    nph_mutex_unlock(&ed->resolve_lock);
    ed->assets.stale = true;
    NPH_LOG_INFO(&LOG_EDITOR, "project: sources in %s, cooked files in %s",
                 editor_project_assets_dir(p), dir);
    return NPH_RESULT_OK;
}

void editor_assets_close(Editor *ed)
{
    // The resolver may be running on another thread: it lets go of the
    // project before the project goes.
    nph_mutex_lock(&ed->resolve_lock);
    EditorProject *p = ed->project;
    ed->project = NULL;
    nph_mutex_unlock(&ed->resolve_lock);
    editor_project_destroy(p);
    if (ed->assets.entries != NULL) {
        NPH_FREE_ARRAY(ed->alloc, EditorAssetEntry, ed->assets.entries, ed->assets.entry_cap);
    }
    memset(&ed->assets, 0, sizeof ed->assets);
}

NphResult editor_resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len)
{
    Editor *ed = (Editor *)user;
    NPH_ASSERT(ed != NULL);
    nph_mutex_lock(&ed->resolve_lock);
    const NphResult r = ed->project != NULL
                            ? editor_project_resolve(ed->project, guid, buf, cap, out_len)
                            : NPH_RESULT_NOT_FOUND;
    nph_mutex_unlock(&ed->resolve_lock);
    return r;
}

EditorProject *editor_project(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->project;
}

// ---------------------------------------------------------------------------
// Placing cooked scenes
// ---------------------------------------------------------------------------

// The most entities a cooked scene can become: a root, an entity per node,
// and one more per instance, light, and camera (nph_scene_import_cooked).
static u32 entities_for(NphAssetLoader *loader, NphGuid scene, NphAssetHandle *out_hold)
{
    *out_hold = NPH_ASSET_HANDLE_NULL;
    if (nph_asset_load_guid(loader, scene, out_hold) != NPH_RESULT_OK) {
        return 0U;
    }
    NphSceneView v;
    const NphAsset *asset = nph_asset_get(loader, *out_hold);
    if (asset == NULL || nph_scene_view(asset, &v) != NPH_RESULT_OK) {
        return 0U;
    }
    const NphSceneHeader *h = v.header;
    return 1U + h->node_count + h->instance_count + h->light_count + h->camera_count;
}

// Adds a cooked scene to the edited world at `at` as one undoable step,
// through a scratch world (edit.h editor_edit_paste), and selects it.
static NphResult place_scene(Editor *ed, NphGuid scene, const char *name, NphDVec3 at)
{
    NphWorld *edit = nph_engine_edit_world(ed->engine);
    NphAssetLoader *loader = nph_engine_loader(ed->engine);
    NphAssetHandle hold;
    const u32 capacity = entities_for(loader, scene, &hold);
    NphResult r = capacity != 0U ? NPH_RESULT_OK : NPH_RESULT_NOT_FOUND;
    NphWorld *scratch = NULL;
    if (r == NPH_RESULT_OK) {
        NphWorldDesc wd;
        memset(&wd, 0, sizeof wd);
        wd.alloc = ed->alloc;
        wd.max_entities = capacity;
        wd.registry_from = edit;
        r = nph_world_create(&wd, &scratch);
    }
    NphEntity root = NPH_ENTITY_NULL;
    if (r == NPH_RESULT_OK) {
        NphSceneImportDesc d;
        memset(&d, 0, sizeof d);
        d.world = scratch;
        d.loader = loader;
        d.scene = scene;
        d.root_transform = nph_local_transform_identity();
        d.root_transform.translation = at;
        d.name = name;
        d.scratch = ed->alloc;
        r = nph_scene_import_cooked(&d, &root, NULL);
    }
    NphEntity placed = NPH_ENTITY_NULL;
    if (r == NPH_RESULT_OK) {
        char label[EDITOR_HISTORY_LABEL_MAX];
        (void)snprintf(label, sizeof label, "Import %s", name);
        r = editor_edit_paste(ed->history, scratch, root, NPH_ENTITY_NULL, label, &placed);
    }
    if (scratch != NULL) {
        nph_world_destroy(scratch);
    }
    if (!nph_asset_handle_is_null(hold)) {
        nph_asset_release(loader, hold);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "placing %s: %s", name, nph_result_string(r));
        return r;
    }
    editor_selection_set(&ed->selection, placed);
    NPH_LOG_INFO(&LOG_EDITOR, "placed %s", name);
    return NPH_RESULT_OK;
}

static EditorPlacement *placement_of(Editor *ed, u32 id)
{
    for (u32 i = 0; i < EDITOR_PROJECT_REQUESTS; ++i) {
        if (ed->assets.placements[i].id == id) {
            return &ed->assets.placements[i];
        }
    }
    return NULL;
}

// The point a scene asked for from the browser goes: the one under the
// middle of the viewport (else the camera's pivot).
static NphDVec3 point_ahead(Editor *ed)
{
    const f32 *r = ed->viewport.rect;
    NphDVec3 at = ed->camera.pivot;
    if (ed->viewport.shown) {
        (void)editor_viewport_point(ed, r[0] + r[2] * 0.5F, r[1] + r[3] * 0.5F, &at);
    }
    return at;
}

// Cooks a source; a placement waits for it at `at` when `place`.
static NphResult cook(Editor *ed, const char *key, bool force, bool place, NphDVec3 at)
{
    EditorPlacement *slot = place ? placement_of(ed, 0U) : NULL;
    if (place && slot == NULL) {
        return NPH_RESULT_FULL;
    }
    u32 id = 0U;
    const NphResult r = editor_project_cook(ed->project, key, force, 0U, &id);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "cooking %s: %s", key[0] != '\0' ? key : "the sources",
                      nph_result_string(r));
        return r;
    }
    if (slot != NULL) {
        memset(slot, 0, sizeof *slot);
        slot->id = id;
        slot->place = true;
        slot->resolved = true;
        slot->at = at;
    }
    return NPH_RESULT_OK;
}

void editor_assets_place(Editor *ed, const char *key, NphDVec3 at)
{
    if (ed->project == NULL || nph_engine_is_playing(ed->engine)) {
        return;
    }
    NphGuid scene;
    if (editor_project_scene_of(ed->project, key, &scene)) {
        const NphStr stem = nph_path_stem(nph_str_from_cstr(key));
        char name[EDITOR_IMPORT_NAME_MAX];
        (void)snprintf(name, sizeof name, "%.*s", (int)stem.len, stem.data);
        (void)place_scene(ed, scene, name, at);
        return;
    }
    (void)cook(ed, key, false, true, at); // placed once cooked
}

NphResult editor_import(Editor *ed, const char *const *paths, u32 count, bool place, f32 x, f32 y)
{
    NPH_ASSERT(ed != NULL);
    if (ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    EditorPlacement *slot = place ? placement_of(ed, 0U) : NULL;
    if (place && slot == NULL) {
        return NPH_RESULT_FULL;
    }
    u32 id = 0U;
    const NphResult r =
        editor_project_import(ed->project, paths, count, ed->assets.folder, 0U, &id);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "importing %u files: %s", count, nph_result_string(r));
        return r;
    }
    if (slot != NULL) {
        memset(slot, 0, sizeof *slot);
        slot->id = id;
        slot->place = true;
        slot->mouse[0] = x;
        slot->mouse[1] = y;
    }
    // The browser shows the files arriving and the cook's progress.
    ed->show.assets = true;
    ed->assets.reveal = true;
    NPH_LOG_INFO(&LOG_EDITOR, "importing %u %s", count, count == 1U ? "file" : "files");
    return NPH_RESULT_OK;
}

static void finished(Editor *ed, const EditorImportResult *res)
{
    ed->assets.stale = true;
    if (res->result != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "import failed: %s (%u assets cooked, %u sources failed)",
                      nph_result_string(res->result), res->cooked, res->failed);
    } else {
        NPH_LOG_INFO(&LOG_EDITOR, "import done: %u assets cooked, %u already current", res->cooked,
                     res->skipped);
    }
    EditorPlacement *slot = placement_of(ed, res->id);
    if (slot == NULL) {
        return;
    }
    if (slot->place && slot->resolved) {
        for (u32 i = 0; i < res->scene_count; ++i) {
            (void)place_scene(ed, res->scenes[i], res->scene_names[i], slot->at);
        }
    }
    memset(slot, 0, sizeof *slot);
}

void editor_assets_update(Editor *ed)
{
    if (ed->project == NULL) {
        return;
    }
    // Where files were dropped, worked out while the camera is still where
    // it was when they were.
    for (u32 i = 0; i < EDITOR_PROJECT_REQUESTS; ++i) {
        EditorPlacement *s = &ed->assets.placements[i];
        if (s->id != 0U && s->place && !s->resolved) {
            s->place = editor_viewport_point(ed, s->mouse[0], s->mouse[1], &s->at);
            s->resolved = true;
        }
    }
    // Scenes join the edited world outside play only.
    if (nph_engine_is_playing(ed->engine)) {
        return;
    }
    EditorImportResult res;
    while (editor_project_poll(ed->project, &res)) {
        finished(ed, &res);
    }
}

// ---------------------------------------------------------------------------
// The browser
// ---------------------------------------------------------------------------

// The source key of `name` in the folder shown; false when it does not fit.
static bool key_of(const EditorAssets *a, const char *name, char *out, u64 cap)
{
    const int n = a->folder[0] == '\0' ? snprintf(out, (size_t)cap, "%s", name)
                                       : snprintf(out, (size_t)cap, "%s/%s", a->folder, name);
    return n >= 0 && (u64)n < cap;
}

static int compare_names(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        const char ca = nph_ascii_to_lower(*a);
        const char cb = nph_ascii_to_lower(*b);
        if (ca != cb || ca == '\0') {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
    }
}

// Folders first, then names without case.
static int compare_entries(const void *pa, const void *pb)
{
    const EditorAssetEntry *a = (const EditorAssetEntry *)pa;
    const EditorAssetEntry *b = (const EditorAssetEntry *)pb;
    if (a->is_directory != b->is_directory) {
        return a->is_directory ? -1 : 1;
    }
    return compare_names(a->name, b->name);
}

static EditorAssetEntry *push_entry(Editor *ed)
{
    EditorAssets *a = &ed->assets;
    if (a->entry_count == a->entry_cap) {
        const u32 cap = a->entry_cap != 0U ? a->entry_cap * 2U : 32U;
        EditorAssetEntry *grown = (EditorAssetEntry *)NPH_REALLOC(
            ed->alloc, a->entries, sizeof(EditorAssetEntry) * (u64)a->entry_cap,
            sizeof(EditorAssetEntry) * (u64)cap, NPH_ALIGNOF(EditorAssetEntry));
        if (grown == NULL) {
            return NULL;
        }
        a->entries = grown;
        a->entry_cap = cap;
    }
    return &a->entries[a->entry_count++];
}

static void list_folder(Editor *ed)
{
    EditorAssets *a = &ed->assets;
    EditorProject *p = ed->project;
    a->stale = false;
    a->generation = editor_project_generation(p);
    a->listed_ns = nph_time_now_ns();
    a->entry_count = 0U;
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    (void)snprintf(path, sizeof path, "%s%s%s", editor_project_assets_dir(p),
                   a->folder[0] != '\0' ? "/" : "", a->folder);
    NphDirIterator it;
    if (nph_dir_open(path, &it) != NPH_RESULT_OK) {
        // Not there (yet): an import into it may be creating it, so the
        // folder stays and shows empty until it is.
        return;
    }
    NphDirEntry d;
    while (nph_dir_next(&it, &d)) {
        if (d.name[0] == '.') {
            continue; // hidden, and the copies a copy is renamed from (".part")
        }
        const u64 n = strlen(d.name);
        if (n > 5U && strcmp(d.name + n - 5U, ".part") == 0) {
            continue;
        }
        EditorAssetEntry *e = push_entry(ed);
        if (e == NULL) {
            NPH_LOG_ERROR(&LOG_EDITOR, "assets: out of memory listing %s", path);
            break;
        }
        memcpy(e->name, d.name, sizeof e->name);
        e->is_directory = d.is_directory != 0;
        e->kind = e->is_directory ? (u32)COOKER_SOURCE_NONE : (u32)cooker_source_kind(d.name);
        e->cooked = false;
        if (e->kind != (u32)COOKER_SOURCE_NONE) {
            char key[EDITOR_PROJECT_PATH_MAX];
            e->cooked = key_of(a, d.name, key, sizeof key) && editor_project_is_cooked(p, key);
        }
    }
    nph_dir_close(&it);
    qsort(a->entries, (size_t)a->entry_count, sizeof *a->entries, compare_entries);
}

static void set_folder(Editor *ed, const char *folder)
{
    (void)snprintf(ed->assets.folder, sizeof ed->assets.folder, "%s", folder);
    ed->assets.selected[0] = '\0';
    ed->assets.stale = true;
}

// "assets / models / props": each part leads back to its folder.
static void breadcrumb(Editor *ed)
{
    EditorAssets *a = &ed->assets;
    if (igSmallButton("assets")) {
        set_folder(ed, "");
        return;
    }
    const u64 n = strlen(a->folder);
    u64 start = 0U;
    for (u64 i = 0; i <= n; ++i) {
        if (i < n && a->folder[i] != '/') {
            continue;
        }
        if (i > start) {
            char part[NPH_DIR_ENTRY_NAME_MAX];
            (void)snprintf(part, sizeof part, "%.*s", (int)(i - start), a->folder + start);
            igSameLine(0.0F, 4.0F);
            igTextDisabled("/");
            igSameLine(0.0F, 4.0F);
            igPushID_Int((int)i);
            const bool clicked = igSmallButton(part);
            igPopID();
            if (clicked && i < n) {
                char prefix[EDITOR_PROJECT_PATH_MAX];
                (void)snprintf(prefix, sizeof prefix, "%.*s", (int)i, a->folder);
                set_folder(ed, prefix);
                return;
            }
        }
        start = i + 1U;
    }
}

static const char *kind_name(const EditorAssetEntry *e)
{
    if (e->is_directory) {
        return "folder";
    }
    switch ((CookerSourceKind)e->kind) {
    case COOKER_SOURCE_SCENE:
        return "scene";
    case COOKER_SOURCE_IMAGE:
        return "image";
    case COOKER_SOURCE_NONE:
        break;
    }
    return "";
}

typedef enum BrowserAction {
    ACTION_NONE = 0,
    ACTION_OPEN,       // a folder: show it
    ACTION_PLACE,      // a scene: into the edited world, in front of the camera
    ACTION_COOK,       // cook it (a folder: its scenes)
    ACTION_COOK_AGAIN, // cook it even when current
} BrowserAction;

static BrowserAction context_menu(const EditorAssetEntry *e, bool playing)
{
    BrowserAction action = ACTION_NONE;
    if (!igBeginPopupContextItem(NULL, ImGuiPopupFlags_MouseButtonRight)) {
        return action;
    }
    if (e->is_directory && igMenuItem_Bool("Open", NULL, false, true)) {
        action = ACTION_OPEN;
    }
    if (e->kind == (u32)COOKER_SOURCE_SCENE &&
        igMenuItem_Bool("Place in Scene", "double-click", false, !playing)) {
        action = ACTION_PLACE;
    }
    if (e->is_directory || e->kind != (u32)COOKER_SOURCE_NONE) {
        if (igMenuItem_Bool("Cook", NULL, false, true)) {
            action = ACTION_COOK;
        }
        if (igMenuItem_Bool("Cook Again", NULL, false, true)) {
            action = ACTION_COOK_AGAIN;
        }
    }
    igEndPopup();
    return action;
}

// What a double-click does: open a folder, place a scene, cook an image.
static BrowserAction double_click_action(const EditorAssetEntry *e, bool playing)
{
    if (e->is_directory) {
        return ACTION_OPEN;
    }
    switch ((CookerSourceKind)e->kind) {
    case COOKER_SOURCE_SCENE:
        return playing ? ACTION_NONE : ACTION_PLACE;
    case COOKER_SOURCE_IMAGE:
        return ACTION_COOK;
    case COOKER_SOURCE_NONE:
        break;
    }
    return ACTION_NONE;
}

static void act(Editor *ed, const EditorAssetEntry *e, BrowserAction action)
{
    char key[EDITOR_PROJECT_PATH_MAX];
    if (!key_of(&ed->assets, e->name, key, sizeof key)) {
        NPH_LOG_ERROR(&LOG_EDITOR, "assets: the path of %s is too long", e->name);
        return;
    }
    switch (action) {
    case ACTION_OPEN:
        set_folder(ed, key);
        break;
    case ACTION_PLACE:
        editor_assets_place(ed, key, point_ahead(ed));
        break;
    case ACTION_COOK:
    case ACTION_COOK_AGAIN:
        (void)cook(ed, key, action == ACTION_COOK_AGAIN, false, nph_dvec3_zero());
        break;
    case ACTION_NONE:
        break;
    }
}

void editor_panel_assets(Editor *ed)
{
    if (!igBegin(EDITOR_WINDOW_ASSETS, &ed->show.assets, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    EditorProject *p = ed->project;
    EditorAssets *a = &ed->assets;
    if (p == NULL) {
        igTextDisabled("no project");
        igEnd();
        return;
    }
    if (a->stale || a->generation != editor_project_generation(p) ||
        nph_time_now_ns() - a->listed_ns > RELIST_NS) {
        list_folder(ed);
    }
    const bool playing = nph_engine_is_playing(ed->engine);
    breadcrumb(ed);
    igSameLine(0.0F, 16.0F);
    if (igSmallButton("Refresh")) {
        a->stale = true;
    }
    igSameLine(0.0F, -1.0F);
    if (igSmallButton("Cook Folder")) {
        (void)cook(ed, a->folder, false, false, nph_dvec3_zero());
    }
    char status[256];
    editor_project_status(p, status, sizeof status);
    const u32 pending = editor_project_pending(p);
    if (pending != 0U) {
        igSameLine(0.0F, 16.0F);
        igTextDisabled("%s (%u waiting)", status[0] != '\0' ? status : "Starting", pending);
    }
    igSeparator();
    BrowserAction action = ACTION_NONE;
    u32 target = 0U;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (a->entry_count != 0U && igBeginTable("##assets", 3, flags, ui_vec2(0.0F, 0.0F), 0.0F)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0F, 0U);
        igTableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
        igTableSetupColumn("Cooked", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
        igTableHeadersRow();
        for (u32 i = 0; i < a->entry_count; ++i) {
            const EditorAssetEntry *e = &a->entries[i];
            igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
            igTableNextColumn();
            igPushID_Int((int)i);
            char label[NPH_DIR_ENTRY_NAME_MAX + 2U];
            (void)snprintf(label, sizeof label, "%s%s", e->name, e->is_directory ? "/" : "");
            const bool selected = strcmp(a->selected, e->name) == 0;
            if (igSelectable_Bool(label, selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                  ui_vec2(0.0F, 0.0F))) {
                memcpy(a->selected, e->name, sizeof a->selected);
                if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
                    action = double_click_action(e, playing);
                    target = i;
                }
            }
            // A scene drags into the viewport, which places it under the mouse.
            if (e->kind == (u32)COOKER_SOURCE_SCENE &&
                igBeginDragDropSource(ImGuiDragDropFlags_None)) {
                char key[EDITOR_PROJECT_PATH_MAX];
                if (key_of(a, e->name, key, sizeof key)) {
                    (void)igSetDragDropPayload(EDITOR_DRAG_ASSET, key, strlen(key) + 1U,
                                               ImGuiCond_None);
                }
                igText("Place %s", e->name);
                igEndDragDropSource();
            }
            const BrowserAction chosen = context_menu(e, playing);
            if (chosen != ACTION_NONE) {
                action = chosen;
                target = i;
            }
            igTableNextColumn();
            igTextUnformatted(kind_name(e), NULL);
            igTableNextColumn();
            if (e->kind != (u32)COOKER_SOURCE_NONE) {
                igTextUnformatted(e->cooked ? "yes" : "no", NULL);
            }
            igPopID();
        }
        igEndTable();
    }
    if (a->entry_count == 0U) {
        igTextDisabled("Drop scenes (.gltf, .glb) or images on the window to import them here.");
    }
    // After the table: an action may list another folder.
    if (action != ACTION_NONE) {
        const EditorAssetEntry chosen = a->entries[target];
        act(ed, &chosen, action);
    }
    igEnd();
}

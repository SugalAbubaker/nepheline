// SPDX-License-Identifier: Apache-2.0
// The scene being edited (docs/editor.md "Scenes"): its file, whether it
// has unsaved changes, and new, open, and save. Unsaved means the history's
// state differs from the one saved or opened, so undoing back to it reads
// as clean.
#include <nepheline/core/assert.h>
#include <nepheline/platform/file.h>
#include <nepheline/scene/scene_io.h>

#include "editor_internal.h"

#include <stdio.h>
#include <string.h>

bool editor_scene_dirty(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->history != NULL && editor_history_state(ed->history) != ed->doc_saved_state;
}

const char *editor_scene_path(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->doc_path;
}

static void forget_document(Editor *ed)
{
    editor_history_clear(ed->history);
    editor_selection_clear(&ed->selection);
    ed->doc_saved_state = editor_history_state(ed->history);
}

NphResult editor_new_scene(Editor *ed)
{
    NPH_ASSERT(ed != NULL && ed->engine != NULL);
    if (nph_engine_is_playing(ed->engine)) {
        return NPH_RESULT_INVALID_STATE;
    }
    forget_document(ed);
    NPH_TRY(nph_engine_clear_scene(ed->engine));
    ed->doc_path[0] = '\0';
    NPH_LOG_INFO(&LOG_EDITOR, "new scene");
    return NPH_RESULT_OK;
}

// Loads `path` into a scratch world with the edited world's components:
// a file that would not load leaves the scene alone.
static NphResult validate(Editor *ed, const char *path, char *error, u64 cap)
{
    NphWorld *edit = nph_engine_edit_world(ed->engine);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, ed->alloc, &data, &size));
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = ed->alloc;
    wd.max_entities = nph_world_entity_capacity(edit);
    wd.registry_from = edit;
    NphWorld *scratch = NULL;
    NphResult r = nph_world_create(&wd, &scratch);
    NphSceneResult res;
    memset(&res, 0, sizeof res);
    if (r == NPH_RESULT_OK) {
        u32 magic = 0U;
        if (size >= sizeof magic) {
            memcpy(&magic, data, sizeof magic);
        }
        r = magic == NPH_SCENE_BINARY_MAGIC
                ? nph_scene_load_binary(scratch, data, size, NULL, &res)
                : nph_scene_load_text(scratch, (NphStr){(const char *)data, size}, NULL, &res);
        nph_world_destroy(scratch);
    }
    nph_file_free_all(ed->alloc, data, size);
    (void)snprintf(error, (size_t)cap, "%s", res.error);
    return r;
}

NphResult editor_open_scene(Editor *ed, const char *path)
{
    NPH_ASSERT(ed != NULL && ed->engine != NULL && path != NULL);
    if (nph_engine_is_playing(ed->engine)) {
        return NPH_RESULT_INVALID_STATE;
    }
    char error[NPH_SCENE_ERROR_MAX];
    error[0] = '\0';
    const NphResult r = validate(ed, path, error, sizeof error);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "%s: %s%s%s", path, nph_result_string(r),
                      error[0] != '\0' ? ": " : "", error);
        return r;
    }
    forget_document(ed);
    NPH_TRY(nph_engine_clear_scene(ed->engine));
    NPH_TRY(nph_engine_load_scene(ed->engine, path));
    (void)snprintf(ed->doc_path, sizeof ed->doc_path, "%s", path);
    NPH_LOG_INFO(&LOG_EDITOR, "opened %s (%u entities)", path,
                 nph_world_entity_count(nph_engine_edit_world(ed->engine)));
    return NPH_RESULT_OK;
}

NphResult editor_save_scene(Editor *ed, const char *path)
{
    NPH_ASSERT(ed != NULL && ed->engine != NULL);
    const char *target = path != NULL ? path : ed->doc_path;
    if (target[0] == '\0') {
        return NPH_RESULT_INVALID_ARGUMENT; // untitled: Save As
    }
    const NphResult r = nph_engine_save_scene(ed->engine, target);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "saving %s: %s", target, nph_result_string(r));
        return r;
    }
    if (target != ed->doc_path) {
        (void)snprintf(ed->doc_path, sizeof ed->doc_path, "%s", target);
    }
    ed->doc_saved_state = editor_history_state(ed->history);
    NPH_LOG_INFO(&LOG_EDITOR, "saved %s", ed->doc_path);
    return NPH_RESULT_OK;
}

void editor_update_title(Editor *ed)
{
    NphWindow *w = nph_engine_window(ed->engine);
    if (w == NULL) {
        return;
    }
    const char *slash = strrchr(ed->doc_path, '/');
    const char *back = strrchr(ed->doc_path, '\\');
    const char *name = ed->doc_path;
    if (slash != NULL || back != NULL) {
        name = (slash > back ? slash : back) + 1;
    }
    char title[sizeof ed->title];
    (void)snprintf(title, sizeof title, "Nepheline Editor - %s%s",
                   ed->doc_path[0] != '\0' ? name : "Untitled", editor_scene_dirty(ed) ? " *" : "");
    if (strcmp(title, ed->title) != 0) {
        memcpy(ed->title, title, sizeof title);
        nph_window_set_title(w, title);
    }
}

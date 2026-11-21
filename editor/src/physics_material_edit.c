// SPDX-License-Identifier: Apache-2.0
// The physics material panel's material (docs/editor.md "Physics material"; ADR-0108 decision 7).
//
// One physics material's source is open at a time. While the world plays, a
// change reaches every shape naming the material from the next step
// (nph_engine_physics_material_set, at the sync point), with nothing written,
// as the material panel draws one; the source is set so again when play
// begins, over a cook that may not have landed. When a change settles it is
// written to the source and cooked, which the next play reads. Each settled
// change is a step of the panel's own history, as the material panel's is: a
// physics material is not scene state, and changing one does not make the
// scene unsaved. Physics materials given to Colliders are steps of the scene's
// history (the inspector).
#include "editor_internal.h"

#include <cook/cooker.h>
#include <stdio.h>
#include <string.h>

#define UNDO_MAX  64U  // settled changes kept for undo
#define NEW_NAMES 100U // "New Physics Material", then "New Physics Material 2" to "... 100"

void editor_physics_material_init(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    memset(m, 0, sizeof *m);
    NPH_ARRAY_INIT(&m->undo, ed->alloc);
    NPH_ARRAY_INIT(&m->redo, ed->alloc);
}

void editor_physics_material_free(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    editor_physics_material_close(ed);
    NPH_ARRAY_FREE(&m->undo);
    NPH_ARRAY_FREE(&m->redo);
}

static bool source_path(const Editor *ed, const char *key, char *out, u64 cap)
{
    const int n = snprintf(out, (size_t)cap, "%s/%s", editor_project_assets_dir(ed->project), key);
    return n > 0 && (u64)n < cap;
}

// The source set into the world playing, wherever its Colliders name the
// material (none may: NOT_FOUND is nothing to say).
static void set_live(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    m->live_dirty = false;
    m->live_set = true;
    NphPhysicsMaterialHeader h;
    cook_physics_material_source_header(&m->source, &h);
    const NphResult r = nph_engine_physics_material_set(ed->engine, m->guid, &h);
    if (r != NPH_RESULT_OK && r != NPH_RESULT_NOT_FOUND) {
        NPH_LOG_WARN(&LOG_EDITOR, "playing %s: %s", m->key, nph_result_string(r));
    }
}

// Asks for the source to be cooked: at once, or when the cook in flight is
// done, or when the project has room again.
static NphResult ask_cook(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (m->cook != 0U) {
        m->cook_wanted = true; // after the one in flight, which may have read the source before
        return NPH_RESULT_OK;
    }
    u32 id = 0U;
    const NphResult r = editor_project_cook(ed->project, m->key, false, 0U, &id);
    if (r == NPH_RESULT_FULL) {
        m->cook_wanted = true;
        return NPH_RESULT_OK;
    }
    if (r != NPH_RESULT_OK) {
        (void)snprintf(m->error, sizeof m->error, "not cooked: %s", nph_result_string(r));
        NPH_LOG_ERROR(&LOG_EDITOR, "cooking %s: %s", m->key, nph_result_string(r));
        return r;
    }
    m->cook = id;
    m->cook_wanted = false;
    return NPH_RESULT_OK;
}

// Writes the source and cooks it.
static NphResult write_and_cook(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    if (!source_path(ed, m->key, path, sizeof path)) {
        return NPH_RESULT_OVERFLOW;
    }
    const NphResult r = cook_physics_material_source_save(&m->source, ed->alloc, path);
    if (r != NPH_RESULT_OK) {
        (void)snprintf(m->error, sizeof m->error, "not written: %s", nph_result_string(r));
        NPH_LOG_ERROR(&LOG_EDITOR, "writing %s: %s", m->key, nph_result_string(r));
        return r;
    }
    m->error[0] = '\0';
    ++m->revision;
    return ask_cook(ed);
}

// Ends the change being made: when it changed the source, the source before
// it is a step of the panel's history, and the source is written and cooked.
static NphResult settle(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (!m->changing) {
        return NPH_RESULT_OK;
    }
    m->changing = false;
    m->panel_changing = false;
    if (cook_physics_material_source_equal(&m->before, &m->source)) {
        return NPH_RESULT_OK; // back where it began: no step
    }
    if (m->undo.len == UNDO_MAX) {
        NPH_ARRAY_REMOVE_AT(&m->undo, 0U);
    }
    const CookPhysicsMaterialSource before = m->before;
    if (NPH_ARRAY_PUSH(&m->undo, before) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "%s: out of memory for its history; the change stays", m->key);
    }
    NPH_ARRAY_CLEAR(&m->redo);
    return write_and_cook(ed);
}

// ---------------------------------------------------------------------------
// Opening, changing, and closing
// ---------------------------------------------------------------------------

NphResult editor_physics_material_open(Editor *ed, const char *key)
{
    NPH_ASSERT(ed != NULL);
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (key == NULL || cooker_source_kind(key) != COOKER_SOURCE_PHYSICS_MATERIAL ||
        strlen(key) >= sizeof m->key) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (m->open && strcmp(m->key, key) == 0) {
        ed->show.physics_material = true;
        m->reveal = true;
        return NPH_RESULT_OK; // already open, with its history
    }
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    if (!source_path(ed, key, path, sizeof path)) {
        return NPH_RESULT_OVERFLOW;
    }
    CookPhysicsMaterialSource s;
    const NphResult r = cook_physics_material_source_load(ed->alloc, path, &s);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "opening %s: %s", key, nph_result_string(r));
        return r;
    }
    editor_physics_material_close(ed);
    m->open = true;
    (void)snprintf(m->key, sizeof m->key, "%s", key);
    m->guid = cooker_asset_guid(key, "physics_material");
    m->source = s;
    m->changing = false;
    m->panel_changing = false;
    m->live_dirty = false;
    m->live_set = false; // set into play at the next sync point, when playing
    m->cook = 0U;
    m->cook_wanted = false;
    m->error[0] = '\0';
    NPH_ARRAY_CLEAR(&m->undo);
    NPH_ARRAY_CLEAR(&m->redo);
    ed->show.physics_material = true;
    m->reveal = true;
    NPH_LOG_INFO(&LOG_EDITOR, "editing physics material %s", key);
    return NPH_RESULT_OK;
}

NphResult editor_physics_material_new(Editor *ed, const char *folder, char *out_key, u64 cap)
{
    NPH_ASSERT(ed != NULL && folder != NULL);
    if (ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    char key[EDITOR_PROJECT_PATH_MAX];
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    const char *sep = folder[0] != '\0' ? "/" : "";
    bool free_name = false;
    for (u32 n = 1U; n <= NEW_NAMES && !free_name; ++n) {
        const int k =
            n == 1U
                ? snprintf(key, sizeof key, "%s%sNew Physics Material.npmat", folder, sep)
                : snprintf(key, sizeof key, "%s%sNew Physics Material %u.npmat", folder, sep, n);
        if (k <= 0 || (u64)k >= sizeof key || !source_path(ed, key, path, sizeof path)) {
            NPH_LOG_ERROR(&LOG_EDITOR, "a new physics material in %s: the path is too long",
                          folder);
            return NPH_RESULT_OVERFLOW;
        }
        free_name = !nph_fs_exists(path);
    }
    if (!free_name) {
        NPH_LOG_ERROR(&LOG_EDITOR, "a new physics material in %s: %u names are taken", folder,
                      NEW_NAMES);
        return NPH_RESULT_BUSY;
    }
    if (out_key != NULL) {
        if (cap <= strlen(key)) {
            return NPH_RESULT_OVERFLOW;
        }
        memcpy(out_key, key, strlen(key) + 1U);
    }
    char dir[EDITOR_PROJECT_PATH_MAX * 2U];
    (void)snprintf(dir, sizeof dir, "%s%s%s", editor_project_assets_dir(ed->project), sep, folder);
    NPH_TRY(nph_fs_create_directories(dir));
    CookPhysicsMaterialSource s;
    cook_physics_material_source_default(&s);
    NphResult r = cook_physics_material_source_save(&s, ed->alloc, path);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "creating %s: %s", key, nph_result_string(r));
        return r;
    }
    r = editor_physics_material_open(ed, key);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    ed->assets.stale = true; // the browser lists it
    NPH_LOG_INFO(&LOG_EDITOR, "created %s", key);
    // Cooked at once, so that it can be given to Colliders.
    return ask_cook(ed);
}

const CookPhysicsMaterialSource *editor_physics_material_source(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->physics_material.open ? &ed->physics_material.source : NULL;
}

const char *editor_physics_material_key(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->physics_material.open ? ed->physics_material.key : NULL;
}

NphResult editor_physics_material_change(Editor *ed, const CookPhysicsMaterialSource *now,
                                         bool settled)
{
    NPH_ASSERT(ed != NULL && now != NULL);
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (!m->open) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (cook_physics_material_source_validate(now) != NPH_RESULT_OK) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!cook_physics_material_source_equal(now, &m->source)) {
        if (!m->changing) {
            m->before = m->source;
            m->changing = true;
        }
        m->source = *now;
        m->live_dirty = true;
    }
    return settled ? settle(ed) : NPH_RESULT_OK;
}

static bool swap_history(Editor *ed, EditorPhysicsMaterialSnapshots *from,
                         EditorPhysicsMaterialSnapshots *to)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (!m->open) {
        return false;
    }
    NPH_IGNORE_RESULT(settle(ed)); // a change being made is the one undone; logged
    if (from->len == 0U) {
        return false;
    }
    const CookPhysicsMaterialSource now = m->source;
    if (NPH_ARRAY_PUSH(to, now) != NPH_RESULT_OK) {
        return false; // nothing changes
    }
    m->source = NPH_ARRAY_POP(from);
    m->live_dirty = true;
    NPH_IGNORE_RESULT(write_and_cook(ed)); // logged, and shown in the panel
    return true;
}

bool editor_physics_material_undo(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return swap_history(ed, &ed->physics_material.undo, &ed->physics_material.redo);
}

bool editor_physics_material_redo(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return swap_history(ed, &ed->physics_material.redo, &ed->physics_material.undo);
}

void editor_physics_material_close(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (!m->open) {
        return;
    }
    if (ed->project != NULL) {
        NPH_IGNORE_RESULT(settle(ed)); // logged
    }
    m->open = false;
    m->changing = false;
    m->panel_changing = false;
    m->live_dirty = false; // what play has, it keeps; what settled is written
    m->live_set = false;
    m->focused = false;
    NPH_ARRAY_CLEAR(&m->undo);
    NPH_ARRAY_CLEAR(&m->redo);
    // A cook in flight still lands, and reaches the shapes playing the material.
}

bool editor_physics_material_focused(const Editor *ed)
{
    return ed->physics_material.open && ed->physics_material.focused;
}

void editor_physics_material_update(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (!m->open || ed->project == NULL) {
        return;
    }
    if (ed->engine == NULL || !nph_engine_is_playing(ed->engine)) {
        m->live_set = false; // the next play takes the source when it begins
    } else if (m->live_dirty || !m->live_set) {
        set_live(ed);
    }
    if (m->cook_wanted && m->cook == 0U) {
        NPH_IGNORE_RESULT(ask_cook(ed)); // logged
    }
}

void editor_physics_material_finished(Editor *ed, const EditorImportResult *res)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (!m->open || m->cook == 0U || res->id != m->cook) {
        return;
    }
    m->cook = 0U;
    if (res->result != NPH_RESULT_OK) {
        (void)snprintf(m->error, sizeof m->error, "the cook failed (%s): the console says why",
                       nph_result_string(res->result));
    } else {
        m->error[0] = '\0';
    }
    if (m->cook_wanted) {
        NPH_IGNORE_RESULT(ask_cook(ed)); // logged
    }
}

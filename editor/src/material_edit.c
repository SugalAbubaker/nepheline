// SPDX-License-Identifier: Apache-2.0
// The material panel's material (docs/editor.md "Materials"; ADR-0105 decisions 4 to 6).
//
// One material's source is open at a time. A change is drawn from the next
// frame where anything draws the material (nph_scene_renderer_material_set,
// at the sync point, once a frame at most: each call rewrites the renderer's
// material table), with nothing written. When it settles, it is written to the source and
// cooked, and the cook reloads the material in place (panel_assets.c). Each
// settled change is a step of the panel's own history, which is not the
// scene's: a material is not scene state, and changing one does not make the
// scene unsaved. Materials given to entities' slots are steps of the scene's
// history.
#include <nepheline/runtime/scene_render.h>

#include "editor_internal.h"

#include <cook/cooker.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_MAX   64U   // settled changes kept for undo
#define NEW_NAMES  100U  // "New Material", then "New Material 2" to "New Material 100"
#define IMAGES_MAX 4096U // images a slot's list holds
#define DIR_DEPTH  16U   // folders deep the images are looked for

void editor_material_init(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
    memset(m, 0, sizeof *m);
    NPH_ARRAY_INIT(&m->undo, ed->alloc);
    NPH_ARRAY_INIT(&m->redo, ed->alloc);
    NPH_ARRAY_INIT(&m->images, ed->alloc);
}

void editor_material_free(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
    editor_material_close(ed);
    NPH_ARRAY_FREE(&m->undo);
    NPH_ARRAY_FREE(&m->redo);
    NPH_ARRAY_FREE(&m->images);
}

static bool source_path(const Editor *ed, const char *key, char *out, u64 cap)
{
    const int n = snprintf(out, (size_t)cap, "%s/%s", editor_project_assets_dir(ed->project), key);
    return n > 0 && (u64)n < cap;
}

// Draws the source from the next frame wherever the material is drawn
// (ADR-0105 decision 4). Its textures stay the cooked ones: a change of
// image is drawn once it is cooked.
static void draw_live(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
    m->live_pending = false;
    NphSceneRenderer *sr = ed->engine != NULL ? nph_engine_scene_renderer(ed->engine) : NULL;
    if (sr == NULL) {
        return;
    }
    NphGuid none[NPH_MATERIAL_TEXTURE_COUNT];
    memset(none, 0, sizeof none);
    NphMaterialHeader header;
    cook_material_source_header(&m->source, none, &header);
    const NphMaterialUvTransform uv = cook_material_source_uv(&m->source);
    const NphResult r = nph_scene_renderer_material_set(sr, m->guid, &header, &uv);
    if (r == NPH_RESULT_INVALID_STATE) {
        m->live_pending = true; // still loading: the next frame
    } else if (r != NPH_RESULT_OK && r != NPH_RESULT_NOT_FOUND) {
        NPH_LOG_WARN(&LOG_EDITOR, "drawing %s: %s", m->key, nph_result_string(r));
    }
}

// Asks for the source to be cooked: at once, or when the cook in flight is
// done, or when the project has room again.
static NphResult ask_cook(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
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
    EditorMaterialEdit *m = &ed->material;
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    if (!source_path(ed, m->key, path, sizeof path)) {
        return NPH_RESULT_OVERFLOW;
    }
    const NphResult r = cook_material_source_save(&m->source, ed->alloc, path);
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
    EditorMaterialEdit *m = &ed->material;
    if (!m->changing) {
        return NPH_RESULT_OK;
    }
    m->changing = false;
    m->panel_changing = false;
    if (cook_material_source_equal(&m->before, &m->source)) {
        return NPH_RESULT_OK; // back where it began: no step
    }
    if (m->undo.len == UNDO_MAX) {
        NPH_ARRAY_REMOVE_AT(&m->undo, 0U);
    }
    const CookMaterialSource before = m->before;
    if (NPH_ARRAY_PUSH(&m->undo, before) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "%s: out of memory for its history; the change stays", m->key);
    }
    NPH_ARRAY_CLEAR(&m->redo);
    return write_and_cook(ed);
}

// ---------------------------------------------------------------------------
// Opening, changing, and closing
// ---------------------------------------------------------------------------

NphResult editor_material_open(Editor *ed, const char *key)
{
    NPH_ASSERT(ed != NULL);
    EditorMaterialEdit *m = &ed->material;
    if (ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (key == NULL || cooker_source_kind(key) != COOKER_SOURCE_MATERIAL ||
        strlen(key) >= sizeof m->key) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (m->open && strcmp(m->key, key) == 0) {
        ed->show.material = true;
        m->reveal = true;
        return NPH_RESULT_OK; // already open, with its history
    }
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    if (!source_path(ed, key, path, sizeof path)) {
        return NPH_RESULT_OVERFLOW;
    }
    CookMaterialSource s;
    const NphResult r = cook_material_source_load(ed->alloc, path, &s);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "opening %s: %s", key, nph_result_string(r));
        return r;
    }
    editor_material_close(ed);
    m->open = true;
    (void)snprintf(m->key, sizeof m->key, "%s", key);
    m->guid = cooker_asset_guid(key, "material");
    m->source = s;
    m->changing = false;
    m->panel_changing = false;
    m->live_dirty = false;
    m->live_pending = false;
    m->cook = 0U;
    m->cook_wanted = false;
    m->error[0] = '\0';
    NPH_ARRAY_CLEAR(&m->undo);
    NPH_ARRAY_CLEAR(&m->redo);
    ed->show.material = true;
    m->reveal = true;
    NPH_LOG_INFO(&LOG_EDITOR, "editing material %s", key);
    return NPH_RESULT_OK;
}

NphResult editor_material_new(Editor *ed, const char *folder, char *out_key, u64 cap)
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
        const int k = n == 1U
                          ? snprintf(key, sizeof key, "%s%sNew Material.nmat", folder, sep)
                          : snprintf(key, sizeof key, "%s%sNew Material %u.nmat", folder, sep, n);
        if (k <= 0 || (u64)k >= sizeof key || !source_path(ed, key, path, sizeof path)) {
            NPH_LOG_ERROR(&LOG_EDITOR, "a new material in %s: the path is too long", folder);
            return NPH_RESULT_OVERFLOW;
        }
        free_name = !nph_fs_exists(path);
    }
    if (!free_name) {
        NPH_LOG_ERROR(&LOG_EDITOR, "a new material in %s: %u names are taken", folder, NEW_NAMES);
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
    CookMaterialSource s;
    cook_material_source_default(&s);
    NphResult r = cook_material_source_save(&s, ed->alloc, path);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "creating %s: %s", key, nph_result_string(r));
        return r;
    }
    r = editor_material_open(ed, key);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    ed->assets.stale = true; // the browser lists it
    NPH_LOG_INFO(&LOG_EDITOR, "created %s", key);
    // Cooked at once, so that it can be given to entities.
    return ask_cook(ed);
}

const CookMaterialSource *editor_material_source(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->material.open ? &ed->material.source : NULL;
}

const char *editor_material_key(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->material.open ? ed->material.key : NULL;
}

NphResult editor_material_change(Editor *ed, const CookMaterialSource *now, bool settled)
{
    NPH_ASSERT(ed != NULL && now != NULL);
    EditorMaterialEdit *m = &ed->material;
    if (!m->open) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (cook_material_source_validate(now) != NPH_RESULT_OK) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!cook_material_source_equal(now, &m->source)) {
        if (!m->changing) {
            m->before = m->source;
            m->changing = true;
        }
        m->source = *now;
        m->live_dirty = true;
    }
    return settled ? settle(ed) : NPH_RESULT_OK;
}

static bool swap_history(Editor *ed, EditorMaterialSnapshots *from, EditorMaterialSnapshots *to)
{
    EditorMaterialEdit *m = &ed->material;
    if (!m->open) {
        return false;
    }
    NPH_IGNORE_RESULT(settle(ed)); // a change being made is the one undone; logged
    if (from->len == 0U) {
        return false;
    }
    const CookMaterialSource now = m->source;
    if (NPH_ARRAY_PUSH(to, now) != NPH_RESULT_OK) {
        return false; // nothing changes
    }
    m->source = NPH_ARRAY_POP(from);
    m->live_dirty = true;
    NPH_IGNORE_RESULT(write_and_cook(ed)); // logged, and shown in the panel
    return true;
}

bool editor_material_undo(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return swap_history(ed, &ed->material.undo, &ed->material.redo);
}

bool editor_material_redo(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return swap_history(ed, &ed->material.redo, &ed->material.undo);
}

void editor_material_close(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorMaterialEdit *m = &ed->material;
    if (!m->open) {
        return;
    }
    if (ed->project != NULL) {
        NPH_IGNORE_RESULT(settle(ed)); // logged
    }
    m->open = false;
    m->changing = false;
    m->panel_changing = false;
    m->live_dirty = false; // what settled is written: its cook reloads it
    m->live_pending = false;
    m->focused = false;
    NPH_ARRAY_CLEAR(&m->undo);
    NPH_ARRAY_CLEAR(&m->redo);
    // A cook in flight still lands, and reloads the material where it is drawn.
}

bool editor_material_focused(const Editor *ed)
{
    return ed->material.open && ed->material.focused;
}

void editor_material_update(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
    if (!m->open || ed->project == NULL) {
        return;
    }
    // A change is drawn here, once a frame at most. While a change is being
    // made it is drawn every frame, so that a reload finishing meanwhile (an
    // earlier change's cook) does not show the cooked source over it; and it
    // is tried again when the renderer could not take it yet.
    if (m->live_dirty || m->changing || m->live_pending) {
        m->live_dirty = false;
        draw_live(ed);
    }
    if (m->cook_wanted && m->cook == 0U) {
        NPH_IGNORE_RESULT(ask_cook(ed)); // logged
    }
}

void editor_material_finished(Editor *ed, const EditorImportResult *res)
{
    EditorMaterialEdit *m = &ed->material;
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

// ---------------------------------------------------------------------------
// Materials on entities (ADR-0105 decision 6)
// ---------------------------------------------------------------------------

// The slots the entity's loaded mesh has (1 when it is not loaded).
static u32 slot_count(Editor *ed, NphEntity e)
{
    const NphSceneRenderer *sr = nph_engine_scene_renderer(ed->engine);
    const NphAsset *mesh = sr != NULL ? nph_scene_renderer_mesh_asset(sr, e) : NULL;
    NphMeshView v;
    u32 n = 1U;
    if (mesh != NULL && nph_mesh_view(mesh, &v) == NPH_RESULT_OK) {
        for (u32 i = 0; i < v.header->submesh_count; ++i) {
            n = nph_max_u32(n, v.submeshes[i].material_slot + 1U);
        }
    }
    return nph_min_u32(n, NPH_MESH_RENDERER_MATERIALS + NPH_MESH_MATERIALS_EXTRA);
}

NphResult editor_material_assign(Editor *ed, NphEntity e, u32 slot, const char *key)
{
    NPH_ASSERT(ed != NULL);
    if (ed->project == NULL || ed->engine == NULL || nph_engine_is_playing(ed->engine)) {
        return NPH_RESULT_INVALID_STATE;
    }
    const u32 slots = NPH_MESH_RENDERER_MATERIALS + NPH_MESH_MATERIALS_EXTRA;
    if (key == NULL || cooker_source_kind(key) != COOKER_SOURCE_MATERIAL ||
        (slot != EDITOR_MATERIAL_EVERY_SLOT && slot >= slots)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphWorld *w = nph_engine_edit_world(ed->engine);
    const NphRuntimeComponents *rc = nph_engine_components(ed->engine);
    const NphMeshRenderer *now =
        nph_world_is_alive(w, e) ? NPH_WORLD_GET(w, e, rc->mesh_renderer, NphMeshRenderer) : NULL;
    if (now == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    NphMeshRenderer mr = *now;
    NphMeshMaterials mm;
    const NphMeshMaterials *now_extra = NPH_WORLD_GET(w, e, rc->mesh_materials, NphMeshMaterials);
    if (now_extra != NULL) {
        mm = *now_extra;
    } else {
        memset(&mm, 0, sizeof mm);
    }
    const NphGuid material = cooker_asset_guid(key, "material");
    const u32 first = slot == EDITOR_MATERIAL_EVERY_SLOT ? 0U : slot;
    const u32 end = slot == EDITOR_MATERIAL_EVERY_SLOT ? slot_count(ed, e) : slot + 1U;
    bool extra = false;
    for (u32 s = first; s < end; ++s) {
        if (s < NPH_MESH_RENDERER_MATERIALS) {
            nph_mesh_renderer_set_material(&mr, s, material);
        } else {
            nph_mesh_materials_set(&mm, s - NPH_MESH_RENDERER_MATERIALS, material);
            extra = true;
        }
    }
    NPH_TRY(editor_history_begin(ed->history, "Set Material"));
    NphResult r = NPH_RESULT_OK;
    if (extra) {
        r = editor_history_set(ed->history, e, rc->mesh_materials, &mm);
    }
    // The MeshRenderer too, even unchanged: the scene renderer reads the
    // extra slots when it does.
    if (r == NPH_RESULT_OK) {
        r = editor_history_set(ed->history, e, rc->mesh_renderer, &mr);
    }
    if (r != NPH_RESULT_OK) {
        editor_history_abort(ed->history);
        NPH_LOG_WARN(&LOG_EDITOR, "giving %s: %s", key, nph_result_string(r));
        return r;
    }
    editor_history_end(ed->history);
    if (!editor_project_is_cooked(ed->project, key)) {
        u32 id = 0U;
        const NphResult c = editor_project_cook(ed->project, key, false, 0U, &id);
        if (c != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_EDITOR, "cooking %s: %s", key, nph_result_string(c));
        }
    }
    char label[64];
    editor_entity_label(w, e, label, sizeof label);
    if (slot == EDITOR_MATERIAL_EVERY_SLOT) {
        NPH_LOG_INFO(&LOG_EDITOR, "%s: %s on every slot", label, key);
    } else {
        NPH_LOG_INFO(&LOG_EDITOR, "%s: %s on slot %u", label, key, slot);
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The project's images
// ---------------------------------------------------------------------------

static void list_images_in(Editor *ed, const char *rel, u32 depth)
{
    EditorMaterialEdit *m = &ed->material;
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    (void)snprintf(path, sizeof path, "%s%s%s", editor_project_assets_dir(ed->project),
                   rel[0] != '\0' ? "/" : "", rel);
    NphDirIterator it;
    if (nph_dir_open(path, &it) != NPH_RESULT_OK) {
        return;
    }
    NphDirEntry d;
    while (m->images.len < IMAGES_MAX && nph_dir_next(&it, &d)) {
        if (d.name[0] == '.') {
            continue; // hidden, and the copies being written (".part")
        }
        EditorImageKey k;
        const int n =
            snprintf(k.key, sizeof k.key, "%s%s%s", rel, rel[0] != '\0' ? "/" : "", d.name);
        if (n <= 0 || (u64)n >= sizeof k.key) {
            continue;
        }
        if (d.is_directory) {
            if (depth < DIR_DEPTH) {
                list_images_in(ed, k.key, depth + 1U);
            }
            continue;
        }
        if (cooker_source_kind(d.name) == COOKER_SOURCE_IMAGE &&
            cook_material_image_key_valid(nph_str_from_cstr(k.key)) &&
            NPH_ARRAY_PUSH(&m->images, k) != NPH_RESULT_OK) {
            break; // out of memory: the list is cut
        }
    }
    nph_dir_close(&it);
}

static int compare_images(const void *a, const void *b)
{
    return strcmp(((const EditorImageKey *)a)->key, ((const EditorImageKey *)b)->key);
}

void editor_material_list_images(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
    NPH_ARRAY_CLEAR(&m->images);
    if (ed->project == NULL) {
        return;
    }
    list_images_in(ed, "", 0U);
    if (m->images.len > 1U) {
        qsort(m->images.data, (size_t)m->images.len, sizeof *m->images.data, compare_images);
    }
}

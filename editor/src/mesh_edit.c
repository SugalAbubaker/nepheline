// SPDX-License-Identifier: Apache-2.0
// Mesh editing (docs/editor.md "Mesh editing"; CLAUDE.md §8.13 "basic mesh
// editing (vertex/face transform, extrude), saved as cooked meshes"): the
// editable mesh an entity draws, edited in place.
//
//   - Editing reads the mesh's `.nmesh` source and draws it, as it changes,
//     in place of the cooked mesh: a render mesh of its own over an instance
//     of its own, while the scene renderer hides the entity's.
//   - Vertices or faces are selected by clicking (Ctrl adds or removes), and
//     the gizmo moves, rotates, and scales them about their centre in the
//     entity's space. Faces extrude as one region.
//   - Each change is a step of the editing's own history (a copy of the mesh
//     before it), undone and redone with the usual keys while editing.
//   - Ending saves the source and cooks it again on the import thread. The
//     preview stays until the scene renderer draws the cooked mesh again,
//     reloaded, so the entity is never missing from the view.
#include <nepheline/core/assert.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/primitives.h>

#include "editor_internal.h"

#include <cook/cooker.h>
#include <stdio.h>
#include <string.h>

#define PICK_PIXELS   12.0F // how near the mouse a vertex must be on screen
#define UNDO_MAX      64U   // changes kept for undo
#define COLOR_EDGE    NPH_RENDER_RGBA(210, 210, 220, 255)
#define COLOR_VERTEX  NPH_RENDER_RGBA(240, 240, 250, 255)
#define COLOR_PICKED  NPH_RENDER_RGBA(255, 160, 40, 255)
#define MESH_GIZMO_ID 0x4D45534800000000ULL // "MESH": the gizmo's id for the selection

// ---------------------------------------------------------------------------
// The edit's state
// ---------------------------------------------------------------------------

static NphWorldTransform entity_transform(const Editor *ed)
{
    return editor_world_transform_of(nph_engine_edit_world(ed->engine), ed->mesh_edit.entity);
}

static u32 element_count(const EditorMeshEdit *me)
{
    return me->mode == EDITOR_MESH_FACES ? me->mesh.faces.len : me->mesh.positions.len;
}

// The selection flags sized to the elements: kept when the count did not
// change, cleared otherwise.
static NphResult selection_fit(EditorMeshEdit *me)
{
    const u32 n = element_count(me);
    if (me->selected.len != n) {
        NPH_ARRAY_CLEAR(&me->selected);
        NPH_TRY(NPH_ARRAY_RESIZE(&me->selected, n));
    }
    return NPH_RESULT_OK;
}

static void selection_clear(EditorMeshEdit *me)
{
    if (me->selected.len != 0U) {
        memset(me->selected.data, 0, (size_t)me->selected.len);
    }
}

static void snapshots_clear(EditorMeshEditSnapshots *s)
{
    for (u32 i = 0; i < s->len; ++i) {
        cook_polymesh_free(&s->data[i]);
    }
    NPH_ARRAY_CLEAR(s);
}

// The vertices the selection moves: the selected ones, or the corners of
// the selected faces, each once.
static NphResult selected_vertices(Editor *ed, NphArrayU32 *out)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    NPH_ARRAY_CLEAR(out);
    if (me->mode == EDITOR_MESH_VERTICES) {
        for (u32 v = 0; v < me->selected.len; ++v) {
            if (me->selected.data[v] != 0U) {
                NPH_TRY(NPH_ARRAY_PUSH(out, v));
            }
        }
        return NPH_RESULT_OK;
    }
    u8 *seen = NPH_ALLOC_ARRAY_ZEROED(ed->alloc, u8, (u64)me->mesh.positions.len + 1U);
    if (seen == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphResult r = NPH_RESULT_OK;
    for (u32 f = 0; f < me->selected.len && r == NPH_RESULT_OK; ++f) {
        if (me->selected.data[f] == 0U) {
            continue;
        }
        const CookPolyFace *face = &me->mesh.faces.data[f];
        for (u32 i = 0; i < face->count && r == NPH_RESULT_OK; ++i) {
            const u32 v = me->mesh.corners.data[face->first + i];
            if (seen[v] == 0U) {
                seen[v] = 1U;
                r = NPH_ARRAY_PUSH(out, v);
            }
        }
    }
    NPH_FREE_ARRAY(ed->alloc, u8, seen, (u64)me->mesh.positions.len + 1U);
    return r;
}

// A copy of the mesh before a change, for undo; a new change drops redo.
static NphResult push_undo(EditorMeshEdit *me, NphAllocator *alloc)
{
    if (me->undo.len == UNDO_MAX) {
        cook_polymesh_free(&me->undo.data[0]);
        NPH_ARRAY_REMOVE_AT(&me->undo, 0U);
    }
    CookPolyMesh copy;
    NPH_TRY(cook_polymesh_copy(&me->mesh, alloc, &copy));
    const NphResult r = NPH_ARRAY_PUSH(&me->undo, copy);
    if (r != NPH_RESULT_OK) {
        cook_polymesh_free(&copy);
        return r;
    }
    snapshots_clear(&me->redo);
    return NPH_RESULT_OK;
}

static void changed(EditorMeshEdit *me)
{
    me->changed = true;
    me->preview_stale = true;
}

// ---------------------------------------------------------------------------
// The preview
// ---------------------------------------------------------------------------

static void drop_preview(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    NphRenderer *r = nph_engine_renderer(ed->engine);
    if (r == NULL) {
        return;
    }
    if (nph_render_instance_is_valid(r, me->preview_instance)) {
        nph_render_instance_destroy(r, me->preview_instance);
    }
    if (nph_render_mesh_is_valid(r, me->preview)) {
        nph_render_mesh_destroy(r,
                                me->preview); // after the instance, and after the frames using it
    }
    me->preview_instance = NPH_RENDER_INSTANCE_NULL;
    me->preview = NPH_RENDER_MESH_NULL;
}

static NphRenderTransform render_transform_of(const NphWorldTransform *wt)
{
    NphRenderTransform t;
    t.linear = nph_world_transform_linear(wt);
    t.translation = wt->translation;
    return t;
}

// The mesh as edited, triangulated as the cooker will (cook/polymesh.h),
// drawn with the default material.
static NphResult rebuild_preview(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    me->preview_stale = false;
    NphRenderer *r = nph_engine_renderer(ed->engine);
    if (r == NULL) {
        return NPH_RESULT_OK;
    }
    CookMesh mesh;
    NphResult res = cook_polymesh_to_mesh(&me->mesh, me->name, &mesh);
    NphRenderVertexInput *v = NULL;
    const u32 count = mesh.vertices.len;
    if (res == NPH_RESULT_OK && (count == 0U || mesh.indices.len == 0U)) {
        drop_preview(ed); // nothing left to draw
        cook_mesh_free(&mesh);
        return NPH_RESULT_OK;
    }
    if (res == NPH_RESULT_OK) {
        v = NPH_ALLOC_ARRAY(ed->alloc, NphRenderVertexInput, count);
        res = v != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < count && res == NPH_RESULT_OK; ++i) {
        const CookVertex *x = &mesh.vertices.data[i];
        v[i].position = x->position;
        v[i].normal = nph_vec3_normalize_or(x->normal, nph_vec3_unit_y());
        v[i].uv = x->uv0;
        v[i].color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    }
    NphRenderMeshData data;
    memset(&data, 0, sizeof data);
    if (res == NPH_RESULT_OK) {
        res = nph_render_mesh_data_build(ed->alloc, v, count, mesh.indices.data, mesh.indices.len,
                                         &data);
    }
    NphRenderMesh created = NPH_RENDER_MESH_NULL;
    if (res == NPH_RESULT_OK) {
        NphRenderSubmesh sub;
        NphRenderMeshDesc desc;
        nph_render_mesh_data_desc(&data, "mesh edit", &sub, &desc);
        res = nph_render_mesh_create(r, &desc, &created);
        nph_render_mesh_data_free(&data);
    }
    if (res == NPH_RESULT_OK) {
        // The instance moves to the new mesh: a new one, the old pair gone.
        drop_preview(ed);
        me->preview = created;
        NphRenderInstanceDesc d;
        memset(&d, 0, sizeof d);
        d.mesh = created;
        const NphWorldTransform wt = entity_transform(ed);
        d.transform = render_transform_of(&wt);
        res = nph_render_instance_create(r, &d, &me->preview_instance);
    }
    if (v != NULL) {
        NPH_FREE_ARRAY(ed->alloc, NphRenderVertexInput, v, count);
    }
    cook_mesh_free(&mesh);
    if (res != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "drawing the mesh being edited: %s", nph_result_string(res));
    }
    return res;
}

// The cooked mesh back in view: the preview goes and the entity shows.
static void hand_back(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    drop_preview(ed);
    NphSceneRenderer *sr = nph_engine_scene_renderer(ed->engine);
    if (sr != NULL) {
        (void)nph_scene_renderer_set_hidden(sr, me->entity, false);
    }
    me->committing = false;
    me->commit = 0U;
}

// ---------------------------------------------------------------------------
// Beginning and ending
// ---------------------------------------------------------------------------

bool editor_mesh_editing(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->mesh_edit.active;
}

const CookPolyMesh *editor_mesh_edit_mesh(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->mesh_edit.active ? &ed->mesh_edit.mesh : NULL;
}

NphResult editor_mesh_edit_begin(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorMeshEdit *me = &ed->mesh_edit;
    if (ed->project == NULL || ed->engine == NULL || nph_engine_is_playing(ed->engine)) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (me->active) {
        return NPH_RESULT_OK;
    }
    const NphWorld *w = nph_engine_edit_world(ed->engine);
    const NphEntity e = ed->selection.primary;
    const NphMeshRenderer *m = (const NphMeshRenderer *)nph_world_get(
        w, e, nph_engine_components(ed->engine)->mesh_renderer);
    char key[EDITOR_PROJECT_PATH_MAX];
    char sub[32];
    if (m == NULL ||
        !editor_project_source_of(ed->project, m->mesh, key, sizeof key, sub, sizeof sub) ||
        strcmp(sub, "mesh") != 0 || cooker_source_kind(key) != COOKER_SOURCE_MESH) {
        char label[64];
        editor_entity_label(w, e, label, sizeof label);
        NPH_LOG_WARN(&LOG_EDITOR, "%s does not draw an editable mesh of the project", label);
        return NPH_RESULT_NOT_FOUND;
    }
    // A commit still on its way for another entity: its cooked mesh shows
    // when it lands, without its preview.
    if (me->committing) {
        hand_back(ed);
    }
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    (void)snprintf(path, sizeof path, "%s/%s", editor_project_assets_dir(ed->project), key);
    cook_polymesh_free(&me->mesh);
    const NphResult r = cook_polymesh_load(ed->alloc, path, &me->mesh);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "editing %s: %s", key, nph_result_string(r));
        cook_polymesh_free(&me->mesh);
        return r;
    }
    me->active = true;
    me->entity = e;
    me->asset = m->mesh;
    (void)snprintf(me->key, sizeof me->key, "%s", key);
    const NphStr stem = nph_path_stem(nph_str_from_cstr(key));
    (void)snprintf(me->name, sizeof me->name, "%.*s", (int)stem.len, stem.data);
    me->mode = EDITOR_MESH_VERTICES;
    me->changed = false;
    NPH_ARRAY_CLEAR(&me->selected);
    NPH_IGNORE_RESULT(selection_fit(me)); // empty when it cannot grow
    snapshots_clear(&me->undo);
    snapshots_clear(&me->redo);
    // Drawn as it is edited, in place of the cooked mesh.
    NphSceneRenderer *sr = nph_engine_scene_renderer(ed->engine);
    if (sr != NULL) {
        (void)nph_scene_renderer_set_hidden(sr, e, true);
    }
    me->preview_stale = true;
    NPH_LOG_INFO(&LOG_EDITOR, "editing %s", key);
    return NPH_RESULT_OK;
}

void editor_mesh_edit_end(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->active) {
        return;
    }
    me->active = false;
    snapshots_clear(&me->undo);
    snapshots_clear(&me->redo);
    NPH_ARRAY_CLEAR(&me->selected);
    if (!me->changed) {
        cook_polymesh_free(&me->mesh);
        hand_back(ed);
        return;
    }
    // Saved and cooked again; the preview stays until the cooked mesh is back.
    if (me->preview_stale) {
        NPH_IGNORE_RESULT(rebuild_preview(ed)); // logged
    }
    char path[EDITOR_PROJECT_PATH_MAX * 2U];
    (void)snprintf(path, sizeof path, "%s/%s", editor_project_assets_dir(ed->project), me->key);
    NphResult r = cook_polymesh_save(&me->mesh, path);
    u32 id = 0U;
    if (r == NPH_RESULT_OK) {
        r = editor_project_cook(ed->project, me->key, false, 0U, &id);
    }
    cook_polymesh_free(&me->mesh);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_EDITOR, "saving %s: %s", me->key, nph_result_string(r));
        hand_back(ed);
        return;
    }
    me->committing = true;
    me->commit = id;
    me->commit_done = false;
    me->stale_instance = NPH_RENDER_INSTANCE_NULL;
    NPH_LOG_INFO(&LOG_EDITOR, "saved %s", me->key);
}

void editor_mesh_edit_finished(Editor *ed, const EditorImportResult *res)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->committing || me->commit_done || res->id != me->commit) {
        return;
    }
    me->commit_done = true;
    me->commit_ok = res->result == NPH_RESULT_OK;
    me->reloading = false;
    for (u32 i = 0; i < res->written_count; ++i) {
        me->reloading = me->reloading || nph_guid_equals(res->written[i], me->asset);
    }
    // The reload replaces the entity's instance at this frame's scene sync:
    // the one there now is the old one.
    me->stale_instance =
        nph_scene_renderer_instance(nph_engine_scene_renderer(ed->engine), me->entity);
}

void editor_mesh_edit_update(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->active && !me->committing) {
        return;
    }
    const NphWorld *w = nph_engine_edit_world(ed->engine);
    if (!nph_world_is_alive(w, me->entity)) {
        // Deleted (or undone) meanwhile: the edit goes with it.
        if (me->active) {
            me->active = false;
            snapshots_clear(&me->undo);
            snapshots_clear(&me->redo);
            cook_polymesh_free(&me->mesh);
        }
        drop_preview(ed);
        me->committing = false;
        return;
    }
    if (me->active && (nph_engine_is_playing(ed->engine) ||
                       !nph_entity_equals(ed->selection.primary, me->entity))) {
        editor_mesh_edit_end(ed); // another entity selected, or play began
    }
    if (me->active && me->preview_stale) {
        NPH_IGNORE_RESULT(rebuild_preview(ed)); // logged
    }
    NphRenderer *r = nph_engine_renderer(ed->engine);
    if (r != NULL && nph_render_instance_is_valid(r, me->preview_instance)) {
        const NphWorldTransform wt = entity_transform(ed);
        const NphRenderTransform t = render_transform_of(&wt);
        NPH_IGNORE_RESULT(nph_render_instance_set_transform(r, me->preview_instance, &t));
    }
    if (me->committing && me->commit_done) {
        const NphRenderInstance now =
            nph_scene_renderer_instance(nph_engine_scene_renderer(ed->engine), me->entity);
        const bool back = r != NULL && nph_render_instance_is_valid(r, now) &&
                          now.value != me->stale_instance.value;
        if (!me->commit_ok || !me->reloading || back) {
            hand_back(ed);
        }
    }
}

void editor_mesh_edit_free(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    if (ed->engine != NULL && (me->active || me->committing)) {
        hand_back(ed);
    }
    cook_polymesh_free(&me->mesh);
    snapshots_clear(&me->undo);
    snapshots_clear(&me->redo);
    NPH_ARRAY_FREE(&me->undo);
    NPH_ARRAY_FREE(&me->redo);
    NPH_ARRAY_FREE(&me->selected);
    NPH_ARRAY_FREE(&me->drag_vertices);
    NPH_ARRAY_FREE(&me->drag_start);
    memset(me, 0, sizeof *me);
}

void editor_mesh_edit_init(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    memset(me, 0, sizeof *me);
    cook_polymesh_init(&me->mesh, ed->alloc);
    NPH_ARRAY_INIT(&me->undo, ed->alloc);
    NPH_ARRAY_INIT(&me->redo, ed->alloc);
    NPH_ARRAY_INIT(&me->selected, ed->alloc);
    NPH_ARRAY_INIT(&me->drag_vertices, ed->alloc);
    NPH_ARRAY_INIT(&me->drag_start, ed->alloc);
}

// ---------------------------------------------------------------------------
// Changes
// ---------------------------------------------------------------------------

NphResult editor_mesh_edit_select(Editor *ed, EditorMeshSelect mode, const u32 *items, u32 count,
                                  bool add)
{
    NPH_ASSERT(ed != NULL && (items != NULL || count == 0U));
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->active) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (mode != me->mode) {
        me->mode = mode;
        NPH_ARRAY_CLEAR(&me->selected);
        add = false;
    }
    NPH_TRY(selection_fit(me));
    for (u32 i = 0; i < count; ++i) {
        if (items[i] >= me->selected.len) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    if (!add) {
        selection_clear(me);
    }
    for (u32 i = 0; i < count; ++i) {
        me->selected.data[items[i]] = 1U;
    }
    return NPH_RESULT_OK;
}

static void select_all(EditorMeshEdit *me)
{
    if (selection_fit(me) == NPH_RESULT_OK && me->selected.len != 0U) {
        memset(me->selected.data, 1, (size_t)me->selected.len);
    }
}

NphResult editor_mesh_edit_move(Editor *ed, NphVec3 offset)
{
    NPH_ASSERT(ed != NULL);
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->active) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(selected_vertices(ed, &me->drag_vertices));
    if (me->drag_vertices.len == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(push_undo(me, ed->alloc));
    for (u32 i = 0; i < me->drag_vertices.len; ++i) {
        NphVec3 *p = &me->mesh.positions.data[me->drag_vertices.data[i]];
        *p = nph_vec3_add(*p, offset);
    }
    changed(me);
    return NPH_RESULT_OK;
}

NphResult editor_mesh_edit_extrude(Editor *ed, f32 distance)
{
    NPH_ASSERT(ed != NULL);
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->active || me->mode != EDITOR_MESH_FACES) {
        return NPH_RESULT_INVALID_STATE;
    }
    NphArrayU32 faces;
    NPH_ARRAY_INIT(&faces, ed->alloc);
    NphResult r = NPH_RESULT_OK;
    for (u32 f = 0; f < me->selected.len && r == NPH_RESULT_OK; ++f) {
        if (me->selected.data[f] != 0U) {
            r = NPH_ARRAY_PUSH(&faces, f);
        }
    }
    if (r == NPH_RESULT_OK && faces.len == 0U) {
        r = NPH_RESULT_INVALID_STATE; // nothing selected
    }
    if (r == NPH_RESULT_OK) {
        r = push_undo(me, ed->alloc);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_polymesh_extrude(&me->mesh, faces.data, faces.len, distance, NULL);
        if (r != NPH_RESULT_OK) {
            // Back to the copy taken before (a failed extrusion leaves extra
            // positions behind).
            cook_polymesh_free(&me->mesh);
            me->mesh = NPH_ARRAY_POP(&me->undo);
        }
    }
    if (r == NPH_RESULT_OK) {
        // The extruded faces stay selected; the sides come after them, new
        // and not selected.
        r = NPH_ARRAY_RESIZE(&me->selected, me->mesh.faces.len);
        changed(me);
    }
    NPH_ARRAY_FREE(&faces);
    return r;
}

// The distance an extrusion goes when the editor picks it: a quarter of
// the selected faces' mean size.
static f32 extrude_distance(const EditorMeshEdit *me)
{
    f32 area = 0.0F;
    u32 n = 0U;
    for (u32 f = 0; f < me->selected.len; ++f) {
        if (me->selected.data[f] != 0U) {
            area += cook_polymesh_face_area(&me->mesh, f);
            ++n;
        }
    }
    return n != 0U ? 0.25F * nph_f32_sqrt(area / (f32)n) : 0.0F;
}

static bool swap_history(EditorMeshEdit *me, EditorMeshEditSnapshots *from,
                         EditorMeshEditSnapshots *to)
{
    if (!me->active || from->len == 0U) {
        return false;
    }
    if (NPH_ARRAY_PUSH(to, me->mesh) != NPH_RESULT_OK) {
        return false;
    }
    me->mesh = NPH_ARRAY_POP(from);
    NPH_IGNORE_RESULT(selection_fit(me)); // cleared when the elements changed
    changed(me);
    return true;
}

bool editor_mesh_edit_undo(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return swap_history(&ed->mesh_edit, &ed->mesh_edit.undo, &ed->mesh_edit.redo);
}

bool editor_mesh_edit_redo(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return swap_history(&ed->mesh_edit, &ed->mesh_edit.redo, &ed->mesh_edit.undo);
}

// ---------------------------------------------------------------------------
// The gizmo on the selection
// ---------------------------------------------------------------------------

static bool drag_begin(Editor *ed, void *user)
{
    NPH_UNUSED(user);
    EditorMeshEdit *me = &ed->mesh_edit;
    if (selected_vertices(ed, &me->drag_vertices) != NPH_RESULT_OK || me->drag_vertices.len == 0U ||
        push_undo(me, ed->alloc) != NPH_RESULT_OK) {
        return false;
    }
    NPH_ARRAY_CLEAR(&me->drag_start);
    NphVec3 sum = nph_vec3_zero();
    for (u32 i = 0; i < me->drag_vertices.len; ++i) {
        const NphVec3 p = me->mesh.positions.data[me->drag_vertices.data[i]];
        if (NPH_ARRAY_PUSH(&me->drag_start, p) != NPH_RESULT_OK) {
            return false;
        }
        sum = nph_vec3_add(sum, p);
    }
    me->drag_center = nph_vec3_scale(sum, 1.0F / (f32)me->drag_vertices.len);
    return true;
}

// The gizmo stands on the selection's centre with the entity's axes (its
// start transform's linear part is the entity's), so a vertex at `l` goes
// to c + E^-1 dt + E^-1 L (l - c): E the entity's linear part, L the
// gizmo's now, dt how far its pivot moved.
static void drag_apply(Editor *ed, const NphWorldTransform *start, const NphWorldTransform *moved,
                       void *user)
{
    NPH_UNUSED(user);
    EditorMeshEdit *me = &ed->mesh_edit;
    const NphWorldTransform wt = entity_transform(ed);
    NphMat3 inverse;
    if (!nph_mat3_inverse(nph_world_transform_linear(&wt), &inverse)) {
        return; // a degenerate entity: its space has no inverse
    }
    const NphMat3 m = nph_mat3_mul(inverse, nph_world_transform_linear(moved));
    const NphVec3 dt =
        nph_mat3_mul_vec3(inverse, nph_dvec3_relative(moved->translation, start->translation));
    const NphVec3 c = nph_vec3_add(me->drag_center, dt);
    for (u32 i = 0; i < me->drag_vertices.len; ++i) {
        const NphVec3 l = me->drag_start.data[i];
        me->mesh.positions.data[me->drag_vertices.data[i]] =
            nph_vec3_add(c, nph_mat3_mul_vec3(m, nph_vec3_sub(l, me->drag_center)));
    }
    changed(me);
}

static void drag_end(Editor *ed, void *user)
{
    NPH_UNUSED(user);
    NPH_ARRAY_CLEAR(&ed->mesh_edit.drag_vertices);
    NPH_ARRAY_CLEAR(&ed->mesh_edit.drag_start);
}

// ---------------------------------------------------------------------------
// Picking elements
// ---------------------------------------------------------------------------

// Möller & Trumbore, "Fast, Minimum Storage Ray/Triangle Intersection",
// JGT 1997: the ray parameter of the hit, or false.
static bool ray_triangle(NphVec3 o, NphVec3 d, NphVec3 a, NphVec3 b, NphVec3 c, f32 *out_t)
{
    const NphVec3 e1 = nph_vec3_sub(b, a);
    const NphVec3 e2 = nph_vec3_sub(c, a);
    const NphVec3 p = nph_vec3_cross(d, e2);
    const f32 det = nph_vec3_dot(e1, p);
    if (nph_f32_abs(det) < 1e-12F) {
        return false;
    }
    const f32 inv = 1.0F / det;
    const NphVec3 s = nph_vec3_sub(o, a);
    const f32 u = nph_vec3_dot(s, p) * inv;
    if (u < 0.0F || u > 1.0F) {
        return false;
    }
    const NphVec3 q = nph_vec3_cross(s, e1);
    const f32 v = nph_vec3_dot(d, q) * inv;
    if (v < 0.0F || u + v > 1.0F) {
        return false;
    }
    const f32 t = nph_vec3_dot(e2, q) * inv;
    if (t <= 0.0F) {
        return false;
    }
    *out_t = t;
    return true;
}

// The element under (mx, my): the vertex nearest on screen within
// PICK_PIXELS, or the face the ray meets first.
static bool pick_element(Editor *ed, const EditorView *v, f32 mx, f32 my, u32 *out)
{
    const EditorMeshEdit *me = &ed->mesh_edit;
    const NphWorldTransform wt = entity_transform(ed);
    if (me->mode == EDITOR_MESH_VERTICES) {
        f32 best = PICK_PIXELS * PICK_PIXELS;
        bool any = false;
        for (u32 i = 0; i < me->mesh.positions.len; ++i) {
            f32 sx = 0.0F;
            f32 sy = 0.0F;
            if (!editor_view_project(v, nph_world_transform_point(&wt, me->mesh.positions.data[i]),
                                     &sx, &sy)) {
                continue;
            }
            const f32 d2 = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
            if (d2 <= best) {
                best = d2;
                *out = i;
                any = true;
            }
        }
        return any;
    }
    // The ray in the entity's space.
    NphMat3 inverse;
    if (!nph_mat3_inverse(nph_world_transform_linear(&wt), &inverse)) {
        return false;
    }
    const NphVec3 o = nph_mat3_mul_vec3(inverse, nph_dvec3_relative(v->eye, wt.translation));
    const NphVec3 d = nph_mat3_mul_vec3(inverse, editor_view_ray(v, mx, my));
    u32 tri[(COOK_POLYMESH_MAX_FACE_CORNERS - 2U) * 3U];
    f32 nearest = 0.0F;
    bool any = false;
    for (u32 f = 0; f < me->mesh.faces.len; ++f) {
        const CookPolyFace *face = &me->mesh.faces.data[f];
        const u32 n = cook_polymesh_triangulate_face(&me->mesh, f, tri);
        for (u32 t = 0; t < n; ++t) {
            const NphVec3 a =
                me->mesh.positions.data[me->mesh.corners.data[face->first + tri[(u64)t * 3U]]];
            const NphVec3 b =
                me->mesh.positions.data[me->mesh.corners.data[face->first + tri[(u64)t * 3U + 1U]]];
            const NphVec3 c =
                me->mesh.positions.data[me->mesh.corners.data[face->first + tri[(u64)t * 3U + 2U]]];
            f32 at = 0.0F;
            if (ray_triangle(o, d, a, b, c, &at) && (!any || at < nearest)) {
                nearest = at;
                *out = f;
                any = true;
            }
        }
    }
    return any;
}

bool editor_mesh_edit_viewport(Editor *ed, const EditorView *v, bool hovered, ImDrawList *dl)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    if (!me->active) {
        return false;
    }
    // The gizmo stands on the selection's centre as it is now (a drag in
    // progress keeps the vertices it began with), when there is one.
    bool gizmo = false;
    if (!ed->gizmo_dragging && selected_vertices(ed, &me->drag_vertices) != NPH_RESULT_OK) {
        NPH_ARRAY_CLEAR(&me->drag_vertices);
    }
    if (me->drag_vertices.len != 0U) {
        NphVec3 sum = nph_vec3_zero();
        for (u32 i = 0; i < me->drag_vertices.len; ++i) {
            sum = nph_vec3_add(sum, me->mesh.positions.data[me->drag_vertices.data[i]]);
        }
        const NphVec3 center = nph_vec3_scale(sum, 1.0F / (f32)me->drag_vertices.len);
        EditorGizmoTarget target;
        memset(&target, 0, sizeof target);
        target.transform = entity_transform(ed);
        target.transform.translation = nph_world_transform_point(&target.transform, center);
        target.id = MESH_GIZMO_ID ^ me->entity.value;
        target.begin = drag_begin;
        target.apply = drag_apply;
        target.end = drag_end;
        gizmo = editor_gizmo_drive(ed, v, hovered, dl, &target);
    } else {
        gizmo = editor_gizmo_drive(ed, v, hovered, dl, NULL);
    }
    // A click picks an element; Ctrl adds or removes it; empty space clears.
    const ImGuiIO *io = igGetIO_Nil();
    if (hovered && !gizmo && !io->KeyAlt && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        u32 element = 0U;
        const bool any = pick_element(ed, v, io->MousePos.x, io->MousePos.y, &element);
        if (selection_fit(me) == NPH_RESULT_OK) {
            if (io->KeyCtrl) {
                if (any) {
                    me->selected.data[element] ^= 1U;
                }
            } else {
                selection_clear(me);
                if (any) {
                    me->selected.data[element] = 1U;
                }
            }
        }
    }
    return gizmo;
}

void editor_mesh_edit_overlays(Editor *ed, const EditorView *v)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    NphRenderer *r = nph_engine_renderer(ed->engine);
    if (!me->active || r == NULL) {
        return;
    }
    const NphDVec3 origin = nph_render_debug_origin(r);
    const NphWorldTransform wt = entity_transform(ed);
    const bool faces = me->mode == EDITOR_MESH_FACES;
    for (u32 f = 0; f < me->mesh.faces.len; ++f) {
        const CookPolyFace *face = &me->mesh.faces.data[f];
        const bool picked = faces && f < me->selected.len && me->selected.data[f] != 0U;
        for (u32 i = 0; i < face->count; ++i) {
            const NphVec3 a = me->mesh.positions.data[me->mesh.corners.data[face->first + i]];
            const NphVec3 b =
                me->mesh.positions
                    .data[me->mesh.corners.data[face->first + (i + 1U) % face->count]];
            (void)nph_render_debug_line(
                r, nph_dvec3_relative(nph_world_transform_point(&wt, a), origin),
                nph_dvec3_relative(nph_world_transform_point(&wt, b), origin),
                picked ? COLOR_PICKED : COLOR_EDGE, NPH_RENDER_DEBUG_OVERLAY);
        }
        if (faces) {
            const NphDVec3 c =
                nph_world_transform_point(&wt, cook_polymesh_face_center(&me->mesh, f));
            const f32 size = 4.0F * editor_view_world_per_unit(v, editor_view_depth(v, c));
            nph_render_debug_cross(r, nph_dvec3_relative(c, origin), size,
                                   picked ? COLOR_PICKED : COLOR_VERTEX, NPH_RENDER_DEBUG_OVERLAY);
        }
    }
    if (!faces) {
        for (u32 i = 0; i < me->mesh.positions.len; ++i) {
            const NphDVec3 p = nph_world_transform_point(&wt, me->mesh.positions.data[i]);
            const bool picked = i < me->selected.len && me->selected.data[i] != 0U;
            const f32 size = 5.0F * editor_view_world_per_unit(v, editor_view_depth(v, p));
            nph_render_debug_cross(r, nph_dvec3_relative(p, origin), size,
                                   picked ? COLOR_PICKED : COLOR_VERTEX, NPH_RENDER_DEBUG_OVERLAY);
        }
    }
}

void editor_mesh_edit_toolbar(Editor *ed)
{
    EditorMeshEdit *me = &ed->mesh_edit;
    igTextUnformatted(me->name, NULL);
    igSameLine(0.0F, 12.0F);
    static const char *const modes[2] = {"Vertices", "Faces"};
    for (u32 i = 0; i < 2U; ++i) {
        const bool on = (u32)me->mode == i;
        if (on) {
            igPushStyleColor_U32(ImGuiCol_Button, igGetColorU32_Col(ImGuiCol_ButtonActive, 1.0F));
        }
        if (igButton(modes[i], ui_vec2(0.0F, 0.0F)) && !on) {
            NPH_IGNORE_RESULT(editor_mesh_edit_select(ed, (EditorMeshSelect)i, NULL, 0U, false));
        }
        if (on) {
            igPopStyleColor(1);
        }
        igSameLine(0.0F, -1.0F);
    }
    igBeginDisabled(me->mode != EDITOR_MESH_FACES);
    if (igButton("Extrude", ui_vec2(0.0F, 0.0F))) {
        editor_mesh_edit_extrude_selection(ed);
    }
    igEndDisabled();
    igSameLine(0.0F, -1.0F);
    if (igButton("Done", ui_vec2(0.0F, 0.0F))) {
        editor_mesh_edit_end(ed);
    }
    igSameLine(0.0F, 16.0F);
    igTextDisabled("1 2: vertices faces  Ctrl+E: extrude  Ctrl+A: all  Tab: done");
}

void editor_mesh_edit_extrude_selection(Editor *ed)
{
    const NphResult r = editor_mesh_edit_extrude(ed, extrude_distance(&ed->mesh_edit));
    if (r != NPH_RESULT_OK && r != NPH_RESULT_INVALID_STATE) {
        NPH_LOG_ERROR(&LOG_EDITOR, "extruding: %s", nph_result_string(r));
    }
}

void editor_mesh_edit_select_all(Editor *ed)
{
    if (ed->mesh_edit.active) {
        select_all(&ed->mesh_edit);
    }
}

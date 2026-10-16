// SPDX-License-Identifier: Apache-2.0
// The outliner (docs/editor.md "Outliner"): the world's entities as a tree
// by their hierarchy, in slot order.
//   - A click selects (Ctrl adds or removes); a double-click frames the
//     entity in the viewport.
//   - Dragging an entity onto another makes it a child, keeping where it is
//     in the world; dropped on the empty space below, it becomes a root.
//   - F2, or Rename in the context menu, renames in place. The context menu
//     also creates children, duplicates, and deletes.
//   - A filter shows the entities whose name contains it, as a flat list.
// While playing it shows the play world and does not change it.
#include <nepheline/core/assert.h>

#include "editor_internal.h"

#include <ctype.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

static void select_from_click(Editor *ed, NphEntity e)
{
    if (igGetIO_Nil()->KeyCtrl) {
        editor_selection_toggle(&ed->selection, e);
    } else {
        editor_selection_set(&ed->selection, e);
    }
}

static void report(NphResult r, const char *what)
{
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "%s: %s", what, nph_result_string(r));
    }
}

void editor_begin_rename(Editor *ed, NphEntity e)
{
    const NphWorld *w = nph_engine_edit_world(ed->engine);
    if (nph_engine_is_playing(ed->engine) || !nph_world_is_alive(w, e)) {
        return;
    }
    const NphName *name = NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_NAME, NphName);
    (void)snprintf(ed->rename_buf, sizeof ed->rename_buf, "%s",
                   name != NULL ? name->text.text : "");
    ed->renaming = e;
    ed->rename_focus = true;
}

static void rename_field(Editor *ed, NphEntity e)
{
    igSetNextItemWidth(-FLT_MIN);
    if (ed->rename_focus) {
        igSetKeyboardFocusHere(0);
        ed->rename_focus = false;
    }
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (igInputText("##rename", ed->rename_buf, sizeof ed->rename_buf, flags, NULL, NULL)) {
        report(editor_edit_rename(ed->history, e, ed->rename_buf), "rename");
        ed->renaming = NPH_ENTITY_NULL;
    } else if (igIsItemDeactivated() || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        ed->renaming = NPH_ENTITY_NULL; // clicked away: cancelled
    }
}

static void context_menu(Editor *ed, NphEntity e, bool editable)
{
    if (!igBeginPopupContextItem(NULL, ImGuiPopupFlags_MouseButtonRight)) {
        return;
    }
    if (!editor_selection_contains(&ed->selection, e)) {
        editor_selection_set(&ed->selection, e);
    }
    igBeginDisabled(!editable);
    if (igMenuItem_Bool("Create Child", NULL, false, true)) {
        NphEntity child = NPH_ENTITY_NULL;
        report(editor_edit_create(ed->history, "Entity", e, NULL, &child), "create");
        editor_selection_set(&ed->selection, child);
    }
    if (igMenuItem_Bool("Rename", "F2", false, true)) {
        editor_begin_rename(ed, e);
    }
    if (igMenuItem_Bool("Duplicate", "Ctrl+D", false, true)) {
        editor_duplicate_selection(ed);
    }
    if (igMenuItem_Bool("Delete", "Del", false, true)) {
        editor_delete_selection(ed);
    }
    igEndDisabled();
    igSeparator();
    if (igMenuItem_Bool("Frame", "F", false, true)) {
        editor_viewport_frame_selection(ed);
    }
    igEndPopup();
}

static void drag_and_drop(Editor *ed, NphEntity e, const char *label, bool editable)
{
    if (igBeginDragDropSource(ImGuiDragDropFlags_None)) {
        (void)igSetDragDropPayload(EDITOR_DRAG_ENTITY, &e, sizeof e, ImGuiCond_None);
        igTextUnformatted(label, NULL);
        igEndDragDropSource();
    }
    if (editable && igBeginDragDropTarget()) {
        const ImGuiPayload *p =
            igAcceptDragDropPayload(EDITOR_DRAG_ENTITY, ImGuiDragDropFlags_None);
        if (p != NULL && p->DataSize == (int)sizeof(NphEntity)) {
            NphEntity dragged;
            memcpy(&dragged, p->Data, sizeof dragged);
            const NphResult r = editor_edit_reparent(ed->history, dragged, e);
            if (r == NPH_RESULT_INVALID_ARGUMENT) {
                NPH_LOG_WARN(&LOG_EDITOR, "an entity cannot go under itself");
            } else {
                report(r, "reparent");
            }
        }
        igEndDragDropTarget();
    }
}

static void node(Editor *ed, const NphWorld *w, NphEntity e, bool editable)
{
    char label[96];
    editor_entity_label(w, e, label, sizeof label);
    igPushID_Int((int)e.value);
    if (nph_entity_equals(ed->renaming, e)) {
        rename_field(ed, e);
        igPopID();
        return;
    }
    const bool leaf = nph_entity_is_null(nph_world_first_child(w, e));
    const NphEntity primary = ed->selection.primary;
    if (ed->outliner_reveal && !leaf && !nph_entity_equals(primary, e) &&
        editor_is_under(w, primary, e)) {
        igSetNextItemOpen(true, ImGuiCond_Always); // down to a newly selected entity
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (editor_selection_contains(&ed->selection, e)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const bool open = igTreeNodeEx_StrStr("node", flags, "%s", label);
    if (igIsItemClicked(ImGuiMouseButton_Left) && !igIsItemToggledOpen()) {
        select_from_click(ed, e);
    }
    if (igIsItemHovered(ImGuiHoveredFlags_None) &&
        igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
        editor_selection_set(&ed->selection, e);
        editor_viewport_frame_selection(ed);
    }
    drag_and_drop(ed, e, label, editable);
    context_menu(ed, e, editable);
    if (open) {
        for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
             c = nph_world_next_sibling(w, c)) {
            node(ed, w, c, editable);
        }
        igTreePop();
    }
    igPopID();
}

// Case-insensitive substring.
static bool contains(const char *text, const char *part)
{
    const u64 n = strlen(part);
    for (const char *t = text; *t != '\0'; ++t) {
        u64 i = 0U;
        while (i < n && t[i] != '\0' &&
               tolower((unsigned char)t[i]) == tolower((unsigned char)part[i])) {
            ++i;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0U;
}

static void flat_list(Editor *ed, const NphWorld *w, bool editable)
{
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        if (nph_entity_is_null(e)) {
            continue;
        }
        char label[96];
        editor_entity_label(w, e, label, sizeof label);
        if (!contains(label, ed->outliner_filter)) {
            continue;
        }
        igPushID_Int((int)e.value);
        if (igSelectable_Bool(label, editor_selection_contains(&ed->selection, e),
                              ImGuiSelectableFlags_None, ui_vec2(0.0F, 0.0F))) {
            select_from_click(ed, e);
        }
        drag_and_drop(ed, e, label, editable);
        context_menu(ed, e, editable);
        igPopID();
    }
}

void editor_panel_outliner(Editor *ed)
{
    if (!igBegin(EDITOR_WINDOW_OUTLINER, &ed->show.outliner, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    NphEngine *engine = ed->engine;
    const bool editable = !nph_engine_is_playing(engine);
    const NphWorld *w = nph_engine_world(engine);
    igBeginDisabled(!editable);
    if (igSmallButton("+ Entity")) {
        NphEntity e = NPH_ENTITY_NULL;
        report(editor_edit_create(ed->history, "Entity", NPH_ENTITY_NULL, NULL, &e), "create");
        editor_selection_set(&ed->selection, e);
    }
    igEndDisabled();
    igSameLine(0.0F, -1.0F);
    igSetNextItemWidth(-FLT_MIN);
    (void)igInputText("##filter", ed->outliner_filter, sizeof ed->outliner_filter,
                      ImGuiInputTextFlags_None, NULL, NULL);
    if (!editable) {
        igTextDisabled("playing: the play copy, read only");
    }
    if (igBeginChild_Str("##tree", ui_vec2(0.0F, 0.0F), ImGuiChildFlags_None,
                         ImGuiWindowFlags_None)) {
        ed->outliner_reveal = !nph_entity_equals(ed->selection.primary, ed->outliner_primary) &&
                              nph_world_is_alive(w, ed->selection.primary);
        ed->outliner_primary = ed->selection.primary;
        if (ed->outliner_filter[0] != '\0') {
            flat_list(ed, w, editable);
        } else {
            for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
                const NphEntity e = nph_world_entity_at(w, i);
                if (!nph_entity_is_null(e) && nph_entity_is_null(nph_world_parent(w, e))) {
                    node(ed, w, e, editable);
                }
            }
        }
        // A click on the empty space clears the selection; its context menu
        // creates a root.
        if (igIsWindowHovered(ImGuiHoveredFlags_None) &&
            igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !igIsAnyItemHovered()) {
            editor_selection_clear(&ed->selection);
        }
        if (editable &&
            igBeginPopupContextWindow("##outliner", ImGuiPopupFlags_MouseButtonRight |
                                                        ImGuiPopupFlags_NoOpenOverItems)) {
            if (igMenuItem_Bool("Create Entity", NULL, false, true)) {
                NphEntity e = NPH_ENTITY_NULL;
                report(editor_edit_create(ed->history, "Entity", NPH_ENTITY_NULL, NULL, &e),
                       "create");
                editor_selection_set(&ed->selection, e);
            }
            igEndPopup();
        }
    }
    igEndChild();
    // Dropped on the empty space: a root.
    if (editable && igBeginDragDropTarget()) {
        const ImGuiPayload *p =
            igAcceptDragDropPayload(EDITOR_DRAG_ENTITY, ImGuiDragDropFlags_None);
        if (p != NULL && p->DataSize == (int)sizeof(NphEntity)) {
            NphEntity dragged;
            memcpy(&dragged, p->Data, sizeof dragged);
            report(editor_edit_reparent(ed->history, dragged, NPH_ENTITY_NULL), "reparent");
        }
        igEndDragDropTarget();
    }
    igEnd();
}

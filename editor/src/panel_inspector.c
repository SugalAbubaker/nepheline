// SPDX-License-Identifier: Apache-2.0
// The inspector (docs/editor.md "Inspector"; CLAUDE.md §8.13 "Inspector
// (driven by ECS reflection metadata)"): the primary selection's
// components, each field edited by its kind and its hints
// (scene/component.h):
//   - numbers are dragged, within RANGE bounds;
//   - ANGLE fields are shown in degrees;
//   - COLOR vectors use a colour editor;
//   - rotations are edited as Euler angles;
//   - TEXT, string ids, and GUIDs are typed;
//   - entity references take an entity dropped from the outliner.
// READONLY fields are shown greyed and HIDDEN ones not at all.
//
// While editing, a change goes through the history: a drag, or typing into
// a field, is one undoable step (merged by the widget). While playing, the
// play copy is changed directly; those changes go with it when play stops.
#include <nepheline/core/assert.h>
#include <nepheline/core/string_id.h>
#include <nepheline/math/quat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/scene/component.h>

#include "editor_internal.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

// U+00B0, in UTF-8: the default font has it.
#define DEGREE "\xC2\xB0"

static ImGuiDataType data_type(NphFieldKind kind)
{
    switch (kind) {
    case NPH_FIELD_U8:
        return ImGuiDataType_U8;
    case NPH_FIELD_U16:
        return ImGuiDataType_U16;
    case NPH_FIELD_U32:
        return ImGuiDataType_U32;
    case NPH_FIELD_U64:
        return ImGuiDataType_U64;
    case NPH_FIELD_I8:
        return ImGuiDataType_S8;
    case NPH_FIELD_I16:
        return ImGuiDataType_S16;
    case NPH_FIELD_I32:
        return ImGuiDataType_S32;
    case NPH_FIELD_I64:
        return ImGuiDataType_S64;
    case NPH_FIELD_F64:
    case NPH_FIELD_DVEC3:
        return ImGuiDataType_Double;
    default:
        return ImGuiDataType_Float;
    }
}

// An integer bound (from the field's f32 hints) in the field's own type.
static void int_bound(NphFieldKind kind, f32 value, void *out)
{
    switch (kind) {
    case NPH_FIELD_U8: {
        const u8 v = (u8)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    case NPH_FIELD_U16: {
        const u16 v = (u16)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    case NPH_FIELD_U32: {
        const u32 v = (u32)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    case NPH_FIELD_U64: {
        const u64 v = (u64)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    case NPH_FIELD_I8: {
        const i8 v = (i8)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    case NPH_FIELD_I16: {
        const i16 v = (i16)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    case NPH_FIELD_I32: {
        const i32 v = (i32)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    default: {
        const i64 v = (i64)value;
        memcpy(out, &v, sizeof v);
        break;
    }
    }
}

typedef struct FieldEdit {
    bool changed;  // the widget changed the value this frame
    bool finished; // the edit ended (the widget was released or left)
    u64 key;       // the widget, to merge a drag into one step
} FieldEdit;

static void note_widget(FieldEdit *edit, bool changed)
{
    if (changed) {
        edit->changed = true;
        edit->key = (u64)igGetItemID();
    }
    if (igIsItemDeactivatedAfterEdit()) {
        edit->finished = true;
    }
}

static void entity_field(Editor *ed, const NphWorld *w, const NphFieldDesc *f, u8 *value,
                         FieldEdit *edit)
{
    NphEntity ref;
    memcpy(&ref, value + f->offset, sizeof ref);
    char label[96];
    if (nph_entity_is_null(ref)) {
        (void)snprintf(label, sizeof label, "(none)");
    } else {
        editor_entity_label(w, ref, label, sizeof label);
    }
    (void)igButton(label, ui_vec2(-FLT_MIN, 0.0F));
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("drop an entity from the outliner here; right-click to clear");
    }
    bool changed = false;
    if (igBeginDragDropTarget()) {
        const ImGuiPayload *p =
            igAcceptDragDropPayload(EDITOR_DRAG_ENTITY, ImGuiDragDropFlags_None);
        if (p != NULL && p->DataSize == (int)sizeof(NphEntity)) {
            memcpy(value + f->offset, p->Data, sizeof(NphEntity));
            changed = true;
        }
        igEndDragDropTarget();
    }
    if (igIsItemClicked(ImGuiMouseButton_Right) && !nph_entity_is_null(ref)) {
        const NphEntity none = NPH_ENTITY_NULL;
        memcpy(value + f->offset, &none, sizeof none);
        changed = true;
    }
    if (igIsItemClicked(ImGuiMouseButton_Left) && !nph_entity_is_null(ref)) {
        editor_selection_set(&ed->selection, ref);
    }
    if (changed) {
        edit->changed = true;
        edit->key = (u64)igGetItemID();
        edit->finished = true;
    }
}

static void text_field(u8 *value, const NphFieldDesc *f, FieldEdit *edit)
{
    NphText t;
    memcpy(&t, value + f->offset, sizeof t);
    char buf[NPH_TEXT_CAPACITY];
    memcpy(buf, t.text, sizeof buf);
    buf[sizeof buf - 1U] = '\0';
    igSetNextItemWidth(-FLT_MIN);
    const bool changed = igInputText("##v", buf, sizeof buf, ImGuiInputTextFlags_None, NULL, NULL);
    if (changed) {
        (void)nph_text_set(&t, nph_str_from_cstr(buf));
        memcpy(value + f->offset, &t, sizeof t);
    }
    note_widget(edit, changed);
}

static void str_id_field(NphWorld *w, u8 *value, const NphFieldDesc *f, FieldEdit *edit)
{
    NphStrId id;
    memcpy(&id, value + f->offset, sizeof id);
    char buf[128];
    const NphStr known = nph_str_table_lookup(nph_world_strings(w), id);
    if (known.data != NULL) {
        (void)snprintf(buf, sizeof buf, "%.*s", (int)known.len, known.data);
    } else {
        (void)snprintf(buf, sizeof buf, "#%016llx", (unsigned long long)id.value);
    }
    igSetNextItemWidth(-FLT_MIN);
    if (igInputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
        NphStrId interned;
        if (nph_str_table_intern(nph_world_strings(w), nph_str_from_cstr(buf), &interned) ==
            NPH_RESULT_OK) {
            memcpy(value + f->offset, &interned, sizeof interned);
            edit->changed = true;
            edit->key = (u64)igGetItemID();
            edit->finished = true;
        }
    }
}

static void guid_field(u8 *value, const NphFieldDesc *f, FieldEdit *edit)
{
    NphGuid g;
    memcpy(&g, value + f->offset, sizeof g);
    char buf[NPH_GUID_STRING_LEN + 1U];
    if (nph_guid_is_null(g)) {
        buf[0] = '\0';
    } else {
        nph_guid_to_string(g, buf);
    }
    igSetNextItemWidth(-FLT_MIN);
    if (igInputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
        NphGuid parsed = NPH_GUID_NULL;
        if (buf[0] == '\0' || nph_guid_parse(nph_str_from_cstr(buf), &parsed)) {
            memcpy(value + f->offset, &parsed, sizeof parsed);
            edit->changed = true;
            edit->key = (u64)igGetItemID();
            edit->finished = true;
        }
    }
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("an asset's GUID: 32 hexadecimal digits (empty: none)");
    }
}

// One field of `value` (a copy of the component); widgets under the field's id.
static void field(Editor *ed, NphWorld *w, const NphFieldDesc *f, u8 *value, FieldEdit *edit)
{
    const NphFieldKind kind = (NphFieldKind)f->kind;
    const bool ranged = (f->flags & (u32)NPH_FIELD_FLAG_RANGE) != 0U;
    const bool angle = (f->flags & (u32)NPH_FIELD_FLAG_ANGLE) != 0U;
    const bool color = (f->flags & (u32)NPH_FIELD_FLAG_COLOR) != 0U;
    u8 *p = value + f->offset;
    igSetNextItemWidth(-FLT_MIN);
    switch (kind) {
    case NPH_FIELD_BOOL: {
        b32 b = 0;
        memcpy(&b, p, sizeof b);
        bool v = b != 0;
        const bool changed = igCheckbox("##v", &v);
        if (changed) {
            b = v ? 1 : 0;
            memcpy(p, &b, sizeof b);
        }
        note_widget(edit, changed);
        break;
    }
    case NPH_FIELD_U8:
    case NPH_FIELD_U16:
    case NPH_FIELD_U32:
    case NPH_FIELD_U64:
    case NPH_FIELD_I8:
    case NPH_FIELD_I16:
    case NPH_FIELD_I32:
    case NPH_FIELD_I64: {
        u64 lo = 0U;
        u64 hi = 0U;
        int_bound(kind, f->min, &lo);
        int_bound(kind, f->max, &hi);
        note_widget(edit, igDragScalar("##v", data_type(kind), p, 0.2F, ranged ? &lo : NULL,
                                       ranged ? &hi : NULL, NULL,
                                       ranged ? ImGuiSliderFlags_AlwaysClamp : 0));
        break;
    }
    case NPH_FIELD_F32: {
        f32 v = 0.0F;
        memcpy(&v, p, sizeof v);
        const f32 k = angle ? NPH_RAD_TO_DEG : 1.0F;
        f32 shown = v * k;
        const f32 lo = f->min * k;
        const f32 hi = f->max * k;
        const f32 speed = ranged && hi > lo ? (hi - lo) * 0.002F : 0.01F;
        const bool changed = igDragScalar(
            "##v", ImGuiDataType_Float, &shown, speed, ranged ? &lo : NULL, ranged ? &hi : NULL,
            angle ? "%.2f" DEGREE : "%.4g", ranged ? ImGuiSliderFlags_AlwaysClamp : 0);
        if (changed) {
            v = shown / k;
            memcpy(p, &v, sizeof v);
        }
        note_widget(edit, changed);
        break;
    }
    case NPH_FIELD_F64:
        note_widget(edit, igDragScalar("##v", ImGuiDataType_Double, p, 0.01F, NULL, NULL, "%.6g",
                                       ImGuiSliderFlags_None));
        break;
    case NPH_FIELD_VEC2:
    case NPH_FIELD_VEC3:
    case NPH_FIELD_VEC4: {
        const int n = kind == NPH_FIELD_VEC2 ? 2 : (kind == NPH_FIELD_VEC3 ? 3 : 4);
        f32 v[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        memcpy(v, p, sizeof(f32) * (u32)n);
        bool changed = false;
        const ImGuiColorEditFlags cflags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
        if (color && n == 3) {
            changed = igColorEdit3("##v", v, cflags);
        } else if (color && n == 4) {
            changed = igColorEdit4("##v", v, cflags);
        } else {
            changed = igDragScalarN("##v", ImGuiDataType_Float, v, n, 0.01F, NULL, NULL, "%.3f",
                                    ImGuiSliderFlags_None);
        }
        if (changed) {
            memcpy(p, v, sizeof(f32) * (u32)n);
        }
        note_widget(edit, changed);
        break;
    }
    case NPH_FIELD_QUAT: {
        NphQuat q;
        memcpy(&q, p, sizeof q);
        const NphVec3 euler = nph_quat_to_euler(nph_quat_normalize_or_identity(q));
        f32 deg[3] = {euler.x * NPH_RAD_TO_DEG, euler.y * NPH_RAD_TO_DEG, euler.z * NPH_RAD_TO_DEG};
        const bool changed = igDragScalarN("##v", ImGuiDataType_Float, deg, 3, 0.5F, NULL, NULL,
                                           "%.1f" DEGREE, ImGuiSliderFlags_None);
        if (changed) {
            q = nph_quat_from_euler(nph_vec3(deg[0] * NPH_DEG_TO_RAD, deg[1] * NPH_DEG_TO_RAD,
                                             deg[2] * NPH_DEG_TO_RAD));
            memcpy(p, &q, sizeof q);
        }
        note_widget(edit, changed);
        break;
    }
    case NPH_FIELD_DVEC3:
        note_widget(edit, igDragScalarN("##v", ImGuiDataType_Double, p, 3, 0.01F, NULL, NULL,
                                        "%.3f", ImGuiSliderFlags_None));
        break;
    case NPH_FIELD_ENTITY:
        entity_field(ed, w, f, value, edit);
        break;
    case NPH_FIELD_STR_ID:
        str_id_field(w, value, f, edit);
        break;
    case NPH_FIELD_TEXT:
        text_field(value, f, edit);
        break;
    case NPH_FIELD_GUID:
        guid_field(value, f, edit);
        break;
    default:
        igTextDisabled("(%s)", nph_field_kind_name(kind));
        break;
    }
}

// Applies an edited component: through the history while editing, straight
// into the play copy while playing.
static void apply(Editor *ed, NphWorld *w, NphEntity e, NphComponentId id, const u8 *value,
                  const FieldEdit *edit, const char *component)
{
    NphResult r = NPH_RESULT_OK;
    if (nph_engine_is_playing(ed->engine)) {
        r = nph_world_set(w, e, id, value);
    } else {
        char label[EDITOR_HISTORY_LABEL_MAX];
        (void)snprintf(label, sizeof label, "Edit %s", component);
        r = editor_history_set_merged(ed->history, e, id, value, edit->key ^ ((u64)e.value << 32U),
                                      label);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "editing %s: %s", component, nph_result_string(r));
    }
}

static void component(Editor *ed, NphWorld *w, NphEntity e, NphComponentId id)
{
    const NphComponentDesc *d = nph_world_component_desc(w, id);
    if (d == NULL || (d->flags & (u32)NPH_COMPONENT_FLAG_INTERNAL) != 0U) {
        return;
    }
    const bool playing = nph_engine_is_playing(ed->engine);
    // WorldTransform is the transform system's; everything else can go.
    bool keep = true;
    const bool removable = id != NPH_COMPONENT_ID_WORLD_TRANSFORM && !playing;
    igPushID_Int((int)id);
    const bool open = igCollapsingHeader_BoolPtr(d->name, removable ? &keep : NULL,
                                                 ImGuiTreeNodeFlags_DefaultOpen);
    if (!keep) {
        const NphResult r = editor_edit_remove_component(ed->history, e, id);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_EDITOR, "removing %s: %s", d->name, nph_result_string(r));
        }
        igPopID();
        return;
    }
    if (!open || d->size == 0U) {
        if (open) {
            igTextDisabled("a tag: no fields");
        }
        igPopID();
        return;
    }
    u8 value[NPH_ECS_MAX_COMPONENT_SIZE];
    memcpy(value, nph_world_get(w, e, id), d->size);
    FieldEdit edit;
    memset(&edit, 0, sizeof edit);
    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
    if (igBeginTable("##fields", 2, flags, ui_vec2(0.0F, 0.0F), 0.0F)) {
        igTableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch, 0.35F, 0U);
        igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.65F, 0U);
        for (u32 i = 0; i < d->field_count; ++i) {
            const NphFieldDesc *f = &d->fields[i];
            if ((f->flags & (u32)NPH_FIELD_FLAG_HIDDEN) != 0U) {
                continue;
            }
            igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
            (void)igTableNextColumn();
            igAlignTextToFramePadding();
            igTextUnformatted(f->name, NULL);
            (void)igTableNextColumn();
            igPushID_Int((int)i);
            igBeginDisabled((f->flags & (u32)NPH_FIELD_FLAG_READONLY) != 0U);
            field(ed, w, f, value, &edit);
            igEndDisabled();
            igPopID();
        }
        igEndTable();
    }
    if (edit.changed) {
        apply(ed, w, e, id, value, &edit, d->name);
    }
    if (edit.finished && !playing) {
        editor_history_end_merge(ed->history);
    }
    igPopID();
}

static void add_component(Editor *ed, NphWorld *w, NphEntity e)
{
    igSetNextItemWidth(-FLT_MIN);
    if (!igBeginCombo("##add", "Add Component", ImGuiComboFlags_None)) {
        return;
    }
    for (NphComponentId id = 0; id < nph_world_component_count(w); ++id) {
        const NphComponentDesc *d = nph_world_component_desc(w, id);
        if (d == NULL || (d->flags & (u32)NPH_COMPONENT_FLAG_INTERNAL) != 0U ||
            nph_world_has(w, e, id)) {
            continue;
        }
        if (igSelectable_Bool(d->name, false, ImGuiSelectableFlags_None, ui_vec2(0.0F, 0.0F))) {
            const NphResult r = editor_edit_add_component(ed->history, e, id);
            if (r != NPH_RESULT_OK) {
                NPH_LOG_WARN(&LOG_EDITOR, "adding %s: %s", d->name, nph_result_string(r));
            }
        }
    }
    igEndCombo();
}

void editor_panel_inspector(Editor *ed)
{
    if (!igBegin(EDITOR_WINDOW_INSPECTOR, &ed->show.inspector, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    NphWorld *w = nph_engine_world(ed->engine);
    const NphEntity e = ed->selection.primary;
    if (nph_entity_is_null(e) || !nph_world_is_alive(w, e)) {
        igTextDisabled("nothing selected");
        igEnd();
        return;
    }
    char label[96];
    editor_entity_label(w, e, label, sizeof label);
    igText("%s", label);
    igSameLine(0.0F, -1.0F);
    igTextDisabled("0x%08x", e.value);
    if (ed->selection.count > 1U) {
        igSameLine(0.0F, -1.0F);
        igTextDisabled("(%u selected)", ed->selection.count);
    }
    if (nph_engine_is_playing(ed->engine)) {
        igTextColored(ui_vec4(1.0F, 0.8F, 0.35F, 1.0F), "playing: edits change the play copy only");
    }
    igSeparator();
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    const u32 n = nph_world_entity_components(w, e, ids, NPH_ECS_MAX_COMPONENTS);
    for (u32 i = 0; i < n; ++i) {
        component(ed, w, e, ids[i]);
        if (!nph_world_is_alive(w, e)) {
            break;
        }
    }
    if (!nph_engine_is_playing(ed->engine) && nph_world_is_alive(w, e)) {
        igSeparator();
        add_component(ed, w, e);
    }
    igEnd();
}

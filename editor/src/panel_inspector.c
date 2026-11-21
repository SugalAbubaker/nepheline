// SPDX-License-Identifier: Apache-2.0
// The inspector (docs/editor.md "Inspector"; CLAUDE.md §8.13 "Inspector
// (driven by ECS reflection metadata)"): the primary selection's
// components, each field edited by its kind and its hints
// (scene/component.h):
//   - numbers are dragged, within RANGE bounds;
//   - ANGLE fields are shown in degrees;
//   - COLOR vectors use a colour editor;
//   - rotations are edited as Euler angles;
//   - an ENUM's value is picked from its names, and a BITS mask's bits are
//     boxes in a popup, named or numbered (ADR-0108);
//   - TEXT, string ids, and GUIDs are typed;
//   - entity references take an entity dropped from the outliner;
//   - a GUID that names an asset of a type (NPH_FIELD_ASSET) shows its
//     source, takes an asset of its kind dropped from the asset browser
//     (a material, a physics material, an editable mesh, a sound), picks
//     one of the project's from a list (a scene's parts too: its materials,
//     its collision), and opens a `.nmat` or a `.npmat` in its panel
//     (ADR-0105, ADR-0108).
// READONLY fields are shown greyed and HIDDEN ones not at all.
//
// While editing, a change goes through the history: a drag, or typing into
// a field, is one undoable step (merged by the widget). While playing, the
// play copy is changed directly; those changes go with it when play stops.
#include <nepheline/asset/asset.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/string_id.h>
#include <nepheline/math/quat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/scene/component.h>

#include "editor_internal.h"

#include <cook/cooker.h>
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

// ---------------------------------------------------------------------------
// Integers by their kind
// ---------------------------------------------------------------------------

// An integer field's value, sign-extended from a signed kind.
static i64 int_get(NphFieldKind kind, const u8 *p)
{
    switch (kind) {
    case NPH_FIELD_U8: {
        u8 v = 0U;
        memcpy(&v, p, sizeof v);
        return (i64)v;
    }
    case NPH_FIELD_U16: {
        u16 v = 0U;
        memcpy(&v, p, sizeof v);
        return (i64)v;
    }
    case NPH_FIELD_U32: {
        u32 v = 0U;
        memcpy(&v, p, sizeof v);
        return (i64)v;
    }
    case NPH_FIELD_I8: {
        i8 v = 0;
        memcpy(&v, p, sizeof v);
        return (i64)v;
    }
    case NPH_FIELD_I16: {
        i16 v = 0;
        memcpy(&v, p, sizeof v);
        return (i64)v;
    }
    case NPH_FIELD_I32: {
        i32 v = 0;
        memcpy(&v, p, sizeof v);
        return (i64)v;
    }
    default: { // U64, I64: the bits as they are
        i64 v = 0;
        memcpy(&v, p, sizeof v);
        return v;
    }
    }
}

// Writes `v` into an integer field, truncated to its kind.
static void int_set(NphFieldKind kind, u8 *p, i64 v)
{
    switch (kind) {
    case NPH_FIELD_U8:
    case NPH_FIELD_I8: {
        const u8 b = (u8)((u64)v & 0xFFU);
        memcpy(p, &b, sizeof b);
        break;
    }
    case NPH_FIELD_U16:
    case NPH_FIELD_I16: {
        const u16 b = (u16)((u64)v & 0xFFFFU);
        memcpy(p, &b, sizeof b);
        break;
    }
    case NPH_FIELD_U32:
    case NPH_FIELD_I32: {
        const u32 b = (u32)((u64)v & 0xFFFFFFFFU);
        memcpy(p, &b, sizeof b);
        break;
    }
    default:
        memcpy(p, &v, sizeof v);
        break;
    }
}

// Copies a name into `out` (NUL-terminated, cut to fit).
static void name_text(NphStr name, char *out, u64 cap)
{
    (void)snprintf(out, (size_t)cap, "%.*s", (int)name.len, name.data);
}

static void note_discrete(FieldEdit *edit, u64 widget)
{
    edit->changed = true;
    edit->key = widget;
    edit->finished = true; // one step a pick
}

// An enumeration (NPH_FIELD_ENUM): its value's name, picked from a list of
// them. A value with no name shows its number.
static void enum_field(const NphFieldDesc *f, u8 *p, FieldEdit *edit)
{
    const NphFieldKind kind = (NphFieldKind)f->kind;
    const i64 v = int_get(kind, p);
    const u32 count = nph_field_name_count(f);
    char preview[80];
    if (v >= 0 && v < (i64)count) {
        name_text(nph_field_name(f, (u32)v), preview, sizeof preview);
    } else {
        (void)snprintf(preview, sizeof preview, "%lld (no name)", (long long)v);
    }
    if (!igBeginCombo("##v", preview, ImGuiComboFlags_None)) {
        return;
    }
    for (u32 i = 0U; i < count; ++i) {
        char item[80];
        name_text(nph_field_name(f, i), item, sizeof item);
        igPushID_Int((int)i);
        if (igSelectable_Bool(item, v == (i64)i, ImGuiSelectableFlags_None, ui_vec2(0.0F, 0.0F)) &&
            v != (i64)i) {
            int_set(kind, p, (i64)i);
            note_discrete(edit, (u64)igGetItemID());
        }
        igPopID();
    }
    igEndCombo();
}

// The bits a mask's summary counts as "all": its named ones, or every bit
// of its width.
static u64 bits_all(const NphFieldDesc *f, u32 width)
{
    const u32 named = nph_field_name_count(f);
    const u32 n = named > 0U ? named : width;
    return n >= 64U ? NPH_U64_MAX : (1ULL << n) - 1ULL;
}

// "none", "all", or the bits set: their names, else their numbers with runs
// joined ("bits 0, 3-5"), cut short with "..." when they do not fit.
static void bits_summary(const NphFieldDesc *f, u64 v, u32 width, char *out, u64 cap)
{
    const u64 all = bits_all(f, width);
    if (v == 0U) {
        (void)snprintf(out, (size_t)cap, "none");
        return;
    }
    if (v == all) {
        (void)snprintf(out, (size_t)cap, "all");
        return;
    }
    const u32 named = nph_field_name_count(f);
    u64 len = 0U;
    out[0] = '\0';
    if (named == 0U) { // numbers alone would read as a value
        const bool one = (v & (v - 1U)) == 0U;
        len = (u64)snprintf(out, (size_t)cap, "%s", one ? "bit " : "bits ");
    }
    bool first = true;
    for (u32 i = 0U; i < width; ++i) {
        if (((v >> i) & 1U) == 0U) {
            continue;
        }
        char item[80];
        if (i < named) {
            name_text(nph_field_name(f, i), item, sizeof item);
        } else {
            u32 last = i;
            while (last + 1U < width && last + 1U >= named && ((v >> (last + 1U)) & 1U) != 0U) {
                ++last;
            }
            if (last > i + 1U) {
                (void)snprintf(item, sizeof item, "%u-%u", i, last);
                i = last;
            } else {
                (void)snprintf(item, sizeof item, "%u", i);
            }
        }
        const int n = snprintf(out + len, (size_t)(cap - len), "%s%s", first ? "" : ", ", item);
        first = false;
        if (n < 0 || len + (u64)n + 4U >= cap) {
            (void)snprintf(out + (len < cap - 4U ? len : cap - 4U), 4U, "...");
            return;
        }
        len += (u64)n;
    }
}

// A bit mask (NPH_FIELD_BITS): what it holds, and a box a bit in a popup,
// named or numbered.
static void bits_field(const NphFieldDesc *f, u8 *p, FieldEdit *edit)
{
    const NphFieldKind kind = (NphFieldKind)f->kind;
    const u32 width = f->size * 8U;
    u64 v = (u64)int_get(kind, p);
    char summary[160];
    bits_summary(f, v, width, summary, sizeof summary);
    if (igButton(summary, ui_vec2(-FLT_MIN, 0.0F))) {
        (void)igOpenPopup_Str("##bits", ImGuiPopupFlags_None);
    }
    if (!igBeginPopup("##bits", ImGuiWindowFlags_None)) {
        return;
    }
    const u32 named = nph_field_name_count(f);
    bool changed = false;
    u64 widget = 0U;
    if (named > 0U) {
        for (u32 i = 0U; i < named; ++i) {
            char item[80];
            name_text(nph_field_name(f, i), item, sizeof item);
            igPushID_Int((int)i);
            bool on = ((v >> i) & 1U) != 0U;
            if (igCheckbox(item, &on)) {
                v = on ? v | (1ULL << i) : v & ~(1ULL << i);
                changed = true;
                widget = (u64)igGetItemID();
            }
            igPopID();
        }
    } else {
        const u64 all = bits_all(f, width);
        if (igSmallButton("All")) {
            v = all;
            changed = true;
            widget = (u64)igGetItemID();
        }
        igSameLine(0.0F, -1.0F);
        if (igSmallButton("None")) {
            v = 0U;
            changed = true;
            widget = (u64)igGetItemID();
        }
        const ImGuiTableFlags tflags =
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
        if (igBeginTable("##grid", 8, tflags, ui_vec2(0.0F, 0.0F), 0.0F)) {
            for (u32 i = 0U; i < width; ++i) {
                (void)igTableNextColumn();
                char item[12];
                (void)snprintf(item, sizeof item, "%u", i);
                bool on = ((v >> i) & 1U) != 0U;
                if (igCheckbox(item, &on)) {
                    v = on ? v | (1ULL << i) : v & ~(1ULL << i);
                    changed = true;
                    widget = (u64)igGetItemID();
                }
            }
            igEndTable();
        }
    }
    igEndPopup();
    if (changed) {
        int_set(kind, p, (i64)v);
        note_discrete(edit, widget);
    }
}

// ---------------------------------------------------------------------------
// Asset fields
// ---------------------------------------------------------------------------

// What the inspector takes for a GUID that names an asset of a type
// (NPH_FIELD_ASSET; ADR-0108): the payload a source of it is dragged as
// from the browser, the kind of source that cooks to it and the sub-asset
// it cooks it as, what a null GUID means, and what the tooltip calls it.
typedef struct AssetKind {
    u32 type;         // NphAssetType
    const char *drag; // NULL: no source cooks to it alone (a scene's part): picked from the list
    u32 source;       // CookerSourceKind
    const char *sub;  // its sub-asset in that source
    const char *none; // a null GUID's label
    const char *noun;
} AssetKind;

static const AssetKind k_asset_kinds[] = {
    {NPH_ASSET_TYPE_MATERIAL, EDITOR_DRAG_MATERIAL, COOKER_SOURCE_MATERIAL, "material", "(default)",
     "a material"},
    {NPH_ASSET_TYPE_PHYSICS_MATERIAL, EDITOR_DRAG_PHYSICS_MATERIAL, COOKER_SOURCE_PHYSICS_MATERIAL,
     "physics_material", "(its own values)", "a physics material"},
    {NPH_ASSET_TYPE_MESH, EDITOR_DRAG_ASSET, COOKER_SOURCE_MESH, "mesh", "(none)",
     "an editable mesh"},
    {NPH_ASSET_TYPE_SOUND, EDITOR_DRAG_SOUND, COOKER_SOURCE_SOUND, "sound", "(none)", "a sound"},
    {NPH_ASSET_TYPE_COLLISION, NULL, COOKER_SOURCE_NONE, NULL, "(none)", "a collision asset"},
};

static const AssetKind *asset_kind_of(u32 type)
{
    for (u32 i = 0U; i < NPH_ARRAY_COUNT(k_asset_kinds); ++i) {
        if (k_asset_kinds[i].type == type) {
            return &k_asset_kinds[i];
        }
    }
    return NULL;
}

// The asset a drop of the source `key`, dragged as `drag`, gives a field of
// kind `k`, its cook queued when the source has none yet. False for a drag
// or a source of another kind.
static bool asset_drop_guid(Editor *ed, const AssetKind *k, const char *drag, const char *key,
                            NphGuid *out)
{
    if (k->drag == NULL || strcmp(drag, k->drag) != 0 ||
        cooker_source_kind(key) != (CookerSourceKind)k->source) {
        return false;
    }
    *out = cooker_asset_guid(key, k->sub);
    if (ed->project != NULL && !editor_project_is_cooked(ed->project, key)) {
        u32 id = 0U;
        NPH_IGNORE_RESULT(editor_project_cook(ed->project, key, false, 0U, &id));
    }
    return true;
}

// A dragged source this field takes: peeked at while it hovers, accepted
// (and highlighted) only when it is of the field's kind.
static const char *accept_source(const AssetKind *k)
{
    if (k->drag == NULL) {
        return NULL;
    }
    const ImGuiPayload *peek = igAcceptDragDropPayload(k->drag, ImGuiDragDropFlags_AcceptPeekOnly);
    if (peek == NULL || peek->DataSize <= 0 ||
        ((const char *)peek->Data)[peek->DataSize - 1] != '\0' ||
        cooker_source_kind((const char *)peek->Data) != (CookerSourceKind)k->source) {
        return NULL;
    }
    const ImGuiPayload *p = igAcceptDragDropPayload(k->drag, ImGuiDragDropFlags_None);
    return p != NULL ? (const char *)p->Data : NULL;
}

static bool contains_nocase(const char *hay, const char *needle)
{
    const u64 n = strlen(needle);
    for (const char *h = hay; *h != '\0'; ++h) {
        u64 i = 0U;
        while (i < n && h[i] != '\0' && nph_ascii_to_lower(h[i]) == nph_ascii_to_lower(needle[i])) {
            ++i;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0U;
}

// The list of the project's assets of a type, as a popup draws it.
#define PICK_SHOWN 200U

typedef struct AssetPick {
    const char *filter;
    const char *own; // the sub-asset a source of the kind cooks it as: named by its key alone
    NphGuid current;
    NphGuid picked;
    bool chosen;
    u32 shown;
    u32 more; // matches not shown
} AssetPick;

static void pick_visit(void *user, NphGuid guid, const char *key, const char *sub)
{
    AssetPick *pick = (AssetPick *)user;
    char label[EDITOR_PROJECT_PATH_MAX + 40U];
    const bool own = pick->own != NULL && strcmp(sub, pick->own) == 0;
    (void)snprintf(label, sizeof label, "%s%s%s", key, own ? "" : " ", own ? "" : sub);
    if (!contains_nocase(label, pick->filter)) {
        return;
    }
    if (pick->shown == PICK_SHOWN) {
        pick->more += 1U;
        return;
    }
    igPushID_Int((int)pick->shown);
    if (igSelectable_Bool(label, nph_guid_equals(guid, pick->current), ImGuiSelectableFlags_None,
                          ui_vec2(0.0F, 0.0F))) {
        pick->picked = guid;
        pick->chosen = true;
    }
    igPopID();
    pick->shown += 1U;
}

// The popup of the project's assets of the field's type, filtered by what
// is typed; true with `out` when one (or none) was chosen.
static bool asset_picker(Editor *ed, const AssetKind *k, NphGuid current, NphGuid *out)
{
    if (!igBeginPopup("##pick", ImGuiWindowFlags_None)) {
        return false;
    }
    bool chosen = false;
    igSetNextItemWidth(360.0F);
    (void)igInputTextWithHint("##filter", "filter", ed->asset_filter, sizeof ed->asset_filter,
                              ImGuiInputTextFlags_None, NULL, NULL);
    if (igSelectable_Bool(k->none, nph_guid_is_null(current), ImGuiSelectableFlags_None,
                          ui_vec2(0.0F, 0.0F))) {
        *out = NPH_GUID_NULL;
        chosen = true;
    }
    AssetPick pick;
    memset(&pick, 0, sizeof pick);
    pick.filter = ed->asset_filter;
    pick.own = k->sub;
    pick.current = current;
    if (igBeginChild_Str("##list", ui_vec2(360.0F, 240.0F), ImGuiChildFlags_None,
                         ImGuiWindowFlags_None)) {
        if (ed->project != NULL) {
            editor_project_each_asset(ed->project, k->type, pick_visit, &pick);
        }
        if (pick.more > 0U) {
            igTextDisabled("%u more: type to narrow the list", pick.more);
        } else if (pick.shown == 0U) {
            igTextDisabled("the project has none");
        }
    }
    igEndChild();
    if (pick.chosen) {
        *out = pick.picked;
        chosen = true;
    }
    if (chosen) {
        igCloseCurrentPopup();
    }
    igEndPopup();
    return chosen;
}

// A GUID that names an asset of a type (NPH_FIELD_ASSET): its source, or
// what none means; an asset of its kind dropped from the browser takes it,
// the list beside it picks one of the project's, right-click clears it, and
// Edit opens a material or physics material in its panel (ADR-0105,
// ADR-0108).
static void asset_field(Editor *ed, u8 *value, const NphFieldDesc *f, FieldEdit *edit)
{
    const AssetKind *k = asset_kind_of(f->asset_type);
    const AssetKind any = {f->asset_type, NULL, COOKER_SOURCE_NONE, NULL, "(none)", "an asset"};
    if (k == NULL) {
        k = &any; // a type no source cooks alone: its assets picked from the list
    }
    NphGuid g;
    memcpy(&g, value + f->offset, sizeof g);
    char key[EDITOR_PROJECT_PATH_MAX];
    char sub[32];
    const bool known = !nph_guid_is_null(g) && ed->project != NULL &&
                       editor_project_source_of(ed->project, g, key, sizeof key, sub, sizeof sub);
    char label[EDITOR_PROJECT_PATH_MAX + 40U];
    if (nph_guid_is_null(g)) {
        (void)snprintf(label, sizeof label, "%s", k->none);
    } else if (known && k->sub != NULL && strcmp(sub, k->sub) == 0) {
        (void)snprintf(label, sizeof label, "%s", key);
    } else if (known) {
        (void)snprintf(label, sizeof label, "%s %s", key, sub); // a glTF's: "city.gltf material/3"
    } else {
        char hex[NPH_GUID_STRING_LEN + 1U];
        nph_guid_to_string(g, hex);
        (void)snprintf(label, sizeof label, "%s", hex);
    }
    const CookerSourceKind source = known ? cooker_source_kind(key) : COOKER_SOURCE_NONE;
    const bool editable =
        source == COOKER_SOURCE_MATERIAL || source == COOKER_SOURCE_PHYSICS_MATERIAL;
    const f32 room = (editable ? 48.0F : 0.0F) + igGetFrameHeight() + 4.0F;
    (void)igButton(label, ui_vec2(-room, 0.0F));
    const u64 widget = (u64)igGetItemID();
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        if (k->drag != NULL) {
            igSetTooltip("drop %s from the Assets panel here, or pick one from the list; "
                         "right-click for %s",
                         k->noun, k->none);
        } else {
            igSetTooltip("pick %s of the project's from the list; right-click for %s", k->noun,
                         k->none);
        }
    }
    bool changed = false;
    NphGuid next = g;
    if (igBeginDragDropTarget()) {
        const char *dropped = accept_source(k);
        NphGuid guid;
        if (dropped != NULL && k->drag != NULL && asset_drop_guid(ed, k, k->drag, dropped, &guid)) {
            next = guid;
            changed = true;
        }
        igEndDragDropTarget();
    }
    if (igIsItemClicked(ImGuiMouseButton_Right) && !nph_guid_is_null(g)) {
        next = NPH_GUID_NULL;
        changed = true;
    }
    igSameLine(0.0F, 4.0F);
    if (igArrowButton("##list", ImGuiDir_Down)) {
        ed->asset_filter[0] = '\0';
        (void)igOpenPopup_Str("##pick", ImGuiPopupFlags_None);
    }
    if (asset_picker(ed, k, g, &next)) {
        changed = !nph_guid_equals(next, g);
    }
    if (editable) {
        igSameLine(0.0F, -1.0F);
        if (igSmallButton("Edit")) {
            if (source == COOKER_SOURCE_MATERIAL) {
                NPH_IGNORE_RESULT(editor_material_open(ed, key)); // logged
            } else {
                NPH_IGNORE_RESULT(editor_physics_material_open(ed, key)); // logged
            }
        }
    }
    if (changed) {
        memcpy(value + f->offset, &next, sizeof next);
        note_discrete(edit, widget);
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
        if ((f->flags & (u32)NPH_FIELD_FLAG_ENUM) != 0U) {
            enum_field(f, p, edit);
            break;
        }
        if ((f->flags & (u32)NPH_FIELD_FLAG_BITS) != 0U) {
            bits_field(f, p, edit);
            break;
        }
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
        if (f->asset_type != 0U) {
            asset_field(ed, value, f, edit);
        } else {
            guid_field(value, f, edit);
        }
        break;
    default:
        igTextDisabled("(%s)", nph_field_kind_name(kind));
        break;
    }
}

// Applies an edited component: through the history while editing, straight
// into the play copy while playing.
static NphResult apply(Editor *ed, NphWorld *w, NphEntity e, NphComponentId id, const u8 *value,
                       const FieldEdit *edit, const char *component)
{
    NphResult r = NPH_RESULT_OK;
    const NphRuntimeComponents *rc = nph_engine_components(ed->engine);
    const NphMeshRenderer *mr = NPH_WORLD_GET(w, e, rc->mesh_renderer, NphMeshRenderer);
    if (nph_engine_is_playing(ed->engine)) {
        r = nph_world_set(w, e, id, value);
    } else if (id == rc->mesh_materials && mr != NULL) {
        // The extra slots are read when the MeshRenderer changes
        // (runtime/components.h): both, as one step.
        const NphMeshRenderer same = *mr;
        char label[EDITOR_HISTORY_LABEL_MAX];
        (void)snprintf(label, sizeof label, "Edit %s", component);
        r = editor_history_begin(ed->history, label);
        if (r == NPH_RESULT_OK) {
            r = editor_history_set(ed->history, e, id, value);
            if (r == NPH_RESULT_OK) {
                r = editor_history_set(ed->history, e, rc->mesh_renderer, &same);
            }
            if (r == NPH_RESULT_OK) {
                editor_history_end(ed->history);
            } else {
                editor_history_abort(ed->history);
            }
        }
    } else {
        char label[EDITOR_HISTORY_LABEL_MAX];
        (void)snprintf(label, sizeof label, "Edit %s", component);
        r = editor_history_set_merged(ed->history, e, id, value, edit->key ^ ((u64)e.value << 32U),
                                      label);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "editing %s: %s", component, nph_result_string(r));
    }
    return r;
}

NphResult editor_inspector_drop(Editor *ed, NphEntity e, NphComponentId id, const char *field,
                                const char *drag, const char *key)
{
    NPH_ASSERT(ed != NULL && field != NULL && drag != NULL && key != NULL);
    NphWorld *w = nph_engine_world(ed->engine);
    const NphComponentDesc *d = nph_world_component_desc(w, id);
    if (d == NULL || d->size == 0U || !nph_world_is_alive(w, e) || !nph_world_has(w, e, id)) {
        return NPH_RESULT_NOT_FOUND;
    }
    const NphFieldDesc *f = nph_component_find_field(d, nph_str_from_cstr(field));
    if (f == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    const AssetKind *k = f->kind == (u32)NPH_FIELD_GUID ? asset_kind_of(f->asset_type) : NULL;
    NphGuid guid;
    if (k == NULL || (f->flags & (u32)NPH_FIELD_FLAG_READONLY) != 0U ||
        !asset_drop_guid(ed, k, drag, key, &guid)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u8 value[NPH_ECS_MAX_COMPONENT_SIZE];
    memcpy(value, nph_world_get(w, e, id), d->size);
    memcpy(value + f->offset, &guid, sizeof guid);
    FieldEdit edit;
    memset(&edit, 0, sizeof edit);
    edit.changed = true;
    edit.finished = true;
    edit.key = nph_hash64(field, strlen(field), (u64)id); // a step of its own
    const NphResult r = apply(ed, w, e, id, value, &edit, d->name);
    if (!nph_engine_is_playing(ed->engine)) {
        editor_history_end_merge(ed->history);
    }
    return r;
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
        NPH_IGNORE_RESULT(apply(ed, w, e, id, value, &edit, d->name)); // logged
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

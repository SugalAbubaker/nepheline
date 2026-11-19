// SPDX-License-Identifier: Apache-2.0
// The material panel (docs/editor.md "Materials"; ADR-0105 decision 5): the
// open material's source as widgets, over material_edit.c. Colours are shown
// and edited in sRGB and kept linear, as the source keeps them. A drag is
// drawn as it moves and settles when it is let go; a click, a choice, or a
// dropped image settles at once.
#include <nepheline/render/reference.h>

#include "editor_internal.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

#define DEGREE        "\xC2\xB0"
#define PANEL_PREVIEW 96.0F // the open material's preview, pixels a side

static const char *const k_slot_names[NPH_MATERIAL_TEXTURE_COUNT] = {
    "Base colour", "Metallic-roughness", "Normal", "Occlusion", "Emissive"};
static const char *const k_alpha_names[3] = {"Opaque", "Mask", "Blend"};
static const char *const k_wrap_names[3] = {"Repeat", "Clamp", "Mirror"};

typedef struct PanelEdit {
    bool changed; // a widget changed the source this frame
    bool settled; // and the change is done
} PanelEdit;

// A widget that drags: changed as it moves, settled when it is let go.
static void dragged(PanelEdit *pe, bool changed)
{
    pe->changed = pe->changed || changed;
    pe->settled = pe->settled || igIsItemDeactivatedAfterEdit();
}

// A widget that changes at once: a box, a choice, a drop.
static void clicked(PanelEdit *pe, bool changed)
{
    if (changed) {
        pe->changed = true;
        pe->settled = true;
    }
}

static f32 unit(f32 v)
{
    return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
}

// A linear colour as its sRGB values, edited, and back.
static bool colour3(const char *label, NphVec3 *linear)
{
    f32 v[3] = {nph_render_ref_linear_to_srgb(linear->x), nph_render_ref_linear_to_srgb(linear->y),
                nph_render_ref_linear_to_srgb(linear->z)};
    if (!igColorEdit3(label, v, ImGuiColorEditFlags_None)) {
        return false;
    }
    *linear = nph_vec3(unit(nph_render_ref_srgb_to_linear(v[0])),
                       unit(nph_render_ref_srgb_to_linear(v[1])),
                       unit(nph_render_ref_srgb_to_linear(v[2])));
    return true;
}

// As colour3, with an alpha that is no colour, so stays as it is.
static bool colour4(const char *label, NphVec4 *linear)
{
    f32 v[4] = {nph_render_ref_linear_to_srgb(linear->x), nph_render_ref_linear_to_srgb(linear->y),
                nph_render_ref_linear_to_srgb(linear->z), linear->w};
    if (!igColorEdit4(label, v, ImGuiColorEditFlags_AlphaBar)) {
        return false;
    }
    *linear = nph_vec4(unit(nph_render_ref_srgb_to_linear(v[0])),
                       unit(nph_render_ref_srgb_to_linear(v[1])),
                       unit(nph_render_ref_srgb_to_linear(v[2])), unit(v[3]));
    return true;
}

static void surface(CookMaterialSource *s, PanelEdit *pe)
{
    igSeparatorText("Surface");
    dragged(pe, colour4("Base colour", &s->base_color));
    dragged(pe, igSliderFloat("Metallic", &s->metallic, 0.0F, 1.0F, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp));
    dragged(pe, igSliderFloat("Roughness", &s->roughness, 0.0F, 1.0F, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp));
    dragged(pe, colour3("Emissive", &s->emissive));
    dragged(pe, igDragFloat("Emissive strength", &s->emissive_strength, 0.05F, 0.0F,
                            COOK_MATERIAL_MAX_EMISSIVE, "%.3g", ImGuiSliderFlags_AlwaysClamp));
    dragged(pe, igDragFloat("Normal scale", &s->normal_scale, 0.01F, -COOK_MATERIAL_MAX_NORMAL,
                            COOK_MATERIAL_MAX_NORMAL, "%.3f", ImGuiSliderFlags_AlwaysClamp));
    dragged(pe, igSliderFloat("Occlusion strength", &s->occlusion_strength, 0.0F, 1.0F, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp));
}

static void alpha(CookMaterialSource *s, PanelEdit *pe)
{
    igSeparatorText("Alpha");
    const u32 mode = s->alpha < 3U ? s->alpha : 0U;
    if (igBeginCombo("Mode", k_alpha_names[mode], ImGuiComboFlags_None)) {
        for (u32 i = 0; i < 3U; ++i) {
            if (igSelectable_Bool(k_alpha_names[i], mode == i, ImGuiSelectableFlags_None,
                                  ui_vec2(0.0F, 0.0F))) {
                clicked(pe, s->alpha != i);
                s->alpha = i;
            }
        }
        igEndCombo();
    }
    igBeginDisabled(s->alpha != (u32)NPH_MATERIAL_ALPHA_MASK);
    dragged(pe, igSliderFloat("Cutoff", &s->alpha_cutoff, 0.0F, 1.0F, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp));
    igEndDisabled();
    bool two = s->double_sided != 0;
    if (igCheckbox("Double-sided", &two)) {
        s->double_sided = two ? 1 : 0;
        clicked(pe, true);
    }
    bool unlit = s->unlit != 0;
    if (igCheckbox("Unlit", &unlit)) {
        s->unlit = unlit ? 1 : 0;
        clicked(pe, true);
    }
}

static void tiling(CookMaterialSource *s, PanelEdit *pe)
{
    igSeparatorText("Tiling");
    f32 t[2] = {s->tiling.x, s->tiling.y};
    const bool tiled = igDragFloat2("Tiling", t, 0.01F, COOK_MATERIAL_MIN_TILING,
                                    COOK_MATERIAL_MAX_TILING, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    if (tiled) {
        s->tiling = nph_vec2(t[0], t[1]);
    }
    dragged(pe, tiled);
    dragged(pe,
            igDragFloat("Rotation", &s->rotation, 0.5F, -COOK_MATERIAL_MAX_ROTATION,
                        COOK_MATERIAL_MAX_ROTATION, "%.1f" DEGREE, ImGuiSliderFlags_AlwaysClamp));
    f32 o[2] = {s->offset.x, s->offset.y};
    const bool moved = igDragFloat2("Offset", o, 0.01F, -COOK_MATERIAL_MAX_OFFSET,
                                    COOK_MATERIAL_MAX_OFFSET, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    if (moved) {
        s->offset = nph_vec2(o[0], o[1]);
    }
    dragged(pe, moved);
}

// Sets `t`'s image to `key`, a project's image; false when it is already, or is no image's key.
static bool set_image(CookMaterialTexture *t, const char *key)
{
    if (strcmp(t->image, key) == 0 || !cook_material_image_key_valid(nph_str_from_cstr(key))) {
        return false;
    }
    (void)snprintf(t->image, sizeof t->image, "%s", key);
    return true;
}

static void wrap(const char *label, u8 *value, PanelEdit *pe)
{
    const u8 now = *value < 3U ? *value : 0U;
    if (igBeginCombo(label, k_wrap_names[now], ImGuiComboFlags_None)) {
        for (u8 i = 0U; i < 3U; ++i) {
            if (igSelectable_Bool(k_wrap_names[i], now == i, ImGuiSelectableFlags_None,
                                  ui_vec2(0.0F, 0.0F))) {
                clicked(pe, *value != i);
                *value = i;
            }
        }
        igEndCombo();
    }
}

// A slot's image, chosen from the project's images or dropped from the
// asset browser, and how it is sampled.
static void texture(Editor *ed, CookMaterialSource *s, u32 slot, PanelEdit *pe)
{
    CookMaterialTexture *t = &s->textures[slot];
    igPushID_Int((int)slot);
    igSeparatorText(k_slot_names[slot]);
    const char *shown = t->image[0] != '\0' ? t->image : "(none)";
    if (igBeginCombo("Image", shown, ImGuiComboFlags_HeightLarge)) {
        if (igIsWindowAppearing()) {
            editor_material_list_images(ed); // listed as the list opens
        }
        if (igSelectable_Bool("(none)", t->image[0] == '\0', ImGuiSelectableFlags_None,
                              ui_vec2(0.0F, 0.0F)) &&
            t->image[0] != '\0') {
            t->image[0] = '\0';
            clicked(pe, true);
        }
        const EditorImageKeys *images = &ed->material.images;
        for (u32 i = 0; i < images->len; ++i) {
            const char *key = images->data[i].key;
            if (igSelectable_Bool(key, strcmp(t->image, key) == 0, ImGuiSelectableFlags_None,
                                  ui_vec2(0.0F, 0.0F))) {
                clicked(pe, set_image(t, key));
            }
        }
        igEndCombo();
    }
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("an image of the project: choose it here, or drop it from the Assets panel");
    }
    if (igBeginDragDropTarget()) {
        const ImGuiPayload *p = igAcceptDragDropPayload(EDITOR_DRAG_IMAGE, ImGuiDragDropFlags_None);
        if (p != NULL && p->DataSize > 0 && ((const char *)p->Data)[p->DataSize - 1] == '\0') {
            clicked(pe, set_image(t, (const char *)p->Data));
        }
        igEndDragDropTarget();
    }
    if (t->image[0] != '\0') {
        wrap("Wrap U", &t->wrap_u, pe);
        wrap("Wrap V", &t->wrap_v, pe);
        bool nearest = t->nearest != 0U;
        if (igCheckbox("Nearest", &nearest)) {
            t->nearest = nearest ? 1U : 0U;
            clicked(pe, true);
        }
    }
    igPopID();
}

void editor_panel_material(Editor *ed)
{
    EditorMaterialEdit *m = &ed->material;
    if (m->reveal) {
        m->reveal = false;
        igSetNextWindowFocus();
    }
    bool show = ed->show.material;
    const bool drawn = igBegin(EDITOR_WINDOW_MATERIAL, &show, ImGuiWindowFlags_None);
    m->focused = drawn && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (drawn && !m->open) {
        igTextDisabled("No material is open.");
        igTextWrapped("Double-click a material in the Assets panel to open it, or make one in the "
                      "folder the Assets panel shows.");
        igBeginDisabled(ed->project == NULL);
        if (igButton("New Material", ui_vec2(0.0F, 0.0F))) {
            NPH_IGNORE_RESULT(editor_material_new(ed, ed->assets.folder, NULL, 0U)); // logged
        }
        igEndDisabled();
    } else if (drawn) {
        igTextUnformatted(m->key, NULL);
        igSameLine(0.0F, 16.0F);
        igBeginDisabled(m->undo.len == 0U && !m->changing);
        if (igSmallButton("Undo")) {
            (void)editor_material_undo(ed);
        }
        igEndDisabled();
        igSameLine(0.0F, -1.0F);
        igBeginDisabled(m->redo.len == 0U);
        if (igSmallButton("Redo")) {
            (void)editor_material_redo(ed);
        }
        igEndDisabled();
        if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            igSetTooltip("Ctrl+Z and Ctrl+Y undo and redo here while this panel has the focus");
        }
        // Its preview as last cooked (ADR-0105 decision 7): drawn again once a change settles
        // and is cooked; the viewport shows a change while it is being made.
        const ImTextureID preview = editor_material_preview(ed, m->key);
        if (preview != 0U) {
            igImage(ui_texture_ref(preview), ui_vec2(PANEL_PREVIEW, PANEL_PREVIEW),
                    ui_vec2(0.0F, 0.0F), ui_vec2(1.0F, 1.0F));
        }
        if (m->error[0] != '\0') {
            igTextColored(ui_vec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", m->error);
        } else if (m->cook != 0U || m->cook_wanted) {
            igTextDisabled("cooking");
        }
        CookMaterialSource s = m->source;
        PanelEdit pe = {false, false};
        surface(&s, &pe);
        alpha(&s, &pe);
        tiling(&s, &pe);
        for (u32 i = 0; i < NPH_MATERIAL_TEXTURE_COUNT; ++i) {
            texture(ed, &s, i, &pe);
        }
        // A change the widgets make settles when its widget is let go, and
        // whenever no widget is held any more (a colour picker's window, a
        // typed value left). One made through editor_material_change settles
        // when its caller says.
        const bool settled = pe.settled || !igIsAnyItemActive();
        if (pe.changed || (m->panel_changing && settled)) {
            const NphResult r = editor_material_change(ed, pe.changed ? &s : &m->source, settled);
            if (r != NPH_RESULT_OK) {
                NPH_LOG_WARN(&LOG_EDITOR, "%s: %s", m->key, nph_result_string(r));
            }
            m->panel_changing = m->changing;
        }
    }
    igEnd();
    if (!show && ed->show.material) {
        editor_material_close(ed); // the window closed: nothing is unsaved
    }
    ed->show.material = show;
}

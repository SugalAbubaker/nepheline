// SPDX-License-Identifier: Apache-2.0
// The physics material panel (docs/editor.md "Physics material"; ADR-0108
// decision 7): the open physics material's source as widgets, over
// physics_material_edit.c. A drag settles when it is let go, a choice at once.
#include "editor_internal.h"

#include <stdio.h>
#include <string.h>

#define CUBED "\xC2\xB3"

static const char *const k_combine_labels[NPH_PHYSICS_MATERIAL_COMBINE_COUNT] = {
    "Default", "Average", "Minimum", "Multiply", "Maximum"};
static const char *const k_combine_help[NPH_PHYSICS_MATERIAL_COMBINE_COUNT] = {
    "", "the mean of the two", "the smaller of the two: ice", "the product of the two",
    "the larger of the two"};

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

// How a value combines with a touching surface's: a list of the modes, each
// told in its tooltip. `by_default` is what the default does for this value.
static void combine(const char *label, const char *by_default, u32 *mode, PanelEdit *pe)
{
    const u32 now = *mode < (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT ? *mode : 0U;
    if (igBeginCombo(label, k_combine_labels[now], ImGuiComboFlags_None)) {
        for (u32 i = 0; i < (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT; ++i) {
            if (igSelectable_Bool(k_combine_labels[i], now == i, ImGuiSelectableFlags_None,
                                  ui_vec2(0.0F, 0.0F))) {
                if (*mode != i) {
                    pe->changed = true;
                    pe->settled = true;
                }
                *mode = i;
            }
            if (igIsItemHovered(ImGuiHoveredFlags_None)) {
                igSetTooltip("%s", i == 0U ? by_default : k_combine_help[i]);
            }
        }
        igEndCombo();
    }
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("of two touching surfaces' modes the later in the list decides");
    }
}

static void values(CookPhysicsMaterialSource *s, PanelEdit *pe)
{
    igSeparatorText("Surface");
    dragged(pe,
            igDragFloat("Friction", &s->friction, 0.005F, 0.0F, NPH_PHYSICS_MATERIAL_MAX_FRICTION,
                        "%.3f", ImGuiSliderFlags_AlwaysClamp));
    combine("Friction combine", "the geometric mean of the two", &s->friction_combine, pe);
    dragged(pe, igSliderFloat("Restitution", &s->restitution, 0.0F, 1.0F, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp));
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("how much of its speed a body keeps bouncing off: 0 none, 1 all");
    }
    combine("Restitution combine", "the larger of the two", &s->restitution_combine, pe);
    igSeparatorText("Mass");
    dragged(pe, igDragFloat("Density", &s->density, 5.0F, 0.0F, NPH_PHYSICS_MATERIAL_MAX_DENSITY,
                            "%.1f kg/m" CUBED, ImGuiSliderFlags_AlwaysClamp));
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("water is 1000, wood about 600, steel 7850; a RigidBody's own mass wins");
    }
}

void editor_panel_physics_material(Editor *ed)
{
    EditorPhysicsMaterialEdit *m = &ed->physics_material;
    if (m->reveal) {
        m->reveal = false;
        igSetNextWindowFocus();
    }
    bool show = ed->show.physics_material;
    const bool drawn = igBegin(EDITOR_WINDOW_PHYSICS_MATERIAL, &show, ImGuiWindowFlags_None);
    m->focused = drawn && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (drawn && !m->open) {
        igTextDisabled("No physics material is open.");
        igTextWrapped("Double-click a physics material in the Assets panel to open it, or make one "
                      "in the folder the Assets panel shows.");
        igBeginDisabled(ed->project == NULL);
        if (igButton("New Physics Material", ui_vec2(0.0F, 0.0F))) {
            NPH_IGNORE_RESULT(editor_physics_material_new(ed, ed->assets.folder, NULL, 0U));
        }
        igEndDisabled();
    } else if (drawn) {
        igTextUnformatted(m->key, NULL);
        igSameLine(0.0F, 16.0F);
        igBeginDisabled(m->undo.len == 0U && !m->changing);
        if (igSmallButton("Undo")) {
            (void)editor_physics_material_undo(ed);
        }
        igEndDisabled();
        igSameLine(0.0F, -1.0F);
        igBeginDisabled(m->redo.len == 0U);
        if (igSmallButton("Redo")) {
            (void)editor_physics_material_redo(ed);
        }
        igEndDisabled();
        if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            igSetTooltip("Ctrl+Z and Ctrl+Y undo and redo here while this panel has the focus");
        }
        if (m->error[0] != '\0') {
            igTextColored(ui_vec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", m->error);
        } else if (m->cook != 0U || m->cook_wanted) {
            igTextDisabled("cooking");
        }
        CookPhysicsMaterialSource s = m->source;
        PanelEdit pe = {false, false};
        values(&s, &pe);
        igTextWrapped("Drop it from the Assets panel on a Collider's physics_material in the "
                      "inspector to give it to that Collider.");
        // A change the widgets make settles when its widget is let go, and
        // whenever no widget is held any more (a typed value left). One made
        // through editor_physics_material_change settles when its caller says.
        const bool settled = pe.settled || !igIsAnyItemActive();
        if (pe.changed || (m->panel_changing && settled)) {
            const NphResult r =
                editor_physics_material_change(ed, pe.changed ? &s : &m->source, settled);
            if (r != NPH_RESULT_OK) {
                NPH_LOG_WARN(&LOG_EDITOR, "%s: %s", m->key, nph_result_string(r));
            }
            m->panel_changing = m->changing;
        }
    }
    igEnd();
    if (!show && ed->show.physics_material) {
        editor_physics_material_close(ed); // the window closed: nothing is unsaved
    }
    ed->show.physics_material = show;
}

// SPDX-License-Identifier: Apache-2.0
// The statistics panel (docs/editor.md "Statistics"; CLAUDE.md §8.11 "a
// debug overlay with frame stats, cvars, and a console"): the last frame's
// times and counts, and every cvar, editable in place through the registry
// (core/cvar.h), with its type checks, ranges, and read-only and cheat
// rules.
#include <nepheline/core/assert.h>
#include <nepheline/core/cvar.h>

#include "editor_internal.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

static void frame_stats(Editor *ed)
{
    NphEngine *e = ed->engine;
    const NphEngineFrameStats *s = nph_engine_frame_stats(e);
    if (!igCollapsingHeader_TreeNodeFlags("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    igText("frame %llu   %.3f ms (%.0f fps)", (unsigned long long)s->frame, s->frame_ms,
           s->frame_ms > 0.0 ? 1000.0 / s->frame_ms : 0.0);
    igText("sync %.3f ms (simulation wait %.3f)   render %.3f ms", s->sync_ms, s->sim_wait_ms,
           s->render_ms);
    igText("%s%s   steps %u   total %llu   dropped %.3f ms",
           nph_engine_is_playing(e) ? "playing" : "editing",
           nph_engine_is_paused(e) ? " (paused)" : "", s->steps, (unsigned long long)s->total_steps,
           (f64)s->dropped_ns / 1.0e6);
    igText("entities %u", nph_world_entity_count(nph_engine_world(e)));
    if (igCollapsingHeader_TreeNodeFlags("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
        const NphRenderFrameStats *r = &s->render;
        const bool production = r->pipeline == NPH_RENDER_PIPELINE_PRODUCTION;
        igText("%s pipeline (r.pipeline)", production ? "production" : "reference");
        igText("instances %u (culled %u)   draws %u (blended %u, dropped %u)", r->instances,
               r->culled, r->draws, r->blended, r->dropped);
        igText("triangles %u   lights %u   debug lines %u", r->triangles, r->lights,
               r->debug_lines);
        if (production) {
            // The GPU's counters, from frame vis_frame (a couple behind).
            igText("GPU: clusters %u   triangles %u%s", r->vis_clusters, r->vis_triangles,
                   r->vis_overflow != 0U ? "   (lists overflowed)" : "");
            igText("culled instances: frustum %u, occlusion %u", r->vis_instances_culled_frustum,
                   r->vis_instances_culled_occlusion);
            igText("culled clusters: frustum %u, cone %u, occlusion %u",
                   r->vis_meshlets_culled_frustum, r->vis_meshlets_culled_cone,
                   r->vis_meshlets_culled_occlusion);
        }
        igText("viewport %ux%u", ed->viewport.width, ed->viewport.height);
    }
    if (igCollapsingHeader_TreeNodeFlags("Scene mirror", ImGuiTreeNodeFlags_None)) {
        const NphSceneRenderStats *m = &s->scene;
        igText("entities %u   instances %u   lights %u", m->entities, m->instances, m->lights);
        igText("waiting %u   failed %u", m->pending, m->failed);
        igText("assets: loading %u, ready %u, failed %u", m->assets_loading, m->assets_ready,
               m->assets_failed);
        igText("last sync: created %u, destroyed %u, moved %u", m->created, m->destroyed, m->moved);
    }
}

static void report(NphResult r, const NphCvar *cv)
{
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "%s: %s", cv->name, nph_result_string(r));
    }
}

static void cvar_value(NphCvarRegistry *reg, NphCvar *cv)
{
    switch (cv->type) {
    case NPH_CVAR_TYPE_BOOL: {
        bool v = cv->value_bool != 0;
        if (igCheckbox("##value", &v)) {
            report(nph_cvar_set_bool(reg, cv, v ? 1 : 0), cv);
        }
        break;
    }
    case NPH_CVAR_TYPE_INT: {
        i64 v = cv->value_int;
        if (igDragScalar("##value", ImGuiDataType_S64, &v, 0.25F, &cv->min_int, &cv->max_int, NULL,
                         ImGuiSliderFlags_AlwaysClamp)) {
            report(nph_cvar_set_int(reg, cv, v), cv);
        }
        break;
    }
    case NPH_CVAR_TYPE_FLOAT: {
        f64 v = cv->value_float;
        const f64 span = cv->max_float - cv->min_float;
        const f32 speed = span > 0.0 && span < 1.0e6 ? (f32)(span * 0.001) : 0.01F;
        if (igDragScalar("##value", ImGuiDataType_Double, &v, speed, &cv->min_float, &cv->max_float,
                         "%.4g", ImGuiSliderFlags_AlwaysClamp)) {
            report(nph_cvar_set_float(reg, cv, v), cv);
        }
        break;
    }
    case NPH_CVAR_TYPE_STRING: {
        char buf[256];
        (void)snprintf(buf, sizeof buf, "%s", cv->value_string != NULL ? cv->value_string : "");
        if (igInputText("##value", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue, NULL,
                        NULL)) {
            report(nph_cvar_set_string(reg, cv, nph_str_from_cstr(buf)), cv);
        }
        break;
    }
    default:
        igTextDisabled("?");
        break;
    }
}

static void cvars(Editor *ed)
{
    char *filter = ed->cvar_filter;
    if (!igCollapsingHeader_TreeNodeFlags("Cvars", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    igSetNextItemWidth(-FLT_MIN);
    (void)igInputText("##filter", filter, sizeof ed->cvar_filter, ImGuiInputTextFlags_None, NULL,
                      NULL);
    NphCvarRegistry *reg = nph_engine_cvars(ed->engine);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!igBeginTable("##cvars", 2, flags, ui_vec2(0.0F, 0.0F), 0.0F)) {
        return;
    }
    igTableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.2F, 0U);
    igTableHeadersRow();
    const u32 n = nph_cvar_registry_count(reg);
    for (u32 i = 0; i < n; ++i) {
        NphCvar *cv = nph_cvar_registry_at(reg, i);
        if (filter[0] != '\0' && strstr(cv->name, filter) == NULL) {
            continue;
        }
        igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
        (void)igTableNextColumn();
        igAlignTextToFramePadding();
        igTextUnformatted(cv->name, NULL);
        if (cv->description != NULL) {
            igSetItemTooltip("%s%s", cv->description,
                             (cv->flags & (u32)NPH_CVAR_FLAG_LATCHED) != 0U
                                 ? "\n(takes effect after a restart)"
                                 : "");
        }
        (void)igTableNextColumn();
        const bool locked = (cv->flags & (u32)NPH_CVAR_FLAG_READ_ONLY) != 0U ||
                            ((cv->flags & (u32)NPH_CVAR_FLAG_CHEAT) != 0U && !reg->cheats_enabled);
        igPushID_Str(cv->name);
        igBeginDisabled(locked);
        igSetNextItemWidth(-FLT_MIN);
        cvar_value(reg, cv);
        igEndDisabled();
        igPopID();
    }
    igEndTable();
}

void editor_panel_stats(Editor *ed)
{
    if (!igBegin(EDITOR_WINDOW_STATS, &ed->show.stats, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    frame_stats(ed);
    cvars(ed);
    igEnd();
}

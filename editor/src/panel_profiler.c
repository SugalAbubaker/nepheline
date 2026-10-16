// SPDX-License-Identifier: Apache-2.0
// The profiler panel (docs/editor.md "Profiler"; CLAUDE.md §8.11 "an
// in-editor timeline viewer"): the frame times, a timeline of the newest
// complete frame (one band per thread, a row per zone depth), the CPU zone
// and GPU pass statistics over the history window, and the tracked
// allocators. Pausing keeps the frame on screen; the trace export writes
// the whole history (profile/profiler.h).
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/render/gpu_profiler.h>

#include "editor_internal.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

#define LANE_LABEL_WIDTH 8.0F // in font heights
#define MAX_DEPTH        32U
#define ZONE_TEXT        0xFF000000U // opaque black (ImGui packs colours as ABGR)

NphResult editor_profiler_panel_init(Editor *ed)
{
    EditorProfilerPanel *p = &ed->profiler;
    memset(p, 0, sizeof *p);
    p->segments = NPH_ALLOC_ARRAY(ed->alloc, NphProfileSegment, EDITOR_PROFILER_SEGMENTS);
    if (p->segments == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    (void)snprintf(p->trace_path, sizeof p->trace_path, "nph_editor_trace.json");
    return NPH_RESULT_OK;
}

void editor_profiler_panel_free(Editor *ed)
{
    EditorProfilerPanel *p = &ed->profiler;
    if (p->segments != NULL) {
        NPH_FREE_ARRAY(ed->alloc, NphProfileSegment, p->segments, EDITOR_PROFILER_SEGMENTS);
    }
    memset(p, 0, sizeof *p);
}

static void capture(EditorProfilerPanel *p, const NphProfiler *prof)
{
    p->time_count = nph_profiler_frame_times(prof, p->times, EDITOR_PROFILER_TIMES);
    p->has_frame = nph_profiler_frame(prof, 0U, &p->frame);
    p->segment_count =
        p->has_frame ? nph_profiler_frame_segments(prof, 0U, p->segments, EDITOR_PROFILER_SEGMENTS)
                     : 0U;
}

// A stable colour per zone name.
static ImU32 zone_color(u32 name)
{
    const u64 h = nph_hash64(&name, sizeof name, 0x6e70685f70726f66ULL);
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
    igColorConvertHSVtoRGB((f32)(h & 0xFFFFU) / 65535.0F, 0.55F, 0.80F, &r, &g, &b);
    return igColorConvertFloat4ToU32(ui_vec4(r, g, b, 1.0F));
}

// ---------------------------------------------------------------------------
// The timeline
// ---------------------------------------------------------------------------

static void timeline(EditorProfilerPanel *p, const NphProfiler *prof)
{
    if (!p->has_frame) {
        igTextDisabled("no complete frame yet");
        return;
    }
    igSetNextItemWidth(igGetFontSize() * 10.0F);
    (void)igDragFloat("ms shown (0: the frame)", &p->ms_width, 0.05F, 0.0F, 1000.0F, "%.2f",
                      ImGuiSliderFlags_None);
    const u32 lanes = nph_profiler_lane_count(prof);
    u8 depth[256];
    memset(depth, 0, sizeof depth);
    for (u32 i = 0; i < p->segment_count; ++i) {
        const NphProfileSegment *s = &p->segments[i];
        if (s->lane < 256U && s->depth + 1U > depth[s->lane]) {
            depth[s->lane] = (u8)(s->depth + 1U < MAX_DEPTH ? s->depth + 1U : MAX_DEPTH);
        }
    }
    const f32 row = igGetTextLineHeight() + 2.0F;
    const f32 label_w = igGetFontSize() * LANE_LABEL_WIDTH;
    const ImVec2_c origin = igGetCursorScreenPos();
    const ImVec2_c avail = igGetContentRegionAvail();
    const f32 width = avail.x - label_w;
    if (width <= 1.0F) {
        return;
    }
    const f64 shown_ms = p->ms_width > 0.0F ? (f64)p->ms_width : p->frame.duration_ms;
    const f64 px_per_ms = shown_ms > 0.0 ? (f64)width / shown_ms : 0.0;
    ImDrawList *dl = igGetWindowDrawList();
    const ImU32 text = igGetColorU32_Col(ImGuiCol_Text, 1.0F);
    const ImU32 band = igGetColorU32_Col(ImGuiCol_FrameBg, 1.0F);
    f32 lane_y[256];
    f32 y = origin.y;
    for (u32 l = 0; l < lanes && l < 256U; ++l) {
        lane_y[l] = y;
        const f32 h = (f32)(depth[l] > 0U ? depth[l] : 1U) * row;
        ImDrawList_AddRectFilled(dl, ui_vec2(origin.x + label_w, y),
                                 ui_vec2(origin.x + avail.x, y + h), band, 0.0F, 0);
        const char *name = nph_profiler_lane_name(prof, l);
        ImDrawList_AddText_Vec2(dl, ui_vec2(origin.x, y + 1.0F), text, name, NULL);
        y += h + 4.0F;
    }
    const f32 total_h = y - origin.y;
    igPushClipRect(ui_vec2(origin.x + label_w, origin.y),
                   ui_vec2(origin.x + avail.x, origin.y + total_h), true);
    const ImVec2_c mouse = igGetMousePos();
    const NphProfileSegment *hovered = NULL;
    for (u32 i = 0; i < p->segment_count; ++i) {
        const NphProfileSegment *s = &p->segments[i];
        if (s->lane >= 256U || s->lane >= lanes || s->depth >= MAX_DEPTH) {
            continue;
        }
        const f64 t0 = nph_profiler_ticks_to_ms(prof, s->start - p->frame.start);
        const f64 t1 = nph_profiler_ticks_to_ms(prof, s->end - p->frame.start);
        const f32 x0 = origin.x + label_w + (f32)(t0 * px_per_ms);
        f32 x1 = origin.x + label_w + (f32)(t1 * px_per_ms);
        if (x0 > origin.x + avail.x) {
            continue;
        }
        x1 = x1 < x0 + 1.0F ? x0 + 1.0F : x1; // at least a pixel
        const f32 sy = lane_y[s->lane] + (f32)s->depth * row;
        const ImVec2_c a = ui_vec2(x0, sy);
        const ImVec2_c b = ui_vec2(x1, sy + row - 1.0F);
        ImDrawList_AddRectFilled(dl, a, b, zone_color(s->name), 0.0F, 0);
        const char *name = nph_profiler_name(prof, s->name);
        const ImVec2_c ts = igCalcTextSize(name, NULL, false, -1.0F);
        if (ts.x + 4.0F < x1 - x0) {
            ImDrawList_AddText_Vec2(dl, ui_vec2(x0 + 2.0F, sy), ZONE_TEXT, name, NULL);
        }
        if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
            hovered = s;
        }
    }
    igPopClipRect();
    igDummy(ui_vec2(avail.x, total_h));
    if (hovered != NULL && igIsItemHovered(ImGuiHoveredFlags_None)) {
        const f64 ms = nph_profiler_ticks_to_ms(prof, hovered->end - hovered->start);
        igSetTooltip("%s\n%.3f ms on %s, depth %u%s%s", nph_profiler_name(prof, hovered->name), ms,
                     nph_profiler_lane_name(prof, hovered->lane), (u32)hovered->depth,
                     (hovered->flags & NPH_PROFILE_SEGMENT_CONTINUED) != 0U ? "\ncontinued" : "",
                     (hovered->flags & NPH_PROFILE_SEGMENT_CONTINUES) != 0U ? "\ncontinues" : "");
    }
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

static bool begin_table(const char *id, int columns)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    return igBeginTable(id, columns, flags, ui_vec2(0.0F, 0.0F), 0.0F);
}

static void zones_table(EditorProfilerPanel *p, const NphProfiler *prof)
{
    const u32 n = nph_profiler_zone_stats(prof, p->zones, EDITOR_PROFILER_ZONES);
    if (!begin_table("##zones", 5)) {
        return;
    }
    igTableSetupScrollFreeze(0, 1);
    igTableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch, 3.0F, 0U);
    igTableSetupColumn("mean ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("max ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("last ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("calls/frame", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableHeadersRow();
    for (u32 i = 0; i < n; ++i) {
        const NphProfilerZoneStats *z = &p->zones[i];
        igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
        (void)igTableNextColumn();
        igTextUnformatted(z->name, NULL);
        (void)igTableNextColumn();
        igText("%.3f", z->mean_ms);
        (void)igTableNextColumn();
        igText("%.3f", z->max_ms);
        (void)igTableNextColumn();
        igText("%.3f", z->last_ms);
        (void)igTableNextColumn();
        igText("%.1f", z->calls_per_frame);
    }
    igEndTable();
}

static void passes_table(EditorProfilerPanel *p, NphRenderGpuProfiler *gpu)
{
    if (gpu == NULL) {
        igTextDisabled("no GPU");
        return;
    }
    const u32 n = nph_render_gpu_profiler_stats(gpu, p->passes, EDITOR_PROFILER_PASSES);
    if (!begin_table("##passes", 7)) {
        return;
    }
    igTableSetupScrollFreeze(0, 1);
    igTableSetupColumn("pass", ImGuiTableColumnFlags_WidthStretch, 3.0F, 0U);
    igTableSetupColumn("last ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("mean ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("p99 ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("max ms", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("read MB", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("write MB", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableHeadersRow();
    for (u32 i = 0; i < n; ++i) {
        const NphRenderPassStats *s = &p->passes[i];
        igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
        (void)igTableNextColumn();
        igTextUnformatted(s->name, NULL);
        (void)igTableNextColumn();
        igText("%.3f", s->last_ms);
        (void)igTableNextColumn();
        igText("%.3f", s->mean_ms);
        (void)igTableNextColumn();
        igText("%.3f", s->p99_ms);
        (void)igTableNextColumn();
        igText("%.3f", s->max_ms);
        (void)igTableNextColumn();
        igText("%.1f", (f64)s->read_bytes / (1024.0 * 1024.0));
        (void)igTableNextColumn();
        igText("%.1f", (f64)s->write_bytes / (1024.0 * 1024.0));
    }
    igEndTable();
}

static void memory_table(EditorProfilerPanel *p, const NphProfiler *prof)
{
    const u32 n = nph_profiler_memory_stats(prof, p->memory, EDITOR_PROFILER_MEMORY);
    if (!begin_table("##memory", 5)) {
        return;
    }
    igTableSetupScrollFreeze(0, 1);
    igTableSetupColumn("allocator", ImGuiTableColumnFlags_WidthStretch, 3.0F, 0U);
    igTableSetupColumn("MB", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("peak MB", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("blocks", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("total allocs", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableHeadersRow();
    for (u32 i = 0; i < n; ++i) {
        const NphMemoryStats *m = &p->memory[i];
        igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
        (void)igTableNextColumn();
        igTextUnformatted(m->tag, NULL);
        (void)igTableNextColumn();
        igText("%.2f", (f64)m->bytes / (1024.0 * 1024.0));
        (void)igTableNextColumn();
        igText("%.2f", (f64)m->peak_bytes / (1024.0 * 1024.0));
        (void)igTableNextColumn();
        igText("%llu", (unsigned long long)m->allocations);
        (void)igTableNextColumn();
        igText("%llu", (unsigned long long)m->total_allocations);
    }
    igEndTable();
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

static void header(Editor *ed, const NphProfiler *prof)
{
    EditorProfilerPanel *p = &ed->profiler;
    if (igSmallButton(p->paused ? "Resume" : "Pause")) {
        p->paused = !p->paused;
    }
    igSameLine(0.0F, -1.0F);
    if (igSmallButton("Save trace")) {
        const NphResult r = nph_profiler_save_chrome_trace(prof, 0U, ed->alloc, p->trace_path);
        if (r == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_EDITOR, "trace saved to %s", p->trace_path);
        } else {
            NPH_LOG_ERROR(&LOG_EDITOR, "trace %s: %s", p->trace_path, nph_result_string(r));
        }
    }
    igSameLine(0.0F, -1.0F);
    igSetNextItemWidth(igGetFontSize() * 16.0F);
    (void)igInputText("##trace", p->trace_path, sizeof p->trace_path, ImGuiInputTextFlags_None,
                      NULL, NULL);
    if (p->has_frame) {
        igSameLine(0.0F, -1.0F);
        igText("frame %llu: %.3f ms, %u segments%s", (unsigned long long)p->frame.index,
               p->frame.duration_ms, p->segment_count,
               p->frame.dropped_events > 0U ? " (events dropped)" : "");
    }
    f32 top = 0.0F;
    for (u32 i = 0; i < p->time_count; ++i) {
        top = p->times[i] > top ? p->times[i] : top;
    }
    char overlay[64];
    (void)snprintf(overlay, sizeof overlay, "frame ms (max %.2f)", (f64)top);
    igPlotLines_FloatPtr("##times", p->times, (int)p->time_count, 0, overlay, 0.0F,
                         top > 0.0F ? top * 1.1F : 1.0F, ui_vec2(-FLT_MIN, igGetFontSize() * 3.0F),
                         (int)sizeof(f32));
}

void editor_panel_profiler(Editor *ed)
{
    if (!igBegin(EDITOR_WINDOW_PROFILER, &ed->show.profiler, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    const NphProfiler *prof = nph_engine_profiler(ed->engine);
    EditorProfilerPanel *p = &ed->profiler;
    if (prof == NULL) {
        igTextDisabled("profiling is off: set prof.enabled and restart");
        igEnd();
        return;
    }
    if (!p->paused) {
        capture(p, prof);
    }
    header(ed, prof);
    if (igBeginTabBar("##profiler", ImGuiTabBarFlags_None)) {
        if (igBeginTabItem("Timeline", NULL, ImGuiTabItemFlags_None)) {
            timeline(p, prof);
            igEndTabItem();
        }
        if (igBeginTabItem("CPU zones", NULL, ImGuiTabItemFlags_None)) {
            zones_table(p, prof);
            igEndTabItem();
        }
        if (igBeginTabItem("GPU passes", NULL, ImGuiTabItemFlags_None)) {
            passes_table(p, nph_engine_gpu_profiler(ed->engine));
            igEndTabItem();
        }
        if (igBeginTabItem("Memory", NULL, ImGuiTabItemFlags_None)) {
            memory_table(p, prof);
            igEndTabItem();
        }
        igEndTabBar();
    }
    igEnd();
}

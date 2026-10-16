// SPDX-License-Identifier: Apache-2.0
// The console panel (docs/editor.md "Console"): the log, filtered by level
// and by text, over a command line into the engine's console
// (runtime/console.h): Enter runs a line, Up and Down walk the history,
// and Tab completes command and cvar names.
#include <nepheline/core/assert.h>
#include <nepheline/runtime/console.h>

#include "editor_internal.h"

#include <float.h>
#include <string.h>

NphResult editor_console_init(Editor *ed)
{
    EditorConsole *c = &ed->console;
    memset(c, 0, sizeof *c);
    NPH_TRY(editor_log_lines_init(&c->lines, ed->alloc, EDITOR_CONSOLE_LINES));
    c->history_pos = -1;
    for (u32 i = (u32)NPH_LOG_LEVEL_DEBUG; i < (u32)NPH_LOG_LEVEL_COUNT; ++i) {
        c->show[i] = true;
    }
    c->auto_scroll = true;
    return NPH_RESULT_OK;
}

void editor_console_free(Editor *ed)
{
    editor_log_lines_free(&ed->console.lines);
}

static ImVec4_c level_color(u32 level)
{
    switch ((NphLogLevel)level) {
    case NPH_LOG_LEVEL_TRACE:
        return ui_vec4(0.50F, 0.50F, 0.52F, 1.0F);
    case NPH_LOG_LEVEL_DEBUG:
        return ui_vec4(0.62F, 0.64F, 0.70F, 1.0F);
    case NPH_LOG_LEVEL_WARN:
        return ui_vec4(1.00F, 0.80F, 0.35F, 1.0F);
    case NPH_LOG_LEVEL_ERROR:
        return ui_vec4(1.00F, 0.42F, 0.38F, 1.0F);
    case NPH_LOG_LEVEL_FATAL:
        return ui_vec4(1.00F, 0.25F, 0.60F, 1.0F);
    case NPH_LOG_LEVEL_INFO:
    default:
        return ui_vec4(0.86F, 0.86F, 0.86F, 1.0F);
    }
}

static bool visible(const EditorConsole *c, const EditorLogLine *line)
{
    if (line->level >= (u32)NPH_LOG_LEVEL_COUNT || !c->show[line->level]) {
        return false;
    }
    return c->filter[0] == '\0' || strstr(line->text, c->filter) != NULL;
}

// ---------------------------------------------------------------------------
// The command line
// ---------------------------------------------------------------------------

static void replace_input(ImGuiInputTextCallbackData *data, int from, const char *text, u64 len)
{
    ImGuiInputTextCallbackData_DeleteChars(data, from, data->BufTextLen - from);
    ImGuiInputTextCallbackData_InsertChars(data, from, text, text + len);
}

static void history(Editor *ed, ImGuiInputTextCallbackData *data)
{
    EditorConsole *c = &ed->console;
    const NphConsole *console = nph_engine_console(ed->engine);
    const i32 count = (i32)nph_console_history_count(console);
    if (count == 0) {
        return;
    }
    i32 pos = c->history_pos;
    if (data->EventKey == ImGuiKey_UpArrow) {
        pos = pos < 0 ? count - 1 : (pos > 0 ? pos - 1 : 0);
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        pos = pos >= 0 && pos + 1 < count ? pos + 1 : -1;
    }
    if (pos == c->history_pos) {
        return;
    }
    c->history_pos = pos;
    const NphStr line = pos >= 0 ? nph_console_history_at(console, (u32)pos) : NPH_STR("");
    replace_input(data, 0, line.data, line.len);
}

// Completes the first word of the line (a command or cvar name): to the
// only match, or as far as all the matches agree, listing them.
static void complete(Editor *ed, ImGuiInputTextCallbackData *data)
{
    NphConsole *console = nph_engine_console(ed->engine);
    const char *buf = data->Buf;
    int end = data->CursorPos;
    for (int i = 0; i < end; ++i) {
        if (buf[i] == ' ') {
            return; // past the name
        }
    }
    const NphStr prefix = {buf, (u64)end};
    const char *matches[EDITOR_CONSOLE_COMPLETIONS];
    const u32 total = nph_console_complete(console, prefix, matches, EDITOR_CONSOLE_COMPLETIONS);
    const u32 n = total < EDITOR_CONSOLE_COMPLETIONS ? total : EDITOR_CONSOLE_COMPLETIONS;
    if (n == 0U) {
        return;
    }
    if (n == 1U) {
        replace_input(data, 0, matches[0], strlen(matches[0]));
        ImGuiInputTextCallbackData_InsertChars(data, data->CursorPos, " ", NULL);
        return;
    }
    u64 common = strlen(matches[0]);
    for (u32 i = 1; i < n; ++i) {
        u64 k = 0U;
        while (k < common && matches[i][k] != '\0' && matches[i][k] == matches[0][k]) {
            ++k;
        }
        common = k;
    }
    if (common > (u64)end) {
        replace_input(data, 0, matches[0], common);
    }
    char list[512];
    u64 used = 0U;
    for (u32 i = 0; i < n && used + 2U < sizeof list; ++i) {
        const u64 len = strlen(matches[i]);
        const u64 room = sizeof list - used - 2U;
        const u64 take = len < room ? len : room;
        memcpy(list + used, matches[i], (size_t)take);
        used += take;
        list[used++] = ' ';
    }
    list[used] = '\0';
    nph_console_printf(console, "%s%s", list, total > n ? "..." : "");
}

static int input_callback(ImGuiInputTextCallbackData *data)
{
    Editor *ed = (Editor *)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        history(ed, data);
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        complete(ed, data);
    }
    return 0;
}

static void execute(Editor *ed)
{
    EditorConsole *c = &ed->console;
    NphStr line = nph_str_trim(nph_str_from_cstr(c->input));
    if (line.len > 0U) {
        NphConsole *console = nph_engine_console(ed->engine);
        nph_console_printf(console, "> %.*s", (int)line.len, line.data);
        (void)nph_console_execute(console, line); // failures are printed
    }
    c->input[0] = '\0';
    c->history_pos = -1;
    c->auto_scroll = true;
    c->focus_input = true;
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

static void toolbar(EditorConsole *c)
{
    static const char *const names[NPH_LOG_LEVEL_COUNT] = {"trace", "debug", "info",
                                                           "warn",  "error", "fatal"};
    if (igSmallButton("Clear")) {
        editor_log_lines_clear(&c->lines);
    }
    igSameLine(0.0F, -1.0F);
    (void)igCheckbox("follow", &c->auto_scroll);
    for (u32 i = 0; i < (u32)NPH_LOG_LEVEL_COUNT; ++i) {
        igSameLine(0.0F, -1.0F);
        igPushStyleColor_Vec4(ImGuiCol_Text, level_color(i));
        (void)igCheckbox(names[i], &c->show[i]);
        igPopStyleColor(1);
    }
    igSameLine(0.0F, -1.0F);
    igSetNextItemWidth(igGetFontSize() * 12.0F);
    (void)igInputText("filter", c->filter, sizeof c->filter, ImGuiInputTextFlags_None, NULL, NULL);
}

static void draw_lines(EditorConsole *c)
{
    const f32 footer = igGetFrameHeightWithSpacing();
    if (!igBeginChild_Str("##log", ui_vec2(0.0F, -footer), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        igEndChild();
        return;
    }
    const u32 count = editor_log_lines_count(&c->lines);
    bool every_line = c->filter[0] == '\0';
    for (u32 i = 0; i < (u32)NPH_LOG_LEVEL_COUNT; ++i) {
        every_line = every_line && c->show[i];
    }
    if (every_line) {
        // Every line shows: clip to the visible rows.
        ImGuiListClipper clipper;
        memset(&clipper, 0, sizeof clipper);
        ImGuiListClipper_Begin(&clipper, (int)count, -1.0F);
        while (ImGuiListClipper_Step(&clipper)) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const EditorLogLine *line = editor_log_lines_at(&c->lines, (u32)row);
                igPushStyleColor_Vec4(ImGuiCol_Text, level_color(line->level));
                igTextUnformatted(line->text, line->text + line->length);
                igPopStyleColor(1);
            }
        }
    } else {
        for (u32 i = 0; i < count; ++i) {
            const EditorLogLine *line = editor_log_lines_at(&c->lines, i);
            if (!visible(c, line)) {
                continue;
            }
            igPushStyleColor_Vec4(ImGuiCol_Text, level_color(line->level));
            igTextUnformatted(line->text, line->text + line->length);
            igPopStyleColor(1);
        }
    }
    // Follow new lines while scrolled to the bottom.
    if (c->auto_scroll && c->lines.pushed != c->seen &&
        igGetScrollY() >= igGetScrollMaxY() - 1.0F) {
        igSetScrollHereY(1.0F);
    }
    c->seen = c->lines.pushed;
    igEndChild();
}

void editor_panel_console(Editor *ed)
{
    EditorConsole *c = &ed->console;
    if (!igBegin(EDITOR_WINDOW_CONSOLE, &ed->show.console, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    toolbar(c);
    igSeparator();
    draw_lines(c);
    igSetNextItemWidth(-FLT_MIN);
    if (c->focus_input) {
        igSetKeyboardFocusHere(0);
        c->focus_input = false;
    }
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackCompletion |
                                      ImGuiInputTextFlags_CallbackHistory;
    if (igInputText("##command", c->input, sizeof c->input, flags, input_callback, ed)) {
        execute(ed);
    }
    igEnd();
}

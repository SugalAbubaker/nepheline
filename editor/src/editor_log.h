// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's logging (docs/editor.md "Console"): its category, and the
// lines the console panel shows.
//
//   - EditorLogLines: a ring of formatted log lines with their levels, the
//     oldest overwritten first. Single-threaded: the editor's thread.
//   - EditorLogSink: a log sink (core/log.h) for the logger thread. It
//     formats each record into its own EditorLogLines under a mutex, and
//     the editor moves them into the panel's lines once per frame
//     (editor_log_sink_drain), so the panel draws without holding a lock
//     the logger thread waits on.
//
// Costs (CLAUDE.md §1.4): init allocates the ring; pushing, the sink, and
// draining copy lines and allocate nothing.

#include <nepheline/core/log.h>
#include <nepheline/platform/thread.h>

NPH_LOG_CATEGORY_DECLARE(LOG_EDITOR);

// A line's text, NUL included; longer records are cut.
#define EDITOR_LOG_LINE_MAX 256U

typedef struct EditorLogLine {
    u64 sequence; // 0 for the first line pushed into its ring, then counting up
    u32 level;    // NphLogLevel
    u32 length;   // bytes before the NUL
    char text[EDITOR_LOG_LINE_MAX];
} EditorLogLine;

typedef struct EditorLogLines {
    EditorLogLine *lines;
    u32 capacity; // a power of two
    u64 pushed;   // lines pushed since init or the last clear
    NphAllocator *alloc;
} EditorLogLines;

// NPH_RESULT_INVALID_ARGUMENT unless `capacity` is a power of two >= 2;
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult editor_log_lines_init(EditorLogLines *l, NphAllocator *alloc, u32 capacity);
void editor_log_lines_free(EditorLogLines *l);

// Appends a line (cut to EDITOR_LOG_LINE_MAX - 1 bytes), overwriting the
// oldest when full.
void editor_log_lines_push(EditorLogLines *l, NphLogLevel level, const char *text, u64 length);

// The lines kept, and line `index` of them (0: the oldest kept).
u32 editor_log_lines_count(const EditorLogLines *l);
const EditorLogLine *editor_log_lines_at(const EditorLogLines *l, u32 index);

void editor_log_lines_clear(EditorLogLines *l);

typedef struct EditorLogSink {
    NphMutex lock;
    EditorLogLines pending; // under `lock`
} EditorLogSink;

// `capacity` lines wait for the editor between drains; more are dropped
// oldest first, and the drain says how many.
NPH_MUST_USE NphResult editor_log_sink_init(EditorLogSink *s, NphAllocator *alloc, u32 capacity);

// Remove the sink from the log first (nph_log_remove_sink).
void editor_log_sink_free(EditorLogSink *s);

// The NphLogSinkFn: `user` is the EditorLogSink. NPH_THREADSAFE.
void editor_log_sink_fn(const NphLogRecord *record, void *user);

// Moves the waiting lines into `into`, oldest first, after a line counting
// the ones dropped since the last drain, if any. Returns the number of
// lines moved (the count line excluded). NPH_THREADSAFE against the sink.
u32 editor_log_sink_drain(EditorLogSink *s, EditorLogLines *into);

// SPDX-License-Identifier: Apache-2.0
// The editor's logging. See editor_log.h.
#include "editor_log.h"

#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>

#include <stdio.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_EDITOR, "editor", NPH_LOG_LEVEL_INFO);

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

NphResult editor_log_lines_init(EditorLogLines *l, NphAllocator *alloc, u32 capacity)
{
    NPH_ASSERT(l != NULL && alloc != NULL);
    memset(l, 0, sizeof *l);
    if (capacity < 2U || !nph_is_pow2_u32(capacity)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    l->lines = NPH_ALLOC_ARRAY(alloc, EditorLogLine, capacity);
    if (l->lines == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    l->capacity = capacity;
    l->alloc = alloc;
    return NPH_RESULT_OK;
}

void editor_log_lines_free(EditorLogLines *l)
{
    if (l->lines != NULL) {
        NPH_FREE_ARRAY(l->alloc, EditorLogLine, l->lines, l->capacity);
    }
    memset(l, 0, sizeof *l);
}

void editor_log_lines_push(EditorLogLines *l, NphLogLevel level, const char *text, u64 length)
{
    NPH_ASSERT(l->lines != NULL && text != NULL);
    EditorLogLine *line = &l->lines[l->pushed & (l->capacity - 1U)];
    const u64 n = length < EDITOR_LOG_LINE_MAX - 1U ? length : EDITOR_LOG_LINE_MAX - 1U;
    memcpy(line->text, text, (size_t)n);
    line->text[n] = '\0';
    line->length = (u32)n;
    line->level = (u32)level;
    line->sequence = l->pushed;
    ++l->pushed;
}

u32 editor_log_lines_count(const EditorLogLines *l)
{
    return l->pushed < l->capacity ? (u32)l->pushed : l->capacity;
}

const EditorLogLine *editor_log_lines_at(const EditorLogLines *l, u32 index)
{
    NPH_ASSERT(index < editor_log_lines_count(l));
    const u64 first = l->pushed - editor_log_lines_count(l);
    return &l->lines[(first + index) & (l->capacity - 1U)];
}

void editor_log_lines_clear(EditorLogLines *l)
{
    l->pushed = 0U;
}

// ---------------------------------------------------------------------------
// The sink
// ---------------------------------------------------------------------------

NphResult editor_log_sink_init(EditorLogSink *s, NphAllocator *alloc, u32 capacity)
{
    NPH_ASSERT(s != NULL);
    memset(s, 0, sizeof *s);
    NPH_TRY(editor_log_lines_init(&s->pending, alloc, capacity));
    nph_mutex_init(&s->lock);
    return NPH_RESULT_OK;
}

void editor_log_sink_free(EditorLogSink *s)
{
    if (s->pending.lines == NULL) {
        return;
    }
    nph_mutex_destroy(&s->lock);
    editor_log_lines_free(&s->pending);
}

void editor_log_sink_fn(const NphLogRecord *record, void *user)
{
    EditorLogSink *s = (EditorLogSink *)user;
    char text[EDITOR_LOG_LINE_MAX];
    const u64 n = nph_log_format(record, text, sizeof text, false, false);
    nph_mutex_lock(&s->lock);
    editor_log_lines_push(&s->pending, (NphLogLevel)record->level, text,
                          n < sizeof text ? n : sizeof text - 1U);
    nph_mutex_unlock(&s->lock);
}

u32 editor_log_sink_drain(EditorLogSink *s, EditorLogLines *into)
{
    nph_mutex_lock(&s->lock);
    const u32 count = editor_log_lines_count(&s->pending);
    const u64 dropped = s->pending.pushed - count;
    if (dropped > 0U) {
        char note[96];
        const int n =
            snprintf(note, sizeof note, "(%llu log lines dropped: the editor fell behind)",
                     (unsigned long long)dropped);
        editor_log_lines_push(into, NPH_LOG_LEVEL_WARN, note, n > 0 ? (u64)n : 0U);
    }
    for (u32 i = 0; i < count; ++i) {
        const EditorLogLine *line = editor_log_lines_at(&s->pending, i);
        editor_log_lines_push(into, (NphLogLevel)line->level, line->text, line->length);
    }
    editor_log_lines_clear(&s->pending);
    nph_mutex_unlock(&s->lock);
    return count;
}

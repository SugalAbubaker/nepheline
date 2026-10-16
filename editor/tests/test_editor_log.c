// SPDX-License-Identifier: Apache-2.0
// The console panel's log (editor/src/editor_log.h): the line ring, and the
// sink the logger thread fills while the editor drains it.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/platform/thread.h>

#include <ntest/ntest.h>

#include "editor_log.h"

#include <stdio.h>
#include <string.h>

typedef struct LogFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} LogFixture;

NTEST_SETUP(LogFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(LogFixture)
{
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static void push_text(EditorLogLines *l, NphLogLevel level, const char *text)
{
    editor_log_lines_push(l, level, text, strlen(text));
}

NTEST_F(LogFixture, lines_keep_the_newest_in_order)
{
    EditorLogLines l;
    NTEST_ASSERT_EQ_INT(editor_log_lines_init(&l, f->a, 4U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(editor_log_lines_count(&l), 0U);
    char text[16];
    for (u32 i = 0; i < 6U; ++i) {
        (void)snprintf(text, sizeof text, "line %u", i);
        push_text(&l, (NphLogLevel)(i % (u32)NPH_LOG_LEVEL_COUNT), text);
    }
    NTEST_ASSERT_EQ_UINT(editor_log_lines_count(&l), 4U);
    for (u32 i = 0; i < 4U; ++i) {
        const EditorLogLine *line = editor_log_lines_at(&l, i);
        (void)snprintf(text, sizeof text, "line %u", i + 2U);
        NTEST_EXPECT_EQ_STR(line->text, text);
        NTEST_EXPECT_EQ_UINT(line->length, (u32)strlen(text));
        NTEST_EXPECT_EQ_UINT(line->sequence, (u64)(i + 2U));
        NTEST_EXPECT_EQ_UINT(line->level, (i + 2U) % (u32)NPH_LOG_LEVEL_COUNT);
    }
    editor_log_lines_clear(&l);
    NTEST_EXPECT_EQ_UINT(editor_log_lines_count(&l), 0U);
    editor_log_lines_free(&l);
    editor_log_lines_free(&l); // twice is harmless
}

NTEST_F(LogFixture, long_lines_are_cut)
{
    EditorLogLines l;
    NTEST_ASSERT_EQ_INT(editor_log_lines_init(&l, f->a, 2U), NPH_RESULT_OK);
    char text[400];
    memset(text, 'x', sizeof text);
    editor_log_lines_push(&l, NPH_LOG_LEVEL_INFO, text, sizeof text);
    const EditorLogLine *line = editor_log_lines_at(&l, 0U);
    NTEST_EXPECT_EQ_UINT(line->length, EDITOR_LOG_LINE_MAX - 1U);
    NTEST_EXPECT_EQ_UINT((u32)strlen(line->text), EDITOR_LOG_LINE_MAX - 1U);
    editor_log_lines_free(&l);
}

NTEST_F(LogFixture, capacities_must_be_powers_of_two)
{
    EditorLogLines l;
    NTEST_EXPECT_EQ_INT(editor_log_lines_init(&l, f->a, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_log_lines_init(&l, f->a, 1U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_log_lines_init(&l, f->a, 6U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(l.lines);
}

static NphLogRecord record(NphLogLevel level, const char *message)
{
    NphLogRecord r;
    memset(&r, 0, sizeof r);
    r.timestamp_ns = 1700000000ULL * 1000000000ULL;
    r.category = "test";
    r.file = __FILE__;
    r.line = 1U;
    r.level = (u16)level;
    r.thread_index = 1U;
    (void)snprintf(r.message, sizeof r.message, "%s", message);
    return r;
}

NTEST_F(LogFixture, the_sink_formats_records_and_the_drain_moves_them)
{
    EditorLogSink sink;
    NTEST_ASSERT_EQ_INT(editor_log_sink_init(&sink, f->a, 8U), NPH_RESULT_OK);
    EditorLogLines panel;
    NTEST_ASSERT_EQ_INT(editor_log_lines_init(&panel, f->a, 16U), NPH_RESULT_OK);
    NphLogRecord r = record(NPH_LOG_LEVEL_INFO, "first message");
    editor_log_sink_fn(&r, &sink);
    r = record(NPH_LOG_LEVEL_ERROR, "second message");
    editor_log_sink_fn(&r, &sink);
    NTEST_EXPECT_EQ_UINT(editor_log_sink_drain(&sink, &panel), 2U);
    NTEST_ASSERT_EQ_UINT(editor_log_lines_count(&panel), 2U);
    const EditorLogLine *a = editor_log_lines_at(&panel, 0U);
    const EditorLogLine *b = editor_log_lines_at(&panel, 1U);
    NTEST_EXPECT_NOT_NULL(strstr(a->text, "first message"));
    NTEST_EXPECT_NOT_NULL(strstr(a->text, "[test]"));
    NTEST_EXPECT_EQ_UINT(a->level, (u32)NPH_LOG_LEVEL_INFO);
    NTEST_EXPECT_NOT_NULL(strstr(b->text, "second message"));
    NTEST_EXPECT_EQ_UINT(b->level, (u32)NPH_LOG_LEVEL_ERROR);
    // Drained lines are gone from the sink.
    NTEST_EXPECT_EQ_UINT(editor_log_sink_drain(&sink, &panel), 0U);
    NTEST_EXPECT_EQ_UINT(editor_log_lines_count(&panel), 2U);
    editor_log_lines_free(&panel);
    editor_log_sink_free(&sink);
}

NTEST_F(LogFixture, a_full_sink_says_how_many_lines_it_dropped)
{
    EditorLogSink sink;
    NTEST_ASSERT_EQ_INT(editor_log_sink_init(&sink, f->a, 4U), NPH_RESULT_OK);
    EditorLogLines panel;
    NTEST_ASSERT_EQ_INT(editor_log_lines_init(&panel, f->a, 16U), NPH_RESULT_OK);
    char text[32];
    for (u32 i = 0; i < 10U; ++i) {
        (void)snprintf(text, sizeof text, "message %u", i);
        const NphLogRecord r = record(NPH_LOG_LEVEL_INFO, text);
        editor_log_sink_fn(&r, &sink);
    }
    NTEST_EXPECT_EQ_UINT(editor_log_sink_drain(&sink, &panel), 4U);
    NTEST_ASSERT_EQ_UINT(editor_log_lines_count(&panel), 5U);
    const EditorLogLine *note = editor_log_lines_at(&panel, 0U);
    NTEST_EXPECT_EQ_UINT(note->level, (u32)NPH_LOG_LEVEL_WARN);
    NTEST_EXPECT_NOT_NULL(strstr(note->text, "6 log lines dropped"));
    NTEST_EXPECT_NOT_NULL(strstr(editor_log_lines_at(&panel, 1U)->text, "message 6"));
    NTEST_EXPECT_NOT_NULL(strstr(editor_log_lines_at(&panel, 4U)->text, "message 9"));
    editor_log_lines_free(&panel);
    editor_log_sink_free(&sink);
}

// The logger thread fills the sink while the editor drains it: every line
// arrives once, in order.
typedef struct Producer {
    EditorLogSink *sink;
    u32 count;
} Producer;

static void produce(void *user)
{
    const Producer *p = (const Producer *)user;
    char text[32];
    for (u32 i = 0; i < p->count; ++i) {
        (void)snprintf(text, sizeof text, "#%u#", i);
        const NphLogRecord r = record(NPH_LOG_LEVEL_DEBUG, text);
        editor_log_sink_fn(&r, p->sink);
    }
}

static u32 parse_number(const char *text)
{
    const char *hash = strchr(text, '#');
    u32 v = 0U;
    for (const char *c = hash != NULL ? hash + 1 : text; *c >= '0' && *c <= '9'; ++c) {
        v = v * 10U + (u32)(*c - '0');
    }
    return v;
}

NTEST_F(LogFixture, lines_cross_threads_once_and_in_order)
{
    enum {
        COUNT = 20000
    };
    EditorLogSink sink;
    NTEST_ASSERT_EQ_INT(editor_log_sink_init(&sink, f->a, 32768U), NPH_RESULT_OK);
    EditorLogLines panel;
    NTEST_ASSERT_EQ_INT(editor_log_lines_init(&panel, f->a, 32768U), NPH_RESULT_OK);
    Producer p = {&sink, (u32)COUNT};
    NphThread thread;
    NphThreadDesc td;
    memset(&td, 0, sizeof td);
    td.fn = produce;
    td.user = &p;
    td.name = "log producer";
    NTEST_ASSERT_EQ_INT(nph_thread_create(&thread, &td), NPH_RESULT_OK);
    u32 drained = 0U;
    while (drained < (u32)COUNT / 2U) {
        drained += editor_log_sink_drain(&sink, &panel);
    }
    nph_thread_join(&thread);
    drained += editor_log_sink_drain(&sink, &panel);
    NTEST_ASSERT_EQ_UINT(drained, (u32)COUNT);
    NTEST_ASSERT_EQ_UINT(editor_log_lines_count(&panel), (u32)COUNT);
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        const EditorLogLine *line = editor_log_lines_at(&panel, i);
        if (parse_number(line->text) != i) {
            NTEST_FAIL("line %u reads \"%s\"", i, line->text);
        }
    }
    editor_log_lines_free(&panel);
    editor_log_sink_free(&sink);
}

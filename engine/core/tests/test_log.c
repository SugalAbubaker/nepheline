// SPDX-License-Identifier: Apache-2.0
// Single-threaded behaviour of the logger: levels, sinks, ordering, drops,
// truncation, formatting, thread lifecycle. Multi-threaded flushing is
// stressed when platform threads arrive in Phase 3.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/log.h>

#include "test_support.h"

#include <stdio.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_TEST, "test", NPH_LOG_LEVEL_TRACE);
NPH_LOG_CATEGORY_DEFINE(LOG_OTHER, "other", NPH_LOG_LEVEL_TRACE);

typedef struct Capture {
    NphLogRecord records[64];
    u32 count;
} Capture;

static void capture_sink(const NphLogRecord *record, void *user)
{
    Capture *c = (Capture *)user;
    if (c->count < NPH_ARRAY_COUNT(c->records)) {
        c->records[c->count] = *record;
    }
    c->count += 1U;
}

typedef struct LogFixture {
    NphDebugAllocator da;
    Capture capture;
} LogFixture;

NTEST_SETUP(LogFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    const NphLogConfig config = {nph_debug_allocator_allocator(&f->da), 8U, 4U};
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_log_is_initialized());
    NTEST_ASSERT_EQ_INT(nph_log_add_sink(capture_sink, &f->capture, NPH_LOG_LEVEL_TRACE),
                        NPH_RESULT_OK);
    nph_log_set_level(&LOG_TEST, NPH_LOG_LEVEL_TRACE);
    nph_log_set_level(&LOG_OTHER, NPH_LOG_LEVEL_TRACE);
    nph_log_set_global_level(NPH_LOG_LEVEL_TRACE);
}

NTEST_TEARDOWN(LogFixture)
{
    nph_log_shutdown();
    NTEST_ASSERT_FALSE(nph_log_is_initialized());
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(LogFixture, records_are_delivered_in_order_with_their_fields)
{
    NPH_LOG_INFO(&LOG_TEST, "first %d", 1);
    const u32 line = (u32)__LINE__ - 1U;
    NPH_LOG_WARN(&LOG_OTHER, "second %s", "two");
    NPH_LOG_ERROR(&LOG_TEST, "third");
    NTEST_ASSERT_EQ_UINT(f->capture.count, 0U); // nothing until flush
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 3U);
    NTEST_ASSERT_EQ_UINT(f->capture.count, 3U);
    const NphLogRecord *r = &f->capture.records[0];
    NTEST_ASSERT_EQ_STR(r->message, "first 1");
    NTEST_ASSERT_EQ_STR(r->category, "test");
    NTEST_ASSERT_EQ_UINT(r->level, NPH_LOG_LEVEL_INFO);
    NTEST_ASSERT_EQ_UINT(r->line, line);
    NTEST_ASSERT_NOT_NULL(strstr(r->file, "test_log.c"));
    NTEST_ASSERT_EQ_UINT(r->thread_index, 1U);
    NTEST_ASSERT_EQ_UINT(r->sequence, 0U);
    NTEST_ASSERT_EQ_UINT(r->dropped_before, 0U);
    NTEST_ASSERT_GT_UINT(r->timestamp_ns, 0U);
    NTEST_ASSERT_EQ_STR(f->capture.records[1].message, "second two");
    NTEST_ASSERT_EQ_STR(f->capture.records[1].category, "other");
    NTEST_ASSERT_EQ_UINT(f->capture.records[1].level, NPH_LOG_LEVEL_WARN);
    NTEST_ASSERT_EQ_UINT(f->capture.records[1].sequence, 1U);
    NTEST_ASSERT_EQ_STR(f->capture.records[2].message, "third");
    NTEST_ASSERT_LE_UINT(f->capture.records[0].timestamp_ns, f->capture.records[2].timestamp_ns);
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 0U);
}

NTEST_F(LogFixture, category_and_global_levels_filter_before_storing)
{
    nph_log_set_level(&LOG_TEST, NPH_LOG_LEVEL_WARN);
    NTEST_ASSERT_EQ_INT(nph_log_get_level(&LOG_TEST), NPH_LOG_LEVEL_WARN);
    NTEST_ASSERT_FALSE(NPH_LOG_ENABLED(&LOG_TEST, NPH_LOG_LEVEL_INFO));
    NTEST_ASSERT_TRUE(NPH_LOG_ENABLED(&LOG_TEST, NPH_LOG_LEVEL_WARN));
    NPH_LOG_INFO(&LOG_TEST, "hidden");
    NPH_LOG_WARN(&LOG_TEST, "shown");
    NPH_LOG_INFO(&LOG_OTHER, "other category still verbose");
    nph_log_set_global_level(NPH_LOG_LEVEL_ERROR);
    NTEST_ASSERT_EQ_INT(nph_log_get_global_level(), NPH_LOG_LEVEL_ERROR);
    NPH_LOG_WARN(&LOG_TEST, "hidden by the global level");
    NPH_LOG_ERROR(&LOG_OTHER, "error passes");
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 3U);
    NTEST_ASSERT_EQ_STR(f->capture.records[0].message, "shown");
    NTEST_ASSERT_EQ_STR(f->capture.records[1].message, "other category still verbose");
    NTEST_ASSERT_EQ_STR(f->capture.records[2].message, "error passes");
    nph_log_set_level(&LOG_TEST, NPH_LOG_LEVEL_OFF);
    NPH_LOG_FATAL(&LOG_TEST, "off");
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 0U);
}

NTEST_F(LogFixture, sinks_filter_by_level_and_can_be_removed)
{
    Capture errors;
    memset(&errors, 0, sizeof errors);
    NTEST_ASSERT_EQ_INT(nph_log_add_sink(capture_sink, &errors, NPH_LOG_LEVEL_ERROR),
                        NPH_RESULT_OK);
    NPH_LOG_INFO(&LOG_TEST, "info");
    NPH_LOG_ERROR(&LOG_TEST, "error");
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 2U);
    NTEST_ASSERT_EQ_UINT(f->capture.count, 2U);
    NTEST_ASSERT_EQ_UINT(errors.count, 1U);
    NTEST_ASSERT_EQ_STR(errors.records[0].message, "error");
    NTEST_ASSERT_TRUE(nph_log_remove_sink(capture_sink, &errors));
    NTEST_ASSERT_FALSE(nph_log_remove_sink(capture_sink, &errors));
    NPH_LOG_ERROR(&LOG_TEST, "after removal");
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 1U);
    NTEST_ASSERT_EQ_UINT(errors.count, 1U);
    // Sink table capacity.
    Capture extra[NPH_LOG_MAX_SINKS];
    u32 added = 0U;
    for (u32 i = 0; i < NPH_LOG_MAX_SINKS; ++i) {
        if (nph_log_add_sink(capture_sink, &extra[i], NPH_LOG_LEVEL_TRACE) == NPH_RESULT_OK) {
            ++added;
        }
    }
    NTEST_ASSERT_EQ_UINT(added, NPH_LOG_MAX_SINKS - 1U); // the fixture's sink holds one slot
    for (u32 i = 0; i < added; ++i) {
        NTEST_ASSERT_TRUE(nph_log_remove_sink(capture_sink, &extra[i]));
    }
}

NTEST_F(LogFixture, long_messages_are_truncated_with_an_ellipsis)
{
    char text[600];
    memset(text, 'x', sizeof text - 1U);
    text[sizeof text - 1U] = '\0';
    NPH_LOG_INFO(&LOG_TEST, "%s", text);
    nph_log_write_str(&LOG_TEST, NPH_LOG_LEVEL_INFO, __FILE__, (u32)__LINE__,
                      nph_str(text, sizeof text - 1U));
    nph_log_write_str(&LOG_TEST, NPH_LOG_LEVEL_INFO, __FILE__, (u32)__LINE__, NPH_STR("short"));
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 3U);
    for (u32 i = 0; i < 2U; ++i) {
        const char *m = f->capture.records[i].message;
        NTEST_ASSERT_EQ_UINT(strlen(m), NPH_LOG_MESSAGE_MAX - 1U);
        NTEST_ASSERT_EQ_STR(m + NPH_LOG_MESSAGE_MAX - 4U, "...");
        NTEST_ASSERT_EQ_UINT(m[0], 'x');
    }
    NTEST_ASSERT_EQ_STR(f->capture.records[2].message, "short");
}

NTEST_F(LogFixture, full_ring_drops_and_reports_the_count)
{
    for (u32 i = 0; i < 12U; ++i) {
        NPH_LOG_INFO(&LOG_TEST, "msg %u", i);
    }
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 8U); // ring capacity
    NTEST_ASSERT_EQ_STR(f->capture.records[7].message, "msg 7");
    NPH_LOG_INFO(&LOG_TEST, "after");
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 1U);
    NTEST_ASSERT_EQ_STR(f->capture.records[8].message, "after");
    NTEST_ASSERT_EQ_UINT(f->capture.records[8].dropped_before, 4U);
    NTEST_ASSERT_EQ_UINT(f->capture.records[8].sequence, 12U);
    char line[512];
    nph_log_format(&f->capture.records[8], line, sizeof line, false, false);
    NTEST_ASSERT_NOT_NULL(strstr(line, "[4 earlier records dropped]"));
}

NTEST_F(LogFixture, flush_limit)
{
    for (u32 i = 0; i < 5U; ++i) {
        NPH_LOG_DEBUG(&LOG_TEST, "d%u", i);
    }
    NTEST_ASSERT_EQ_UINT(nph_log_flush(2U), 2U);
    NTEST_ASSERT_EQ_UINT(f->capture.count, 2U);
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 3U);
    NTEST_ASSERT_EQ_STR(f->capture.records[4].message, "d4");
}

NTEST_F(LogFixture, thread_attach_and_detach_lifecycle)
{
    NTEST_ASSERT_EQ_INT(nph_log_thread_attach(), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_log_thread_attach(), NPH_RESULT_OK); // idempotent
    NPH_LOG_INFO(&LOG_TEST, "before detach");
    nph_log_thread_detach();
    nph_log_thread_detach(); // harmless
    // The retired ring is still drained...
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 1U);
    NTEST_ASSERT_EQ_UINT(f->capture.records[0].thread_index, 1U);
    // ...and a later write from the same thread attaches a fresh ring.
    NPH_LOG_INFO(&LOG_TEST, "after detach");
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 1U);
    NTEST_ASSERT_EQ_UINT(f->capture.records[1].thread_index, 2U);
    NTEST_ASSERT_EQ_UINT(f->capture.records[1].sequence, 0U);
    NphDebugAllocStats s;
    nph_debug_allocator_get_stats(&f->da, &s);
    NTEST_ASSERT_EQ_UINT(s.live_count, 2U); // one LogThread and its ring buffer
}

NTEST(log, init_validation_and_uninitialised_writes)
{
    NphLogConfig config = {nph_allocator_system(), 6U, 4U};
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_INVALID_ARGUMENT);
    config.ring_capacity = 8U;
    config.max_threads = NPH_LOG_MAX_THREADS + 1U;
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_FALSE(nph_log_is_initialized());
    NTEST_ASSERT_EQ_INT(nph_log_thread_attach(), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(nph_log_flush(0U), 0U);
    // Without init a record goes straight to stderr; this must not crash.
    NPH_LOG_INFO(&LOG_TEST, "bootstrap path: %s", "stderr");
    config.max_threads = 0U;
    config.ring_capacity = 0U;
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_ALREADY_EXISTS);
    nph_log_shutdown();
    nph_log_shutdown();
    NTEST_ASSERT_FALSE(nph_log_is_initialized());
}

NTEST(log, format_layout)
{
    NphLogRecord rec;
    memset(&rec, 0, sizeof rec);
    // 13:04:05.006 UTC on some day.
    rec.timestamp_ns = ((13ULL * 3600ULL + 4ULL * 60ULL + 5ULL) * 1000ULL + 6ULL) * 1000000ULL +
                       86400ULL * 1000000000ULL * 3ULL;
    rec.category = "render";
    rec.file = "C:\\src\\engine\\render\\swapchain.c";
    rec.line = 42U;
    rec.level = NPH_LOG_LEVEL_INFO;
    snprintf(rec.message, sizeof rec.message, "hello %d", 7);
    char out[256];
    u64 n = nph_log_format(&rec, out, sizeof out, false, false);
    NTEST_ASSERT_EQ_STR(out, "13:04:05.006 INFO  [render] hello 7");
    NTEST_ASSERT_EQ_UINT(n, strlen(out));
    nph_log_format(&rec, out, sizeof out, false, true);
    NTEST_ASSERT_EQ_STR(out, "13:04:05.006 INFO  [render] hello 7 (swapchain.c:42)");
    rec.level = NPH_LOG_LEVEL_WARN; // WARN and above always show the location
    nph_log_format(&rec, out, sizeof out, false, false);
    NTEST_ASSERT_EQ_STR(out, "13:04:05.006 WARN  [render] hello 7 (swapchain.c:42)");
    rec.dropped_before = 3U;
    nph_log_format(&rec, out, sizeof out, false, false);
    NTEST_ASSERT_EQ_STR(
        out, "13:04:05.006 WARN  [render] hello 7 (swapchain.c:42) [3 earlier records dropped]");
    nph_log_format(&rec, out, sizeof out, true, false);
    NTEST_ASSERT_NOT_NULL(strstr(out, "\x1b[33mWARN \x1b[0m"));
    // Truncation reports the full length like snprintf.
    n = nph_log_format(&rec, out, 10U, false, false);
    NTEST_ASSERT_GT_UINT(n, 10U);
    NTEST_ASSERT_EQ_UINT(strlen(out), 9U);
    NTEST_ASSERT_EQ_STR(nph_log_level_name(NPH_LOG_LEVEL_TRACE), "TRACE");
    NTEST_ASSERT_EQ_STR(nph_log_level_name(NPH_LOG_LEVEL_FATAL), "FATAL");
    NTEST_ASSERT_EQ_STR(nph_log_level_name(NPH_LOG_LEVEL_OFF), "?");
}

NTEST(log, file_sink_writes_lines)
{
    const char *path = "nph_test_log_sink.txt";
    remove(path);
    NphLogFileSink sink;
    NTEST_ASSERT_EQ_INT(nph_log_file_sink_open(&sink, path), NPH_RESULT_OK);
    NphLogRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.category = "test";
    rec.file = "f.c";
    rec.level = NPH_LOG_LEVEL_ERROR;
    snprintf(rec.message, sizeof rec.message, "to file");
    nph_log_file_sink(&rec, &sink);
    nph_log_file_sink_close(&sink);
    nph_log_file_sink_close(&sink);
    FILE *f = fopen(path, "rb");
    NTEST_ASSERT_NOT_NULL(f);
    if (f == NULL) {
        return; // the assertion above already failed the test
    }
    char line[256];
    NTEST_ASSERT_NOT_NULL(fgets(line, sizeof line, f));
    fclose(f);
    remove(path);
    NTEST_ASSERT_NOT_NULL(strstr(line, "ERROR [test] to file (f.c:0)"));
    NTEST_ASSERT_EQ_UINT(line[strlen(line) - 1U], '\n');
    // The console sink runs without a configuration object.
    rec.level = NPH_LOG_LEVEL_DEBUG;
    snprintf(rec.message, sizeof rec.message, "console sink smoke test");
    nph_log_console_sink(&rec, NULL);
    NphLogConsoleSink cfg = {true, true};
    nph_log_console_sink(&rec, &cfg);
    NphLogFileSink missing;
    NTEST_ASSERT_EQ_INT(nph_log_file_sink_open(&missing, "no_such_dir/x/y/z.log"),
                        NPH_RESULT_IO_ERROR);
    nph_log_file_sink(&rec, &missing); // ignored
}

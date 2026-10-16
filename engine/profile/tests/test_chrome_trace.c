// SPDX-License-Identifier: Apache-2.0
// The Chrome Trace Event Format export: the text parses as JSON and holds
// the lanes, the frames, the segments with their times, and the counters.
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>

#include "test_profile_support.h"

#include <stdio.h>

#if NPH_PROFILER_ENABLED

NTEST_SETUP(ProfilerFixture)
{
    profiler_fixture_setup(f, 4096U, NULL);
}

NTEST_TEARDOWN(ProfilerFixture)
{
    profiler_fixture_teardown(f);
}

// Builds two frames: 1000..2000 and 2000..3500 ns, with zones and a counter.
static void build_history(NphProfiler *p)
{
    rec(EV_FRAME(41U, 1000U));
    rec(EV_BEGIN("update \"quoted\"", 1100U));
    rec(EV_BEGIN("physics", 1200U));
    rec(EV_END(1700U));
    rec(EV_END(1800U));
    rec_counter("entities", 250.0, 1900U);
    rec(EV_FRAME(42U, 2000U));
    rec(EV_BEGIN("render", 2500U));
    rec(EV_END(3250U));
    rec(EV_FRAME(43U, 3500U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(p), 2U);
}

static const NphJsonValue *find_event(const NphJsonValue *events, const char *name, const char *ph)
{
    NPH_JSON_FOREACH(e, events)
    {
        if (nph_str_equals(nph_json_get_str(e, NPH_STR("name"), NPH_STR("")),
                           nph_str_from_cstr(name)) &&
            nph_str_equals(nph_json_get_str(e, NPH_STR("ph"), NPH_STR("")),
                           nph_str_from_cstr(ph))) {
            return e;
        }
    }
    return NULL;
}

NTEST_F(ProfilerFixture, the_trace_parses_and_holds_every_record)
{
    build_history(f->p);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_debug_allocator_allocator(&f->da));
    NTEST_ASSERT_EQ_INT(nph_profiler_write_chrome_trace(f->p, 0U, &sb), NPH_RESULT_OK);
    NphJsonDoc doc;
    NTEST_ASSERT_EQ_INT(
        nph_json_parse(nph_sb_str(&sb), nph_debug_allocator_allocator(&f->da), &doc),
        NPH_RESULT_OK);
    const NphJsonValue *events = nph_json_object_get(doc.root, NPH_STR("traceEvents"));
    NTEST_ASSERT_NOT_NULL(events);
    NTEST_ASSERT_EQ_UINT(events->type, NPH_JSON_ARRAY);
    NTEST_ASSERT_TRUE(nph_str_equals(
        nph_json_get_str(doc.root, NPH_STR("displayTimeUnit"), NPH_STR("")), NPH_STR("ms")));

    // Lanes: the main thread named, with a sort index.
    const NphJsonValue *lane = find_event(events, "thread_name", "M");
    NTEST_ASSERT_NOT_NULL(lane);
    const NphJsonValue *args = nph_json_object_get(lane, NPH_STR("args"));
    NTEST_ASSERT_TRUE(
        nph_str_equals(nph_json_get_str(args, NPH_STR("name"), NPH_STR("")), NPH_STR("main")));
    NTEST_ASSERT_NOT_NULL(find_event(events, "thread_sort_index", "M"));
    NTEST_ASSERT_NOT_NULL(find_event(events, "process_name", "M"));

    // Frames as global instants, in microseconds from the first frame.
    const NphJsonValue *f41 = find_event(events, "frame 41", "i");
    const NphJsonValue *f42 = find_event(events, "frame 42", "i");
    NTEST_ASSERT_NOT_NULL(f41);
    NTEST_ASSERT_NOT_NULL(f42);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(f41, NPH_STR("ts"), -1.0), 0.0, 1e-9);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(f42, NPH_STR("ts"), -1.0), 1.0, 1e-9);
    NTEST_ASSERT_NULL(find_event(events, "frame 43", "i")); // not complete

    // Segments as complete events; names escaped.
    const NphJsonValue *update = find_event(events, "update \"quoted\"", "X");
    NTEST_ASSERT_NOT_NULL(update);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(update, NPH_STR("ts"), -1.0), 0.1, 1e-9);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(update, NPH_STR("dur"), -1.0), 0.7, 1e-9);
    const NphJsonValue *physics = find_event(events, "physics", "X");
    NTEST_ASSERT_NOT_NULL(physics);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(physics, NPH_STR("dur"), -1.0), 0.5, 1e-9);
    NTEST_ASSERT_EQ_INT(nph_json_get_i64(physics, NPH_STR("tid"), -1),
                        nph_json_get_i64(update, NPH_STR("tid"), -2));
    const NphJsonValue *render = find_event(events, "render", "X");
    NTEST_ASSERT_NOT_NULL(render);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(render, NPH_STR("ts"), -1.0), 1.5, 1e-9);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(render, NPH_STR("dur"), -1.0), 0.75, 1e-9);

    // Counters, at their frame's start.
    const NphJsonValue *counter = find_event(events, "entities", "C");
    NTEST_ASSERT_NOT_NULL(counter);
    NTEST_ASSERT_NEAR_F64(
        nph_json_get_f64(nph_json_object_get(counter, NPH_STR("args")), NPH_STR("value"), 0.0),
        250.0, 0.0);
    nph_json_doc_free(&doc);

    // Only the newest frame.
    nph_sb_clear(&sb);
    NTEST_ASSERT_EQ_INT(nph_profiler_write_chrome_trace(f->p, 1U, &sb), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_json_parse(nph_sb_str(&sb), nph_debug_allocator_allocator(&f->da), &doc),
        NPH_RESULT_OK);
    events = nph_json_object_get(doc.root, NPH_STR("traceEvents"));
    NTEST_ASSERT_NULL(find_event(events, "frame 41", "i"));
    const NphJsonValue *only = find_event(events, "render", "X");
    NTEST_ASSERT_NOT_NULL(only);
    NTEST_ASSERT_NEAR_F64(nph_json_get_f64(only, NPH_STR("ts"), -1.0), 0.5, 1e-9);
    nph_json_doc_free(&doc);
    nph_sb_free(&sb);
}

NTEST_F(ProfilerFixture, an_empty_history_is_still_a_valid_trace)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_debug_allocator_allocator(&f->da));
    NTEST_ASSERT_EQ_INT(nph_profiler_write_chrome_trace(f->p, 0U, &sb), NPH_RESULT_OK);
    NphJsonDoc doc;
    NTEST_ASSERT_EQ_INT(
        nph_json_parse(nph_sb_str(&sb), nph_debug_allocator_allocator(&f->da), &doc),
        NPH_RESULT_OK);
    nph_json_doc_free(&doc);
    nph_sb_free(&sb);
}

NTEST_F(ProfilerFixture, the_trace_is_saved_to_a_file)
{
    build_history(f->p);
    char dir[512];
    u64 len = 0;
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(dir, sizeof dir, &len), NPH_RESULT_OK);
    char path[600];
    (void)snprintf(path, sizeof path, "%s/nph_profile_trace_test.json", dir);
    NTEST_ASSERT_EQ_INT(
        nph_profiler_save_chrome_trace(f->p, 0U, nph_debug_allocator_allocator(&f->da), path),
        NPH_RESULT_OK);
    void *data = NULL;
    u64 size = 0;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &data, &size),
                        NPH_RESULT_OK);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_debug_allocator_allocator(&f->da));
    NTEST_ASSERT_EQ_INT(nph_profiler_write_chrome_trace(f->p, 0U, &sb), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, sb.len);
    NTEST_ASSERT_EQ_MEM(data, sb.data, size);
    nph_sb_free(&sb);
    nph_file_free_all(nph_allocator_system(), data, size);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_file(path), NPH_RESULT_OK);
    // An unwritable path reports the file system's error.
    NTEST_ASSERT_NE_INT(nph_profiler_save_chrome_trace(f->p, 0U,
                                                       nph_debug_allocator_allocator(&f->da),
                                                       "/nonexistent-dir/nested/trace.json"),
                        NPH_RESULT_OK);
}

#else

NTEST(chrome_trace, compiled_out)
{
    NTEST_SKIP("the profiler is compiled out of this configuration");
}

#endif

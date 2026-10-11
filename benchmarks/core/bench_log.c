// SPDX-License-Identifier: Apache-2.0
// Logging and string benchmarks (docs/core.md).
//
//   log_write_enabled      NPH_LOG_INFO with a format argument into the thread
//                          ring, flushed to a counting sink every 512 records
//   log_write_disabled     NPH_LOG_DEBUG below the category level (the cost of
//                          a disabled log call)
//   log_flush_per_record   flush cost per record delivered to one sink
//   sb_append_small        string builder appends of short views
//   sb_appendf_int         printf-style append of an integer
//   str_find_64B           substring search in a 64-byte string
//   str_id_16B             nph_str_id of a 16-byte name

#include <nepheline/core/log.h>
#include <nepheline/core/string.h>
#include <nepheline/core/string_id.h>

#include <nbench/nbench.h>

#include <stdlib.h>

NPH_LOG_CATEGORY_DEFINE(LOG_BENCH, "bench", NPH_LOG_LEVEL_INFO);

static u64 g_sink_count;

static void counting_sink(const NphLogRecord *record, void *user)
{
    (void)record;
    (void)user;
    ++g_sink_count;
}

static void log_setup(void)
{
    const NphLogConfig config = {nph_allocator_system(), 1024U, 8U};
    if (nph_log_init(&config) != NPH_RESULT_OK ||
        nph_log_add_sink(counting_sink, NULL, NPH_LOG_LEVEL_TRACE) != NPH_RESULT_OK) {
        abort();
    }
}

NBENCH(log_write_enabled)
{
    log_setup();
    nbench_set_items_per_iteration(state, 512U);
    u32 n = 0U;
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < 512U; ++i) {
            NPH_LOG_INFO(&LOG_BENCH, "frame %u: %u draws", n, i);
        }
        ++n;
        nbench_pause_timing(state);
        nph_log_flush(0U);
        nbench_resume_timing(state);
    }
    nph_log_shutdown();
}

NBENCH(log_write_disabled)
{
    log_setup();
    u32 n = 0U;
    while (nbench_keep_running(state)) {
        NPH_LOG_DEBUG(&LOG_BENCH, "never stored %u", n);
        ++n;
    }
    nbench_do_not_optimize_u64(n);
    nph_log_shutdown();
}

NBENCH(log_flush_per_record)
{
    log_setup();
    nbench_set_items_per_iteration(state, 512U);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        for (u32 i = 0; i < 512U; ++i) {
            NPH_LOG_INFO(&LOG_BENCH, "record %u", i);
        }
        nbench_resume_timing(state);
        nph_log_flush(0U);
    }
    nbench_do_not_optimize_u64(g_sink_count);
    nph_log_shutdown();
}

NBENCH(sb_append_small)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    nbench_set_items_per_iteration(state, 1024U);
    while (nbench_keep_running(state)) {
        nph_sb_clear(&sb);
        for (u32 i = 0; i < 1024U; ++i) {
            if (nph_sb_append(&sb, NPH_STR("item, ")) != NPH_RESULT_OK) {
                abort();
            }
        }
    }
    nbench_do_not_optimize(sb.data);
    nph_sb_free(&sb);
}

NBENCH(sb_appendf_int)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    nbench_set_items_per_iteration(state, 256U);
    while (nbench_keep_running(state)) {
        nph_sb_clear(&sb);
        for (u32 i = 0; i < 256U; ++i) {
            if (nph_sb_appendf(&sb, "%u;", i) != NPH_RESULT_OK) {
                abort();
            }
        }
    }
    nbench_do_not_optimize(sb.data);
    nph_sb_free(&sb);
}

NBENCH(sb_append_u64)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    nbench_set_items_per_iteration(state, 256U);
    while (nbench_keep_running(state)) {
        nph_sb_clear(&sb);
        for (u32 i = 0; i < 256U; ++i) {
            if (nph_sb_append_u64(&sb, (u64)i * 1000003U) != NPH_RESULT_OK) {
                abort();
            }
        }
    }
    nbench_do_not_optimize(sb.data);
    nph_sb_free(&sb);
}

NBENCH(str_find_64B)
{
    const NphStr haystack = NPH_STR("textures/environment/forest/oak_tree_bark_albedo_2k.nph.png");
    const NphStr needle = NPH_STR("albedo");
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_str_find(haystack, needle);
    }
    nbench_do_not_optimize_u64(acc);
}

NBENCH(str_id_16B)
{
    const NphStr name = NPH_STR("TransformMatrix1");
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_str_id(name).value;
    }
    nbench_do_not_optimize_u64(acc);
}

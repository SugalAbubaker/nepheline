// SPDX-License-Identifier: Apache-2.0
// The Chrome Trace Event Format export of the profiler's history (Google,
// "Trace Event Format", 2016; read by chrome://tracing and Perfetto). See
// profiler.h.
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>

#include "profile_internal.h"

#include <stdio.h>

// Appends a time in microseconds, with nanosecond precision, from a tick
// count: integer formatting only, so a trace of a second of frames (a few
// hundred thousand events) is written in milliseconds rather than the
// seconds that printf-style float formatting would take.
static NphResult append_us(NphStrBuilder *out, const NphProfiler *p, u64 ticks)
{
    const u64 ns = (u64)((f64)ticks * 1e9 / (f64)p->tick_hz);
    NPH_TRY(nph_sb_append_u64(out, ns / 1000U));
    const u64 frac = ns % 1000U;
    const char digits[5] = {'.', (char)('0' + (char)(frac / 100U)),
                            (char)('0' + (char)((frac / 10U) % 10U)),
                            (char)('0' + (char)(frac % 10U)), '\0'};
    return nph_sb_append(out, nph_str(digits, 4U));
}

static NphResult append_metadata(NphStrBuilder *out, const char *what, u32 tid, const char *key,
                                 NphStr value, bool string_value)
{
    NPH_TRY(nph_sb_append_cstr(out, "{\"name\":\""));
    NPH_TRY(nph_sb_append_cstr(out, what));
    NPH_TRY(nph_sb_append_cstr(out, "\",\"ph\":\"M\",\"pid\":1,\"tid\":"));
    NPH_TRY(nph_sb_append_u64(out, tid));
    NPH_TRY(nph_sb_append_cstr(out, ",\"args\":{\""));
    NPH_TRY(nph_sb_append_cstr(out, key));
    NPH_TRY(nph_sb_append_cstr(out, "\":"));
    if (string_value) {
        NPH_TRY(nph_json_write_escaped(out, value));
    } else {
        NPH_TRY(nph_sb_append(out, value));
    }
    return nph_sb_append_cstr(out, "}},\n");
}

NphResult nph_profiler_write_chrome_trace(const NphProfiler *p, u32 max_frames, NphStrBuilder *out)
{
    NPH_ASSERT(p != NULL && out != NULL);
    u32 frames = nph_profiler_frame_count(p);
    if (max_frames != 0U && max_frames < frames) {
        frames = max_frames;
    }
    NPH_TRY(nph_sb_append_cstr(out, "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n"));
    NPH_TRY(append_metadata(out, "process_name", 0U, "name", NPH_STR("Nepheline"), true));
    for (u32 lane = 0; lane < p->lane_count; ++lane) {
        NPH_TRY(append_metadata(out, "thread_name", lane + 1U, "name",
                                nph_str_from_cstr(p->lanes[lane].name), true));
        char index[16];
        const int len = snprintf(index, sizeof index, "%u", lane);
        NPH_TRY(append_metadata(out, "thread_sort_index", lane + 1U, "sort_index",
                                nph_str(index, (u64)len), false));
    }
    const FrameRecord *oldest = profiler_frame_record(p, frames > 0U ? frames - 1U : 0U);
    const u64 origin = oldest != NULL ? oldest->start : 0U;
    for (u32 age = frames; age > 0U; --age) {
        const FrameRecord *f = profiler_frame_record(p, age - 1U);
        NPH_ASSERT(f != NULL);
        NPH_TRY(nph_sb_append_cstr(out, "{\"name\":\"frame "));
        NPH_TRY(nph_sb_append_u64(out, f->index));
        NPH_TRY(nph_sb_append_cstr(out, "\",\"ph\":\"i\",\"s\":\"g\",\"pid\":1,\"tid\":0,\"ts\":"));
        NPH_TRY(append_us(out, p, f->start - origin));
        NPH_TRY(nph_sb_append_cstr(out, "},\n"));
        for (u32 i = 0; i < f->segment_count; ++i) {
            const NphProfileSegment *s = profiler_segment(p, f->first_segment + i);
            NPH_TRY(nph_sb_append_cstr(out, "{\"name\":"));
            NPH_TRY(nph_json_write_escaped(out, nph_str_from_cstr(nph_profiler_name(p, s->name))));
            NPH_TRY(nph_sb_append_cstr(out, ",\"ph\":\"X\",\"pid\":1,\"tid\":"));
            NPH_TRY(nph_sb_append_u64(out, (u64)s->lane + 1U));
            NPH_TRY(nph_sb_append_cstr(out, ",\"ts\":"));
            NPH_TRY(append_us(out, p, s->start - origin));
            NPH_TRY(nph_sb_append_cstr(out, ",\"dur\":"));
            NPH_TRY(append_us(out, p, s->end - s->start));
            NPH_TRY(nph_sb_append_cstr(out, "},\n"));
        }
        // The counters' last samples in this frame, at its start.
        const u64 slot = (p->frame_total - age) % p->history;
        for (u32 name = 0; name < p->name_count; ++name) {
            const u64 i = (u64)name * p->history + slot;
            if (p->counter_present[i] == 0U) {
                continue;
            }
            NPH_TRY(nph_sb_append_cstr(out, "{\"name\":"));
            NPH_TRY(nph_json_write_escaped(out, nph_str_from_cstr(p->names[name])));
            NPH_TRY(nph_sb_append_cstr(out, ",\"ph\":\"C\",\"pid\":1,\"tid\":0,\"ts\":"));
            NPH_TRY(append_us(out, p, f->start - origin));
            NPH_TRY(nph_sb_append_cstr(out, ",\"args\":{\"value\":"));
            NPH_TRY(nph_sb_append_f64(out, p->counter_value[i]));
            NPH_TRY(nph_sb_append_cstr(out, "}},\n"));
        }
    }
    // A last metadata record so every event above can end with a comma.
    NPH_TRY(nph_sb_append_cstr(out,
                               "{\"name\":\"process_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":0,"
                               "\"args\":{\"sort_index\":0}}\n]}\n"));
    return NPH_RESULT_OK;
}

NphResult nph_profiler_save_chrome_trace(const NphProfiler *p, u32 max_frames,
                                         NphAllocator *scratch, const char *path)
{
    NPH_ASSERT(p != NULL && scratch != NULL && path != NULL);
    NphStrBuilder sb;
    nph_sb_init(&sb, scratch);
    NphResult r = nph_profiler_write_chrome_trace(p, max_frames, &sb);
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(path, sb.data, sb.len);
    }
    nph_sb_free(&sb);
    return r;
}

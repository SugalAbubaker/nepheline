// SPDX-License-Identifier: Apache-2.0
// The GPU profiler (render/gpu_profiler.h): a rolling window of GPU times
// per render-graph pass name, fed from nph_gpu_rg_timings.
//
// Each tracked pass has a ring of `history` samples; a frame that does not
// run the pass leaves its ring untouched, so the statistics describe the
// frames that ran it. The frame total is a ring of its own, filled with
// the sum of every pass each frame, and so is the frame's span, from the
// first pass's beginning to the last one's end.
#include <nepheline/core/log.h>
#include <nepheline/platform/file.h>
#include <nepheline/render/gpu_profiler.h>
#include <nepheline/render/renderer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The rings after the passes': the frame total, then the frame's span.
#define TOTALS 2U

typedef struct ProfPass {
    const char *name;
    NphGpuQueueType queue;
    f32 *ring; // history samples, milliseconds
    u32 head;  // next write
    u32 count; // valid samples
    f32 last_ms;
    u64 read_bytes;
    u64 write_bytes;
} ProfPass;

struct NphRenderGpuProfiler {
    NphAllocator *alloc;
    u32 history;
    u32 max_passes;
    ProfPass *passes; // max_passes + TOTALS: then the frame total and the span
    u32 pass_count;
    f32 *rings;   // (max_passes + TOTALS) x history
    f32 *scratch; // history, for percentiles
    u64 frames;
    u64 last_frame;
    b32 has_frame;
};

NphResult nph_render_gpu_profiler_create(const NphRenderGpuProfilerDesc *desc,
                                         NphRenderGpuProfiler **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphRenderGpuProfiler *p = NPH_ALLOC_TYPE(desc->alloc, NphRenderGpuProfiler);
    if (p == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(p, 0, sizeof *p);
    p->alloc = desc->alloc;
    p->history = desc->history != 0U ? desc->history : 240U;
    p->max_passes = desc->max_passes != 0U ? desc->max_passes : 64U;
    const u64 slots = (u64)p->max_passes + TOTALS;
    p->passes = NPH_ALLOC_ARRAY_ZEROED(p->alloc, ProfPass, slots);
    p->rings = NPH_ALLOC_ARRAY_ZEROED(p->alloc, f32, slots * p->history);
    p->scratch = NPH_ALLOC_ARRAY_ZEROED(p->alloc, f32, p->history);
    if (p->passes == NULL || p->rings == NULL || p->scratch == NULL) {
        nph_render_gpu_profiler_destroy(p);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u64 i = 0; i < slots; ++i) {
        p->passes[i].ring = p->rings + i * p->history;
    }
    p->passes[p->max_passes].name = "frame";
    p->passes[p->max_passes + 1U].name = "span";
    *out = p;
    return NPH_RESULT_OK;
}

void nph_render_gpu_profiler_destroy(NphRenderGpuProfiler *p)
{
    if (p == NULL) {
        return;
    }
    const u64 slots = (u64)p->max_passes + TOTALS;
    if (p->passes != NULL) {
        NPH_FREE_ARRAY(p->alloc, ProfPass, p->passes, slots);
    }
    if (p->rings != NULL) {
        NPH_FREE_ARRAY(p->alloc, f32, p->rings, slots * p->history);
    }
    if (p->scratch != NULL) {
        NPH_FREE_ARRAY(p->alloc, f32, p->scratch, p->history);
    }
    NPH_FREE_TYPE(p->alloc, NphRenderGpuProfiler, p);
}

void nph_render_gpu_profiler_reset(NphRenderGpuProfiler *p)
{
    NPH_ASSERT(p != NULL);
    for (u32 i = 0; i < p->max_passes + TOTALS; ++i) {
        p->passes[i].head = 0U;
        p->passes[i].count = 0U;
        p->passes[i].last_ms = 0.0F;
        p->passes[i].read_bytes = 0U;
        p->passes[i].write_bytes = 0U;
    }
    p->frames = 0U;
}

static void push_sample(ProfPass *pass, u32 history, f32 ms)
{
    pass->ring[pass->head] = ms;
    pass->head = (pass->head + 1U) % history;
    if (pass->count < history) {
        ++pass->count;
    }
    pass->last_ms = ms;
}

static ProfPass *find_pass(NphRenderGpuProfiler *p, const char *name)
{
    for (u32 i = 0; i < p->pass_count; ++i) {
        // Pass names are static strings: a pointer match is the common case.
        if (p->passes[i].name == name || strcmp(p->passes[i].name, name) == 0) {
            return &p->passes[i];
        }
    }
    if (p->pass_count == p->max_passes) {
        return NULL;
    }
    ProfPass *pass = &p->passes[p->pass_count++];
    pass->name = name;
    return pass;
}

u32 nph_render_gpu_profiler_update(NphRenderGpuProfiler *p, const NphGpuRenderGraph *graph)
{
    NPH_ASSERT(p != NULL && graph != NULL);
    NphGpuRgPassTiming timings[128];
    u64 frame = 0U;
    const u32 n = nph_gpu_rg_timings(graph, timings, 128U, &frame);
    if (n == 0U || (p->has_frame && frame == p->last_frame)) {
        return 0U;
    }
    p->has_frame = 1;
    p->last_frame = frame;
    // Passes that run several times a frame under one name (the IBL
    // levels) are summed into one sample.
    f32 total = 0.0F;
    u64 total_read = 0U;
    u64 total_write = 0U;
    u64 span_end = 0U;
    u32 recorded = 0U;
    f32 frame_ms[128];
    ProfPass *seen[128];
    u32 seen_count = 0U;
    for (u32 i = 0; i < n; ++i) {
        const f32 ms = (f32)((f64)timings[i].gpu_ns / 1.0e6);
        total += ms;
        // begin_ns counts from the earliest beginning, so the span is the
        // latest end.
        const u64 end = timings[i].begin_ns + timings[i].gpu_ns;
        span_end = end > span_end ? end : span_end;
        total_read += timings[i].read_bytes;
        total_write += timings[i].write_bytes;
        ProfPass *pass = find_pass(p, timings[i].name);
        if (pass == NULL) {
            continue;
        }
        u32 k = 0U;
        while (k < seen_count && seen[k] != pass) {
            ++k;
        }
        if (k == seen_count) {
            seen[seen_count] = pass;
            frame_ms[seen_count] = 0.0F;
            pass->read_bytes = 0U;
            pass->write_bytes = 0U;
            ++seen_count;
        }
        frame_ms[k] += ms;
        pass->queue = timings[i].queue;
        pass->read_bytes += timings[i].read_bytes;
        pass->write_bytes += timings[i].write_bytes;
    }
    for (u32 k = 0; k < seen_count; ++k) {
        push_sample(seen[k], p->history, frame_ms[k]);
        ++recorded;
    }
    ProfPass *sum = &p->passes[p->max_passes];
    push_sample(sum, p->history, total);
    sum->read_bytes = total_read;
    sum->write_bytes = total_write;
    push_sample(&p->passes[p->max_passes + 1U], p->history, (f32)((f64)span_end / 1.0e6));
    p->frames += 1U;
    return recorded;
}

u64 nph_render_gpu_profiler_frames(const NphRenderGpuProfiler *p)
{
    NPH_ASSERT(p != NULL);
    return p->frames;
}

static int compare_f32(const void *a, const void *b)
{
    const f32 x = *(const f32 *)a;
    const f32 y = *(const f32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void pass_stats(NphRenderGpuProfiler *p, const ProfPass *pass, NphRenderPassStats *out)
{
    memset(out, 0, sizeof *out);
    out->name = pass->name;
    out->queue = pass->queue;
    out->samples = pass->count;
    out->last_ms = (f64)pass->last_ms;
    out->read_bytes = pass->read_bytes;
    out->write_bytes = pass->write_bytes;
    if (pass->count == 0U) {
        return;
    }
    f64 sum = 0.0;
    f32 lo = pass->ring[0];
    f32 hi = pass->ring[0];
    for (u32 i = 0; i < pass->count; ++i) {
        const f32 v = pass->ring[i];
        sum += (f64)v;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        p->scratch[i] = v;
    }
    out->mean_ms = sum / (f64)pass->count;
    out->min_ms = (f64)lo;
    out->max_ms = (f64)hi;
    qsort(p->scratch, pass->count, sizeof(f32), compare_f32);
    // Nearest rank: the smallest value with at least 99 % at or below it.
    const u32 rank = (99U * pass->count + 99U) / 100U; // ceil(0.99 n), >= 1
    out->p99_ms = (f64)p->scratch[rank - 1U];
}

u32 nph_render_gpu_profiler_stats(NphRenderGpuProfiler *p, NphRenderPassStats *out, u32 cap)
{
    NPH_ASSERT(p != NULL && (out != NULL || cap == 0U));
    u32 n = 0U;
    for (u32 i = 0; i < p->pass_count && n < cap; ++i) {
        pass_stats(p, &p->passes[i], &out[n++]);
    }
    for (u32 i = 0; i < TOTALS && n < cap; ++i) {
        pass_stats(p, &p->passes[p->max_passes + i], &out[n++]);
    }
    return n;
}

void nph_render_gpu_profiler_log(NphRenderGpuProfiler *p)
{
    NPH_ASSERT(p != NULL);
    NphRenderPassStats stats[64U + TOTALS];
    const u32 n = nph_render_gpu_profiler_stats(p, stats, 64U + TOTALS);
    for (u32 i = 0; i < n; ++i) {
        const NphRenderPassStats *s = &stats[i];
        NPH_LOG_INFO(&NPH_LOG_RENDER,
                     "gpu %-16s mean %7.3f ms  min %7.3f  max %7.3f  p99 %7.3f  (%u frames)  "
                     "declared %.1f MB read, %.1f MB written",
                     s->name, s->mean_ms, s->min_ms, s->max_ms, s->p99_ms, s->samples,
                     (f64)s->read_bytes / 1.0e6, (f64)s->write_bytes / 1.0e6);
    }
}

NphResult nph_render_gpu_profiler_write_json(NphRenderGpuProfiler *p, NphAllocator *alloc,
                                             const char *path)
{
    NPH_ASSERT(p != NULL && alloc != NULL && path != NULL);
    NphRenderPassStats stats[64U + TOTALS];
    const u32 n = nph_render_gpu_profiler_stats(p, stats, 64U + TOTALS);
    const u64 cap = 256U + (u64)n * 384U;
    char *text = NPH_ALLOC_ARRAY(alloc, char, cap);
    if (text == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u64 at = 0U;
    at += (u64)snprintf(text + at, cap - at, "{\n  \"frames\": %llu,\n  \"passes\": [\n",
                        (unsigned long long)p->frames);
    for (u32 i = 0; i < n && at < cap; ++i) {
        const NphRenderPassStats *s = &stats[i];
        at += (u64)snprintf(text + at, cap - at,
                            "    {\"name\": \"%s\", \"samples\": %u, \"mean_ms\": %.6f, "
                            "\"min_ms\": %.6f, \"max_ms\": %.6f, \"p99_ms\": %.6f, "
                            "\"read_bytes\": %llu, \"write_bytes\": %llu}%s\n",
                            s->name, s->samples, s->mean_ms, s->min_ms, s->max_ms, s->p99_ms,
                            (unsigned long long)s->read_bytes, (unsigned long long)s->write_bytes,
                            i + 1U < n ? "," : "");
    }
    if (at < cap) {
        at += (u64)snprintf(text + at, cap - at, "  ]\n}\n");
    }
    NphResult r = NPH_RESULT_OVERFLOW;
    if (at < cap) {
        r = nph_file_write_all(path, text, at);
    }
    NPH_FREE_ARRAY(alloc, char, text, cap);
    return r;
}

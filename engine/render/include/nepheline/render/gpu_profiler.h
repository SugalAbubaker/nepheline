// SPDX-License-Identifier: Apache-2.0
#pragma once

// The GPU profiler (CLAUDE.md §8.6 "a GPU timestamp profiler per
// render-graph pass"; §10.7 "every renderer PR must state its cost in both
// milliseconds and DRAM bytes").
//
// It collects the render graph's per-pass timestamps (and each pass's
// declared footprint, render_graph.h NphGpuRgPassTiming) every frame and
// keeps a rolling window per pass name, from which it reports the mean,
// minimum, maximum, and 99th percentile, the frame total, and a JSON
// report for the benchmarks. A pass that does not run every frame (the
// image-based-lighting passes, for instance) is averaged over the frames
// it ran in.
//
// Costs (§1.4): create allocates the windows (max_passes x history
// floats) and a sort scratch of `history` floats; update and the queries
// allocate nothing; write_json allocates its text from the given
// allocator.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY.

#include <nepheline/gpu/render_graph.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphRenderGpuProfiler NphRenderGpuProfiler;

typedef struct NphRenderGpuProfilerDesc {
    NphAllocator *alloc; // required
    u32 history;         // frames per window (240: one second at the §10 target)
    u32 max_passes;      // distinct pass names tracked (64)
} NphRenderGpuProfilerDesc;

NPH_MUST_USE NphResult nph_render_gpu_profiler_create(const NphRenderGpuProfilerDesc *desc,
                                                      NphRenderGpuProfiler **out);
void nph_render_gpu_profiler_destroy(NphRenderGpuProfiler *p);

// Records the timings of the most recent completed frame of `graph`, once
// per frame after nph_gpu_frame_begin (a frame already recorded is
// ignored). Returns the number of passes recorded.
u32 nph_render_gpu_profiler_update(NphRenderGpuProfiler *p, const NphGpuRenderGraph *graph);

// Discards every window (after a resolution change, for instance).
void nph_render_gpu_profiler_reset(NphRenderGpuProfiler *p);

typedef struct NphRenderPassStats {
    const char *name; // the pass's static name; "frame" for the total
    NphGpuQueueType queue;
    u32 samples; // frames in the window that ran the pass
    f64 last_ms;
    f64 mean_ms;
    f64 min_ms;
    f64 max_ms;
    f64 p99_ms;     // nearest-rank 99th percentile
    u64 read_bytes; // declared footprint in the last frame that ran the pass
    u64 write_bytes;
} NphRenderPassStats;

// Statistics per pass, in the order the passes were first seen, then the
// frame total (the sum of every pass per frame). Returns the number
// written (at most `cap`).
u32 nph_render_gpu_profiler_stats(NphRenderGpuProfiler *p, NphRenderPassStats *out, u32 cap);

// Frames recorded since creation or the last reset.
u64 nph_render_gpu_profiler_frames(const NphRenderGpuProfiler *p);

// Logs one line per pass at info level (render category).
void nph_render_gpu_profiler_log(NphRenderGpuProfiler *p);

// Writes the statistics as JSON ({"frames": n, "passes": [{"name": ...,
// "mean_ms": ..., ...}]}) to `path`. NPH_RESULT_IO_ERROR from the file
// system; NPH_RESULT_OUT_OF_MEMORY when the text cannot be allocated.
NPH_MUST_USE NphResult nph_render_gpu_profiler_write_json(NphRenderGpuProfiler *p,
                                                          NphAllocator *alloc, const char *path);

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
#pragma once

// The profiler: the collecting half of CPU profiling (CLAUDE.md §8.11;
// docs/profile.md; docs/adr/0022-profiling.md).
//
// core/profile.h records zone events into a ring per thread. Once per
// frame the profiler drains every ring, rebuilds the zones, and keeps a
// history of complete frames: per frame, the segments each thread lane
// ran, the frame's duration, and per-zone and per-counter statistics over
// the history window. The editor's timeline and statistics panels read it,
// and it exports the Chrome Trace Event Format (chrome://tracing, Perfetto)
// for external viewing.
//
// How events become segments:
//   - Order. Events of all threads are merged by timestamp. An event newer
//     than the settle window (NphProfilerDesc.settle_ns) waits for the next
//     collection, so an event a thread stamped but had not yet pushed when
//     the rings were drained is still processed in order.
//   - Fibers. A zone stack is kept per fiber (the job system reports every
//     switch, core/profile.h) and per thread for code outside fibers. When
//     a fiber leaves a thread, the pieces its open zones ran there are
//     emitted, and they continue wherever the fiber resumes. A segment is
//     therefore always work one thread really did.
//   - Frames. A FRAME event closes the frame in progress: zones running at
//     that moment are cut there and continue in the next frame, so every
//     segment lies inside exactly one frame.
//   - Repairs. An END with no open zone is dropped, a BEGIN past the depth
//     limit is dropped with its END, and a thread whose ring overflowed has
//     its open zones closed at the next frame boundary. Each repair is
//     counted (NphProfilerStats.anomalies).
//
// Names are interned: a zone name pointer seen for the first time is
// copied, so the history never points into memory that may be freed later.
// Code that frees zone names calls nph_profiler_forget_names first.
//
// Costs (§1.4): create allocates every table from the description; a
// collection allocates only when a thread's pending queue grows (amortised)
// or a new lane or name appears. Collecting costs about the number of
// events times log(threads). The collector records no zone of its own (its
// events would always be too young to settle); the runtime measures the
// call from outside.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY: one thread collects and reads.

#include <nepheline/core/profile.h>
#include <nepheline/core/string.h>
#include <nepheline/profile/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphProfiler NphProfiler;

#define NPH_PROFILER_NO_NAME NPH_U32_MAX

typedef struct NphProfilerDesc {
    NphAllocator *alloc; // required
    u32 history_frames;  // complete frames kept (0: 240, one second at the §10 target)
    u32 max_segments;    // segments kept across the history (0: 262144, 6 MiB)
    u32 max_names;       // distinct zone and counter names (0: 1024)
    u32 max_lanes;       // thread lanes (0: 64)
    u32 max_fibers; // fiber ids tracked; larger ids are treated as threads' own stacks (0: 1024)
    u32 max_depth;  // open zones per fiber or thread (0: 64)
    u32 max_memory; // tracked allocators registered at once (0: 64)
    u64 settle_ns;  // events younger than this wait for the next collection (0: 2 ms)
    u64 tick_hz;    // the time-stamp counter's rate (0: from platform/cpu.h)
} NphProfilerDesc;

// NPH_RESULT_INVALID_ARGUMENT for a missing allocator or a zero-sized table
// after defaults; NPH_RESULT_OUT_OF_MEMORY. Measuring the counter's rate,
// when the CPU does not report it, blocks for about 10 ms.
NPH_MUST_USE NphResult nph_profiler_create(const NphProfilerDesc *desc, NphProfiler **out);

// Releases everything. Safe on NULL. Tracked allocators are not touched.
void nph_profiler_destroy(NphProfiler *p);

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------

// Drains every thread's ring and processes the events older than the settle
// window. Call once per frame. Returns the number of frames completed by
// this call.
u32 nph_profiler_collect(NphProfiler *p);

// Drains and processes every event regardless of its age, then forgets the
// name pointers seen so far (the interned copies stay). For code about to
// free memory that holds zone names: a game module being unloaded, a
// schedule being destroyed. Events recorded later with the old pointers
// would be misnamed, so nothing may still be recording with them.
void nph_profiler_forget_names(NphProfiler *p);

// Discards the history, the statistics, and every open zone. Lanes, names,
// and tracked allocators stay.
void nph_profiler_reset(NphProfiler *p);

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

u64 nph_profiler_tick_hz(const NphProfiler *p);
f64 nph_profiler_ticks_to_ms(const NphProfiler *p, u64 ticks);

// ---------------------------------------------------------------------------
// The history
// ---------------------------------------------------------------------------

typedef enum NphProfileSegmentFlags {
    // The zone began before this piece: at the frame's start or where its
    // fiber resumed.
    NPH_PROFILE_SEGMENT_CONTINUED = 1U << 0U,
    // The zone continues after this piece: in the next frame or where its
    // fiber resumes.
    NPH_PROFILE_SEGMENT_CONTINUES = 1U << 1U,
} NphProfileSegmentFlags;

// A piece of a zone that one thread ran without interruption.
typedef struct NphProfileSegment {
    u64 start; // ticks
    u64 end;
    u32 name; // nph_profiler_name
    u16 lane; // nph_profiler_lane_name
    u8 depth; // 0 for an outermost zone
    u8 flags; // NphProfileSegmentFlags
} NphProfileSegment;

NPH_STATIC_ASSERT(sizeof(NphProfileSegment) == 24U, "a segment is 24 bytes");

typedef struct NphProfilerFrame {
    u64 index; // the value given to nph_profile_frame
    u64 start; // ticks
    u64 end;
    f64 duration_ms;
    u32 segment_count;
    u32 dropped_events; // events threads lost to full rings, seen during this frame
} NphProfilerFrame;

// Complete frames in the history (at most history_frames; fewer when the
// segment ring has overwritten the oldest).
u32 nph_profiler_frame_count(const NphProfiler *p);

// Frame `age` (0: the newest complete frame). false when out of range.
bool nph_profiler_frame(const NphProfiler *p, u32 age, NphProfilerFrame *out);

// Copies up to `cap` segments of frame `age`, in the order they ended, and
// returns how many were copied (0 when out of range).
u32 nph_profiler_frame_segments(const NphProfiler *p, u32 age, NphProfileSegment *out, u32 cap);

// Durations in milliseconds of up to `cap` of the newest frames, oldest
// first, for a frame-time graph. Returns the count.
u32 nph_profiler_frame_times(const NphProfiler *p, f32 *out, u32 cap);

// Lanes are threads, in the order they were first seen.
u32 nph_profiler_lane_count(const NphProfiler *p);
const char *nph_profiler_lane_name(const NphProfiler *p, u32 lane);

// The interned text of a name id, or "?" for an unknown id.
const char *nph_profiler_name(const NphProfiler *p, u32 name);
// The id of an interned name, or NPH_PROFILER_NO_NAME.
u32 nph_profiler_find_name(const NphProfiler *p, NphStr name);

// ---------------------------------------------------------------------------
// Statistics over the history window
// ---------------------------------------------------------------------------

typedef struct NphProfilerZoneStats {
    const char *name;
    u32 name_id;
    u32 frames;          // frames of the window in which the zone ran
    f64 last_ms;         // time spent in it in the newest frame, every thread together
    f64 mean_ms;         // per frame over the window (frames without it count as 0)
    f64 max_ms;          // the largest frame
    f64 calls_per_frame; // zones begun per frame, mean (continued pieces are not calls)
} NphProfilerZoneStats;

// Statistics of the zones that ran in the window, by decreasing mean.
// Returns the number written (at most `cap`); with fewer than
// NphProfilerStats.names entries of room, which zones are kept is by name
// id, not by mean.
u32 nph_profiler_zone_stats(const NphProfiler *p, NphProfilerZoneStats *out, u32 cap);

typedef struct NphProfilerCounterStats {
    const char *name;
    u32 name_id;
    u32 frames; // frames of the window that sampled it
    f64 last;   // the last sample of the newest frame that has one
    f64 mean;   // of each frame's last sample
    f64 min;
    f64 max;
} NphProfilerCounterStats;

// Counters sampled in the window, by name id. Returns the number written.
u32 nph_profiler_counter_stats(const NphProfiler *p, NphProfilerCounterStats *out, u32 cap);

typedef struct NphProfilerStats {
    u64 frames;         // completed since creation or the last reset
    u64 events;         // processed
    u64 segments;       // emitted
    u64 dropped_events; // lost by threads to full rings (core/profile.h)
    u64 anomalies;      // repairs of unbalanced or overflowing streams
    u32 pending;        // events waiting for the settle window
    u32 lanes;
    u32 names;
} NphProfilerStats;

void nph_profiler_stats(const NphProfiler *p, NphProfilerStats *out);

// ---------------------------------------------------------------------------
// Tracked allocators (memory.h): the memory panel's list
// ---------------------------------------------------------------------------

// Registers a tracked allocator for nph_profiler_memory_stats; it must stay
// alive until untracked. NPH_RESULT_FULL at max_memory,
// NPH_RESULT_ALREADY_EXISTS when it is registered.
NPH_MUST_USE NphResult nph_profiler_track_memory(NphProfiler *p, NphTrackedAllocator *tracked);
void nph_profiler_untrack_memory(NphProfiler *p, NphTrackedAllocator *tracked);

// The statistics of every registered allocator, in registration order.
u32 nph_profiler_memory_stats(const NphProfiler *p, NphMemoryStats *out, u32 cap);

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

// Appends the newest `max_frames` complete frames (0: all) to `out` in the
// Chrome Trace Event Format: one complete event ("X") per segment, the
// lanes as threads, a global instant event ("i") per frame start, and the
// counters as counter events ("C"); times in microseconds from the first
// frame's start. NPH_RESULT_OUT_OF_MEMORY when `out` cannot grow.
NPH_MUST_USE NphResult nph_profiler_write_chrome_trace(const NphProfiler *p, u32 max_frames,
                                                       NphStrBuilder *out);

// As nph_profiler_write_chrome_trace, into the file at `path` (text built
// in memory from `scratch` first). NPH_RESULT_IO_ERROR and the other file
// errors from the platform.
NPH_MUST_USE NphResult nph_profiler_save_chrome_trace(const NphProfiler *p, u32 max_frames,
                                                      NphAllocator *scratch, const char *path);

#ifdef __cplusplus
}
#endif

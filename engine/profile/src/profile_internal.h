// SPDX-License-Identifier: Apache-2.0
#pragma once

// The profiler's internal state, shared by the collector (profiler.c) and
// the Chrome trace export (chrome_trace.c). See profiler.h.

#include <nepheline/core/hashmap.h>
#include <nepheline/profile/profiler.h>

#define PROFILER_LANE_NONE NPH_U16_MAX

// A zone that is open on a fiber or a thread stack. `piece_start` is where
// the piece currently running (or about to run, once resumed) began.
typedef struct OpenZone {
    u64 piece_start;
    u32 name;
    u32 continued; // the current piece continues an earlier one
} OpenZone;

typedef struct ZoneStack {
    OpenZone *zones; // max_depth entries
    u32 depth;       // open zones in `zones`
    u32 overflow;    // BEGINs past max_depth whose ENDs are still to be swallowed
    u16 lane;        // the lane running it now, or PROFILER_LANE_NONE while suspended
    u16 reserved;
} ZoneStack;

typedef struct Lane {
    u32 thread_id;
    u32 fiber; // what the thread runs: a fiber id, or NPH_PROFILE_NO_FIBER
    char name[NPH_PROFILE_THREAD_NAME_MAX];
    ZoneStack own;    // the thread's own stack (code outside fibers)
    u64 dropped_seen; // the thread's cumulative drop count at the last drain
    // The thread lost events after the one stamped `lost_after` (the last
    // event drained when the loss was seen): the next frame boundary past
    // that point closes the lane's open zones instead of cutting them.
    b32 lost_events;
    u64 lost_after;
    b32 retired;
    // Events drained but not processed yet, oldest first: [head, len).
    NphProfileEvent *pending;
    u32 pending_head;
    u32 pending_len;
    u32 pending_cap;
    u32 heap_pos; // position in the merge heap, NPH_U32_MAX when absent
} Lane;

typedef struct FrameRecord {
    u64 index;
    u64 start;
    u64 end;
    u64 first_segment; // absolute segment number
    u32 segment_count;
    u32 dropped_events;
} FrameRecord;

struct NphProfiler {
    NphAllocator *alloc;
    u32 history;
    u32 max_segments;
    u32 max_names;
    u32 max_lanes;
    u32 max_fibers;
    u32 max_depth;
    u32 max_memory;
    u64 settle_ticks;
    u64 tick_hz;
    f64 ms_per_tick;

    // Lanes, and the registry slot -> lane cache.
    Lane *lanes;
    u32 lane_count;
    u32 *slot_thread; // per registry slot: the thread id last seen there
    u16 *slot_lane;   // and its lane
    u32 slot_count;

    // Fibers: one stack each, their zones in one block.
    ZoneStack *fibers;
    OpenZone *fiber_zones;

    // Interned names.
    char **names;
    u32 name_count;
    NphHashMap by_pointer; // u64 pointer -> u32 id
    NphHashMap by_text;    // u64 hash of the text -> u32 id

    // Segments, a ring of max_segments; `segment_total` counts every
    // segment ever emitted (the next one's absolute number).
    NphProfileSegment *segments;
    u64 segment_total;

    // Complete frames, a ring of `history`; `frame_total` counts them.
    FrameRecord *frames;
    u64 frame_total;
    FrameRecord open;
    b32 open_valid;
    u32 open_dropped;

    // Zone statistics: per name, a window of per-frame totals.
    f32 *zone_ms;     // max_names x history
    u16 *zone_calls;  // max_names x history
    f64 *zone_sum_ms; // per name, over the window
    u64 *zone_sum_calls;
    u32 *zone_present; // frames of the window with a nonzero total
    // Counter statistics: per name, a window of per-frame last samples.
    f64 *counter_value; // max_names x history
    u8 *counter_present;
    // Per-frame accumulation before a frame closes.
    f64 *acc_ms;
    u32 *acc_calls;
    f64 *acc_counter;
    u8 *acc_counter_set;
    u32 *touched;
    u32 touched_count;
    u8 *touched_flag;

    // The merge heap of lanes with pending events.
    u32 *heap;
    u32 heap_len;

    // The drain batch.
    NphProfileEvent *batch;
    u32 batch_cap;

    NphTrackedAllocator **memory;
    u32 memory_count;

    NphProfilerStats stats;
};

// The frame record `age` frames back from the newest complete one, or NULL
// when it is out of the history or its segments were overwritten.
const FrameRecord *profiler_frame_record(const NphProfiler *p, u32 age);

// Segment number `absolute` of the ring (valid when not overwritten).
NPH_INLINE const NphProfileSegment *profiler_segment(const NphProfiler *p, u64 absolute)
{
    return &p->segments[absolute % p->max_segments];
}

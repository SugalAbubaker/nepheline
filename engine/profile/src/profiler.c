// SPDX-License-Identifier: Apache-2.0
// The profiler's collector: drains the per-thread rings, merges the events
// by timestamp, rebuilds zones per fiber, cuts them at frame boundaries,
// and keeps the history and its statistics. See profiler.h and
// docs/profile.md.
#include <nepheline/core/assert.h>
#include <nepheline/platform/cpu.h>

#include "profile_internal.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_HISTORY  240U
#define DEFAULT_SEGMENTS 262144U
#define DEFAULT_NAMES    1024U
#define DEFAULT_LANES    64U
#define DEFAULT_FIBERS   1024U
#define DEFAULT_DEPTH    64U
#define DEFAULT_MEMORY   64U
#define DEFAULT_SETTLE   (2ULL * 1000000ULL) // 2 ms
#define DRAIN_BATCH      1024U
#define MAX_DEPTH_LIMIT  255U // depths are stored in a u8

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

static void profiler_free(NphProfiler *p)
{
    NphAllocator *a = p->alloc;
    if (p->lanes != NULL) {
        for (u32 i = 0; i < p->lane_count; ++i) {
            if (p->lanes[i].pending != NULL) {
                NPH_FREE_ARRAY(a, NphProfileEvent, p->lanes[i].pending, p->lanes[i].pending_cap);
            }
        }
        if (p->lanes[0].own.zones != NULL) {
            NPH_FREE_ARRAY(a, OpenZone, p->lanes[0].own.zones, (u64)p->max_lanes * p->max_depth);
        }
        NPH_FREE_ARRAY(a, Lane, p->lanes, p->max_lanes);
    }
    if (p->names != NULL) {
        for (u32 i = 0; i < p->name_count; ++i) {
            nph_str_free(a, p->names[i], strlen(p->names[i]));
        }
        NPH_FREE_ARRAY(a, char *, p->names, p->max_names);
    }
    nph_hashmap_free(&p->by_pointer);
    nph_hashmap_free(&p->by_text);
    const u64 window = (u64)p->max_names * p->history;
    NPH_FREE_ARRAY(a, u32, p->slot_thread, NPH_PROFILE_MAX_THREADS);
    NPH_FREE_ARRAY(a, u16, p->slot_lane, NPH_PROFILE_MAX_THREADS);
    NPH_FREE_ARRAY(a, ZoneStack, p->fibers, p->max_fibers);
    NPH_FREE_ARRAY(a, OpenZone, p->fiber_zones, (u64)p->max_fibers * p->max_depth);
    NPH_FREE_ARRAY(a, NphProfileSegment, p->segments, p->max_segments);
    NPH_FREE_ARRAY(a, FrameRecord, p->frames, p->history);
    NPH_FREE_ARRAY(a, f32, p->zone_ms, window);
    NPH_FREE_ARRAY(a, u16, p->zone_calls, window);
    NPH_FREE_ARRAY(a, f64, p->zone_sum_ms, p->max_names);
    NPH_FREE_ARRAY(a, u64, p->zone_sum_calls, p->max_names);
    NPH_FREE_ARRAY(a, u32, p->zone_present, p->max_names);
    NPH_FREE_ARRAY(a, f64, p->counter_value, window);
    NPH_FREE_ARRAY(a, u8, p->counter_present, window);
    NPH_FREE_ARRAY(a, f64, p->acc_ms, p->max_names);
    NPH_FREE_ARRAY(a, u32, p->acc_calls, p->max_names);
    NPH_FREE_ARRAY(a, f64, p->acc_counter, p->max_names);
    NPH_FREE_ARRAY(a, u8, p->acc_counter_set, p->max_names);
    NPH_FREE_ARRAY(a, u32, p->touched, p->max_names);
    NPH_FREE_ARRAY(a, u8, p->touched_flag, p->max_names);
    NPH_FREE_ARRAY(a, u32, p->heap, p->max_lanes);
    NPH_FREE_ARRAY(a, NphProfileEvent, p->batch, p->batch_cap);
    NPH_FREE_ARRAY(a, NphTrackedAllocator *, p->memory, p->max_memory);
    NPH_FREE_TYPE(a, NphProfiler, p);
}

static void reset_stack(ZoneStack *s, u16 lane)
{
    s->depth = 0U;
    s->overflow = 0U;
    s->lane = lane;
}

NphResult nph_profiler_create(const NphProfilerDesc *desc, NphProfiler **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 depth = desc->max_depth != 0U ? desc->max_depth : DEFAULT_DEPTH;
    const u32 lanes = desc->max_lanes != 0U ? desc->max_lanes : DEFAULT_LANES;
    if (depth > MAX_DEPTH_LIMIT || lanes >= PROFILER_LANE_NONE) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u64 hz = desc->tick_hz;
    if (hz == 0U) {
        hz = nph_cpu_info()->tsc_frequency_hz;
    }
    if (hz == 0U) {
        hz = nph_cpu_measure_tsc_hz(0U);
    }
    if (hz == 0U) {
        return NPH_RESULT_UNSUPPORTED;
    }

    NphAllocator *a = desc->alloc;
    NphProfiler *p = NPH_ALLOC_TYPE(a, NphProfiler);
    if (p == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    p->alloc = a;
    p->history = desc->history_frames != 0U ? desc->history_frames : DEFAULT_HISTORY;
    p->max_segments = desc->max_segments != 0U ? desc->max_segments : DEFAULT_SEGMENTS;
    p->max_names = desc->max_names != 0U ? desc->max_names : DEFAULT_NAMES;
    p->max_lanes = lanes;
    p->max_fibers = desc->max_fibers != 0U ? desc->max_fibers : DEFAULT_FIBERS;
    p->max_depth = depth;
    p->max_memory = desc->max_memory != 0U ? desc->max_memory : DEFAULT_MEMORY;
    p->tick_hz = hz;
    p->ms_per_tick = 1000.0 / (f64)hz;
    const u64 settle_ns = desc->settle_ns != 0U ? desc->settle_ns : DEFAULT_SETTLE;
    p->settle_ticks = (u64)((f64)settle_ns * (f64)hz / 1e9);
    p->batch_cap = DRAIN_BATCH;

    bool ok = true;
    const u64 window = (u64)p->max_names * p->history;
    p->lanes = NPH_ALLOC_ARRAY_ZEROED(a, Lane, p->max_lanes);
    OpenZone *lane_zones = NPH_ALLOC_ARRAY_ZEROED(a, OpenZone, (u64)p->max_lanes * p->max_depth);
    if (p->lanes != NULL) {
        p->lanes[0].own.zones = lane_zones; // where profiler_free finds the block
    } else if (lane_zones != NULL) {
        NPH_FREE_ARRAY(a, OpenZone, lane_zones, (u64)p->max_lanes * p->max_depth);
    }
    p->slot_thread = NPH_ALLOC_ARRAY_ZEROED(a, u32, NPH_PROFILE_MAX_THREADS);
    p->slot_lane = NPH_ALLOC_ARRAY(a, u16, NPH_PROFILE_MAX_THREADS);
    p->fibers = NPH_ALLOC_ARRAY_ZEROED(a, ZoneStack, p->max_fibers);
    p->fiber_zones = NPH_ALLOC_ARRAY_ZEROED(a, OpenZone, (u64)p->max_fibers * p->max_depth);
    p->names = NPH_ALLOC_ARRAY_ZEROED(a, char *, p->max_names);
    p->segments = NPH_ALLOC_ARRAY_ZEROED(a, NphProfileSegment, p->max_segments);
    p->frames = NPH_ALLOC_ARRAY_ZEROED(a, FrameRecord, p->history);
    p->zone_ms = NPH_ALLOC_ARRAY_ZEROED(a, f32, window);
    p->zone_calls = NPH_ALLOC_ARRAY_ZEROED(a, u16, window);
    p->zone_sum_ms = NPH_ALLOC_ARRAY_ZEROED(a, f64, p->max_names);
    p->zone_sum_calls = NPH_ALLOC_ARRAY_ZEROED(a, u64, p->max_names);
    p->zone_present = NPH_ALLOC_ARRAY_ZEROED(a, u32, p->max_names);
    p->counter_value = NPH_ALLOC_ARRAY_ZEROED(a, f64, window);
    p->counter_present = NPH_ALLOC_ARRAY_ZEROED(a, u8, window);
    p->acc_ms = NPH_ALLOC_ARRAY_ZEROED(a, f64, p->max_names);
    p->acc_calls = NPH_ALLOC_ARRAY_ZEROED(a, u32, p->max_names);
    p->acc_counter = NPH_ALLOC_ARRAY_ZEROED(a, f64, p->max_names);
    p->acc_counter_set = NPH_ALLOC_ARRAY_ZEROED(a, u8, p->max_names);
    p->touched = NPH_ALLOC_ARRAY_ZEROED(a, u32, p->max_names);
    p->touched_flag = NPH_ALLOC_ARRAY_ZEROED(a, u8, p->max_names);
    p->heap = NPH_ALLOC_ARRAY_ZEROED(a, u32, p->max_lanes);
    p->batch = NPH_ALLOC_ARRAY(a, NphProfileEvent, p->batch_cap);
    p->memory = NPH_ALLOC_ARRAY_ZEROED(a, NphTrackedAllocator *, p->max_memory);
    ok = p->lanes != NULL && lane_zones != NULL && p->slot_thread != NULL && p->slot_lane != NULL &&
         p->fibers != NULL && p->fiber_zones != NULL && p->names != NULL && p->segments != NULL &&
         p->frames != NULL && p->zone_ms != NULL && p->zone_calls != NULL &&
         p->zone_sum_ms != NULL && p->zone_sum_calls != NULL && p->zone_present != NULL &&
         p->counter_value != NULL && p->counter_present != NULL && p->acc_ms != NULL &&
         p->acc_calls != NULL && p->acc_counter != NULL && p->acc_counter_set != NULL &&
         p->touched != NULL && p->touched_flag != NULL && p->heap != NULL && p->batch != NULL &&
         p->memory != NULL;
    const NphHashMapDesc map_desc = nph_hashmap_desc_u64((u32)sizeof(u32));
    ok = ok && nph_hashmap_init(&p->by_pointer, a, &map_desc) == NPH_RESULT_OK &&
         nph_hashmap_init(&p->by_text, a, &map_desc) == NPH_RESULT_OK;
    if (!ok) {
        profiler_free(p);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < NPH_PROFILE_MAX_THREADS; ++i) {
        p->slot_lane[i] = PROFILER_LANE_NONE;
    }
    for (u32 i = 0; i < p->max_lanes; ++i) {
        p->lanes[i].own.zones = lane_zones + (u64)i * p->max_depth;
    }
    for (u32 i = 0; i < p->max_fibers; ++i) {
        p->fibers[i].zones = p->fiber_zones + (u64)i * p->max_depth;
        reset_stack(&p->fibers[i], PROFILER_LANE_NONE);
    }
    *out = p;
    return NPH_RESULT_OK;
}

void nph_profiler_destroy(NphProfiler *p)
{
    if (p != NULL) {
        profiler_free(p);
    }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static u32 intern(NphProfiler *p, const char *name)
{
    if (name == NULL) {
        return NPH_PROFILER_NO_NAME;
    }
    const u32 *hit = (const u32 *)nph_hashmap_find_u64(&p->by_pointer, (u64)(uptr)name);
    if (hit != NULL) {
        return *hit;
    }
    const NphStr text = nph_str_from_cstr(name);
    const u64 hash = nph_str_hash(text);
    const u32 *same = (const u32 *)nph_hashmap_find_u64(&p->by_text, hash);
    u32 id = NPH_PROFILER_NO_NAME;
    if (same != NULL && strcmp(p->names[*same], name) == 0) {
        id = *same;
    } else if (p->name_count < p->max_names) {
        char *copy = NULL;
        if (nph_str_dup(p->alloc, text, &copy) != NPH_RESULT_OK) {
            p->stats.anomalies += 1U;
            return NPH_PROFILER_NO_NAME;
        }
        id = p->name_count++;
        p->names[id] = copy;
        if (same == NULL) {
            // A failure only costs a later lookup by pointer.
            NPH_IGNORE_RESULT(nph_hashmap_insert_u64(&p->by_text, hash, &id));
        }
    } else {
        p->stats.anomalies += 1U;
        return NPH_PROFILER_NO_NAME;
    }
    NPH_IGNORE_RESULT(nph_hashmap_insert_u64(&p->by_pointer, (u64)(uptr)name, &id));
    return id;
}

const char *nph_profiler_name(const NphProfiler *p, u32 name)
{
    NPH_ASSERT(p != NULL);
    return name < p->name_count ? p->names[name] : "?";
}

u32 nph_profiler_find_name(const NphProfiler *p, NphStr name)
{
    NPH_ASSERT(p != NULL);
    for (u32 i = 0; i < p->name_count; ++i) {
        if (nph_str_equals(nph_str_from_cstr(p->names[i]), name)) {
            return i;
        }
    }
    return NPH_PROFILER_NO_NAME;
}

// ---------------------------------------------------------------------------
// Segments and stacks
// ---------------------------------------------------------------------------

static void touch(NphProfiler *p, u32 name)
{
    if (p->touched_flag[name] == 0U) {
        p->touched_flag[name] = 1U;
        p->touched[p->touched_count++] = name;
    }
}

// Emits the current piece of `z`, running on `lane` at `depth`, ending at
// `end`; `continues` marks a piece the zone goes on after. Empty pieces of
// a zone that goes on are skipped (a fiber that resumed and left at once).
static void emit(NphProfiler *p, u16 lane, u32 depth, const OpenZone *z, u64 end, bool continues)
{
    const u64 start = z->piece_start;
    const u64 stop = end > start ? end : start;
    if (continues && stop == start) {
        return;
    }
    NphProfileSegment *seg = &p->segments[p->segment_total % p->max_segments];
    seg->start = start;
    seg->end = stop;
    seg->name = z->name;
    seg->lane = lane;
    seg->depth = (u8)depth;
    seg->flags = (u8)((z->continued != 0U ? (u32)NPH_PROFILE_SEGMENT_CONTINUED : 0U) |
                      (continues ? (u32)NPH_PROFILE_SEGMENT_CONTINUES : 0U));
    p->segment_total += 1U;
    p->stats.segments += 1U;
    if (p->open_valid && z->name < p->max_names) {
        touch(p, z->name);
        p->acc_ms[z->name] += (f64)(stop - start) * p->ms_per_tick;
        if (z->continued == 0U) {
            p->acc_calls[z->name] += 1U;
        }
    }
}

static void stack_push(NphProfiler *p, ZoneStack *s, u32 name, u64 t)
{
    if (s->depth >= p->max_depth) {
        s->overflow += 1U;
        p->stats.anomalies += 1U;
        return;
    }
    OpenZone *z = &s->zones[s->depth++];
    z->piece_start = t;
    z->name = name;
    z->continued = 0U;
}

static void stack_pop(NphProfiler *p, ZoneStack *s, u16 lane, u64 t)
{
    if (s->overflow > 0U) {
        s->overflow -= 1U; // the END of a BEGIN that was dropped
        return;
    }
    if (s->depth == 0U) {
        p->stats.anomalies += 1U; // an END with nothing open
        return;
    }
    s->depth -= 1U;
    emit(p, lane, s->depth, &s->zones[s->depth], t, false);
}

// The open zones of `s` stop running on `lane` at `t`: their pieces there
// are emitted, and each continues where the stack resumes.
static void suspend(NphProfiler *p, ZoneStack *s, u16 lane, u64 t)
{
    for (u32 i = 0; i < s->depth; ++i) {
        emit(p, lane, i, &s->zones[i], t, true);
        s->zones[i].continued = 1U;
    }
    s->lane = PROFILER_LANE_NONE;
}

static void resume(ZoneStack *s, u16 lane, u64 t)
{
    for (u32 i = 0; i < s->depth; ++i) {
        s->zones[i].piece_start = t;
    }
    s->lane = lane;
}

static ZoneStack *stack_of(NphProfiler *p, Lane *lane)
{
    return lane->fiber < p->max_fibers ? &p->fibers[lane->fiber] : &lane->own;
}

// The thread of `lane` switched to `fiber` at `t`.
static void switch_fiber(NphProfiler *p, u16 li, u32 fiber, u64 t)
{
    Lane *lane = &p->lanes[li];
    ZoneStack *from = stack_of(p, lane);
    const u32 to_fiber = fiber < p->max_fibers ? fiber : NPH_PROFILE_NO_FIBER;
    ZoneStack *to = to_fiber < p->max_fibers ? &p->fibers[to_fiber] : &lane->own;
    if (from == to) {
        return;
    }
    suspend(p, from, li, t);
    lane->fiber = to_fiber;
    if (to->lane != PROFILER_LANE_NONE && to->lane != li) {
        // The fiber still runs on another lane: that thread's switch away
        // from it has not been seen (out of order, or lost to a full ring).
        // End its pieces there, and give that lane back its own stack.
        const u16 other = to->lane;
        suspend(p, to, other, t);
        if (p->lanes[other].fiber == to_fiber) {
            p->lanes[other].fiber = NPH_PROFILE_NO_FIBER;
            resume(&p->lanes[other].own, other, t);
        }
        p->stats.anomalies += 1U;
    }
    resume(to, li, t);
}

// ---------------------------------------------------------------------------
// Frames and statistics
// ---------------------------------------------------------------------------

static void recompute_sums(NphProfiler *p)
{
    const u32 window = (u32)nph_min_u64(p->frame_total + 1U, p->history);
    for (u32 n = 0; n < p->name_count; ++n) {
        f64 sum = 0.0;
        u64 calls = 0U;
        u32 present = 0U;
        for (u32 s = 0; s < window; ++s) {
            const u64 i = (u64)n * p->history + s;
            sum += (f64)p->zone_ms[i];
            calls += p->zone_calls[i];
            present += p->zone_ms[i] > 0.0F || p->zone_calls[i] > 0U ? 1U : 0U;
        }
        p->zone_sum_ms[n] = sum;
        p->zone_sum_calls[n] = calls;
        p->zone_present[n] = present;
    }
}

// Moves the open frame's accumulations into window slot `slot`.
static void finalize_stats(NphProfiler *p, u32 slot)
{
    for (u32 n = 0; n < p->name_count; ++n) {
        const u64 i = (u64)n * p->history + slot;
        if (p->zone_ms[i] > 0.0F || p->zone_calls[i] > 0U) {
            p->zone_sum_ms[n] -= (f64)p->zone_ms[i];
            p->zone_sum_calls[n] -= p->zone_calls[i];
            p->zone_present[n] -= 1U;
        }
        p->zone_ms[i] = 0.0F;
        p->zone_calls[i] = 0U;
        p->counter_present[i] = 0U;
    }
    for (u32 t = 0; t < p->touched_count; ++t) {
        const u32 n = p->touched[t];
        const u64 i = (u64)n * p->history + slot;
        if (p->acc_ms[n] > 0.0 || p->acc_calls[n] > 0U) {
            p->zone_ms[i] = (f32)p->acc_ms[n];
            p->zone_calls[i] = (u16)nph_min_u32(p->acc_calls[n], NPH_U16_MAX);
            p->zone_sum_ms[n] += (f64)p->zone_ms[i];
            p->zone_sum_calls[n] += p->zone_calls[i];
            p->zone_present[n] += 1U;
        }
        if (p->acc_counter_set[n] != 0U) {
            p->counter_value[i] = p->acc_counter[n];
            p->counter_present[i] = 1U;
        }
        p->acc_ms[n] = 0.0;
        p->acc_calls[n] = 0U;
        p->acc_counter_set[n] = 0U;
        p->touched_flag[n] = 0U;
    }
    p->touched_count = 0U;
    // The running sums are adjusted by subtraction; recomputing them once
    // per trip around the window bounds the floating-point drift.
    if (slot + 1U == p->history) {
        recompute_sums(p);
    }
}

static void clear_accumulation(NphProfiler *p)
{
    for (u32 t = 0; t < p->touched_count; ++t) {
        const u32 n = p->touched[t];
        p->acc_ms[n] = 0.0;
        p->acc_calls[n] = 0U;
        p->acc_counter_set[n] = 0U;
        p->touched_flag[n] = 0U;
    }
    p->touched_count = 0U;
}

static u32 frame_boundary(NphProfiler *p, u64 index, u64 t)
{
    // Cut every running zone at the boundary. A lane whose thread lost
    // events since the last boundary has its zones closed instead: their
    // ENDs may be among the lost.
    for (u32 l = 0; l < p->lane_count; ++l) {
        const u16 li = (u16)l;
        Lane *lane = &p->lanes[li];
        ZoneStack *s = stack_of(p, lane);
        if (lane->lost_events && t > lane->lost_after) {
            while (s->depth > 0U) {
                stack_pop(p, s, li, t);
                p->stats.anomalies += 1U;
            }
            s->overflow = 0U;
            lane->lost_events = 0;
            continue;
        }
        for (u32 i = 0; i < s->depth; ++i) {
            OpenZone *z = &s->zones[i];
            emit(p, li, i, z, t, true);
            z->piece_start = t;
            z->continued = 1U;
        }
    }
    u32 completed = 0U;
    if (p->open_valid) {
        FrameRecord *slot = &p->frames[p->frame_total % p->history];
        *slot = p->open;
        slot->end = t > p->open.start ? t : p->open.start;
        slot->segment_count =
            (u32)nph_min_u64(p->segment_total - p->open.first_segment, p->max_segments);
        slot->dropped_events = p->open_dropped;
        finalize_stats(p, (u32)(p->frame_total % p->history));
        p->frame_total += 1U;
        p->stats.frames += 1U;
        completed = 1U;
    } else {
        clear_accumulation(p); // pieces from before the first frame
    }
    p->open.index = index;
    p->open.start = t;
    p->open.end = t;
    p->open.first_segment = p->segment_total;
    p->open.segment_count = 0U;
    p->open.dropped_events = 0U;
    p->open_valid = 1;
    p->open_dropped = 0U;
    return completed;
}

static void counter_sample(NphProfiler *p, u32 name, u64 bits)
{
    if (!p->open_valid || name >= p->max_names) {
        return;
    }
    f64 value;
    memcpy(&value, &bits, sizeof value);
    touch(p, name);
    p->acc_counter[name] = value;
    p->acc_counter_set[name] = 1U;
}

static u32 process(NphProfiler *p, u16 li, const NphProfileEvent *e)
{
    Lane *lane = &p->lanes[li];
    p->stats.events += 1U;
    switch (e->type) {
    case NPH_PROFILE_EVENT_BEGIN:
        stack_push(p, stack_of(p, lane), intern(p, e->name), e->ticks);
        return 0U;
    case NPH_PROFILE_EVENT_END:
        stack_pop(p, stack_of(p, lane), li, e->ticks);
        return 0U;
    case NPH_PROFILE_EVENT_FIBER:
        switch_fiber(p, li, (u32)nph_min_u64(e->value, NPH_PROFILE_NO_FIBER), e->ticks);
        return 0U;
    case NPH_PROFILE_EVENT_FRAME:
        return frame_boundary(p, e->value, e->ticks);
    case NPH_PROFILE_EVENT_COUNTER:
        counter_sample(p, intern(p, e->name), e->value);
        return 0U;
    default:
        p->stats.anomalies += 1U;
        return 0U;
    }
}

// ---------------------------------------------------------------------------
// Draining and merging
// ---------------------------------------------------------------------------

static Lane *lane_for(NphProfiler *p, u32 slot, const NphProfileThreadInfo *info, u16 *out_index)
{
    if (p->slot_thread[slot] == info->id && p->slot_lane[slot] != PROFILER_LANE_NONE) {
        *out_index = p->slot_lane[slot];
        return &p->lanes[*out_index];
    }
    NPH_ASSERT(p->lanes != NULL);
    u16 index = PROFILER_LANE_NONE;
    for (u32 i = 0; i < p->lane_count; ++i) {
        if (p->lanes[i].thread_id == info->id) {
            index = (u16)i;
            break;
        }
    }
    if (index == PROFILER_LANE_NONE) {
        if (p->lane_count >= p->max_lanes) {
            return NULL;
        }
        index = (u16)p->lane_count++;
        Lane *lane = &p->lanes[index];
        lane->thread_id = info->id;
        lane->fiber = NPH_PROFILE_NO_FIBER;
        reset_stack(&lane->own, index);
        lane->heap_pos = NPH_U32_MAX;
    }
    p->slot_thread[slot] = info->id;
    p->slot_lane[slot] = index;
    *out_index = index;
    return &p->lanes[index];
}

static bool append_pending(NphProfiler *p, Lane *lane, const NphProfileEvent *events, u32 n)
{
    if (lane->pending_head == lane->pending_len) {
        lane->pending_head = 0U;
        lane->pending_len = 0U;
    } else if (lane->pending_head > 0U && lane->pending_len + n > lane->pending_cap) {
        const u32 live = lane->pending_len - lane->pending_head;
        memmove(lane->pending, lane->pending + lane->pending_head, (u64)live * sizeof *events);
        lane->pending_head = 0U;
        lane->pending_len = live;
    }
    if (lane->pending_len + n > lane->pending_cap) {
        u32 cap = lane->pending_cap != 0U ? lane->pending_cap : 1024U;
        while (cap < lane->pending_len + n) {
            cap *= 2U;
        }
        NphProfileEvent *grown = (NphProfileEvent *)NPH_REALLOC(
            p->alloc, lane->pending, (u64)lane->pending_cap * sizeof(NphProfileEvent),
            (u64)cap * sizeof(NphProfileEvent), NPH_ALIGNOF(NphProfileEvent));
        if (grown == NULL) {
            return false;
        }
        lane->pending = grown;
        lane->pending_cap = cap;
    }
    memcpy(lane->pending + lane->pending_len, events, (u64)n * sizeof *events);
    lane->pending_len += n;
    return true;
}

// Drains every ring into the lanes' pending queues. Returns the number of
// events the threads lost since the previous drain.
static u64 drain(NphProfiler *p)
{
    u64 lost_total = 0U;
    const u32 slots = nph_profile_thread_slots();
    for (u32 slot = 0; slot < slots && slot < NPH_PROFILE_MAX_THREADS; ++slot) {
        for (;;) {
            NphProfileThreadInfo info;
            const u32 n = nph_profile_drain(slot, p->batch, p->batch_cap, &info);
            if (info.id == 0U) {
                break;
            }
            u16 li = PROFILER_LANE_NONE;
            Lane *lane = lane_for(p, slot, &info, &li);
            if (lane == NULL) {
                p->stats.anomalies += n; // more threads than lanes
            } else {
                memcpy(lane->name, info.name, sizeof lane->name);
                lane->retired = info.retired;
                bool new_loss = false;
                if (info.dropped > lane->dropped_seen) {
                    const u64 lost = info.dropped - lane->dropped_seen;
                    lane->dropped_seen = info.dropped;
                    lane->lost_events = 1;
                    p->stats.dropped_events += lost;
                    lost_total += lost;
                    new_loss = true;
                }
                if (n > 0U && !append_pending(p, lane, p->batch, n)) {
                    p->stats.anomalies += n;
                    lane->lost_events = 1;
                    new_loss = true;
                }
                if (new_loss) {
                    // The gap follows everything drained so far.
                    lane->lost_after = lane->pending_len > lane->pending_head
                                           ? lane->pending[lane->pending_len - 1U].ticks
                                           : lane->lost_after;
                }
            }
            if (n < p->batch_cap) {
                break;
            }
        }
    }
    return lost_total;
}

// The merge heap holds lanes whose next event is ready, keyed by that
// event's timestamp (then the lane, for a deterministic order).
static bool heap_less(const NphProfiler *p, u32 a, u32 b)
{
    const Lane *la = &p->lanes[a];
    const Lane *lb = &p->lanes[b];
    const u64 ta = la->pending[la->pending_head].ticks;
    const u64 tb = lb->pending[lb->pending_head].ticks;
    return ta != tb ? ta < tb : a < b;
}

static void heap_sift_down(NphProfiler *p, u32 i)
{
    for (;;) {
        const u32 l = 2U * i + 1U;
        const u32 r = l + 1U;
        u32 m = i;
        if (l < p->heap_len && heap_less(p, p->heap[l], p->heap[m])) {
            m = l;
        }
        if (r < p->heap_len && heap_less(p, p->heap[r], p->heap[m])) {
            m = r;
        }
        if (m == i) {
            return;
        }
        const u32 t = p->heap[i];
        p->heap[i] = p->heap[m];
        p->heap[m] = t;
        i = m;
    }
}

static void heap_push(NphProfiler *p, u32 lane)
{
    u32 i = p->heap_len++;
    p->heap[i] = lane;
    while (i > 0U) {
        const u32 parent = (i - 1U) / 2U;
        if (!heap_less(p, p->heap[i], p->heap[parent])) {
            break;
        }
        const u32 t = p->heap[i];
        p->heap[i] = p->heap[parent];
        p->heap[parent] = t;
        i = parent;
    }
}

static bool ready(const Lane *lane, u64 cutoff, bool force)
{
    return lane->pending_head < lane->pending_len &&
           (force || lane->pending[lane->pending_head].ticks < cutoff);
}

static u32 merge(NphProfiler *p, u64 cutoff, bool force)
{
    p->heap_len = 0U;
    for (u32 i = 0; i < p->lane_count; ++i) {
        if (ready(&p->lanes[i], cutoff, force)) {
            heap_push(p, i);
        }
    }
    u32 completed = 0U;
    while (p->heap_len > 0U) {
        const u32 li = p->heap[0];
        Lane *lane = &p->lanes[li];
        const NphProfileEvent e = lane->pending[lane->pending_head++];
        completed += process(p, (u16)li, &e);
        if (ready(lane, cutoff, force)) {
            heap_sift_down(p, 0U);
        } else {
            p->heap[0] = p->heap[--p->heap_len];
            if (p->heap_len > 0U) {
                heap_sift_down(p, 0U);
            }
        }
    }
    u32 pending = 0U;
    for (u32 i = 0; i < p->lane_count; ++i) {
        Lane *lane = &p->lanes[i];
        if (lane->pending_head == lane->pending_len) {
            lane->pending_head = 0U;
            lane->pending_len = 0U;
        }
        pending += lane->pending_len - lane->pending_head;
    }
    p->stats.pending = pending;
    return completed;
}

static u32 collect(NphProfiler *p, bool force)
{
    const u64 now = nph_profile_ticks();
    const u64 cutoff = now > p->settle_ticks ? now - p->settle_ticks : 0U;
    const u64 lost = drain(p);
    const u32 completed = merge(p, cutoff, force);
    // Losses are charged to the frame in progress once the drained events
    // are in: the frame the threads were recording when they ran out.
    p->open_dropped += (u32)nph_min_u64(lost, NPH_U32_MAX - p->open_dropped);
    return completed;
}

u32 nph_profiler_collect(NphProfiler *p)
{
    NPH_ASSERT(p != NULL);
    return collect(p, false);
}

void nph_profiler_forget_names(NphProfiler *p)
{
    NPH_ASSERT(p != NULL);
    (void)collect(p, true);
    nph_hashmap_clear(&p->by_pointer);
}

void nph_profiler_reset(NphProfiler *p)
{
    NPH_ASSERT(p != NULL);
    for (u32 i = 0; i < p->lane_count; ++i) {
        Lane *lane = &p->lanes[i];
        lane->fiber = NPH_PROFILE_NO_FIBER;
        reset_stack(&lane->own, (u16)i);
        lane->lost_events = 0;
        lane->pending_head = 0U;
        lane->pending_len = 0U;
    }
    for (u32 i = 0; i < p->max_fibers; ++i) {
        reset_stack(&p->fibers[i], PROFILER_LANE_NONE);
    }
    const u64 window = (u64)p->max_names * p->history;
    memset(p->zone_ms, 0, window * sizeof *p->zone_ms);
    memset(p->zone_calls, 0, window * sizeof *p->zone_calls);
    memset(p->counter_present, 0, window * sizeof *p->counter_present);
    memset(p->zone_sum_ms, 0, (u64)p->max_names * sizeof *p->zone_sum_ms);
    memset(p->zone_sum_calls, 0, (u64)p->max_names * sizeof *p->zone_sum_calls);
    memset(p->zone_present, 0, (u64)p->max_names * sizeof *p->zone_present);
    clear_accumulation(p);
    p->frame_total = 0U;
    p->open_valid = 0;
    p->open_dropped = 0U;
    p->stats.frames = 0U;
    p->stats.pending = 0U;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

u64 nph_profiler_tick_hz(const NphProfiler *p)
{
    NPH_ASSERT(p != NULL);
    return p->tick_hz;
}

f64 nph_profiler_ticks_to_ms(const NphProfiler *p, u64 ticks)
{
    NPH_ASSERT(p != NULL);
    return (f64)ticks * p->ms_per_tick;
}

static u32 valid_frames(const NphProfiler *p)
{
    const u64 oldest_segment =
        p->segment_total > p->max_segments ? p->segment_total - p->max_segments : 0U;
    const u32 kept = (u32)nph_min_u64(p->frame_total, p->history);
    u32 n = 0;
    while (n < kept) {
        const FrameRecord *f = &p->frames[(p->frame_total - 1U - n) % p->history];
        if (f->first_segment < oldest_segment) {
            break;
        }
        ++n;
    }
    return n;
}

const FrameRecord *profiler_frame_record(const NphProfiler *p, u32 age)
{
    if (age >= valid_frames(p)) {
        return NULL;
    }
    return &p->frames[(p->frame_total - 1U - age) % p->history];
}

u32 nph_profiler_frame_count(const NphProfiler *p)
{
    NPH_ASSERT(p != NULL);
    return valid_frames(p);
}

bool nph_profiler_frame(const NphProfiler *p, u32 age, NphProfilerFrame *out)
{
    NPH_ASSERT(p != NULL && out != NULL);
    const FrameRecord *f = profiler_frame_record(p, age);
    if (f == NULL) {
        memset(out, 0, sizeof *out);
        return false;
    }
    out->index = f->index;
    out->start = f->start;
    out->end = f->end;
    out->duration_ms = nph_profiler_ticks_to_ms(p, f->end - f->start);
    out->segment_count = f->segment_count;
    out->dropped_events = f->dropped_events;
    return true;
}

u32 nph_profiler_frame_segments(const NphProfiler *p, u32 age, NphProfileSegment *out, u32 cap)
{
    NPH_ASSERT(p != NULL && (out != NULL || cap == 0U));
    const FrameRecord *f = profiler_frame_record(p, age);
    if (f == NULL) {
        return 0U;
    }
    const u32 n = nph_min_u32(f->segment_count, cap);
    for (u32 i = 0; i < n; ++i) {
        out[i] = *profiler_segment(p, f->first_segment + i);
    }
    return n;
}

u32 nph_profiler_frame_times(const NphProfiler *p, f32 *out, u32 cap)
{
    NPH_ASSERT(p != NULL && (out != NULL || cap == 0U));
    const u32 n = nph_min_u32(valid_frames(p), cap);
    for (u32 i = 0; i < n; ++i) {
        const FrameRecord *f = profiler_frame_record(p, n - 1U - i);
        out[i] = (f32)nph_profiler_ticks_to_ms(p, f->end - f->start);
    }
    return n;
}

u32 nph_profiler_lane_count(const NphProfiler *p)
{
    NPH_ASSERT(p != NULL);
    return p->lane_count;
}

const char *nph_profiler_lane_name(const NphProfiler *p, u32 lane)
{
    NPH_ASSERT(p != NULL);
    return lane < p->lane_count ? p->lanes[lane].name : "?";
}

static int compare_zone_stats(const void *a, const void *b)
{
    const NphProfilerZoneStats *x = (const NphProfilerZoneStats *)a;
    const NphProfilerZoneStats *y = (const NphProfilerZoneStats *)b;
    if (x->mean_ms != y->mean_ms) {
        return x->mean_ms > y->mean_ms ? -1 : 1;
    }
    return x->name_id < y->name_id ? -1 : (x->name_id > y->name_id ? 1 : 0);
}

u32 nph_profiler_zone_stats(const NphProfiler *p, NphProfilerZoneStats *out, u32 cap)
{
    NPH_ASSERT(p != NULL && (out != NULL || cap == 0U));
    const u32 window = (u32)nph_min_u64(p->frame_total, p->history);
    if (window == 0U) {
        return 0U;
    }
    const u32 newest = (u32)((p->frame_total - 1U) % p->history);
    u32 n = 0;
    for (u32 name = 0; name < p->name_count && n < cap; ++name) {
        if (p->zone_present[name] == 0U) {
            continue;
        }
        const f32 *w = &p->zone_ms[(u64)name * p->history];
        f64 max = 0.0;
        for (u32 s = 0; s < window; ++s) {
            max = (f64)w[s] > max ? (f64)w[s] : max;
        }
        NphProfilerZoneStats *z = &out[n++];
        z->name = p->names[name];
        z->name_id = name;
        z->frames = p->zone_present[name];
        z->last_ms = (f64)w[newest];
        z->mean_ms = p->zone_sum_ms[name] > 0.0 ? p->zone_sum_ms[name] / (f64)window : 0.0;
        z->max_ms = max;
        z->calls_per_frame = (f64)p->zone_sum_calls[name] / (f64)window;
    }
    qsort(out, n, sizeof *out, compare_zone_stats);
    return n;
}

u32 nph_profiler_counter_stats(const NphProfiler *p, NphProfilerCounterStats *out, u32 cap)
{
    NPH_ASSERT(p != NULL && (out != NULL || cap == 0U));
    const u32 window = (u32)nph_min_u64(p->frame_total, p->history);
    u32 n = 0;
    for (u32 name = 0; name < p->name_count && n < cap; ++name) {
        u32 frames = 0;
        f64 sum = 0.0;
        f64 lo = 0.0;
        f64 hi = 0.0;
        f64 last = 0.0;
        // Oldest to newest, so the last sample seen is the newest.
        for (u32 age = window; age > 0U; --age) {
            const u32 s = (u32)((p->frame_total - age) % p->history);
            const u64 i = (u64)name * p->history + s;
            if (p->counter_present[i] == 0U) {
                continue;
            }
            const f64 v = p->counter_value[i];
            lo = frames == 0U || v < lo ? v : lo;
            hi = frames == 0U || v > hi ? v : hi;
            sum += v;
            last = v;
            frames += 1U;
        }
        if (frames == 0U) {
            continue;
        }
        NphProfilerCounterStats *c = &out[n++];
        c->name = p->names[name];
        c->name_id = name;
        c->frames = frames;
        c->last = last;
        c->mean = sum / (f64)frames;
        c->min = lo;
        c->max = hi;
    }
    return n;
}

void nph_profiler_stats(const NphProfiler *p, NphProfilerStats *out)
{
    NPH_ASSERT(p != NULL && out != NULL);
    *out = p->stats;
    out->lanes = p->lane_count;
    out->names = p->name_count;
}

// ---------------------------------------------------------------------------
// Tracked allocators
// ---------------------------------------------------------------------------

NphResult nph_profiler_track_memory(NphProfiler *p, NphTrackedAllocator *tracked)
{
    NPH_ASSERT(p != NULL && tracked != NULL);
    for (u32 i = 0; i < p->memory_count; ++i) {
        if (p->memory[i] == tracked) {
            return NPH_RESULT_ALREADY_EXISTS;
        }
    }
    if (p->memory_count >= p->max_memory) {
        return NPH_RESULT_FULL;
    }
    p->memory[p->memory_count++] = tracked;
    return NPH_RESULT_OK;
}

void nph_profiler_untrack_memory(NphProfiler *p, NphTrackedAllocator *tracked)
{
    NPH_ASSERT(p != NULL);
    for (u32 i = 0; i < p->memory_count; ++i) {
        if (p->memory[i] == tracked) {
            for (u32 j = i + 1U; j < p->memory_count; ++j) {
                p->memory[j - 1U] = p->memory[j];
            }
            p->memory_count -= 1U;
            return;
        }
    }
}

u32 nph_profiler_memory_stats(const NphProfiler *p, NphMemoryStats *out, u32 cap)
{
    NPH_ASSERT(p != NULL && (out != NULL || cap == 0U));
    const u32 n = nph_min_u32(p->memory_count, cap);
    for (u32 i = 0; i < n; ++i) {
        nph_tracked_allocator_stats(p->memory[i], &out[i]);
    }
    return n;
}

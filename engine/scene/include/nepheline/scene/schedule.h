// SPDX-License-Identifier: Apache-2.0
#pragma once

// The system scheduler (CLAUDE.md §8.7 "Systems declare component
// read/write access. A scheduler builds a dependency graph and runs
// non-conflicting systems in parallel as jobs"; §2 "Parallel by default";
// docs/scene.md "Systems and the schedule").
//
// A system is a function plus the components it reads and writes. The
// schedule orders systems by their explicit constraints (`after`,
// `before`), then by registration order, and derives the dependency graph
// from their access: a system depends on every earlier system it conflicts
// with (one writes what the other reads or writes). Systems with no path
// between them run at the same time on the job system; a system starts the
// moment its last dependency finishes, not when a whole "phase" does (the
// executor of Bevy's ECS and of Unity's job dependencies, without their
// sync points).
//
// A system either iterates its query itself (`run`) or is called once per
// matching chunk (`run_chunk`); the schedule then splits the chunks across
// jobs (nph_job_parallel_for), so one system can use every core. System
// jobs run at NPH_JOB_PRIORITY_NORMAL, the frame's priority (jobs.h).
//
// Stages: systems flagged MAIN_THREAD or EXCLUSIVE run on the thread that
// calls nph_schedule_run, alone, and divide the schedule into stages. At
// the end of every stage, the command buffers the stage's systems recorded
// are applied, ordered by system and then by chunk (independent of which
// worker ran what), so structural changes are deterministic for any worker
// count (§2 "Deterministic simulation"). EXCLUSIVE systems may change the
// world directly.
//
// While a stage runs, the world's structure is frozen (asserted), so
// queries and chunk pointers stay valid for every system.
//
// Change ticks: every system run is stamped with a fresh world tick
// (nph_world_advance_tick), assigned in schedule order before the stage
// starts; `last_tick` is the tick of the system's previous run, which is
// what change filters compare against.
//
// Costs the caller can see (§1.4): the schedule allocates its tables when
// systems are added and built, and one scratch arena per worker (plus one
// for the calling thread) at creation. A run allocates nothing from the
// heap: chunk lists and command buffers come from the scratch arenas, which
// are reset at the start of every run. Exhausting an arena fails the run
// with NPH_RESULT_OUT_OF_MEMORY.
//
// Thread-safety: a schedule belongs to one thread; nph_schedule_run blocks
// that thread (or parks its fiber) until the run completes.

#include <nepheline/jobs/jobs.h>
#include <nepheline/scene/command.h>
#include <nepheline/scene/query.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphSchedule NphSchedule;
typedef struct NphSystemContext NphSystemContext;

typedef void (*NphSystemFn)(NphSystemContext *ctx);
typedef void (*NphSystemChunkFn)(NphSystemContext *ctx, const NphChunkView *chunk);

typedef enum NphSystemFlags {
    // Runs on the thread calling nph_schedule_run, in a stage of its own
    // (window and device work, anything bound to that thread).
    NPH_SYSTEM_MAIN_THREAD = 1U << 0U,
    // As MAIN_THREAD, and may make structural changes directly.
    NPH_SYSTEM_EXCLUSIVE = 1U << 1U,
} NphSystemFlags;

#define NPH_SCHEDULE_MAX_SYSTEMS 1024U
#define NPH_SYSTEM_MAX_ACCESS    32U
#define NPH_SYSTEM_MAX_ORDERING  16U

typedef struct NphSystemDesc {
    const char *name;           // unique in the schedule (copied)
    NphSystemFn run;            // exactly one of `run` and `run_chunk`
    NphSystemChunkFn run_chunk; // requires a query
    void *user;                 // passed in the context

    // The system's query (copied). Optional for `run` systems.
    const NphQueryTerm *terms;
    u32 term_count;

    // Access outside the query: components the system reads or writes by
    // entity lookup (nph_world_get on a parent, say). Up to
    // NPH_SYSTEM_MAX_ACCESS each.
    const NphComponentId *reads;
    u32 read_count;
    const NphComponentId *writes;
    u32 write_count;

    // Names of systems this one runs after or before, up to
    // NPH_SYSTEM_MAX_ORDERING each. Every name must exist when the
    // schedule is built.
    const char *const *after;
    u32 after_count;
    const char *const *before;
    u32 before_count;

    u32 chunks_per_job; // run_chunk: chunks per job (0: 1)
    u32 flags;          // NphSystemFlags
} NphSystemDesc;

// Context passed to a system.
struct NphSystemContext {
    NphWorld *world;
    NphQuery *query;    // the system's query, NULL without one
    void *user;         // NphSystemDesc::user
    NphJobSystem *jobs; // NULL when the schedule runs on the calling thread only
    f64 dt;             // the value passed to nph_schedule_run
    u32 tick;           // this run's change tick: stamped on what the system writes
    u32 last_tick;      // the previous run's tick (0 before the first run)

    // Private.
    NphSchedule *schedule_;
    u32 system_;
    u32 piece_;
    NphCommandBuffer *commands_;
};

typedef struct NphScheduleDesc {
    NphWorld *world;     // required
    NphJobSystem *jobs;  // NULL: systems run one after another on the calling thread
    NphAllocator *alloc; // required: tables and scratch arenas
    u64 scratch_bytes;   // per scratch arena (one per worker plus one): 0 means 1 MiB
} NphScheduleDesc;

NPH_MUST_USE NphResult nph_schedule_create(const NphScheduleDesc *desc, NphSchedule **out);

// Destroys the schedule and its systems' queries. Safe on NULL.
void nph_schedule_destroy(NphSchedule *s);

// Adds a system; `out_index` (may be NULL) receives its index.
// NPH_RESULT_ALREADY_EXISTS for a duplicate name, NPH_RESULT_INVALID_ARGUMENT
// for a malformed descriptor (no name, not exactly one function, run_chunk
// without a query, too many access or ordering entries, an unregistered
// component, a bad query), NPH_RESULT_FULL at NPH_SCHEDULE_MAX_SYSTEMS.
// The schedule is rebuilt before its next run.
NPH_MUST_USE NphResult nph_schedule_add_system(NphSchedule *s, const NphSystemDesc *desc,
                                               u32 *out_index);

// Orders the systems and builds the dependency graph. Called by
// nph_schedule_run when needed; call it directly to report errors early.
// NPH_RESULT_INVALID_ARGUMENT for an unknown name in `after`/`before` or a
// cycle in the constraints; nph_schedule_error says which.
NPH_MUST_USE NphResult nph_schedule_build(NphSchedule *s);

// The reason for the last failed build or run, or "" (static or owned by
// the schedule).
const char *nph_schedule_error(const NphSchedule *s);

// Runs every enabled system once. Returns the first failure: a build
// error, scratch exhaustion, a failure reported by a system
// (nph_system_report), or the failure of applying a command buffer.
// Systems after a failure still run, so the world stays consistent.
NPH_MUST_USE NphResult nph_schedule_run(NphSchedule *s, f64 dt);

// Disabled systems are skipped (their dependents still run in order).
void nph_schedule_set_enabled(NphSchedule *s, u32 system, bool enabled);

// ---------------------------------------------------------------------------
// Introspection (tests, the editor's profiler panel)
// ---------------------------------------------------------------------------

u32 nph_schedule_system_count(const NphSchedule *s);
const char *nph_schedule_system_name(const NphSchedule *s, u32 system);

// The index of the system named `name`, or NPH_U32_MAX.
u32 nph_schedule_find_system(const NphSchedule *s, NphStr name);

// The systems in execution order (built schedules); `out` holds
// system_count entries.
void nph_schedule_order(const NphSchedule *s, u32 *out);

// The stage of a system in the built schedule.
u32 nph_schedule_stage_of(const NphSchedule *s, u32 system);
u32 nph_schedule_stage_count(const NphSchedule *s);

// True when `later` waits for `earlier`, directly or through other systems
// of its stage, in the built schedule.
bool nph_schedule_depends_on(const NphSchedule *s, u32 later, u32 earlier);

typedef struct NphSystemStats {
    u64 runs;
    u64 last_ns;     // wall time of the last run, from start to its last chunk
    u32 last_chunks; // chunks visited by the last run (run_chunk systems)
    u32 last_tick;
} NphSystemStats;

void nph_schedule_system_stats(const NphSchedule *s, u32 system, NphSystemStats *out);

// ---------------------------------------------------------------------------
// Inside a system
// ---------------------------------------------------------------------------

// The command buffer for structural changes from this system (and, for a
// run_chunk system, from this piece of its chunks). Created on first use
// from the scratch arenas; applied at the end of the stage. NULL when the
// scratch arena is exhausted (the run then fails with OUT_OF_MEMORY).
NphCommandBuffer *nph_system_commands(NphSystemContext *ctx);

// Starts an iteration of the system's query with its change filters
// relative to the previous run and its writes stamped with this run's
// tick. For `run` systems.
void nph_system_query_iter(const NphSystemContext *ctx, NphQueryIter *it);

// Records a failure; nph_schedule_run returns the first one reported.
void nph_system_report(NphSystemContext *ctx, NphResult result);

#ifdef __cplusplus
}
#endif

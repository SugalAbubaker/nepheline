// SPDX-License-Identifier: Apache-2.0
#pragma once

// CPU profiling, the recording half (CLAUDE.md §8.11 "CPU instrumentation
// zones recorded per thread into ring buffers"; docs/profile.md;
// docs/adr/0022-profiling.md).
//
//     NPH_PROFILE_BEGIN("scene.transforms");
//     ...
//     NPH_PROFILE_END();
//
// A zone is a span of CPU work between a begin and an end on one thread.
// Every call appends one 32-byte event (a time-stamp counter reading, the
// static name, a type) to the calling thread's single-producer ring: no
// lock, no allocation, no I/O. The collector (engine/profile, profiler.h)
// drains every ring once per frame, rebuilds the zones, and keeps the
// history that the editor's timeline and the Chrome trace export read.
//
// The recording half lives in core, like logging, so that every module can
// be instrumented: the module order (CLAUDE.md §4) puts profile to the
// right of jobs, gpu, and scene, which are the code most worth measuring.
// Its process-wide state is the exception CLAUDE.md §2 allows for
// profiling sinks.
//
// Scopes: C has no destructors, so a zone is an explicit BEGIN/END pair
// rather than the NPH_PROFILE_SCOPE of the plan; GCC's and Clang's cleanup
// attribute would do it, but MSVC has no equivalent (ADR-0022). Every
// return path between the two must end the zone; the collector repairs an
// unbalanced stream (an END with nothing open is ignored, and zones still
// open when their fiber or thread goes away are closed there).
//
// Fibers: a job may park on a counter and resume on another worker thread
// (jobs.h). The job system reports every switch with nph_profile_fiber and
// the collector keeps a zone stack per fiber, so a zone begun on one thread
// and ended on another is drawn as the pieces each thread actually ran.
//
// Names: a zone or counter name is a NUL-terminated string that must stay
// valid until the profiler has collected the event, normally at the end of
// the frame. String literals of the engine qualify. Code that frees names
// (a game module being reloaded, a schedule being destroyed) has the
// profiler collect first (profiler.h, nph_profiler_forget_names).
//
// Costs (§1.4): a zone is two time-stamp counter reads and two 32-byte
// ring writes, about 20 ns. A thread's ring (ring_capacity events of 32
// bytes) is allocated from the configured allocator by
// nph_profile_thread_attach, or by the thread's first event, which then
// allocates. A full ring drops events and counts them.
//
// Builds: with NPH_PROFILER_ENABLED == 0 (Shipping) the macros expand to
// nothing and the functions record nothing. Elsewhere nothing is recorded
// until nph_profile_init, and recording can be paused at run time.
//
// Thread-safety: the recording functions are NPH_THREADSAFE (each thread
// writes its own ring). Init, shutdown, and the drain are
// NPH_MAIN_THREAD_ONLY: one collector.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_PROFILE_MAX_THREADS     256U
#define NPH_PROFILE_THREAD_NAME_MAX 32U

// The fiber value of a thread running on its own stack (not a job fiber).
#define NPH_PROFILE_NO_FIBER NPH_U32_MAX

typedef enum NphProfileEventType {
    NPH_PROFILE_EVENT_NONE = 0,
    NPH_PROFILE_EVENT_BEGIN,   // name: the zone
    NPH_PROFILE_EVENT_END,     //
    NPH_PROFILE_EVENT_FRAME,   // value: the frame index; the main thread, as a frame starts
    NPH_PROFILE_EVENT_FIBER,   // value: the fiber now running on the thread, or NO_FIBER
    NPH_PROFILE_EVENT_COUNTER, // name: the counter; value: the f64's bits
    NPH_PROFILE_EVENT_COUNT
} NphProfileEventType;

typedef struct NphProfileEvent {
    u64 ticks;        // the time-stamp counter (nph_profile_ticks)
    const char *name; // BEGIN and COUNTER; NULL otherwise
    u64 value;        // FRAME, FIBER, COUNTER
    u32 type;         // NphProfileEventType
    u32 reserved;     // zero
} NphProfileEvent;

NPH_STATIC_ASSERT(sizeof(NphProfileEvent) == 32U, "a profile event is half a cache line");

typedef struct NphProfileConfig {
    NphAllocator *alloc; // required: the thread rings (thread-safe if threads attach lazily)
    u32 ring_capacity;   // events per thread ring, a power of two (0: 16384, i.e. 512 KiB)
    u32 max_threads;     // threads that may record at once (0: 64; at most 256)
} NphProfileConfig;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Starts recording. NPH_RESULT_ALREADY_EXISTS when already initialised,
// NPH_RESULT_INVALID_ARGUMENT for a ring capacity that is not a power of
// two of at least 2 or too many threads. NPH_RESULT_UNSUPPORTED in builds
// without the profiler.
NPH_MUST_USE NphResult nph_profile_init(const NphProfileConfig *config);

// Stops recording and frees every ring, drained or not. Threads other than
// the caller must have stopped recording (detached, or exited).
void nph_profile_shutdown(void);

bool nph_profile_is_initialized(void);

// Pauses (false) and resumes recording. Initialisation enables it.
void nph_profile_set_enabled(bool enabled);
bool nph_profile_is_enabled(void);

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

// Prepares the calling thread's ring and names its lane in the timeline
// (copied; at most NPH_PROFILE_THREAD_NAME_MAX - 1 bytes kept). Calling it
// again renames the thread. NPH_RESULT_INVALID_STATE before init,
// NPH_RESULT_FULL when max_threads rings exist, NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_profile_thread_attach(const char *name);

// Retires the calling thread's ring: the collector frees it once it has
// drained what is left. Call before a thread that recorded exits.
void nph_profile_thread_detach(void);

// ---------------------------------------------------------------------------
// Recording (prefer the macros, which compile out of Shipping)
// ---------------------------------------------------------------------------

void nph_profile_begin(const char *name);
void nph_profile_end(void);
// Marks the start of frame `index`. Called by the thread that runs the
// frame loop.
void nph_profile_frame(u64 index);
// Reports that the calling thread now runs fiber `fiber` (an index the job
// system chooses), or its own stack (NPH_PROFILE_NO_FIBER).
void nph_profile_fiber(u32 fiber);
// A sample of a named value (draw calls, entities), kept per frame.
void nph_profile_counter(const char *name, f64 value);
// Appends a prepared event, with its own timestamp, to the calling
// thread's ring: for tests and for replaying captured streams.
void nph_profile_record(const NphProfileEvent *event);

// The clock events are stamped with: the CPU's time-stamp counter (RDTSC
// on x86-64, CNTVCT on ARM64). Its rate is platform knowledge
// (platform/cpu.h); the collector converts.
u64 nph_profile_ticks(void);

#if NPH_PROFILER_ENABLED
#define NPH_PROFILE_BEGIN(name)          nph_profile_begin(name)
#define NPH_PROFILE_END()                nph_profile_end()
#define NPH_PROFILE_FRAME(index)         nph_profile_frame(index)
#define NPH_PROFILE_FIBER(fiber)         nph_profile_fiber(fiber)
#define NPH_PROFILE_COUNTER(name, value) nph_profile_counter((name), (f64)(value))
#else
#define NPH_PROFILE_BEGIN(name)          ((void)0)
#define NPH_PROFILE_END()                ((void)0)
#define NPH_PROFILE_FRAME(index)         ((void)0)
#define NPH_PROFILE_FIBER(fiber)         ((void)0)
#define NPH_PROFILE_COUNTER(name, value) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Collection (the profile module's collector)
// ---------------------------------------------------------------------------

typedef struct NphProfileThreadInfo {
    u32 id;      // 1-based, never reused between init and shutdown; 0: an empty slot
    b32 retired; // the thread detached; its slot is released once drained
    u64 dropped; // events lost to a full ring since the thread attached
    char name[NPH_PROFILE_THREAD_NAME_MAX];
} NphProfileThreadInfo;

// The number of registry slots (the configured max_threads; 0 before init).
u32 nph_profile_thread_slots(void);

// Takes up to `cap` events, oldest first, from the thread in registry slot
// `slot` and describes the thread in `*info`. Returns the count. An empty
// slot gives 0 and info->id == 0. A retired thread's ring is freed, and
// its slot released, by the call that finds it empty.
u32 nph_profile_drain(u32 slot, NphProfileEvent *out, u32 cap, NphProfileThreadInfo *info);

#ifdef __cplusplus
}
#endif

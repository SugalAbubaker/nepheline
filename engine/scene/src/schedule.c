// SPDX-License-Identifier: Apache-2.0
// The system scheduler: ordering, stages, the dependency graph from
// declared access, and its execution on the job system (schedule.h;
// docs/scene.md "Systems and the schedule").

#include <nepheline/core/arena.h>
#include <nepheline/core/assert.h>
#include <nepheline/platform/time.h>
#include <nepheline/scene/schedule.h>

#include "scene_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SCRATCH_BYTES NPH_MIB(1)
#define SCHEDULE_CMD_BLOCK    4096U
// Ticks older than this are clamped before the wrap-around comparison
// could misread them (world.h, nph_world_clamp_ticks).
#define CLAMP_INTERVAL (1U << 29U)

typedef struct System System;

// One command buffer recorded during a stage, with its place in the
// deterministic apply order.
typedef struct CmdEntry {
    struct CmdEntry *next;
    NphCommandBuffer *cb;
    u32 position; // the system's position in the schedule order
    u32 piece;    // the first chunk of the piece that recorded it
} CmdEntry;

// Per-thread scratch: an arena and the command buffers recorded on that
// thread. Each sits on its own cache lines: workers touch only theirs.
typedef struct Scratch {
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphArena arena;
    CmdEntry *cmds;
    u32 cmd_count;
} Scratch;

typedef struct ScratchAllocator {
    NphAllocator base; // first: the vtable receives &base
    NphSchedule *schedule;
} ScratchAllocator;

struct System {
    NphSchedule *schedule;
    u32 index;
    char *name;
    u64 name_len;
    NphSystemFn run;
    NphSystemChunkFn run_chunk;
    void *user;
    NphQuery *query;
    NphComponentMask reads;
    NphComponentMask writes;
    char *after[NPH_SYSTEM_MAX_ORDERING];
    u32 after_count;
    char *before[NPH_SYSTEM_MAX_ORDERING];
    u32 before_count;
    u32 chunks_per_job;
    u32 flags;
    bool enabled;

    // Built.
    u32 position;
    u32 stage;
    u32 *succ;
    u32 succ_count;
    u32 indegree;
    NphAtomicU32 remaining;

    // Per run.
    u32 tick;
    u32 last_tick;
    bool runs_this_time;

    NphSystemStats stats;
};

struct NphSchedule {
    NphWorld *world;
    NphJobSystem *jobs;
    NphAllocator *alloc;

    System **systems;
    u32 count;
    u32 cap;

    bool built;
    u32 *order;       // position -> system index
    u32 *stage_first; // stage -> first position; [stage_count + 1]
    u32 stage_count;

    Scratch *scratch; // worker_count + 1
    u32 scratch_count;
    u64 scratch_bytes;
    ScratchAllocator scratch_alloc;

    NphJobCounter stage_counter;
    NphAtomicU32 error; // the first failure of the run (an NphResult)
    f64 dt;
    u32 last_clamp;
    char error_text[192];
};

// ---------------------------------------------------------------------------
// Scratch memory
// ---------------------------------------------------------------------------

static Scratch *current_scratch(NphSchedule *s)
{
    const u32 worker = s->jobs != NULL ? nph_job_worker_index() : NPH_JOB_WORKER_NONE;
    return worker < s->scratch_count - 1U ? &s->scratch[worker]
                                          : &s->scratch[s->scratch_count - 1U];
}

static void *scratch_alloc_fn(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(site);
    ScratchAllocator *sa = (ScratchAllocator *)self;
    return nph_arena_alloc(&current_scratch(sa->schedule)->arena, size, align);
}

static void *scratch_realloc_fn(NphAllocator *self, void *ptr, u64 old_size, u64 new_size,
                                u32 align, NphCallsite site)
{
    return nph_allocator_realloc_fallback(self, ptr, old_size, new_size, align, site);
}

static void scratch_free_fn(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    // Scratch memory is reclaimed in bulk at the start of the next run.
    NPH_UNUSED(self);
    NPH_UNUSED(ptr);
    NPH_UNUSED(size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
}

static const NphAllocatorVtable g_scratch_vtable = {scratch_alloc_fn, scratch_realloc_fn,
                                                    scratch_free_fn};

static void report(NphSchedule *s, NphResult r)
{
    if (r == NPH_RESULT_OK) {
        return;
    }
    u32 expected = (u32)NPH_RESULT_OK;
    NPH_IGNORE_RESULT(nph_atomic_compare_exchange_u32(
        &s->error, &expected, (u32)r, NPH_MEMORY_ORDER_ACQ_REL, NPH_MEMORY_ORDER_ACQUIRE));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void system_free(NphSchedule *s, System *sys)
{
    NphAllocator *alloc = s->alloc;
    nph_query_destroy(sys->query);
    if (sys->name != NULL) {
        nph_str_free(alloc, sys->name, sys->name_len);
    }
    for (u32 i = 0; i < sys->after_count; ++i) {
        nph_str_free(alloc, sys->after[i], strlen(sys->after[i]));
    }
    for (u32 i = 0; i < sys->before_count; ++i) {
        nph_str_free(alloc, sys->before[i], strlen(sys->before[i]));
    }
    if (sys->succ != NULL) {
        NPH_FREE_ARRAY(alloc, u32, sys->succ, sys->succ_count);
    }
    NPH_FREE_TYPE(alloc, System, sys);
}

static void free_build(NphSchedule *s)
{
    for (u32 i = 0; i < s->count; ++i) {
        System *sys = s->systems[i];
        if (sys->succ != NULL) {
            NPH_FREE_ARRAY(s->alloc, u32, sys->succ, sys->succ_count);
        }
        sys->succ = NULL;
        sys->succ_count = 0U;
    }
    if (s->order != NULL) {
        NPH_FREE_ARRAY(s->alloc, u32, s->order, s->count);
        s->order = NULL;
    }
    if (s->stage_first != NULL) {
        NPH_FREE_ARRAY(s->alloc, u32, s->stage_first, s->count + 1U);
        s->stage_first = NULL;
    }
    s->stage_count = 0U;
    s->built = false;
}

NphResult nph_schedule_create(const NphScheduleDesc *desc, NphSchedule **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->world == NULL || desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphSchedule *s = NPH_ALLOC_TYPE(desc->alloc, NphSchedule);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    s->world = desc->world;
    s->jobs = desc->jobs;
    s->alloc = desc->alloc;
    s->scratch_bytes = desc->scratch_bytes == 0U ? DEFAULT_SCRATCH_BYTES : desc->scratch_bytes;
    s->scratch_alloc.base.vtable = &g_scratch_vtable;
    s->scratch_alloc.base.name = "schedule scratch";
    s->scratch_alloc.schedule = s;
    s->scratch_count = (s->jobs != NULL ? nph_job_system_worker_count(s->jobs) : 0U) + 1U;
    s->scratch = (Scratch *)NPH_ALLOC_ZEROED(desc->alloc, sizeof(Scratch) * s->scratch_count,
                                             NPH_ALIGNOF(Scratch));
    NphResult r = s->scratch != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    for (u32 i = 0; r == NPH_RESULT_OK && i < s->scratch_count; ++i) {
        r = nph_arena_init_from(&s->scratch[i].arena, desc->alloc, s->scratch_bytes);
    }
    if (r != NPH_RESULT_OK) {
        nph_schedule_destroy(s);
        return r;
    }
    *out = s;
    return NPH_RESULT_OK;
}

void nph_schedule_destroy(NphSchedule *s)
{
    if (s == NULL) {
        return;
    }
    free_build(s);
    for (u32 i = 0; i < s->count; ++i) {
        system_free(s, s->systems[i]);
    }
    if (s->systems != NULL) {
        NPH_FREE_ARRAY(s->alloc, System *, s->systems, s->cap);
    }
    if (s->scratch != NULL) {
        for (u32 i = 0; i < s->scratch_count; ++i) {
            nph_arena_destroy(&s->scratch[i].arena);
        }
        NPH_FREE(s->alloc, s->scratch, sizeof(Scratch) * s->scratch_count, NPH_ALIGNOF(Scratch));
    }
    NPH_FREE_TYPE(s->alloc, NphSchedule, s);
}

// ---------------------------------------------------------------------------
// Adding systems
// ---------------------------------------------------------------------------

static NphResult dup_name(NphAllocator *alloc, const char *name, char **out)
{
    if (name == NULL || name[0] == '\0') {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return nph_str_dup(alloc, nph_str_from_cstr(name), out);
}

static NphResult validate_access(const NphWorld *w, const NphComponentId *ids, u32 count,
                                 bool write)
{
    if (count > NPH_SYSTEM_MAX_ACCESS || (count > 0U && ids == NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < count; ++i) {
        const NphComponentInfo *info = nph_world_info(w, ids[i]);
        if (info == NULL || (write && (info->desc->flags & NPH_COMPONENT_FLAG_INTERNAL) != 0U)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_schedule_add_system(NphSchedule *s, const NphSystemDesc *d, u32 *out_index)
{
    NPH_ASSERT(s != NULL && d != NULL);
    if (out_index != NULL) {
        *out_index = NPH_U32_MAX;
    }
    const NphWorld *w = s->world;
    if (d->name == NULL || d->name[0] == '\0' || (d->run == NULL) == (d->run_chunk == NULL) ||
        (d->run_chunk != NULL && d->term_count == 0U) || d->after_count > NPH_SYSTEM_MAX_ORDERING ||
        d->before_count > NPH_SYSTEM_MAX_ORDERING || (d->after_count > 0U && d->after == NULL) ||
        (d->before_count > 0U && d->before == NULL) ||
        (d->flags & ~(u32)(NPH_SYSTEM_MAIN_THREAD | NPH_SYSTEM_EXCLUSIVE)) != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(validate_access(w, d->reads, d->read_count, false));
    NPH_TRY(validate_access(w, d->writes, d->write_count, true));
    for (u32 i = 0; i < d->term_count; ++i) {
        if (d->terms[i].access == NPH_ACCESS_WRITE && d->terms[i].component < w->component_count &&
            (w->components[d->terms[i].component].desc->flags & NPH_COMPONENT_FLAG_INTERNAL) !=
                0U) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    if (nph_schedule_find_system(s, nph_str_from_cstr(d->name)) != NPH_U32_MAX) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    if (s->count >= NPH_SCHEDULE_MAX_SYSTEMS) {
        return NPH_RESULT_FULL;
    }
    if (s->count == s->cap) {
        const u32 cap = s->cap == 0U ? 16U : s->cap * 2U;
        System **arr = (System **)NPH_REALLOC(s->alloc, s->systems, sizeof(System *) * s->cap,
                                              sizeof(System *) * cap, NPH_ALIGNOF(System *));
        if (arr == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        s->systems = arr;
        s->cap = cap;
    }

    System *sys = NPH_ALLOC_TYPE(s->alloc, System);
    if (sys == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    sys->schedule = s;
    sys->run = d->run;
    sys->run_chunk = d->run_chunk;
    sys->user = d->user;
    sys->chunks_per_job = d->chunks_per_job == 0U ? 1U : d->chunks_per_job;
    sys->flags = d->flags;
    sys->enabled = true;
    NphResult r = dup_name(s->alloc, d->name, &sys->name);
    sys->name_len = sys->name != NULL ? strlen(sys->name) : 0U;
    for (u32 i = 0; r == NPH_RESULT_OK && i < d->after_count; ++i) {
        r = dup_name(s->alloc, d->after[i], &sys->after[i]);
        sys->after_count = r == NPH_RESULT_OK ? i + 1U : i;
    }
    for (u32 i = 0; r == NPH_RESULT_OK && i < d->before_count; ++i) {
        r = dup_name(s->alloc, d->before[i], &sys->before[i]);
        sys->before_count = r == NPH_RESULT_OK ? i + 1U : i;
    }
    if (r == NPH_RESULT_OK && d->term_count > 0U) {
        const NphQueryDesc qd = {d->terms, d->term_count};
        r = nph_query_create(s->world, &qd, &sys->query);
    }
    if (r != NPH_RESULT_OK) {
        system_free(s, sys);
        return r;
    }
    for (u32 i = 0; i < d->term_count; ++i) {
        const NphQueryTerm *t = &d->terms[i];
        if (t->access == NPH_ACCESS_READ) {
            nph_mask_set(&sys->reads, t->component);
        } else if (t->access == NPH_ACCESS_WRITE) {
            nph_mask_set(&sys->writes, t->component);
        }
    }
    for (u32 i = 0; i < d->read_count; ++i) {
        nph_mask_set(&sys->reads, d->reads[i]);
    }
    for (u32 i = 0; i < d->write_count; ++i) {
        nph_mask_set(&sys->writes, d->writes[i]);
    }
    free_build(s);
    sys->index = s->count;
    s->systems[s->count++] = sys;
    if (out_index != NULL) {
        *out_index = sys->index;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

u32 nph_schedule_find_system(const NphSchedule *s, NphStr name)
{
    NPH_ASSERT(s != NULL);
    for (u32 i = 0; i < s->count; ++i) {
        if (nph_str_equals(nph_str(s->systems[i]->name, s->systems[i]->name_len), name)) {
            return i;
        }
    }
    return NPH_U32_MAX;
}

static bool is_barrier(const System *sys)
{
    return (sys->flags & (NPH_SYSTEM_MAIN_THREAD | NPH_SYSTEM_EXCLUSIVE)) != 0U;
}

static bool conflicts(const System *a, const System *b)
{
    return nph_mask_intersects(&a->writes, &b->writes) ||
           nph_mask_intersects(&a->writes, &b->reads) || nph_mask_intersects(&a->reads, &b->writes);
}

// The ordering constraint edges: before[a][b] when a must run before b.
static NphResult constraint_matrix(NphSchedule *s, u8 *before)
{
    const u32 n = s->count;
    for (u32 i = 0; i < n; ++i) {
        const System *sys = s->systems[i];
        for (u32 k = 0; k < sys->after_count; ++k) {
            const u32 j = nph_schedule_find_system(s, nph_str_from_cstr(sys->after[k]));
            if (j == NPH_U32_MAX) {
                (void)snprintf(s->error_text, sizeof s->error_text,
                               "system '%s': unknown system '%s' in 'after'", sys->name,
                               sys->after[k]);
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            before[(u64)j * n + i] = 1U;
        }
        for (u32 k = 0; k < sys->before_count; ++k) {
            const u32 j = nph_schedule_find_system(s, nph_str_from_cstr(sys->before[k]));
            if (j == NPH_U32_MAX) {
                (void)snprintf(s->error_text, sizeof s->error_text,
                               "system '%s': unknown system '%s' in 'before'", sys->name,
                               sys->before[k]);
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            before[(u64)i * n + j] = 1U;
        }
    }
    for (u32 i = 0; i < n; ++i) {
        if (before[(u64)i * n + i] != 0U) {
            (void)snprintf(s->error_text, sizeof s->error_text,
                           "system '%s' is ordered relative to itself", s->systems[i]->name);
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    return NPH_RESULT_OK;
}

static NphResult build(NphSchedule *s)
{
    free_build(s);
    s->error_text[0] = '\0';
    const u32 n = s->count;
    if (n == 0U) {
        s->built = true;
        return NPH_RESULT_OK;
    }
    NphAllocator *alloc = s->alloc;
    u8 *before = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, (u64)n * n);
    u32 *indeg = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, n);
    u8 *placed = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, n);
    s->order = NPH_ALLOC_ARRAY(alloc, u32, n);
    s->stage_first = NPH_ALLOC_ARRAY(alloc, u32, n + 1U);
    NphResult r = (before != NULL && indeg != NULL && placed != NULL && s->order != NULL &&
                   s->stage_first != NULL)
                      ? NPH_RESULT_OK
                      : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        r = constraint_matrix(s, before);
    }

    // Kahn's algorithm, taking the lowest registration index among the
    // ready systems: registration order decides whatever the constraints
    // leave open.
    if (r == NPH_RESULT_OK) {
        for (u32 i = 0; i < n; ++i) {
            for (u32 j = 0; j < n; ++j) {
                indeg[j] += before[(u64)i * n + j];
            }
        }
        for (u32 pos = 0; pos < n; ++pos) {
            u32 pick = NPH_U32_MAX;
            for (u32 j = 0; j < n && pick == NPH_U32_MAX; ++j) {
                if (placed[j] == 0U && indeg[j] == 0U) {
                    pick = j;
                }
            }
            if (pick == NPH_U32_MAX) {
                const System *stuck = NULL;
                for (u32 j = 0; j < n && stuck == NULL; ++j) {
                    stuck = placed[j] == 0U ? s->systems[j] : NULL;
                }
                (void)snprintf(s->error_text, sizeof s->error_text,
                               "the 'after'/'before' constraints form a cycle through '%s'",
                               stuck != NULL ? stuck->name : "?");
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            placed[pick] = 1U;
            s->order[pos] = pick;
            s->systems[pick]->position = pos;
            for (u32 j = 0; j < n; ++j) {
                indeg[j] -= before[(u64)pick * n + j];
            }
        }
    }

    // Stages: a barrier system is a stage of its own.
    if (r == NPH_RESULT_OK) {
        u32 stage = 0U;
        bool open = false;
        for (u32 pos = 0; pos < n; ++pos) {
            System *sys = s->systems[s->order[pos]];
            if (is_barrier(sys)) {
                if (open) {
                    stage += 1U;
                }
                s->stage_first[stage] = pos;
                sys->stage = stage;
                stage += 1U;
                open = false;
            } else {
                if (!open) {
                    s->stage_first[stage] = pos;
                    open = true;
                }
                sys->stage = stage;
            }
        }
        s->stage_count = open ? stage + 1U : stage;
        s->stage_first[s->stage_count] = n;
    }

    // Dependency edges within each stage: a conflict or a constraint
    // between an earlier and a later system.
    for (u32 st = 0; r == NPH_RESULT_OK && st < s->stage_count; ++st) {
        const u32 first = s->stage_first[st];
        const u32 last = s->stage_first[st + 1U];
        for (u32 pa = first; r == NPH_RESULT_OK && pa < last; ++pa) {
            System *a = s->systems[s->order[pa]];
            u32 count = 0U;
            for (u32 pb = pa + 1U; pb < last; ++pb) {
                const System *b = s->systems[s->order[pb]];
                count += (conflicts(a, b) || before[(u64)a->index * n + b->index] != 0U) ? 1U : 0U;
            }
            if (count == 0U) {
                continue;
            }
            a->succ = NPH_ALLOC_ARRAY(alloc, u32, count);
            if (a->succ == NULL) {
                r = NPH_RESULT_OUT_OF_MEMORY;
                break;
            }
            a->succ_count = count;
            u32 k = 0U;
            for (u32 pb = pa + 1U; pb < last; ++pb) {
                System *b = s->systems[s->order[pb]];
                if (conflicts(a, b) || before[(u64)a->index * n + b->index] != 0U) {
                    a->succ[k++] = b->index;
                    b->indegree += 1U;
                }
            }
        }
    }

    if (before != NULL) {
        NPH_FREE_ARRAY(alloc, u8, before, (u64)n * n);
    }
    if (indeg != NULL) {
        NPH_FREE_ARRAY(alloc, u32, indeg, n);
    }
    if (placed != NULL) {
        NPH_FREE_ARRAY(alloc, u8, placed, n);
    }
    if (r != NPH_RESULT_OK) {
        free_build(s);
        for (u32 i = 0; i < n; ++i) {
            s->systems[i]->indegree = 0U;
        }
        return r;
    }
    s->built = true;
    return NPH_RESULT_OK;
}

NphResult nph_schedule_build(NphSchedule *s)
{
    NPH_ASSERT(s != NULL);
    for (u32 i = 0; i < s->count; ++i) {
        s->systems[i]->indegree = 0U;
    }
    return build(s);
}

const char *nph_schedule_error(const NphSchedule *s)
{
    NPH_ASSERT(s != NULL);
    return s->error_text;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

static void register_commands(NphSchedule *s, const System *sys, u32 piece, NphCommandBuffer *cb)
{
    if (cb == NULL || nph_commands_is_empty(cb)) {
        if (cb != NULL && cb->error != NPH_RESULT_OK) {
            report(s, cb->error);
        }
        return;
    }
    Scratch *sc = current_scratch(s);
    CmdEntry *e = NPH_ARENA_ALLOC_TYPE(&sc->arena, CmdEntry);
    if (e == NULL) {
        report(s, NPH_RESULT_OUT_OF_MEMORY);
        return;
    }
    e->cb = cb;
    e->position = sys->position;
    e->piece = piece;
    e->next = sc->cmds;
    sc->cmds = e;
    sc->cmd_count += 1U;
}

typedef struct ChunkRun {
    NphSchedule *s;
    System *sys;
    void **chunks;
    u32 chunk_count;
} ChunkRun;

static NphSystemContext make_context(NphSchedule *s, const System *sys)
{
    NphSystemContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.world = s->world;
    ctx.query = sys->query;
    ctx.user = sys->user;
    ctx.jobs = s->jobs;
    ctx.dt = s->dt;
    ctx.tick = sys->tick;
    ctx.last_tick = sys->last_tick;
    ctx.internal_schedule = s;
    ctx.internal_system = sys->index;
    return ctx;
}

static void chunk_piece(void *user, u32 begin, u32 end)
{
    const ChunkRun *cr = (const ChunkRun *)user;
    NphSystemContext ctx = make_context(cr->s, cr->sys);
    ctx.internal_piece = begin;
    for (u32 i = begin; i < end; ++i) {
        NphChunkView view;
        nph_query_chunk_view(cr->sys->query, cr->chunks[i], ctx.tick, &view);
        cr->sys->run_chunk(&ctx, &view);
    }
    register_commands(cr->s, cr->sys, begin, ctx.internal_commands);
}

static void execute(NphSchedule *s, System *sys)
{
    if (!sys->runs_this_time) {
        return;
    }
    const u64 t0 = nph_time_now_ns();
    u32 chunk_count = 0U;
    if (sys->run != NULL) {
        NphSystemContext ctx = make_context(s, sys);
        sys->run(&ctx);
        register_commands(s, sys, 0U, ctx.internal_commands);
    } else {
        const u32 n = nph_query_collect_chunks(sys->query, sys->last_tick, NULL, 0U);
        void **chunks = NULL;
        if (n > 0U) {
            chunks = (void **)nph_arena_alloc(&current_scratch(s)->arena, sizeof(void *) * n,
                                              NPH_ALIGNOF(void *));
            if (chunks == NULL) {
                report(s, NPH_RESULT_OUT_OF_MEMORY);
            }
        }
        if (chunks != NULL) {
            NPH_VERIFY(nph_query_collect_chunks(sys->query, sys->last_tick, chunks, n) == n);
            ChunkRun cr = {s, sys, chunks, n};
            if (s->jobs != NULL && n > sys->chunks_per_job) {
                NphJobCounter counter = NPH_JOB_COUNTER_INIT;
                if (nph_job_parallel_for(s->jobs, n, sys->chunks_per_job, chunk_piece, &cr,
                                         NPH_JOB_PRIORITY_NORMAL, &counter) == NPH_RESULT_OK) {
                    nph_job_wait(s->jobs, &counter);
                } else {
                    chunk_piece(&cr, 0U, n);
                }
            } else {
                chunk_piece(&cr, 0U, n);
            }
            chunk_count = n;
        }
    }
    sys->stats.runs += 1U;
    sys->stats.last_ns = nph_time_now_ns() - t0;
    sys->stats.last_chunks = chunk_count;
    sys->stats.last_tick = sys->tick;
}

static void launch(NphSchedule *s, System *sys);

static void system_job(void *user)
{
    System *sys = (System *)user;
    NphSchedule *s = sys->schedule;
    execute(s, sys);
    for (u32 i = 0; i < sys->succ_count; ++i) {
        System *next = s->systems[sys->succ[i]];
        if (nph_atomic_fetch_sub_u32(&next->remaining, 1U, NPH_MEMORY_ORDER_ACQ_REL) == 1U) {
            launch(s, next);
        }
    }
}

static void launch(NphSchedule *s, System *sys)
{
    const NphJobDecl decl = {system_job, sys, sys->name};
    if (nph_job_run(s->jobs, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &s->stage_counter) !=
        NPH_RESULT_OK) {
        // The job pool is full: run it here. The counter is still held by
        // the job that called us, so the stage cannot complete meanwhile.
        system_job(sys);
    }
}

static int compare_entries(const void *a, const void *b)
{
    const CmdEntry *x = *(const CmdEntry *const *)a;
    const CmdEntry *y = *(const CmdEntry *const *)b;
    if (x->position != y->position) {
        return x->position < y->position ? -1 : 1;
    }
    if (x->piece != y->piece) {
        return x->piece < y->piece ? -1 : 1;
    }
    return 0;
}

// Applies the stage's command buffers, ordered by system position, then by
// piece: the same order for every worker count.
static void apply_commands(NphSchedule *s)
{
    u32 total = 0U;
    for (u32 i = 0; i < s->scratch_count; ++i) {
        total += s->scratch[i].cmd_count;
    }
    if (total == 0U) {
        return;
    }
    Scratch *own = &s->scratch[s->scratch_count - 1U];
    CmdEntry **list = (CmdEntry **)nph_arena_alloc(&own->arena, sizeof(CmdEntry *) * total,
                                                   NPH_ALIGNOF(CmdEntry *));
    if (list == NULL) {
        report(s, NPH_RESULT_OUT_OF_MEMORY);
        // Drop the buffers: their memory is scratch.
        for (u32 i = 0; i < s->scratch_count; ++i) {
            s->scratch[i].cmds = NULL;
            s->scratch[i].cmd_count = 0U;
        }
        return;
    }
    u32 k = 0U;
    for (u32 i = 0; i < s->scratch_count; ++i) {
        for (CmdEntry *e = s->scratch[i].cmds; e != NULL; e = e->next) {
            list[k++] = e;
        }
        s->scratch[i].cmds = NULL;
        s->scratch[i].cmd_count = 0U;
    }
    qsort((void *)list, total, sizeof(CmdEntry *), compare_entries);
    for (u32 i = 0; i < total; ++i) {
        report(s, nph_commands_apply(list[i]->cb, NULL));
    }
}

static void run_stage(NphSchedule *s, u32 stage)
{
    NphWorld *w = s->world;
    const u32 first = s->stage_first[stage];
    const u32 last = s->stage_first[stage + 1U];
    // Ticks in schedule order, before anything runs.
    for (u32 pos = first; pos < last; ++pos) {
        System *sys = s->systems[s->order[pos]];
        sys->runs_this_time = sys->enabled;
        if (sys->enabled) {
            sys->last_tick = sys->tick;
            sys->tick = nph_world_advance_tick(w);
        }
    }
    System *head = s->systems[s->order[first]];
    if (is_barrier(head)) {
        NPH_ASSERT(last == first + 1U);
        const bool exclusive = (head->flags & NPH_SYSTEM_EXCLUSIVE) != 0U;
        if (!exclusive) {
            w->frozen += 1U;
        }
        execute(s, head);
        if (!exclusive) {
            w->frozen -= 1U;
        }
    } else if (s->jobs == NULL) {
        w->frozen += 1U;
        for (u32 pos = first; pos < last; ++pos) {
            execute(s, s->systems[s->order[pos]]);
        }
        w->frozen -= 1U;
    } else {
        w->frozen += 1U;
        for (u32 pos = first; pos < last; ++pos) {
            System *sys = s->systems[s->order[pos]];
            nph_atomic_store_u32(&sys->remaining, sys->indegree, NPH_MEMORY_ORDER_RELAXED);
        }
        nph_job_counter_init(&s->stage_counter);
        // Launch the systems with no dependencies; each finishing system
        // launches the dependents it was the last dependency of. When the
        // job pool is full, launch runs the system on this thread instead.
        for (u32 pos = first; pos < last; ++pos) {
            System *sys = s->systems[s->order[pos]];
            if (sys->indegree == 0U) {
                launch(s, sys);
            }
        }
        nph_job_wait(s->jobs, &s->stage_counter);
        w->frozen -= 1U;
    }
    apply_commands(s);
}

NphResult nph_schedule_run(NphSchedule *s, f64 dt)
{
    NPH_ASSERT(s != NULL);
    NphWorld *w = s->world;
    NPH_ASSERT_MSG(w->frozen == 0U, "a schedule run started while another runs on the world");
    if (!s->built) {
        NPH_TRY(nph_schedule_build(s));
    }
    s->error_text[0] = '\0';
    nph_atomic_store_u32(&s->error, (u32)NPH_RESULT_OK, NPH_MEMORY_ORDER_RELAXED);
    s->dt = dt;
    for (u32 i = 0; i < s->scratch_count; ++i) {
        nph_arena_reset(&s->scratch[i].arena);
        s->scratch[i].cmds = NULL;
        s->scratch[i].cmd_count = 0U;
    }

    // Keep the change ticks comparable (world.h).
    if (w->tick - s->last_clamp >= CLAMP_INTERVAL && s->count > 0U) {
        u32 *ticks = (u32 *)nph_arena_alloc(&s->scratch[s->scratch_count - 1U].arena,
                                            sizeof(u32) * 2U * s->count, NPH_ALIGNOF(u32));
        if (ticks != NULL) {
            for (u32 i = 0; i < s->count; ++i) {
                ticks[(u64)2U * i] = s->systems[i]->tick;
                ticks[(u64)2U * i + 1U] = s->systems[i]->last_tick;
            }
            nph_world_clamp_ticks(w, ticks, 2U * s->count);
            for (u32 i = 0; i < s->count; ++i) {
                s->systems[i]->tick = ticks[(u64)2U * i];
                s->systems[i]->last_tick = ticks[(u64)2U * i + 1U];
            }
            s->last_clamp = w->tick;
        }
    }

    for (u32 st = 0; st < s->stage_count; ++st) {
        run_stage(s, st);
    }
    const NphResult r = (NphResult)nph_atomic_load_u32(&s->error, NPH_MEMORY_ORDER_ACQUIRE);
    if (r != NPH_RESULT_OK && s->error_text[0] == '\0') {
        (void)snprintf(s->error_text, sizeof s->error_text, "run failed: %s", nph_result_string(r));
    }
    return r;
}

void nph_schedule_set_enabled(NphSchedule *s, u32 system, bool enabled)
{
    NPH_ASSERT(s != NULL && system < s->count);
    s->systems[system]->enabled = enabled;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

u32 nph_schedule_system_count(const NphSchedule *s)
{
    NPH_ASSERT(s != NULL);
    return s->count;
}

const char *nph_schedule_system_name(const NphSchedule *s, u32 system)
{
    NPH_ASSERT(s != NULL && system < s->count);
    return s->systems[system]->name;
}

void nph_schedule_order(const NphSchedule *s, u32 *out)
{
    NPH_ASSERT(s != NULL && s->built);
    if (s->count > 0U) {
        memcpy(out, s->order, sizeof(u32) * s->count);
    }
}

u32 nph_schedule_stage_of(const NphSchedule *s, u32 system)
{
    NPH_ASSERT(s != NULL && s->built && system < s->count);
    return s->systems[system]->stage;
}

u32 nph_schedule_stage_count(const NphSchedule *s)
{
    NPH_ASSERT(s != NULL && s->built);
    return s->stage_count;
}

bool nph_schedule_depends_on(const NphSchedule *s, u32 later, u32 earlier)
{
    NPH_ASSERT(s != NULL && s->built && later < s->count && earlier < s->count);
    const System *a = s->systems[earlier];
    const System *b = s->systems[later];
    if (a->stage != b->stage) {
        return a->stage < b->stage;
    }
    if (a->position >= b->position) {
        return false;
    }
    // Successor edges only point forward in the order, so a scan of the
    // stage in order finds every system reachable from `earlier`.
    u8 reach[NPH_SCHEDULE_MAX_SYSTEMS];
    memset(reach, 0, sizeof reach);
    reach[earlier] = 1U;
    for (u32 pos = a->position; pos < b->position; ++pos) {
        const System *x = s->systems[s->order[pos]];
        if (reach[x->index] == 0U) {
            continue;
        }
        for (u32 i = 0; i < x->succ_count; ++i) {
            reach[x->succ[i]] = 1U;
        }
    }
    return reach[later] != 0U;
}

void nph_schedule_system_stats(const NphSchedule *s, u32 system, NphSystemStats *out)
{
    NPH_ASSERT(s != NULL && system < s->count && out != NULL);
    *out = s->systems[system]->stats;
}

// ---------------------------------------------------------------------------
// Inside a system
// ---------------------------------------------------------------------------

NphCommandBuffer *nph_system_commands(NphSystemContext *ctx)
{
    NPH_ASSERT(ctx != NULL && ctx->internal_schedule != NULL);
    if (ctx->internal_commands != NULL) {
        return ctx->internal_commands;
    }
    NphSchedule *s = ctx->internal_schedule;
    NphCommandBuffer *cb = NPH_ARENA_ALLOC_TYPE(&current_scratch(s)->arena, NphCommandBuffer);
    if (cb == NULL) {
        report(s, NPH_RESULT_OUT_OF_MEMORY);
        return NULL;
    }
    nph_commands_init(cb, ctx->world, &s->scratch_alloc.base, SCHEDULE_CMD_BLOCK);
    ctx->internal_commands = cb;
    return cb;
}

void nph_system_query_iter(const NphSystemContext *ctx, NphQueryIter *it)
{
    NPH_ASSERT(ctx != NULL && ctx->query != NULL);
    nph_query_iter_since(ctx->query, ctx->last_tick, ctx->tick, it);
}

void nph_system_report(NphSystemContext *ctx, NphResult result)
{
    NPH_ASSERT(ctx != NULL && ctx->internal_schedule != NULL);
    report(ctx->internal_schedule, result);
}

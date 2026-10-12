// SPDX-License-Identifier: Apache-2.0
// jobs_demo: the Phase 4 sample (CLAUDE.md §9 "Sample app demonstrating
// the feature"). A fork-join tree, a parallel-for over a particle field,
// an I/O job feeding a simulation step, and the frame pipeline of §8.4:
// the simulation of frame N+1 runs while frame N renders, with the
// latency bounded to one frame by nph_job_wait_value. Prints the timings
// and the job system's statistics; the exit code is non-zero if the
// render checksums disagree with a serial reference.
//
//     jobs_demo [--frames=N] [--workers=N]

#include <nepheline/jobs/jobs.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "jobs_demo", NPH_LOG_LEVEL_INFO);

// ---------------------------------------------------------------------------
// Fork-join: the number of leaves of a binary tree, one job per node
// ---------------------------------------------------------------------------

typedef struct TreeNode {
    NphJobSystem *js;
    u32 level;
    u32 depth;
    u64 leaves;
} TreeNode;

static void tree_job(void *user)
{
    TreeNode *n = (TreeNode *)user;
    if (n->level == n->depth) {
        n->leaves = 1U;
        return;
    }
    TreeNode kids[2];
    NphJobDecl decls[2];
    for (u32 i = 0; i < 2U; ++i) {
        kids[i].js = n->js;
        kids[i].level = n->level + 1U;
        kids[i].depth = n->depth;
        kids[i].leaves = 0U;
        decls[i].fn = tree_job;
        decls[i].user = &kids[i];
        decls[i].name = "tree";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT; // on the stack: valid once the wait returns
    while (nph_job_run(n->js, decls, 2U, NPH_JOB_PRIORITY_NORMAL, &done) != NPH_RESULT_OK) {
        nph_thread_yield(); // pool exhausted (not with these defaults)
    }
    nph_job_wait(n->js, &done);
    n->leaves = kids[0].leaves + kids[1].leaves;
}

// ---------------------------------------------------------------------------
// Particles: simulate with parallel_for, render as a checksum
// ---------------------------------------------------------------------------

#define PARTICLES 200000U

typedef struct Particles {
    f32 *x[2]; // double-buffered positions: simulate writes one while render reads the other
    f32 *vx;
    u32 count;
} Particles;

typedef struct Simulate {
    Particles *p;
    u32 from;
    u32 to;
    f32 dt;
} Simulate;

static void simulate_range(void *user, u32 begin, u32 end)
{
    const Simulate *s = (const Simulate *)user;
    const f32 *src = s->p->x[s->from];
    f32 *dst = s->p->x[s->to];
    const f32 *vx = s->p->vx;
    for (u32 i = begin; i < end; ++i) {
        f32 x = src[i] + vx[i] * s->dt;
        if (x > 100.0F || x < -100.0F) {
            x = -x; // wrap around the field
        }
        dst[i] = x;
    }
}

typedef struct Render {
    const Particles *p;
    u32 buffer;
    NphAtomicU64 checksum;
} Render;

// Additive, so the sum over the pieces of a parallel-for equals the serial
// value over the whole range (wrapping arithmetic).
static u64 hash_range(const f32 *x, u32 begin, u32 end)
{
    u64 h = 0U;
    for (u32 i = begin; i < end; ++i) {
        u32 bits;
        memcpy(&bits, &x[i], sizeof bits);
        h += ((u64)bits ^ ((u64)i << 32U)) * 0x9E3779B97F4A7C15ULL;
    }
    return h;
}

static void render_range(void *user, u32 begin, u32 end)
{
    Render *r = (Render *)user;
    nph_atomic_fetch_add_u64(&r->checksum, hash_range(r->p->x[r->buffer], begin, end),
                             NPH_MEMORY_ORDER_RELAXED);
}

// The render job of a frame: splits the draw work across the workers and
// waits for it, all while the main thread has moved on to the next frame.
typedef struct RenderFrame {
    NphJobSystem *js;
    Render render;
    u64 expected; // the serial checksum, computed on the main thread before the job runs
    u64 result;
    u64 elapsed_ns;
} RenderFrame;

static void render_frame_job(void *user)
{
    RenderFrame *f = (RenderFrame *)user;
    const NphStopwatch sw = nph_stopwatch_start();
    nph_atomic_store_u64(&f->render.checksum, 0U, NPH_MEMORY_ORDER_RELAXED);
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    if (nph_job_parallel_for(f->js, f->render.p->count, 4096U, render_range, &f->render,
                             NPH_JOB_PRIORITY_NORMAL, &done) == NPH_RESULT_OK) {
        nph_job_wait(f->js, &done);
    }
    f->result = nph_atomic_load_u64(&f->render.checksum, NPH_MEMORY_ORDER_RELAXED);
    f->elapsed_ns = nph_stopwatch_elapsed_ns(&sw);
}

// A streaming read standing in for the asset system: blocks on an I/O
// thread, never on a worker, and the simulation of a later frame waits on
// its counter.
static void stream_job(void *user)
{
    NPH_UNUSED(user);
    nph_thread_sleep_ns(200U * NPH_TIME_NS_PER_US);
}

// ---------------------------------------------------------------------------

static void print_stats(const NphJobSystem *js)
{
    NphJobStats s;
    nph_job_system_get_stats(js, &s);
    printf("jobs executed %llu (stolen %llu, nested %llu), I/O jobs %llu\n",
           (unsigned long long)s.jobs_executed, (unsigned long long)s.jobs_stolen,
           (unsigned long long)s.jobs_nested, (unsigned long long)s.jobs_io_executed);
    printf("waits: %llu parked a fiber, %llu found no fiber, %llu blocked a thread; "
           "%llu fiber switches, %llu worker parks\n",
           (unsigned long long)s.waits_switched, (unsigned long long)s.waits_inline,
           (unsigned long long)s.waits_external, (unsigned long long)s.fiber_switches,
           (unsigned long long)s.worker_parks);
}

int main(int argc, char **argv)
{
    u32 frames = 120U;
    u32 workers = 0U;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--frames=", 9U) == 0) {
            frames = (u32)strtoul(argv[i] + 9, NULL, 10);
        } else if (strncmp(argv[i], "--workers=", 10U) == 0) {
            workers = (u32)strtoul(argv[i] + 10, NULL, 10);
        } else {
            fprintf(stderr, "usage: jobs_demo [--frames=N] [--workers=N]\n");
            return 2;
        }
    }

    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = nph_allocator_system();
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_INFO) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        return 1;
    }

    NphAllocator *alloc = nph_allocator_system();
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = alloc;
    desc.worker_count = workers;
    NphJobSystem *js = NULL;
    if (nph_job_system_create(&desc, &js) != NPH_RESULT_OK) {
        fprintf(stderr, "jobs_demo: cannot create the job system\n");
        return 1;
    }
    int status = 0;

    // 1. Fork-join.
    {
        TreeNode root = {js, 0U, 10U, 0U};
        const NphJobDecl decl = {tree_job, &root, "root"};
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        const NphStopwatch sw = nph_stopwatch_start();
        if (nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done) == NPH_RESULT_OK) {
            nph_job_wait(js, &done);
        }
        printf("fork-join: a binary tree of depth 10 (2047 jobs) counted %llu leaves in %.2f ms\n",
               (unsigned long long)root.leaves, nph_stopwatch_elapsed_ms(&sw));
        if (root.leaves != 1024U) {
            status = 1;
        }
    }

    // 2. Particles.
    Particles p;
    p.count = PARTICLES;
    p.x[0] = NPH_ALLOC_ARRAY(alloc, f32, PARTICLES);
    p.x[1] = NPH_ALLOC_ARRAY(alloc, f32, PARTICLES);
    p.vx = NPH_ALLOC_ARRAY(alloc, f32, PARTICLES);
    if (p.x[0] == NULL || p.x[1] == NULL || p.vx == NULL) {
        fprintf(stderr, "jobs_demo: out of memory\n");
        return 1;
    }
    for (u32 i = 0; i < PARTICLES; ++i) {
        p.x[0][i] = (f32)(i % 200U) - 100.0F;
        p.x[1][i] = 0.0F;
        p.vx[i] = ((f32)(i % 17U) - 8.0F) * 3.0F;
    }
    {
        Simulate s = {&p, 0U, 1U, 1.0F / 240.0F};
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        const NphStopwatch sw = nph_stopwatch_start();
        if (nph_job_parallel_for(js, PARTICLES, 4096U, simulate_range, &s, NPH_JOB_PRIORITY_NORMAL,
                                 &done) == NPH_RESULT_OK) {
            nph_job_wait(js, &done);
        }
        printf("parallel_for: integrated %u particles in %.3f ms\n", PARTICLES,
               nph_stopwatch_elapsed_ms(&sw));
    }

    // 3. The frame pipeline: simulate frame N+1 while frame N renders. The
    //    render counter counts frames whose render job is still running;
    //    waiting for it to be at most 1 lets exactly one frame overlap.
    //    Every fourth frame also issues a streaming read on the I/O
    //    threads; the simulation two frames later waits for it.
    RenderFrame render_frames[2];
    memset(render_frames, 0, sizeof render_frames);
    NphJobCounter rendering = NPH_JOB_COUNTER_INIT;
    NphJobCounter streaming = NPH_JOB_COUNTER_INIT;
    u64 sim_ns_total = 0U;
    u64 render_ns_total = 0U;
    u32 mismatches = 0U;
    u32 streams = 0U;
    const NphStopwatch pipeline = nph_stopwatch_start();
    for (u32 frame = 0; frame < frames; ++frame) {
        const u32 cur = frame & 1U;
        const u32 next = cur ^ 1U;
        // Frame N-2 must have finished rendering before its buffers are reused.
        nph_job_wait_value(js, &rendering, 1U);
        if (frame >= 2U && (frame - 2U) % 4U == 0U) {
            nph_job_wait(js, &streaming); // the read issued two frames ago
        }

        const NphStopwatch sw = nph_stopwatch_start();
        Simulate s = {&p, cur, next, 1.0F / 240.0F};
        NphJobCounter sim_done = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(js, PARTICLES, 4096U, simulate_range, &s, NPH_JOB_PRIORITY_NORMAL,
                                 &sim_done) == NPH_RESULT_OK) {
            nph_job_wait(js, &sim_done);
        }
        sim_ns_total += nph_stopwatch_elapsed_ns(&sw);

        if (frame % 4U == 0U) {
            const NphJobDecl io = {stream_job, NULL, "stream"};
            if (nph_job_run_io(js, &io, 1U, &streaming) == NPH_RESULT_OK) {
                ++streams;
            }
        }

        // Render the buffer just written; the main thread does not wait.
        RenderFrame *rf = &render_frames[next];
        if (frame >= 2U) {
            // This slot's previous frame has finished (rendering <= 1 above).
            render_ns_total += rf->elapsed_ns;
            if (rf->result != rf->expected) {
                ++mismatches;
            }
        }
        rf->js = js;
        rf->render.p = &p;
        rf->render.buffer = next;
        rf->expected = hash_range(p.x[next], 0U, PARTICLES);
        const NphJobDecl decl = {render_frame_job, rf, "render"};
        if (nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_HIGH, &rendering) != NPH_RESULT_OK) {
            status = 1;
        }
    }
    nph_job_wait(js, &rendering);
    nph_job_wait(js, &streaming);
    const f64 wall_ms = nph_stopwatch_elapsed_ms(&pipeline);
    for (u32 i = 0; i < 2U; ++i) {
        // The last two frames; an unused slot is all zero and matches itself.
        render_ns_total += render_frames[i].elapsed_ns;
        if (render_frames[i].result != render_frames[i].expected) {
            ++mismatches;
        }
    }
    if (mismatches != 0U) {
        status = 1;
    }
    printf("pipeline: %u frames in %.2f ms (%.3f ms per frame); simulate %.2f ms + render "
           "%.2f ms of work overlapped, %u streaming reads, %u checksum mismatches\n",
           frames, wall_ms, frames != 0U ? wall_ms / (f64)frames : 0.0,
           nph_time_ns_to_ms(sim_ns_total), nph_time_ns_to_ms(render_ns_total), streams,
           mismatches);
    printf("job system: %u workers, %u I/O threads, %u fibers\n", nph_job_system_worker_count(js),
           nph_job_system_io_thread_count(js), nph_job_system_fiber_count(js));
    print_stats(js);

    NPH_FREE_ARRAY(alloc, f32, p.x[0], PARTICLES);
    NPH_FREE_ARRAY(alloc, f32, p.x[1], PARTICLES);
    NPH_FREE_ARRAY(alloc, f32, p.vx, PARTICLES);
    nph_job_system_destroy(js);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}

// SPDX-License-Identifier: Apache-2.0
// Runtime benchmarks (docs/runtime.md).
//
//   engine_frame_headless            one frame of a headless engine playing an
//                                    empty world in lockstep: the sync point,
//                                    a one-step simulation job started, and
//                                    the previous one waited for
//                                    (rt.pipelined), the profiler's collection
//   engine_frame_headless_10k        the same with 10,000 placed entities that
//                                    do not move: the cost a static world adds
//   engine_frame_headless_10k_moving the same with every LocalTransform
//                                    written before each frame (the writes are
//                                    not timed): transforms at the sync point
//                                    and in the step
//   engine_play_copy_10k             play-in-editor on a 10,000-entity world:
//                                    the copy with every handle kept, the
//                                    world switch, and stopping
//   sim_input_frame                  one frame of input into the accumulator
//                                    and one step's snapshot taken
//   console_set_cvar                 a cvar set through the console, its
//                                    echo going to the log's rings as in an
//                                    engine (delivered outside the timing)
//
// Physics in the engine (docs/runtime.md "Physics"):
//
//   engine_frame_physics_1k_resting  a frame of 1,000 crates asleep on a
//                                    floor: what a settled scene adds (the
//                                    bridge finds nothing changed, the step
//                                    nothing awake)
//   engine_frame_physics_1k_spinning a frame of 1,000 bodies turning in place
//                                    (gravity off, never asleep, none
//                                    touching): every body moved by the
//                                    step, written back into its entity, and
//                                    composed by the transforms
//   physics_step_1k_spinning         the same bodies stepped in a physics
//                                    world of their own: the frame above
//                                    less this and an empty frame is what
//                                    the engine adds for 1,000 moving bodies
//
// Drawing between steps (docs/runtime.md "Interpolation"), per instance, on
// an offscreen engine with the cooked showcase (not measured, and so
// labelled, without a Vulkan device or the content):
//
//   scene_sync_10k_moving            the scene renderer bringing 10,000
//                                    instances that all moved up to date as
//                                    they are (sync)
//   scene_interpolate_10k_moving     the same captured and drawn half way
//                                    between their states (capture, then
//                                    sync_at): the difference is what
//                                    interpolation costs a moving instance

#include <nepheline/physics/body.h>
#include <nepheline/physics/shape.h>
#include <nepheline/runtime/console.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/scene_render.h>
#include <nepheline/runtime/sim_input.h>

#include <nbench/nbench.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTITIES  10000U
#define BODIES    1000U
#define INSTANCES 10000U

static NphEngine *headless_engine(void)
{
    static const char *const assignments[2] = {"rt.lockstep=true", "rt.max_entities=65536"};
    NphEngineConfig config;
    memset(&config, 0, sizeof config);
    config.headless = 1;
    config.assignments[0] = assignments[0];
    config.assignments[1] = assignments[1];
    config.assignment_count = 2U;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.config = &config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "runtime bench";
    d.play_on_start = 1;
    NphEngine *e = NULL;
    if (nph_engine_create(&d, &e) != NPH_RESULT_OK) {
        abort();
    }
    return e;
}

static void spawn(NphEngine *e, u32 count, NphEntity *out)
{
    const NphComponentId comps[2] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    if (nph_world_spawn_batch(nph_engine_edit_world(e), comps, 2U, count, out) != NPH_RESULT_OK) {
        abort();
    }
}

// A setup that does not hold: the benchmark would measure something else.
static void fail(const char *what)
{
    fprintf(stderr, "runtime bench: %s\n", what);
    abort();
}

static void frame(NphEngine *e)
{
    if (nph_engine_frame(e) != NPH_RESULT_OK) {
        abort();
    }
}

NBENCH(engine_frame_headless)
{
    NphEngine *e = headless_engine();
    for (u32 i = 0; i < 8U; ++i) {
        frame(e);
    }
    while (nbench_keep_running(state)) {
        frame(e);
    }
    nph_engine_destroy(e);
}

NBENCH(engine_frame_headless_10k)
{
    NphEngine *e = headless_engine();
    NphEntity *entities = (NphEntity *)malloc(sizeof(NphEntity) * ENTITIES);
    if (entities == NULL) {
        abort();
    }
    nph_engine_play_end(e);
    spawn(e, ENTITIES, entities);
    if (nph_engine_play_begin(e, false) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < 8U; ++i) {
        frame(e);
    }
    while (nbench_keep_running(state)) {
        frame(e);
    }
    nph_engine_destroy(e);
    free(entities);
}

NBENCH(engine_frame_headless_10k_moving)
{
    NphEngine *e = headless_engine();
    NphEntity *entities = (NphEntity *)malloc(sizeof(NphEntity) * ENTITIES);
    if (entities == NULL) {
        abort();
    }
    nph_engine_play_end(e);
    spawn(e, ENTITIES, entities);
    if (nph_engine_play_begin(e, false) != NPH_RESULT_OK) {
        abort();
    }
    NphWorld *w = nph_engine_world(e);
    f64 x = 0.0;
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        if (nph_engine_wait(e) != NPH_RESULT_OK) {
            abort();
        }
        x += 0.001;
        for (u32 i = 0; i < ENTITIES; ++i) {
            NPH_WORLD_GET_MUT(w, entities[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
                ->translation.x = x;
        }
        nbench_resume_timing(state);
        frame(e);
    }
    nph_engine_destroy(e);
    free(entities);
}

NBENCH(engine_play_copy_10k)
{
    NphEngine *e = headless_engine();
    NphEntity *entities = (NphEntity *)malloc(sizeof(NphEntity) * ENTITIES);
    if (entities == NULL) {
        abort();
    }
    nph_engine_play_end(e);
    spawn(e, ENTITIES, entities);
    frame(e);
    while (nbench_keep_running(state)) {
        if (nph_engine_play_begin(e, true) != NPH_RESULT_OK) {
            abort();
        }
        nph_engine_play_end(e);
    }
    nph_engine_destroy(e);
    free(entities);
}

// ---------------------------------------------------------------------------
// Physics in the engine
// ---------------------------------------------------------------------------

// Where body `i` of BODIES is: on a 32 x 32 grid on the floor (resting), or
// in a 10 x 10 x 10 block (spinning); 3 m apart, so no two touch.
static NphDVec3 body_position(u32 i, bool resting)
{
    if (resting) {
        const u32 row = i / 32U;
        return nph_dvec3(3.0 * (f64)(i % 32U), 0.5, 3.0 * (f64)row);
    }
    const u32 column = (i / 10U) % 10U;
    const u32 layer = i / 100U;
    return nph_dvec3(3.0 * (f64)(i % 10U), 3.0 * (f64)column, 3.0 * (f64)layer);
}

#define SPIN_VELOCITY nph_vec3(0.3F, 2.0F, 0.0F) // rad/s

// BODIES crates of 1 m in the edited world: resting on a floor, or turning
// in place with gravity off and sleep off.
static void spawn_bodies(NphEngine *e, bool resting)
{
    NphWorld *w = nph_engine_edit_world(e);
    const NphRuntimeComponents *ids = nph_engine_components(e);
    const NphComponentId comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->rigid_body,
                                     ids->collider};
    NphEntity *bodies = (NphEntity *)malloc(sizeof(NphEntity) * BODIES);
    if (bodies == NULL || nph_world_spawn_batch(w, comps, 4U, BODIES, bodies) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < BODIES; ++i) {
        NPH_WORLD_GET_MUT(w, bodies[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
            ->translation = body_position(i, resting);
        if (!resting) {
            NphRigidBody *rb = NPH_WORLD_GET_MUT(w, bodies[i], ids->rigid_body, NphRigidBody);
            rb->gravity_scale = 0.0F;
            rb->angular_velocity = SPIN_VELOCITY;
            rb->can_sleep = 0;
        }
    }
    free(bodies);
    if (resting) {
        const NphComponentId floor_comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                               NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->collider};
        NphEntity floor;
        if (nph_world_spawn(w, floor_comps, 3U, &floor) != NPH_RESULT_OK) {
            abort();
        }
        NPH_WORLD_GET_MUT(w, floor, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
            ->translation = nph_dvec3(46.5, 0.0, 46.5);
        NphCollider *c = NPH_WORLD_GET_MUT(w, floor, ids->collider, NphCollider);
        c->half_extents = nph_vec3(60.0F, 0.5F, 60.0F);
        c->center = nph_vec3(0.0F, -0.5F, 0.0F);
    }
}

static NphEngine *physics_engine(bool resting)
{
    NphEngine *e = headless_engine();
    nph_engine_play_end(e);
    spawn_bodies(e, resting);
    if (nph_engine_play_begin(e, false) != NPH_RESULT_OK) {
        abort();
    }
    return e;
}

NBENCH(engine_frame_physics_1k_resting)
{
    NphEngine *e = physics_engine(true);
    // Half a second at rest puts a body to sleep: settled with a margin.
    for (u32 i = 0; i < 480U; ++i) {
        frame(e);
    }
    if (nph_engine_wait(e) != NPH_RESULT_OK || nph_engine_physics_stats(e)->awake_bodies != 0U) {
        fail("the crates are not all asleep");
    }
    while (nbench_keep_running(state)) {
        frame(e);
    }
    nph_engine_destroy(e);
}

NBENCH(engine_frame_physics_1k_spinning)
{
    NphEngine *e = physics_engine(false);
    for (u32 i = 0; i < 8U; ++i) {
        frame(e);
    }
    while (nbench_keep_running(state)) {
        frame(e);
    }
    nph_engine_destroy(e);
}

NBENCH(physics_step_1k_spinning)
{
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = nph_allocator_system();
    NphJobSystem *jobs = NULL;
    NphPhysicsWorldDesc wd = nph_physics_world_desc_default();
    wd.alloc = nph_allocator_system();
    NphPhysicsWorld *pw = NULL;
    if (nph_job_system_create(&jd, &jobs) != NPH_RESULT_OK ||
        nph_physics_world_create(&wd, &pw) != NPH_RESULT_OK) {
        abort();
    }
    const NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3_splat(0.5F)));
    for (u32 i = 0; i < BODIES; ++i) {
        NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
        bd.position = body_position(i, false);
        bd.angular_velocity = SPIN_VELOCITY;
        bd.gravity_scale = 0.0F;
        bd.enable_sleep = 0;
        bd.user_data = i;
        NphPhysicsBody body;
        NphPhysicsShape shape;
        if (nph_physics_body_create(pw, &bd, &body) != NPH_RESULT_OK ||
            nph_physics_shape_create(pw, body, &sd, &shape) != NPH_RESULT_OK) {
            abort();
        }
    }
    const f32 dt = 1.0F / 240.0F;
    for (u32 i = 0; i < 8U; ++i) {
        if (nph_physics_world_step(pw, dt, jobs) != NPH_RESULT_OK) {
            abort();
        }
    }
    while (nbench_keep_running(state)) {
        if (nph_physics_world_step(pw, dt, jobs) != NPH_RESULT_OK) {
            abort();
        }
    }
    nph_physics_world_destroy(pw);
    nph_job_system_destroy(jobs);
}

// ---------------------------------------------------------------------------
// Drawing between steps
// ---------------------------------------------------------------------------

typedef struct MovingScene {
    NphEngine *engine;
    NphSceneRenderer *sr;
    NphWorld *world;
    NphQuery *move; // WorldTransform written, MeshRenderer read
    f64 y;
} MovingScene;

// An offscreen engine whose world holds INSTANCES drawn entities of the
// showcase's torus on a grid, every one with its instance (made over frames:
// a new instance's upload waits for a frame to flush the staging ring);
// false (the benchmark labelled) without a Vulkan device or the content.
static bool moving_scene_begin(NbenchState *state, MovingScene *ms)
{
    memset(ms, 0, sizeof *ms);
#ifndef NPH_BENCH_SHOWCASE_PACK
    nbench_set_label(state, "not measured: no cooked showcase");
    return false;
#else
    static const char *const assignments[1] = {"rt.max_entities=65536"};
    NphEngineConfig config;
    memset(&config, 0, sizeof config);
    config.packs[0] = NPH_BENCH_SHOWCASE_PACK;
    config.pack_count = 1U;
    config.assignments[0] = assignments[0];
    config.assignment_count = 1U;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.config = &config;
    d.mode = NPH_ENGINE_MODE_OFFSCREEN;
    d.app_name = "runtime bench";
    const NphResult r = nph_engine_create(&d, &ms->engine);
    if (r == NPH_RESULT_UNSUPPORTED) {
        nbench_set_label(state, "not measured: no Vulkan device");
        return false;
    }
    if (r != NPH_RESULT_OK) {
        fail("no offscreen engine");
    }
    ms->sr = nph_engine_scene_renderer(ms->engine);
    ms->world = nph_engine_world(ms->engine);
    const NphRuntimeComponents *ids = nph_engine_components(ms->engine);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->mesh_renderer};
    if (ms->sr == NULL ||
        nph_world_spawn_batch(ms->world, comps, 3U, INSTANCES, NULL) != NPH_RESULT_OK) {
        fail("no scene renderer, or no room for the instances' entities");
    }
    const NphQueryTerm place_terms[2] = {nph_term_write(NPH_COMPONENT_ID_WORLD_TRANSFORM),
                                         nph_term_write(ids->mesh_renderer)};
    const NphQueryTerm move_terms[2] = {nph_term_write(NPH_COMPONENT_ID_WORLD_TRANSFORM),
                                        nph_term_read(ids->mesh_renderer)};
    const NphQueryDesc pd = {place_terms, 2U};
    const NphQueryDesc md = {move_terms, 2U};
    NphQuery *place = NULL;
    if (nph_query_create(ms->world, &pd, &place) != NPH_RESULT_OK ||
        nph_query_create(ms->world, &md, &ms->move) != NPH_RESULT_OK) {
        fail("no query");
    }
    const NphGuid torus = nph_guid_from_cstr("showcase.gltf#mesh/0");
    u32 n = 0U;
    NphQueryIter it;
    nph_query_iter(place, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 0, NphWorldTransform);
        NphMeshRenderer *m = NPH_CHUNK_COLUMN(&v, 1, NphMeshRenderer);
        for (u32 i = 0; i < v.count; ++i, ++n) {
            const u32 row = n / 100U;
            wt[i].translation = nph_dvec3(2.0 * (f64)(n % 100U), 0.0, 2.0 * (f64)row);
            m[i].mesh = torus;
        }
    }
    nph_query_destroy(place);
    // Frames until the torus has loaded and every instance is made.
    for (u32 i = 0; i < 64U && nph_engine_frame_stats(ms->engine)->scene.instances < INSTANCES;
         ++i) {
        frame(ms->engine);
        nph_scene_renderer_wait(ms->sr);
    }
    if (nph_engine_frame_stats(ms->engine)->scene.instances != INSTANCES ||
        nph_engine_wait(ms->engine) != NPH_RESULT_OK) {
        fail("the instances were not made");
    }
    return true;
#endif
}

// Every instance a millimetre on (as a step would move them; not timed).
static void moving_scene_move(MovingScene *ms)
{
    (void)nph_world_advance_tick(ms->world);
    ms->y += 0.001;
    NphQueryIter it;
    nph_query_iter(ms->move, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 0, NphWorldTransform);
        for (u32 i = 0; i < v.count; ++i) {
            wt[i].translation.y = ms->y;
        }
    }
}

static void moving_scene_end(MovingScene *ms)
{
    nph_query_destroy(ms->move);
    nph_engine_destroy(ms->engine);
}

NBENCH(scene_sync_10k_moving)
{
    MovingScene ms;
    if (!moving_scene_begin(state, &ms)) {
        while (nbench_keep_running(state)) {
        }
        return;
    }
    nbench_set_items_per_iteration(state, INSTANCES);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        moving_scene_move(&ms);
        nbench_resume_timing(state);
        if (nph_scene_renderer_sync(ms.sr, NULL) != NPH_RESULT_OK) {
            fail("a sync failed");
        }
    }
    moving_scene_end(&ms);
}

NBENCH(scene_interpolate_10k_moving)
{
    MovingScene ms;
    if (!moving_scene_begin(state, &ms)) {
        while (nbench_keep_running(state)) {
        }
        return;
    }
    nbench_set_items_per_iteration(state, INSTANCES);
    NphSceneRenderStats st;
    memset(&st, 0, sizeof st);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        moving_scene_move(&ms);
        nbench_resume_timing(state);
        if (nph_scene_renderer_capture(ms.sr) != NPH_RESULT_OK ||
            nph_scene_renderer_sync_at(ms.sr, 0.5F, &st) != NPH_RESULT_OK) {
            fail("a capture or a sync failed");
        }
    }
    if (st.interpolating != INSTANCES) {
        fail("the instances were not drawn between their states");
    }
    moving_scene_end(&ms);
}

NBENCH(sim_input_frame)
{
    NphInput *input = NULL;
    NphInputDesc d;
    memset(&d, 0, sizeof d);
    if (nph_input_create(nph_allocator_system(), &d, &input) != NPH_RESULT_OK) {
        abort();
    }
    NphActionId action = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        char name[32];
        name[0] = 'a';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        if (nph_input_action_register(input, name, &action) != NPH_RESULT_OK ||
            nph_input_bind_key(input, action, (NphKey)((u32)NPH_KEY_A + i), NPH_MOD_ANY) !=
                NPH_RESULT_OK) {
            abort();
        }
    }
    NphSimInputAccumulator acc;
    nph_sim_input_reset(&acc);
    NphSimInput out;
    u32 checksum = 0U;
    while (nbench_keep_running(state)) {
        nph_input_begin_frame(input);
        nph_input_end_frame(input);
        nph_sim_input_accumulate(&acc, input);
        nph_sim_input_take(&acc, &out);
        checksum += out.action_count;
    }
    nbench_do_not_optimize(&checksum);
    nph_input_destroy(input);
}

NBENCH(console_set_cvar)
{
    NphLogConfig lc;
    memset(&lc, 0, sizeof lc);
    lc.alloc = nph_allocator_system();
    if (nph_log_init(&lc) != NPH_RESULT_OK) {
        abort();
    }
    NphCvarRegistry cvars;
    if (nph_cvar_registry_init(&cvars, nph_allocator_system()) != NPH_RESULT_OK) {
        abort();
    }
    static NPH_CVAR_INT(rate, "sim.rate", 240, 1, 10000, "steps per second", 0);
    rate.registered = 0U;
    NphConsole *console = NULL;
    if (nph_cvar_register(&cvars, &rate) != NPH_RESULT_OK ||
        nph_console_create(nph_allocator_system(), &cvars, &console) != NPH_RESULT_OK) {
        abort();
    }
    u32 i = 0U;
    while (nbench_keep_running(state)) {
        const NphStr line = (i++ & 1U) != 0U ? NPH_STR("sim.rate 120") : NPH_STR("sim.rate 240");
        if (nph_console_execute(console, line) != NPH_RESULT_OK) {
            abort();
        }
        if ((i & 255U) == 0U) {
            nbench_pause_timing(state);
            (void)nph_log_flush(0U); // no sinks: the records are dropped
            nbench_resume_timing(state);
        }
    }
    nph_console_destroy(console);
    nph_cvar_registry_free(&cvars);
    nph_log_shutdown();
}

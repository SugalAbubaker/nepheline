// SPDX-License-Identifier: Apache-2.0
// scene_demo: the Phase 8 sample (CLAUDE.md §12, "Scene/ECS"). An orrery
// built as an ECS world: a sun, planets on orbit pivots, moons on pivots of
// their own, and an asteroid belt whose thousands of rocks hang under one
// pivot and spin on their own axes. Everything moves through systems on the
// scheduler: orbits and spins write local transforms in parallel per
// chunk, the transform system composes the hierarchy level by level, and a
// main-thread system hands the world transforms of the chunks that changed
// to the renderer (the GPU-driven production path unless --pipeline says
// otherwise). Before the first frame the scene is saved as
// text and as binary, loaded into fresh worlds, and saved again; both
// round trips must reproduce the bytes.
//
//     scene_demo [--frames=N] [--capture=FILE.bmp] [--save=FILE.scene]
//                [--asteroids=N] [--pipeline=production|reference] [--headless] [--vsync]
//
// --headless runs the simulation without a window (and exits 0); otherwise
// exit status 77 means no window system or Vulkan device (CTest
// "skipped"), 1 a failure. Keys: Space pauses, P switches the pipeline,
// T switches temporal anti-aliasing (on, as the runtime's r.taa), B the
// bloom (on, as r.bloom), O the ambient occlusion (on, as r.ao), G the
// global illumination (on, as r.gi), E the automatic exposure (off, as
// r.auto_exposure), the mouse wheel zooms, Escape quits.

#include <nepheline/core/json.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>
#include <nepheline/render/primitives.h>
#include <nepheline/render/renderer.h>
#include <nepheline/scene/scene_io.h>
#include <nepheline/scene/schedule.h>
#include <nepheline/scene/transform.h>

#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "scene_demo", NPH_LOG_LEVEL_INFO);

#define EXIT_SKIPPED 77
#define DEMO_BLOOM   0.04F // the runtime's r.bloom default
#define DEMO_AO      0.5F  // the runtime's r.ao default

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

// Rotation about a local axis, radians per second.
#define DEMO_SPIN_FIELDS(F)                                                                        \
    F(VEC3, axis, NPH_FIELD_NO_HINTS)                                                              \
    F(F32, speed, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))
NPH_COMPONENT_STRUCT(DemoSpin, DEMO_SPIN_FIELDS);
NPH_COMPONENT_DECLARE(DemoSpin);
NPH_COMPONENT_DEFINE(DemoSpin, DEMO_SPIN_FIELDS, "demo.Spin", 0U, NULL)

// An orbit pivot: rotation about +Y, radians per second; children placed
// at a distance from it circle around it.
#define DEMO_ORBIT_FIELDS(F) F(F32, speed, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))
NPH_COMPONENT_STRUCT(DemoOrbit, DEMO_ORBIT_FIELDS);
NPH_COMPONENT_DECLARE(DemoOrbit);
NPH_COMPONENT_DEFINE(DemoOrbit, DEMO_ORBIT_FIELDS, "demo.Orbit", 0U, NULL)

// Something drawn: a mesh and a material from the demo's tables, and the
// render instance (a runtime handle: transient, never saved).
#define DEMO_BODY_FIELDS(F)                                                                        \
    F(U32, mesh, NPH_FIELD_NO_HINTS)                                                               \
    F(U32, material, NPH_FIELD_NO_HINTS)                                                           \
    F(U32, instance, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_TRANSIENT | NPH_FIELD_FLAG_HIDDEN))
NPH_COMPONENT_STRUCT(DemoBody, DEMO_BODY_FIELDS);
NPH_COMPONENT_DECLARE(DemoBody);
NPH_COMPONENT_DEFINE(DemoBody, DEMO_BODY_FIELDS, "demo.Body", 0U, NULL)

typedef struct Ids {
    NphComponentId spin;
    NphComponentId orbit;
    NphComponentId body;
} Ids;

enum {
    MESH_SPHERE = 0,
    MESH_CUBE = 1,
    MESH_COUNT = 2
};

typedef struct Palette {
    NphVec3 color;
    f32 metallic;
    f32 roughness;
    f32 emissive; // cd/m^2
} Palette;

// Material 0 is the sun; 1..6 the planets; 7 the moons; 8 the rocks.
static const Palette g_palette[] = {
    {{1.0F, 0.75F, 0.35F}, 0.0F, 1.0F, 60000.0F}, {{0.55F, 0.52F, 0.50F}, 0.0F, 0.8F, 0.0F},
    {{0.86F, 0.62F, 0.35F}, 0.0F, 0.6F, 0.0F},    {{0.20F, 0.40F, 0.85F}, 0.0F, 0.35F, 0.0F},
    {{0.75F, 0.30F, 0.18F}, 0.0F, 0.7F, 0.0F},    {{0.82F, 0.70F, 0.55F}, 0.0F, 0.5F, 0.0F},
    {{0.60F, 0.75F, 0.80F}, 0.0F, 0.4F, 0.0F},    {{0.70F, 0.70F, 0.70F}, 0.0F, 0.9F, 0.0F},
    {{0.45F, 0.40F, 0.36F}, 0.2F, 0.75F, 0.0F},
};
#define MATERIAL_COUNT ((u32)NPH_ARRAY_COUNT(g_palette))

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

typedef struct Options {
    u64 max_frames;
    const char *capture;
    const char *save;
    u32 asteroids;
    NphRenderPipeline pipeline;
    bool headless;
    bool vsync;
} Options;

static int usage(void)
{
    fprintf(stderr, "usage: scene_demo [--frames=N] [--capture=FILE.bmp] [--save=FILE.scene]\n"
                    "                  [--asteroids=N] [--pipeline=production|reference] "
                    "[--headless] [--vsync]\n");
    return 2;
}

static bool parse(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof *o);
    o->asteroids = 4000U;
    o->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "--frames=", 9U) == 0) {
            o->max_frames = strtoull(a + 9, NULL, 10);
        } else if (strncmp(a, "--capture=", 10U) == 0) {
            o->capture = a + 10;
        } else if (strncmp(a, "--save=", 7U) == 0) {
            o->save = a + 7;
        } else if (strncmp(a, "--asteroids=", 12U) == 0) {
            o->asteroids = (u32)strtoul(a + 12, NULL, 10);
        } else if (strcmp(a, "--pipeline=production") == 0) {
            o->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
        } else if (strcmp(a, "--pipeline=reference") == 0) {
            o->pipeline = NPH_RENDER_PIPELINE_REFERENCE;
        } else if (strcmp(a, "--headless") == 0) {
            o->headless = true;
        } else if (strcmp(a, "--vsync") == 0) {
            o->vsync = true;
        } else {
            return false;
        }
    }
    if (o->asteroids > 12000U) {
        fprintf(stderr, "scene_demo: at most 12000 asteroids (the renderer's instance budget)\n");
        return false;
    }
    if (o->capture != NULL && o->max_frames == 0U) {
        fprintf(stderr, "scene_demo: --capture needs --frames (it captures the last one)\n");
        return false;
    }
    if (o->headless && o->max_frames == 0U) {
        o->max_frames = 240U;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The scene
// ---------------------------------------------------------------------------

static NphResult spawn_body(NphWorld *w, const Ids *ids, const char *name, NphEntity parent,
                            NphLocalTransform local, u32 mesh, u32 material, NphEntity *out)
{
    const NphComponentId comps[4] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->body};
    NPH_TRY(nph_world_spawn(w, comps, mesh == NPH_U32_MAX ? 3U : 4U, out));
    NphName n;
    nph_text_set(&n.text, nph_str_from_cstr(name));
    NPH_TRY(nph_world_set(w, *out, NPH_COMPONENT_ID_NAME, &n));
    NPH_TRY(nph_world_set(w, *out, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &local));
    if (mesh != NPH_U32_MAX) {
        const DemoBody body = {mesh, material, 0U};
        NPH_TRY(nph_world_set(w, *out, ids->body, &body));
    }
    return nph_entity_is_null(parent) ? NPH_RESULT_OK : nph_world_set_parent(w, *out, parent);
}

static NphLocalTransform at(f64 x, f64 y, f64 z, f32 scale)
{
    return nph_local_transform(nph_dvec3(x, y, z), nph_quat_identity(), nph_vec3_splat(scale));
}

static NphResult build_orrery(NphWorld *w, const Ids *ids, u32 asteroids)
{
    NphEntity sun;
    NPH_TRY(
        spawn_body(w, ids, "sun", NPH_ENTITY_NULL, at(0.0, 0.0, 0.0, 2.2F), MESH_SPHERE, 0U, &sun));
    const DemoSpin sun_spin = {{0.0F, 1.0F, 0.0F}, 0.1F};
    NPH_TRY(nph_world_add(w, sun, ids->spin, &sun_spin));

    static const char *const planet_names[6] = {"mercury", "venus",   "earth",
                                                "mars",    "jupiter", "saturn"};
    for (u32 i = 0; i < 6U; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "%s orbit", planet_names[i]);
        // The pivot sits at the sun, unscaled (the sun's scale would
        // stretch every orbit, so pivots are roots).
        NphEntity pivot;
        NPH_TRY(spawn_body(w, ids, name, NPH_ENTITY_NULL, at(0.0, 0.0, 0.0, 1.0F), NPH_U32_MAX, 0U,
                           &pivot));
        const DemoOrbit orbit = {0.9F / (f32)(i + 1U)};
        NPH_TRY(nph_world_add(w, pivot, ids->orbit, &orbit));
        const f32 size = i < 4U ? 0.35F + 0.08F * (f32)i : 0.9F - 0.1F * (f32)(i - 4U);
        NphEntity planet;
        NPH_TRY(spawn_body(w, ids, planet_names[i], pivot, at(4.5 + 3.2 * i, 0.0, 0.0, size),
                           MESH_SPHERE, 1U + i, &planet));
        const DemoSpin spin = {{0.0F, 1.0F, 0.15F}, 0.8F};
        NPH_TRY(nph_world_add(w, planet, ids->spin, &spin));
        if (i >= 2U) {
            // A moon on a pivot of its own: the planet carries it around.
            NphEntity moon_pivot;
            NPH_TRY(spawn_body(w, ids, "moon orbit", planet, at(0.0, 0.0, 0.0, 1.0F), NPH_U32_MAX,
                               0U, &moon_pivot));
            const DemoOrbit moon_orbit = {2.0F};
            NPH_TRY(nph_world_add(w, moon_pivot, ids->orbit, &moon_orbit));
            NphEntity moon;
            NPH_TRY(spawn_body(w, ids, "moon", moon_pivot, at(2.0, 0.3, 0.0, 0.3F), MESH_SPHERE, 7U,
                               &moon));
        }
    }

    // The belt: one pivot carries thousands of rocks, each spinning.
    NphEntity belt;
    NPH_TRY(spawn_body(w, ids, "belt", NPH_ENTITY_NULL, at(0.0, 0.0, 0.0, 1.0F), NPH_U32_MAX, 0U,
                       &belt));
    const DemoOrbit belt_orbit = {0.05F};
    NPH_TRY(nph_world_add(w, belt, ids->orbit, &belt_orbit));
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 8U, 1U);
    for (u32 i = 0; i < asteroids; ++i) {
        const f32 angle = nph_pcg32_next_range_f32(&rng, 0.0F, NPH_TAU);
        const f32 radius = nph_pcg32_next_range_f32(&rng, 25.0F, 29.0F);
        const f32 height = nph_pcg32_next_range_f32(&rng, -0.6F, 0.6F);
        NphLocalTransform l =
            at((f64)(radius * nph_f32_cos(angle)), (f64)height, (f64)(radius * nph_f32_sin(angle)),
               nph_pcg32_next_range_f32(&rng, 0.08F, 0.3F));
        l.rotation = nph_quat_from_axis_angle(
            nph_vec3_normalize(nph_vec3_sub(nph_pcg32_next_vec3(&rng), nph_vec3_splat(0.5F))),
            angle);
        NphEntity rock;
        NPH_TRY(spawn_body(w, ids, "rock", belt, l, MESH_CUBE, 8U, &rock));
        const DemoSpin spin = {
            nph_vec3_normalize(nph_vec3_sub(nph_pcg32_next_vec3(&rng), nph_vec3_splat(0.5F))),
            nph_pcg32_next_range_f32(&rng, 0.5F, 3.0F)};
        NPH_TRY(nph_world_add(w, rock, ids->spin, &spin));
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

static void orbit_chunk(NphSystemContext *ctx, const NphChunkView *v)
{
    NphLocalTransform *l = NPH_CHUNK_COLUMN(v, 0, NphLocalTransform);
    const DemoOrbit *o = NPH_CHUNK_COLUMN(v, 1, DemoOrbit);
    for (u32 i = 0; i < v->count; ++i) {
        const NphQuat step = nph_quat_from_rotation_y(o[i].speed * (f32)ctx->dt);
        l[i].rotation = nph_quat_normalize(nph_quat_mul(step, l[i].rotation));
    }
}

static void spin_chunk(NphSystemContext *ctx, const NphChunkView *v)
{
    NphLocalTransform *l = NPH_CHUNK_COLUMN(v, 0, NphLocalTransform);
    const DemoSpin *s = NPH_CHUNK_COLUMN(v, 1, DemoSpin);
    for (u32 i = 0; i < v->count; ++i) {
        const NphQuat step = nph_quat_from_axis_angle(s[i].axis, s[i].speed * (f32)ctx->dt);
        l[i].rotation = nph_quat_normalize(nph_quat_mul(l[i].rotation, step));
    }
}

typedef struct RenderSync {
    NphRenderer *renderer;
    u32 updated; // instances updated by the last run
} RenderSync;

// Main thread: the render instances of chunks whose world transforms
// changed since the last run.
static void render_sync(NphSystemContext *ctx)
{
    RenderSync *rs = (RenderSync *)ctx->user;
    rs->updated = 0U;
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 0, NphWorldTransform);
        const DemoBody *b = NPH_CHUNK_COLUMN(&v, 1, DemoBody);
        for (u32 i = 0; i < v.count; ++i) {
            NphRenderTransform t;
            t.linear = nph_world_transform_linear(&wt[i]);
            t.translation = wt[i].translation;
            const NphRenderInstance inst = {b[i].instance};
            if (nph_render_instance_set_transform(rs->renderer, inst, &t) != NPH_RESULT_OK) {
                nph_system_report(ctx, NPH_RESULT_INVALID_ARGUMENT);
                return;
            }
            rs->updated += 1U;
        }
    }
}

// ---------------------------------------------------------------------------
// The scene file round trips
// ---------------------------------------------------------------------------

static NphResult round_trips(NphWorld *w, const Options *o)
{
    NphAllocator *a = nph_allocator_system();
    NphStrBuilder first;
    NphStrBuilder second;
    nph_sb_init(&first, a);
    nph_sb_init(&second, a);
    NphBinWriter bin;
    nph_bin_writer_init(&bin, a);
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = a;
    wd.max_entities = nph_world_entity_capacity(w);
    wd.registry_from = w;
    NphWorld *text_world = NULL;
    NphWorld *bin_world = NULL;
    NphSceneResult res;
    memset(&res, 0, sizeof res);

    NphStopwatch sw = nph_stopwatch_start();
    NphResult r = nph_scene_save_text(w, NULL, &first, &res);
    const f64 save_ms = nph_stopwatch_elapsed_ms(&sw);
    if (r == NPH_RESULT_OK) {
        r = nph_world_create(&wd, &text_world);
    }
    sw = nph_stopwatch_start();
    if (r == NPH_RESULT_OK) {
        r = nph_scene_load_text(text_world, nph_sb_str(&first), NULL, &res);
    }
    const f64 load_ms = nph_stopwatch_elapsed_ms(&sw);
    if (r == NPH_RESULT_OK) {
        r = nph_scene_save_text(text_world, NULL, &second, &res);
    }
    if (r == NPH_RESULT_OK && !nph_str_equals(nph_sb_str(&first), nph_sb_str(&second))) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "the text round trip changed the scene");
        r = NPH_RESULT_CORRUPT;
    }
    if (r == NPH_RESULT_OK) {
        NPH_LOG_INFO(&LOG_SAMPLE,
                     "text round trip: %u entities, %llu bytes, saved in %.1f ms, loaded in %.1f "
                     "ms, identical",
                     nph_world_entity_count(w), (unsigned long long)first.len, save_ms, load_ms);
    }

    sw = nph_stopwatch_start();
    if (r == NPH_RESULT_OK) {
        r = nph_scene_save_binary(w, NULL, &bin, &res);
    }
    const f64 bin_save_ms = nph_stopwatch_elapsed_ms(&sw);
    if (r == NPH_RESULT_OK) {
        r = nph_world_create(&wd, &bin_world);
    }
    sw = nph_stopwatch_start();
    if (r == NPH_RESULT_OK) {
        r = nph_scene_load_binary(bin_world, nph_bin_writer_data(&bin), nph_bin_writer_len(&bin),
                                  NULL, &res);
    }
    const f64 bin_load_ms = nph_stopwatch_elapsed_ms(&sw);
    nph_sb_clear(&second);
    if (r == NPH_RESULT_OK) {
        r = nph_scene_save_text(bin_world, NULL, &second, &res);
    }
    if (r == NPH_RESULT_OK && !nph_str_equals(nph_sb_str(&first), nph_sb_str(&second))) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "the binary round trip changed the scene");
        r = NPH_RESULT_CORRUPT;
    }
    if (r == NPH_RESULT_OK) {
        NPH_LOG_INFO(&LOG_SAMPLE,
                     "binary round trip: %llu bytes, saved in %.1f ms, loaded in %.1f ms, "
                     "identical",
                     (unsigned long long)nph_bin_writer_len(&bin), bin_save_ms, bin_load_ms);
    }
    if (r == NPH_RESULT_OK && o->save != NULL) {
        r = nph_file_write_all(o->save, first.data, first.len);
        if (r == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_SAMPLE, "saved the scene to %s", o->save);
        }
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "scene round trip failed: %s %s", nph_result_string(r),
                      res.error);
    }
    nph_world_destroy(bin_world);
    nph_world_destroy(text_world);
    nph_bin_writer_free(&bin);
    nph_sb_free(&first);
    nph_sb_free(&second);
    return r;
}

// ---------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------

typedef struct App {
    Options opt;
    NphAllocator *alloc;
    NphJobSystem *jobs;
    NphWorld *world;
    Ids ids;
    NphTransformSystem *transforms;
    NphSchedule *schedule;
    u32 render_sync_system;
    RenderSync sync;
    NphWindow *window;
    NphGpuDevice *dev;
    NphGpuSwapchain *swapchain;
    NphGpuRenderGraph *graph;
    NphRenderer *renderer;
    NphRenderMesh meshes[MESH_COUNT];
    NphRenderMaterial materials[MATERIAL_COUNT];
    NphGpuBuffer capture;
    NphGpuImage capture_image;
    NphRenderFrameStats render_stats; // the last frame's
} App;

static NphResult build_world(App *app)
{
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = app->alloc;
    NPH_TRY(nph_job_system_create(&jd, &app->jobs));
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = app->alloc;
    wd.max_entities = 16384U;
    NPH_TRY(nph_world_create(&wd, &app->world));
    NPH_TRY(nph_world_register_component(app->world, DemoSpin_component(), &app->ids.spin));
    NPH_TRY(nph_world_register_component(app->world, DemoOrbit_component(), &app->ids.orbit));
    NPH_TRY(nph_world_register_component(app->world, DemoBody_component(), &app->ids.body));
    NPH_TRY(build_orrery(app->world, &app->ids, app->opt.asteroids));
    NPH_TRY(nph_transform_system_create(app->world, app->alloc, &app->transforms));

    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.world = app->world;
    sd.jobs = app->jobs;
    sd.alloc = app->alloc;
    NPH_TRY(nph_schedule_create(&sd, &app->schedule));
    const NphQueryTerm orbit_terms[2] = {nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM),
                                         nph_term_read(app->ids.orbit)};
    NphSystemDesc orbit;
    memset(&orbit, 0, sizeof orbit);
    orbit.name = "demo.orbit";
    orbit.run_chunk = orbit_chunk;
    orbit.terms = orbit_terms;
    orbit.term_count = 2U;
    NPH_TRY(nph_schedule_add_system(app->schedule, &orbit, NULL));
    const NphQueryTerm spin_terms[2] = {nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM),
                                        nph_term_read(app->ids.spin)};
    NphSystemDesc spin;
    memset(&spin, 0, sizeof spin);
    spin.name = "demo.spin";
    spin.run_chunk = spin_chunk;
    spin.terms = spin_terms;
    spin.term_count = 2U;
    spin.chunks_per_job = 2U;
    NPH_TRY(nph_schedule_add_system(app->schedule, &spin, NULL));
    const NphSystemDesc transform = nph_transform_system_desc(app->transforms);
    NPH_TRY(nph_schedule_add_system(app->schedule, &transform, NULL));
    const NphQueryTerm sync_terms[2] = {
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM)),
        nph_term_read(app->ids.body)};
    NphSystemDesc sync;
    memset(&sync, 0, sizeof sync);
    sync.name = "demo.render_sync";
    sync.run = render_sync;
    sync.user = &app->sync;
    sync.terms = sync_terms;
    sync.term_count = 2U;
    sync.flags = NPH_SYSTEM_MAIN_THREAD;
    NPH_TRY(nph_schedule_add_system(app->schedule, &sync, &app->render_sync_system));
    NPH_TRY(nph_schedule_build(app->schedule));
    NphWorldStats ws;
    nph_world_get_stats(app->world, &ws);
    NPH_LOG_INFO(&LOG_SAMPLE, "%u entities in %u archetypes and %u chunks; %u stages", ws.entities,
                 ws.archetypes, ws.chunks, nph_schedule_stage_count(app->schedule));
    return NPH_RESULT_OK;
}

static void log_systems(const App *app)
{
    char line[256];
    int n = 0;
    for (u32 i = 0; i < nph_schedule_system_count(app->schedule); ++i) {
        NphSystemStats st;
        nph_schedule_system_stats(app->schedule, i, &st);
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%s %.3f ms", i > 0U ? ", " : "",
                      nph_schedule_system_name(app->schedule, i), nph_time_ns_to_ms(st.last_ns));
        if (n < 0 || (size_t)n >= sizeof line) {
            break;
        }
    }
    NphTransformStats ts;
    nph_transform_system_stats(app->transforms, &ts);
    NPH_LOG_INFO(&LOG_SAMPLE, "systems: %s; transforms: %u roots, %u children, %u levels", line,
                 ts.roots_updated, ts.children_updated, ts.levels);
    if (app->renderer == NULL) {
        return;
    }
    const NphRenderFrameStats *rs = &app->render_stats;
    if (rs->pipeline == NPH_RENDER_PIPELINE_PRODUCTION) {
        // The GPU's counters arrive a couple of frames late (vis_frame).
        NPH_LOG_INFO(&LOG_SAMPLE,
                     "render (production): %u instances, %u clusters (%u in software), %u "
                     "triangles, %u lights in view; culled: %u + %u instances (frustum + "
                     "occlusion), %u + %u + %u clusters (frustum + cone + occlusion)",
                     rs->instances, rs->vis_clusters, rs->vis_clusters_sw, rs->vis_triangles,
                     rs->vis_lights, rs->vis_instances_culled_frustum,
                     rs->vis_instances_culled_occlusion, rs->vis_meshlets_culled_frustum,
                     rs->vis_meshlets_culled_cone, rs->vis_meshlets_culled_occlusion);
        if (rs->vsm) {
            NPH_LOG_INFO(&LOG_SAMPLE,
                         "sun's virtual shadow map: %u pages needed, %u drawn, %u held, "
                         "resolution bias %u",
                         rs->vsm_requested, rs->vsm_drawn, rs->vsm_resident, rs->vsm_lod_bias);
        }
        if (rs->taa) {
            NPH_LOG_INFO(&LOG_SAMPLE, "temporal anti-aliasing%s",
                         rs->taa_history ? "" : " (history restarted)");
        }
    } else {
        NPH_LOG_INFO(&LOG_SAMPLE,
                     "render (reference): %u instances, %u culled, %u draws, %u triangles",
                     rs->instances, rs->culled, rs->draws, rs->triangles);
    }
    // The metering arrives a couple of frames late, and only with the
    // automatic exposure.
    if (rs->metered_ev100 != 0.0F) {
        NPH_LOG_INFO(&LOG_SAMPLE, "exposure: EV100 %.2f, metered %.2f%s", (f64)rs->ev100,
                     (f64)rs->metered_ev100, rs->bloom ? "; bloom" : "");
    } else {
        NPH_LOG_INFO(&LOG_SAMPLE, "exposure: EV100 %.2f%s", (f64)rs->ev100,
                     rs->bloom ? "; bloom" : "");
    }
}

static NphResult open_device(App *app, int *status)
{
    if (nph_window_system_init() != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no window system available; skipping the rendering");
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline scene_demo";
    if (nph_window_create(app->alloc, &wd, &app->window) != NPH_RESULT_OK) {
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = app->alloc;
    dd.app_name = "scene_demo";
    const NphResult r = nph_gpu_device_create(&dd, &app->dev);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no Vulkan device (%s); skipping the rendering",
                     nph_result_string(r));
        *status = r == NPH_RESULT_UNSUPPORTED ? EXIT_SKIPPED : 1;
        return r;
    }
    NphGpuSwapchainDesc scd;
    memset(&scd, 0, sizeof scd);
    scd.window = app->window;
    scd.vsync = app->opt.vsync;
    scd.prefer_srgb = 1;
    if (nph_gpu_swapchain_create(app->dev, &scd, &app->swapchain) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the window cannot be presented to; skipping the rendering");
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    return NPH_RESULT_OK;
}

static NphResult create_mesh(App *app, u32 index,
                             NphResult (*build)(NphAllocator *, NphRenderMeshData *))
{
    NphRenderMeshData data;
    NPH_TRY(build(app->alloc, &data));
    NphRenderSubmesh submesh;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, index == MESH_SPHERE ? "sphere" : "cube", &submesh, &md);
    const NphResult r = nph_render_mesh_create(app->renderer, &md, &app->meshes[index]);
    nph_render_mesh_data_free(&data);
    return r;
}

static NphResult build_sphere(NphAllocator *a, NphRenderMeshData *out)
{
    return nph_render_primitive_sphere(a, 1.0F, 32U, 16U, out);
}

static NphResult build_cube(NphAllocator *a, NphRenderMeshData *out)
{
    return nph_render_primitive_cube(a, 1.0F, out);
}

static NphResult create_renderer(App *app)
{
    NphRendererDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.alloc = app->alloc;
    rd.dev = app->dev;
    rd.shader_dir = NPH_SHADER_DIR;
    rd.name = "scene_demo";
    NPH_TRY(nph_renderer_create(&rd, &app->renderer));
    app->sync.renderer = app->renderer;
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    ed.intensity = 0.0005F; // deep space: a faint fill so the night sides read
    NPH_TRY(nph_render_set_environment(app->renderer, &ed));
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = app->alloc;
    gd.jobs = app->jobs;
    gd.name = "scene_demo";
    NPH_TRY(nph_gpu_render_graph_create(app->dev, &gd, &app->graph));
    NPH_TRY(create_mesh(app, MESH_SPHERE, build_sphere));
    NPH_TRY(create_mesh(app, MESH_CUBE, build_cube));
    for (u32 i = 0; i < MATERIAL_COUNT; ++i) {
        NphRenderMaterialDesc md;
        nph_render_material_desc_default(&md);
        md.base_color =
            nph_vec4(g_palette[i].color.x, g_palette[i].color.y, g_palette[i].color.z, 1.0F);
        md.metallic = g_palette[i].metallic;
        md.roughness = g_palette[i].roughness;
        md.emissive = nph_vec3_scale(g_palette[i].color, g_palette[i].emissive);
        NPH_TRY(nph_render_material_create(app->renderer, &md, &app->materials[i]));
    }
    // The sun lights the system from its centre.
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3(1.0F, 0.92F, 0.8F);
    ld.intensity = 400000.0F; // candela: about 4000 lux on the inner planets
    ld.position = nph_dvec3_zero();
    NphRenderLight light;
    NPH_TRY(nph_render_light_create(app->renderer, &ld, &light));

    // One render instance per body; its handle goes into the component.
    const NphQueryTerm t[2] = {nph_term_write(app->ids.body),
                               nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM)};
    const NphQueryDesc qd = {t, 2U};
    NphQuery *q = NULL;
    NPH_TRY(nph_query_create(app->world, &qd, &q));
    NphQueryIter it;
    nph_query_iter(q, &it);
    NphChunkView v;
    NphResult r = NPH_RESULT_OK;
    while (r == NPH_RESULT_OK && nph_query_next(&it, &v)) {
        DemoBody *b = NPH_CHUNK_COLUMN(&v, 0, DemoBody);
        for (u32 i = 0; r == NPH_RESULT_OK && i < v.count; ++i) {
            NphRenderInstanceDesc id;
            memset(&id, 0, sizeof id);
            id.mesh = app->meshes[b[i].mesh % MESH_COUNT];
            id.materials = &app->materials[b[i].material % MATERIAL_COUNT];
            id.material_count = 1U;
            id.transform = nph_render_transform_identity();
            NphRenderInstance inst;
            r = nph_render_instance_create(app->renderer, &id, &inst);
            b[i].instance = inst.value;
        }
    }
    nph_query_destroy(q);
    return r;
}

static void handle_input(App *app, bool *paused, f32 *distance, NphRenderView *view)
{
    nph_window_poll_events();
    NphWindowEvent e;
    while (nph_window_pop_event(app->window, &e)) {
        if (e.type == NPH_WINDOW_EVENT_MOUSE_SCROLL) {
            *distance = nph_f32_clamp(*distance * (1.0F - 0.1F * (f32)e.u.scroll.dy), 8.0F, 120.0F);
        } else if (e.type == NPH_WINDOW_EVENT_KEY && e.u.key.action == NPH_KEY_ACTION_PRESS) {
            if (e.u.key.key == NPH_KEY_ESCAPE) {
                nph_window_set_should_close(app->window, true);
            } else if (e.u.key.key == NPH_KEY_SPACE) {
                *paused = !*paused;
            } else if (e.u.key.key == NPH_KEY_P) {
                const bool production = view->pipeline == NPH_RENDER_PIPELINE_PRODUCTION;
                view->pipeline =
                    production ? NPH_RENDER_PIPELINE_REFERENCE : NPH_RENDER_PIPELINE_PRODUCTION;
                NPH_LOG_INFO(&LOG_SAMPLE, "pipeline: %s", production ? "reference" : "production");
            } else if (e.u.key.key == NPH_KEY_T) {
                const bool temporal = view->anti_aliasing == NPH_RENDER_AA_TEMPORAL;
                view->anti_aliasing = temporal ? NPH_RENDER_AA_NONE : NPH_RENDER_AA_TEMPORAL;
                NPH_LOG_INFO(&LOG_SAMPLE, "temporal anti-aliasing: %s", temporal ? "off" : "on");
            } else if (e.u.key.key == NPH_KEY_B) {
                view->bloom = view->bloom > 0.0F ? 0.0F : DEMO_BLOOM;
                NPH_LOG_INFO(&LOG_SAMPLE, "bloom: %s", view->bloom > 0.0F ? "on" : "off");
            } else if (e.u.key.key == NPH_KEY_O) {
                view->ao_radius = view->ao_radius > 0.0F ? 0.0F : DEMO_AO;
                NPH_LOG_INFO(&LOG_SAMPLE, "ambient occlusion: %s",
                             view->ao_radius > 0.0F ? "on" : "off");
            } else if (e.u.key.key == NPH_KEY_G) {
                view->gi = view->gi ? 0 : 1;
                NPH_LOG_INFO(&LOG_SAMPLE, "global illumination: %s", view->gi ? "on" : "off");
            } else if (e.u.key.key == NPH_KEY_E) {
                const bool automatic = view->exposure == NPH_RENDER_EXPOSURE_AUTO;
                view->exposure = automatic ? NPH_RENDER_EXPOSURE_MANUAL : NPH_RENDER_EXPOSURE_AUTO;
                NPH_LOG_INFO(&LOG_SAMPLE, "exposure: %s", automatic ? "manual" : "automatic");
            }
        }
    }
}

static void capture_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    App *app = (App *)user;
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region);
    nph_gpu_cmd_copy_image_to_buffer(cmd, app->capture_image, NPH_GPU_LAYOUT_TRANSFER_SRC,
                                     app->capture, &region, 1U);
}

static void add_capture(App *app, NphGpuImage backbuffer, NphGpuRgImage target)
{
    u32 w = 0U;
    u32 h = 0U;
    nph_gpu_swapchain_extent(app->swapchain, &w, &h);
    NphGpuBufferDesc cbd;
    memset(&cbd, 0, sizeof cbd);
    cbd.size = (u64)w * h * 4U;
    cbd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    cbd.memory = NPH_GPU_MEMORY_READBACK;
    cbd.name = "capture";
    if (nph_gpu_buffer_create(app->dev, &cbd, &app->capture) != NPH_RESULT_OK) {
        return;
    }
    app->capture_image = backbuffer;
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = "capture";
    pd.type = NPH_GPU_RG_PASS_TRANSFER;
    pd.execute = capture_execute;
    pd.user = app;
    NphGpuRgPass *copy = nph_gpu_rg_add_pass(app->graph, &pd);
    nph_gpu_rg_read_image(copy, target, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, 1U);
    nph_gpu_rg_write_buffer(copy, nph_gpu_rg_import_buffer(app->graph, app->capture),
                            NPH_GPU_RG_USE_TRANSFER_DST);
}

static bool render_frame(App *app, const NphRenderView *view, bool capturing, int *status)
{
    if (nph_gpu_frame_begin(app->dev) != NPH_RESULT_OK) {
        *status = 1;
        return false;
    }
    NphGpuImage backbuffer = NPH_GPU_IMAGE_NULL;
    const NphResult ar = nph_gpu_swapchain_acquire(app->swapchain, &backbuffer);
    if (ar == NPH_RESULT_EMPTY) { // minimised
        NPH_IGNORE_RESULT(nph_gpu_frame_end(app->dev));
        nph_thread_sleep_ns(16U * NPH_TIME_NS_PER_MS);
        return true;
    }
    NphResult r = ar;
    if (r == NPH_RESULT_OK) {
        nph_gpu_rg_begin(app->graph);
        NphGpuRgImage target = NPH_GPU_RG_IMAGE_NULL;
        r = nph_render_frame(app->renderer, view, app->graph, backbuffer, &target,
                             &app->render_stats);
        if (r == NPH_RESULT_OK && capturing) {
            add_capture(app, backbuffer, target);
        }
        if (r == NPH_RESULT_OK) {
            nph_gpu_rg_present(app->graph, target, app->swapchain);
            r = nph_gpu_rg_compile(app->graph);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_gpu_rg_execute(app->graph);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_gpu_swapchain_present(app->swapchain);
        }
    }
    const NphResult er = nph_gpu_frame_end(app->dev);
    if (r != NPH_RESULT_OK || er != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "frame failed: %s",
                      nph_result_string(r != NPH_RESULT_OK ? r : er));
        *status = 1;
        return false;
    }
    return true;
}

static void write_capture(App *app, int *status)
{
    if (!nph_gpu_buffer_is_valid(app->dev, app->capture) ||
        (nph_gpu_image_desc(app->dev, app->capture_image)->usage &
         (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC) == 0U) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the swapchain images cannot be read back; no capture");
        return;
    }
    if (nph_gpu_device_wait_idle(app->dev) != NPH_RESULT_OK) {
        *status = 1;
        return;
    }
    u32 w = 0U;
    u32 h = 0U;
    nph_gpu_swapchain_extent(app->swapchain, &w, &h);
    const NphGpuFormat f = nph_gpu_swapchain_format(app->swapchain);
    const bool bgra = f == NPH_GPU_FORMAT_B8G8R8A8_SRGB || f == NPH_GPU_FORMAT_B8G8R8A8_UNORM;
    const NphResult r = capture_write_bmp(
        app->opt.capture, (const u8 *)nph_gpu_buffer_map(app->dev, app->capture), w, h, bgra);
    if (r == NPH_RESULT_OK) {
        NPH_LOG_INFO(&LOG_SAMPLE, "captured the last frame to %s", app->opt.capture);
    } else {
        NPH_LOG_ERROR(&LOG_SAMPLE, "%s: %s", app->opt.capture, nph_result_string(r));
        *status = 1;
    }
}

// The simulation (and, with a renderer, the frames). Every step advances
// 1/60 s of simulated time, so --frames runs are reproducible.
static void run(App *app, int *status)
{
    const f64 dt = 1.0 / 60.0;
    const bool rendering = app->renderer != NULL;
    nph_schedule_set_enabled(app->schedule, app->render_sync_system, rendering);
    NphRenderView view;
    nph_render_view_default(&view,
                            nph_render_camera_look_at(nph_dvec3(0.0, 20.0, 50.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 0.9F, 0.1F));
    view.ev100 = 6.5F;
    view.pipeline = app->opt.pipeline;
    view.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
    view.bloom = DEMO_BLOOM;
    view.ao_radius = DEMO_AO;
    view.gi = 1; // as the runtime's r.gi
    // The automatic exposure (E) adapts one step's time per frame, so
    // --frames runs stay reproducible.
    view.frame_seconds = (f32)dt;
    bool paused = false;
    f32 distance = 52.0F;
    f32 angle = 0.6F;
    const u64 max = app->opt.max_frames;
    const NphStopwatch clock = nph_stopwatch_start();
    NphStopwatch report = nph_stopwatch_start();
    u64 index = 0U;
    while ((max == 0U || index < max) && (!rendering || !nph_window_should_close(app->window))) {
        if (rendering) {
            handle_input(app, &paused, &distance, &view);
        }
        if (!paused) {
            const NphResult r = nph_schedule_run(app->schedule, dt);
            if (r != NPH_RESULT_OK) {
                NPH_LOG_ERROR(&LOG_SAMPLE, "schedule run failed: %s",
                              nph_schedule_error(app->schedule));
                *status = 1;
                return;
            }
            angle += 0.002F;
        }
        if (rendering) {
            const NphDVec3 eye =
                nph_dvec3((f64)(distance * nph_f32_sin(angle)), (f64)(distance * 0.42F),
                          (f64)(distance * nph_f32_cos(angle)));
            view.camera =
                nph_render_camera_look_at(eye, nph_dvec3_zero(), nph_vec3_unit_y(), 0.9F, 0.1F);
            const bool capturing = app->opt.capture != NULL && index + 1U == max;
            if (!render_frame(app, &view, capturing, status)) {
                return;
            }
        }
        ++index;
        if (index == 1U || nph_stopwatch_elapsed_ms(&report) > 2000.0) {
            log_systems(app);
            report = nph_stopwatch_start();
        }
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%llu frames in %.1f ms", (unsigned long long)index,
                 nph_stopwatch_elapsed_ms(&clock));
    log_systems(app);
    if (rendering && app->opt.capture != NULL) {
        write_capture(app, status);
    }
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof app);
    if (!parse(argc, argv, &app.opt)) {
        return usage();
    }
    app.alloc = nph_allocator_system();
    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = app.alloc;
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

    int status = 0;
    if (build_world(&app) != NPH_RESULT_OK || round_trips(app.world, &app.opt) != NPH_RESULT_OK) {
        status = 1;
    } else if (app.opt.headless) {
        run(&app, &status);
    } else if (open_device(&app, &status) == NPH_RESULT_OK) {
        if (create_renderer(&app) == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_SAMPLE,
                         "%s; keys: Space pause, P pipeline, T anti-aliasing, B bloom, E "
                         "exposure, wheel zoom, Escape quit",
                         nph_gpu_device_info(app.dev)->name);
            run(&app, &status);
        } else {
            status = 1;
        }
    } else if (status == EXIT_SKIPPED) {
        // No window or device: the scene still runs, headless, so the
        // sample checks its systems everywhere; the result says "skipped".
        app.opt.max_frames = app.opt.max_frames == 0U ? 240U : app.opt.max_frames;
        int sim_status = 0;
        run(&app, &sim_status);
        status = sim_status != 0 ? sim_status : EXIT_SKIPPED;
    }

    nph_schedule_destroy(app.schedule);
    nph_transform_system_destroy(app.transforms);
    nph_world_destroy(app.world);
    if (app.dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(app.dev));
        nph_gpu_buffer_destroy(app.dev, app.capture);
        nph_gpu_render_graph_destroy(app.graph);
        nph_renderer_destroy(app.renderer);
        nph_gpu_swapchain_destroy(app.swapchain);
        const NphGpuValidationStats vs = nph_gpu_device_validation_stats(app.dev);
        if (vs.errors != 0U) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "%llu validation errors", (unsigned long long)vs.errors);
            status = 1;
        }
        nph_gpu_device_destroy(app.dev);
    }
    nph_window_destroy(app.window);
    nph_window_system_shutdown();
    nph_job_system_destroy(app.jobs);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}

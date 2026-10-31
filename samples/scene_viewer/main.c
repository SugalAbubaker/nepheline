// SPDX-License-Identifier: Apache-2.0
// scene_viewer: a cooked scene to walk through yourself, the reference scenes
// included (docs/benchmarks.md "Reference scenes"), with the frame rate in the
// window's title. render_perf measures these scenes headless along scripted
// cameras; this draws them as it does, with its renderer sizes and its view
// (the production path with temporal anti-aliasing, bloom, ambient occlusion
// and the radiance cache), the world partition's cells and their proxies
// streamed (samples/common/cooked_scene.c), into an image of --size that is
// scaled into the window, so that a smaller display still draws the target's
// pixels.
//
//     scene_viewer --manifest=benchmarks/scenes/landscape.json --scenes=DIR [--view=NAME]
//     scene_viewer --pack=FILE --scene=KEY [--ev100=X] [--sky=X] [--sun-shadows]
//
//     and either: [--size=WxH] [--window=WxH] [--vsync] [--lists=N] [--voxel-pool=MB]
//                 [--proxy-budget=MB] [--texture-budget=MB] [--tiles=WxH] [--walk=M/S]
//                 [--frames=N]
//
// --size is the image drawn (3840x2160, the CLAUDE.md §10 target) and --window
// the window (1920x1080, a clean half of it). --lists multiplies the lists that
// can drop work (16, as ADR-0066 measured the landscape, which overflows the
// defaults; 1: the renderer's own). --voxel-pool sizes the pool of meshes of
// voxels alone, and with it streams the proxies within --proxy-budget (1200 MB
// and 1100 MB for a manifest that cooks voxels, else none). --texture-budget
// streams the textures' levels within that many MB (none: resident whole).
// --tiles lays the scene out W by H times (a manifest's own `tiles` by default).
// --walk moves the camera along its view by itself, and --frames ends the run.
//
// Keys: W/A/S/D move, Q/E down and up, Shift four times as fast, the mouse wheel
// changes the speed, the right mouse button held looks around, 1 to 9 jump to the
// manifest's views, G logs each pass's GPU time, Escape quits. The title gives the
// frame rate, the frame time and the worst frame's over the last half second, and
// the GPU memory in use; it says so where the lists overflow (work is dropped, so
// the numbers flatter the frame) or where proxies are still arriving.
//
// Exit status 77: no window system or Vulkan device (CTest "skipped"); 2: a bad
// command line; 1: the content did not load, a frame failed, or a validation
// error was reported.

#include <nepheline/asset/vfs.h>
#include <nepheline/core/json.h>
#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>
#include <nepheline/render/gpu_profiler.h>
#include <nepheline/render/renderer.h>

#include "cooked_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "scene_viewer", NPH_LOG_LEVEL_INFO);

#define EXIT_BAD_ARGS 2
#define EXIT_SKIPPED  77

// render_perf's renderer sizes, so that the memory is what it measures.
#define MAX_INSTANCES 119041U
#define MAX_MESHES    16384U
#define MAX_CELLS     4096U
#define MAX_LIGHTS    4001U
// Per tile past the first, where the manifest does not say: as render_perf's
// (a tile of the landscape, rounded up).
#define TILE_INSTANCES 49152U
#define TILE_MESHES    4096U
#define TILE_CELLS     512U

#define MAX_VIEWS     16U
#define VOXEL_POOL_MB 1200 // ADR-0066: what the landscape's views hold
#define PROXY_SLACK   100  // of the pool, kept from the stream: its coarsest levels

#define FLY_SPEED    4.0F   // metres per second, as the sandbox's
#define FLY_BOOST    4.0F   // with Shift held
#define LOOK_RADIANS 0.003F // per screen unit of mouse motion
#define PITCH_LIMIT  1.5F   // radians, short of looking straight up or down
#define MAX_STEP     0.25F  // seconds: a stall does not throw the camera away
#define TITLE_PERIOD (NPH_TIME_NS_PER_S / 2U)

typedef struct ViewDef {
    char name[32];
    bool scene_camera;
    NphDVec3 eye;
    NphDVec3 target;
    f32 yfov;
    f32 znear;
    f32 yaw; // degrees about the world's up axis
} ViewDef;

// What to draw and how to see it: a manifest's (benchmarks/scenes), or
// --pack and --scene with one view of the scene's own camera.
typedef struct SceneDef {
    char name[64];
    char pack[1024];
    char scene[128];
    f32 ev100;
    f32 sky;
    bool sun_shadows;
    bool voxels;
    u32 tiles_x;
    u32 tiles_z;
    NphDVec3 tile_pitch; // 0: the first tile's instances' extent
    u32 tile_instances;  // 0: TILE_INSTANCES past the first, and so the others
    u32 tile_meshes;
    u32 tile_cells;
    ViewDef views[MAX_VIEWS];
    u32 view_count;
} SceneDef;

typedef struct Options {
    const char *manifest;
    const char *scenes;
    const char *view;
    const char *pack;
    const char *scene;
    f32 ev100;
    f32 sky;
    bool sun_shadows;
    u32 width; // the image drawn
    u32 height;
    u32 window_width;
    u32 window_height;
    bool vsync;
    u32 lists;
    i64 voxel_pool_mb;   // -1: by the scene (VOXEL_POOL_MB where it cooks voxels)
    i64 proxy_budget_mb; // -1: the pool less PROXY_SLACK
    u64 texture_budget_mb;
    u32 tiles_x; // 0: as the manifest says
    u32 tiles_z;
    f32 walk;
    u64 max_frames;
} Options;

// The camera the keys and the mouse move.
typedef struct Fly {
    NphDVec3 position;
    f32 yaw;
    f32 pitch;
    f32 yfov;
    f32 znear;
    f32 speed;
    bool forward;
    bool back;
    bool left;
    bool right;
    bool down;
    bool up;
    bool boost;
    bool looking;
    bool have_cursor; // the last motion's position is known
    f64 cursor_x;
    f64 cursor_y;
} Fly;

// The title's measurements, over the last TITLE_PERIOD.
typedef struct TitleClock {
    u64 since_ns;
    u64 last_ns;
    u64 frames;
    u64 worst_ns;
} TitleClock;

typedef struct App {
    Options opt;
    SceneDef def;
    NphAllocator *alloc;
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    NphWindow *window;
    NphGpuDevice *dev;
    NphGpuSwapchain *swapchain;
    NphGpuRenderGraph *graph;
    NphRenderer *renderer;
    NphRenderGpuProfiler *profiler;
    CookedScene scene;
    NphGpuImage image;      // what is drawn, at --size
    NphGpuImage backbuffer; // this frame's window image
    Fly fly;
    TitleClock title;
    NphRenderFrameStats stats;
} App;

// ---------------------------------------------------------------------------
// The command line and the manifest
// ---------------------------------------------------------------------------

static int usage(void)
{
    fprintf(stderr, "usage: scene_viewer --manifest=FILE --scenes=DIR [--view=NAME]\n"
                    "       scene_viewer --pack=FILE --scene=KEY [--ev100=X] [--sky=X] "
                    "[--sun-shadows]\n"
                    "       and either: [--size=WxH] [--window=WxH] [--vsync] [--lists=N] "
                    "[--voxel-pool=MB]\n"
                    "                   [--proxy-budget=MB] [--texture-budget=MB] [--tiles=WxH] "
                    "[--walk=M/S] [--frames=N]\n");
    return EXIT_BAD_ARGS;
}

// What follows `name` in `arg`; NULL if `arg` does not start with it.
static const char *value_of(const char *arg, const char *name)
{
    const size_t n = strlen(name);
    return strncmp(arg, name, n) == 0 ? arg + n : NULL;
}

static bool parse_size(const char *text, u32 *out_width, u32 *out_height)
{
    char *end = NULL;
    const unsigned long w = strtoul(text, &end, 10);
    if (end == text || *end != 'x') {
        return false;
    }
    const char *rest = end + 1;
    const unsigned long h = strtoul(rest, &end, 10);
    if (end == rest || *end != '\0' || w == 0UL || h == 0UL || w > 16384UL || h > 16384UL) {
        return false;
    }
    *out_width = (u32)w;
    *out_height = (u32)h;
    return true;
}

static bool parse(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof *o);
    o->ev100 = 12.0F;
    o->sky = 1.0F;
    o->width = 3840U;
    o->height = 2160U;
    o->window_width = 1920U;
    o->window_height = 1080U;
    o->lists = 16U;
    o->voxel_pool_mb = -1;
    o->proxy_budget_mb = -1;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = NULL;
        if ((v = value_of(a, "--manifest=")) != NULL) {
            o->manifest = v;
        } else if ((v = value_of(a, "--scenes=")) != NULL) {
            o->scenes = v;
        } else if ((v = value_of(a, "--view=")) != NULL) {
            o->view = v;
        } else if ((v = value_of(a, "--pack=")) != NULL) {
            o->pack = v;
        } else if ((v = value_of(a, "--scene=")) != NULL) {
            o->scene = v;
        } else if ((v = value_of(a, "--ev100=")) != NULL) {
            o->ev100 = strtof(v, NULL);
        } else if ((v = value_of(a, "--sky=")) != NULL) {
            o->sky = strtof(v, NULL);
        } else if (strcmp(a, "--sun-shadows") == 0) {
            o->sun_shadows = true;
        } else if ((v = value_of(a, "--size=")) != NULL) {
            if (!parse_size(v, &o->width, &o->height)) {
                return false;
            }
        } else if ((v = value_of(a, "--window=")) != NULL) {
            if (!parse_size(v, &o->window_width, &o->window_height)) {
                return false;
            }
        } else if (strcmp(a, "--vsync") == 0) {
            o->vsync = true;
        } else if ((v = value_of(a, "--lists=")) != NULL) {
            o->lists = (u32)strtoul(v, NULL, 10);
        } else if ((v = value_of(a, "--voxel-pool=")) != NULL) {
            o->voxel_pool_mb = (i64)strtoll(v, NULL, 10);
        } else if ((v = value_of(a, "--proxy-budget=")) != NULL) {
            o->proxy_budget_mb = (i64)strtoll(v, NULL, 10);
        } else if ((v = value_of(a, "--texture-budget=")) != NULL) {
            o->texture_budget_mb = strtoull(v, NULL, 10);
        } else if ((v = value_of(a, "--tiles=")) != NULL) {
            if (!parse_size(v, &o->tiles_x, &o->tiles_z)) {
                return false;
            }
        } else if ((v = value_of(a, "--walk=")) != NULL) {
            o->walk = strtof(v, NULL);
        } else if ((v = value_of(a, "--frames=")) != NULL) {
            o->max_frames = strtoull(v, NULL, 10);
        } else {
            return false;
        }
    }
    const bool from_manifest = o->manifest != NULL && o->scenes != NULL;
    const bool from_pack = o->pack != NULL && o->scene != NULL;
    return from_manifest != from_pack && o->lists != 0U && o->sky > 0.0F;
}

static bool json_vec3(const NphJsonValue *object, const char *key, NphDVec3 *out)
{
    const NphJsonValue *a = nph_json_object_get(object, nph_str_from_cstr(key));
    f64 v[3];
    for (u32 i = 0; i < 3U; ++i) {
        if (nph_json_as_f64(nph_json_array_at(a, i), &v[i]) != NPH_RESULT_OK) {
            return false;
        }
    }
    *out = nph_dvec3(v[0], v[1], v[2]);
    return true;
}

// A two-element array as x and z.
static bool json_vec2(const NphJsonValue *object, const char *key, NphDVec3 *out)
{
    const NphJsonValue *a = nph_json_object_get(object, nph_str_from_cstr(key));
    f64 v[2];
    for (u32 i = 0; i < 2U; ++i) {
        if (nph_json_as_f64(nph_json_array_at(a, i), &v[i]) != NPH_RESULT_OK) {
            return false;
        }
    }
    *out = nph_dvec3(v[0], 0.0, v[1]);
    return true;
}

static void json_string(const NphJsonValue *object, const char *key, char *out, u64 cap)
{
    NphStr v = NPH_STR("");
    NPH_IGNORE_RESULT(nph_json_as_str(nph_json_object_get(object, nph_str_from_cstr(key)), &v));
    const u64 n = v.len < cap - 1U ? v.len : cap - 1U;
    memcpy(out, v.data, (size_t)n);
    out[n] = '\0';
}

// The fields of a view that render_perf reads, but for its scripted motion:
// here the camera moves as it is told.
static bool parse_view(const NphJsonValue *v, ViewDef *out)
{
    json_string(v, "name", out->name, sizeof out->name);
    out->scene_camera = nph_json_get_bool(v, NPH_STR("scene_camera"), false);
    out->yfov = (f32)nph_json_get_f64(v, NPH_STR("yfov"), 0.9);
    out->znear = (f32)nph_json_get_f64(v, NPH_STR("near"), 0.1);
    out->yaw = (f32)nph_json_get_f64(v, NPH_STR("yaw"), 0.0);
    return out->name[0] != '\0' && (out->scene_camera || (json_vec3(v, "eye", &out->eye) &&
                                                          json_vec3(v, "target", &out->target)));
}

static bool parse_manifest(const App *app, SceneDef *def)
{
    void *text = NULL;
    u64 size = 0U;
    if (nph_file_read_all(app->opt.manifest, app->alloc, &text, &size) != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "cannot read %s", app->opt.manifest);
        return false;
    }
    NphJsonDoc doc;
    bool ok = nph_json_parse(nph_str((const char *)text, size), app->alloc, &doc) == NPH_RESULT_OK;
    if (ok) {
        const NphJsonValue *root = doc.root;
        char pack[512];
        json_string(root, "name", def->name, sizeof def->name);
        json_string(root, "pack", pack, sizeof pack);
        json_string(root, "scene", def->scene, sizeof def->scene);
        (void)snprintf(def->pack, sizeof def->pack, "%s/%s", app->opt.scenes, pack);
        def->ev100 = (f32)nph_json_get_f64(root, NPH_STR("ev100"), 12.0);
        def->sky = (f32)nph_json_get_f64(root, NPH_STR("sky_intensity"), 1.0);
        def->sun_shadows = nph_json_get_bool(root, NPH_STR("sun_shadows"), false);
        const NphJsonValue *cook = nph_json_object_get(root, NPH_STR("cook"));
        def->voxels = cook != NULL && nph_json_get_bool(cook, NPH_STR("voxels"), false);
        const NphJsonValue *tiles = nph_json_object_get(root, NPH_STR("tiles"));
        if (tiles != NULL) {
            NphDVec3 count = nph_dvec3(1.0, 0.0, 1.0);
            ok = json_vec2(tiles, "count", &count) && json_vec2(tiles, "pitch", &def->tile_pitch);
            def->tiles_x = ok ? (u32)count.x : 0U;
            def->tiles_z = ok ? (u32)count.z : 0U;
            def->tile_instances = (u32)nph_json_get_f64(tiles, NPH_STR("instances"), 0.0);
            def->tile_meshes = (u32)nph_json_get_f64(tiles, NPH_STR("meshes"), 0.0);
            def->tile_cells = (u32)nph_json_get_f64(tiles, NPH_STR("cells"), 0.0);
        }
        const NphJsonValue *views = nph_json_object_get(root, NPH_STR("views"));
        for (u32 i = 0; i < MAX_VIEWS && ok; ++i) {
            const NphJsonValue *v = nph_json_array_at(views, i);
            if (v == NULL) {
                break;
            }
            ok = parse_view(v, &def->views[def->view_count]);
            def->view_count += ok ? 1U : 0U;
        }
        ok = ok && def->name[0] != '\0' && pack[0] != '\0' && def->scene[0] != '\0' &&
             def->view_count != 0U && def->sky > 0.0F;
        nph_json_doc_free(&doc);
    }
    nph_file_free_all(app->alloc, text, size);
    if (!ok) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "%s: not a scene manifest (docs/benchmarks.md)",
                      app->opt.manifest);
    }
    return ok;
}

static bool define_scene(App *app)
{
    SceneDef *def = &app->def;
    memset(def, 0, sizeof *def);
    if (app->opt.manifest != NULL) {
        return parse_manifest(app, def);
    }
    (void)snprintf(def->name, sizeof def->name, "%s", app->opt.scene);
    (void)snprintf(def->pack, sizeof def->pack, "%s", app->opt.pack);
    (void)snprintf(def->scene, sizeof def->scene, "%s", app->opt.scene);
    def->ev100 = app->opt.ev100;
    def->sky = app->opt.sky;
    def->sun_shadows = app->opt.sun_shadows;
    def->tiles_x = app->opt.tiles_x;
    def->tiles_z = app->opt.tiles_z;
    (void)snprintf(def->views[0].name, sizeof def->views[0].name, "the scene's camera");
    def->views[0].scene_camera = true;
    def->view_count = 1U;
    return true;
}

// ---------------------------------------------------------------------------
// The camera
// ---------------------------------------------------------------------------

// Takes up `camera`: its forward direction as a yaw and a pitch (cameras look
// down -Z), which the mouse then changes.
static void fly_set_camera(Fly *f, const NphRenderCamera *camera)
{
    const NphVec3 forward = nph_quat_rotate(camera->orientation, nph_vec3(0.0F, 0.0F, -1.0F));
    f->position = camera->position;
    f->yaw = nph_f32_atan2(-forward.x, -forward.z);
    f->pitch = nph_f32_asin(forward.y);
    f->yfov = camera->yfov;
    f->znear = camera->znear;
}

// The camera that sees the whole of what has no camera of its own.
static NphRenderCamera camera_of_bounds(const CookedScene *scene)
{
    const NphVec3 center = nph_aabb_center(scene->bounds);
    const f32 reach =
        nph_f32_max(nph_vec3_length(nph_vec3_sub(scene->bounds.max, scene->bounds.min)), 1.0F);
    const NphVec3 eye = nph_vec3_add(center, nph_vec3(0.0F, 0.4F * reach, 0.9F * reach));
    return nph_render_camera_look_at(nph_dvec3_from_vec3(eye), nph_dvec3_from_vec3(center),
                                     nph_vec3_unit_y(), 0.9F, 0.05F);
}

static void go_to_view(App *app, u32 index)
{
    const ViewDef *v = &app->def.views[index];
    NphRenderCamera camera;
    if (!v->scene_camera) {
        camera = nph_render_camera_look_at(v->eye, v->target, nph_vec3_unit_y(), v->yfov, v->znear);
    } else if (app->scene.has_camera) {
        camera = app->scene.camera;
    } else {
        camera = camera_of_bounds(&app->scene);
    }
    if (v->yaw != 0.0F) {
        camera.orientation = nph_quat_normalize(
            nph_quat_mul(nph_quat_from_axis_angle(nph_vec3_unit_y(), v->yaw * (NPH_PI / 180.0F)),
                         camera.orientation));
    }
    fly_set_camera(&app->fly, &camera);
    NPH_LOG_INFO(&LOG_SAMPLE, "view: %s", v->name);
}

// A step of `dt` seconds: the keys along the camera's level directions, and
// --walk along its view, level, as render_perf's walks.
static void fly_step(Fly *f, f32 walk, f32 dt)
{
    const NphQuat yaw = nph_quat_from_rotation_y(f->yaw);
    const NphVec3 forward = nph_quat_rotate(yaw, nph_vec3(0.0F, 0.0F, -1.0F));
    const NphVec3 right = nph_quat_rotate(yaw, nph_vec3(1.0F, 0.0F, 0.0F));
    const f32 along = (f->forward ? 1.0F : 0.0F) - (f->back ? 1.0F : 0.0F);
    const f32 across = (f->right ? 1.0F : 0.0F) - (f->left ? 1.0F : 0.0F);
    const f32 rise = (f->up ? 1.0F : 0.0F) - (f->down ? 1.0F : 0.0F);
    const f32 speed = f->speed * (f->boost ? FLY_BOOST : 1.0F);
    NphVec3 move = nph_vec3_scale(forward, along * speed + walk);
    move = nph_vec3_add(move, nph_vec3_scale(right, across * speed));
    move = nph_vec3_add(move, nph_vec3_scale(nph_vec3_unit_y(), rise * speed));
    f->position = nph_dvec3_add(f->position, nph_dvec3_from_vec3(nph_vec3_scale(move, dt)));
}

static NphRenderCamera fly_camera(const Fly *f)
{
    NphRenderCamera c;
    c.position = f->position;
    c.orientation = nph_quat_normalize(
        nph_quat_mul(nph_quat_from_rotation_y(f->yaw), nph_quat_from_rotation_x(f->pitch)));
    c.yfov = f->yfov;
    c.znear = f->znear;
    return c;
}

static void set_looking(App *app, bool looking)
{
    app->fly.looking = looking;
    app->fly.have_cursor = false; // the position jumps as the mode changes
    nph_window_set_cursor_mode(app->window,
                               looking ? NPH_CURSOR_MODE_DISABLED : NPH_CURSOR_MODE_NORMAL);
}

static void key_event(App *app, u32 key, u32 action)
{
    Fly *f = &app->fly;
    if (action == (u32)NPH_KEY_ACTION_REPEAT) {
        return;
    }
    const bool down = action == (u32)NPH_KEY_ACTION_PRESS;
    switch (key) {
    case NPH_KEY_W:
        f->forward = down;
        break;
    case NPH_KEY_S:
        f->back = down;
        break;
    case NPH_KEY_A:
        f->left = down;
        break;
    case NPH_KEY_D:
        f->right = down;
        break;
    case NPH_KEY_Q:
        f->down = down;
        break;
    case NPH_KEY_E:
        f->up = down;
        break;
    case NPH_KEY_LEFT_SHIFT:
        f->boost = down;
        break;
    case NPH_KEY_ESCAPE:
        nph_window_set_should_close(app->window, true);
        break;
    case NPH_KEY_G:
        if (down) {
            nph_render_gpu_profiler_log(app->profiler);
        }
        break;
    default:
        if (down && key >= (u32)NPH_KEY_1 && key <= (u32)NPH_KEY_9 &&
            key - (u32)NPH_KEY_1 < app->def.view_count) {
            go_to_view(app, key - (u32)NPH_KEY_1);
        }
        break;
    }
}

static void mouse_moved(Fly *f, f64 x, f64 y)
{
    if (f->looking && f->have_cursor) {
        f->yaw -= (f32)(x - f->cursor_x) * LOOK_RADIANS;
        f->pitch = nph_f32_clamp(f->pitch - (f32)(y - f->cursor_y) * LOOK_RADIANS, -PITCH_LIMIT,
                                 PITCH_LIMIT);
    }
    f->cursor_x = x;
    f->cursor_y = y;
    f->have_cursor = true;
}

static void handle_input(App *app)
{
    Fly *f = &app->fly;
    nph_window_poll_events();
    NphWindowEvent e;
    while (nph_window_pop_event(app->window, &e)) {
        if (e.type == (u32)NPH_WINDOW_EVENT_KEY) {
            key_event(app, e.u.key.key, e.u.key.action);
        } else if (e.type == (u32)NPH_WINDOW_EVENT_MOUSE_BUTTON) {
            if (e.u.mouse_button.button == (u32)NPH_MOUSE_BUTTON_RIGHT) {
                set_looking(app, e.u.mouse_button.action == (u32)NPH_KEY_ACTION_PRESS);
            }
        } else if (e.type == (u32)NPH_WINDOW_EVENT_MOUSE_MOVE) {
            mouse_moved(f, e.u.mouse_move.x, e.u.mouse_move.y);
        } else if (e.type == (u32)NPH_WINDOW_EVENT_MOUSE_SCROLL) {
            f->speed = nph_f32_clamp(f->speed * (1.0F + 0.25F * (f32)e.u.scroll.dy), 0.25F, 200.0F);
            NPH_LOG_INFO(&LOG_SAMPLE, "speed: %.2f m/s", (f64)f->speed);
        } else if (e.type == (u32)NPH_WINDOW_EVENT_FOCUS && !e.u.focus.focused) {
            // The releases go to whichever window has the focus now.
            f->forward = f->back = f->left = f->right = f->down = f->up = f->boost = false;
            set_looking(app, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The window's title
// ---------------------------------------------------------------------------

// The frame rate and what stands behind it, twice a second (frames counted
// over the real time they took; --vsync would cap them at the display's).
static void title_frame(App *app)
{
    TitleClock *c = &app->title;
    const u64 now = nph_time_now_ns();
    if (c->since_ns == 0U) {
        c->since_ns = now;
        c->last_ns = now;
        return;
    }
    const u64 frame_ns = now - c->last_ns;
    c->last_ns = now;
    c->frames += 1U;
    c->worst_ns = frame_ns > c->worst_ns ? frame_ns : c->worst_ns;
    const u64 elapsed = now - c->since_ns;
    if (elapsed < TITLE_PERIOD) {
        return;
    }
    const f64 seconds = (f64)elapsed / (f64)NPH_TIME_NS_PER_S;
    const NphGpuMemoryStats mem = nph_gpu_device_memory_stats(app->dev);
    char note[64] = "";
    if (app->stats.vis_overflow != 0U || app->stats.vsm_overflow != 0U) {
        (void)snprintf(note, sizeof note, " - LISTS OVERFLOWED: work dropped");
    } else if (app->scene.stream != NULL) {
        NphProxyStreamStats ps;
        nph_proxy_stream_stats(app->scene.stream, &ps);
        if (ps.failed != 0U) {
            (void)snprintf(note, sizeof note, " - %u proxy levels failed", ps.failed);
        } else if (ps.loading != 0U) {
            (void)snprintf(note, sizeof note, " - %u proxy levels arriving", ps.loading);
        }
    }
    char title[192];
    (void)snprintf(title, sizeof title, "%s %ux%u - %.1f fps - %.2f ms, worst %.2f - %.2f GB%s",
                   app->def.name, app->opt.width, app->opt.height, (f64)c->frames / seconds,
                   1000.0 * seconds / (f64)c->frames, (f64)c->worst_ns / 1.0e6,
                   (f64)mem.device_local_used / 1.0e9, note);
    nph_window_set_title(app->window, title);
    c->since_ns = now;
    c->frames = 0U;
    c->worst_ns = 0U;
}

// ---------------------------------------------------------------------------
// Setting up
// ---------------------------------------------------------------------------

static NphResult load_content(App *app)
{
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = app->alloc;
    NPH_TRY(nph_job_system_create(&jd, &app->jobs));
    NPH_TRY(nph_vfs_create(app->alloc, &app->vfs));
    const NphResult r = nph_vfs_mount_pack(app->vfs, NPH_STR("content"), app->def.pack, NULL);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE,
                      "cannot mount %s: %s (scripts/scenes.py cooks the reference "
                      "scenes)",
                      app->def.pack, nph_result_string(r));
        return r;
    }
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = app->alloc;
    ld.vfs = app->vfs;
    ld.jobs = app->jobs;
    return nph_asset_loader_create(&ld, &app->loader);
}

static NphResult open_device(App *app, int *status)
{
    if (nph_window_system_init() != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no window system available; skipping");
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline scene_viewer";
    wd.width = app->opt.window_width;
    wd.height = app->opt.window_height;
    if (nph_window_create(app->alloc, &wd, &app->window) != NPH_RESULT_OK) {
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = app->alloc;
    dd.app_name = "scene_viewer";
    const NphResult r = nph_gpu_device_create(&dd, &app->dev);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no Vulkan device (%s); skipping", nph_result_string(r));
        *status = r == NPH_RESULT_UNSUPPORTED ? EXIT_SKIPPED : 1;
        return r;
    }
    NphGpuSwapchainDesc scd;
    memset(&scd, 0, sizeof scd);
    scd.window = app->window;
    scd.vsync = app->opt.vsync;
    if (nph_gpu_swapchain_create(app->dev, &scd, &app->swapchain) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the window cannot be presented to; skipping");
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    return NPH_RESULT_OK;
}

// The image drawn, in the encoding of the window's: the renderer encodes sRGB
// itself into a UNORM target, and a blit copies the values as they are.
static NphResult create_image(App *app)
{
    const NphGpuFormat f = nph_gpu_swapchain_format(app->swapchain);
    const bool srgb = f == NPH_GPU_FORMAT_B8G8R8A8_SRGB || f == NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    NphGpuImageDesc td;
    memset(&td, 0, sizeof td);
    td.type = NPH_GPU_IMAGE_2D;
    td.format = srgb ? NPH_GPU_FORMAT_R8G8B8A8_SRGB : NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    td.width = app->opt.width;
    td.height = app->opt.height;
    td.mip_count = 1U;
    td.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    td.name = "scene_viewer image";
    return nph_gpu_image_create(app->dev, &td, &app->image);
}

// The tiles asked for: --tiles over the manifest's, one where neither says.
static u32 tile_count(const App *app, u32 *out_x, u32 *out_z)
{
    const u32 x = app->opt.tiles_x != 0U ? app->opt.tiles_x : nph_max_u32(app->def.tiles_x, 1U);
    const u32 z = app->opt.tiles_z != 0U ? app->opt.tiles_z : nph_max_u32(app->def.tiles_z, 1U);
    if (out_x != NULL) {
        *out_x = x;
    }
    if (out_z != NULL) {
        *out_z = z;
    }
    return x * z;
}

// As render_perf's reference scenes: every list that can drop work --lists
// times its default (the renderer clamps them to what its records can
// address).
static NphResult create_renderer(App *app, u64 voxel_pool_bytes)
{
    const u32 lists = app->opt.lists;
    NphRendererDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.alloc = app->alloc;
    rd.dev = app->dev;
    rd.shader_dir = NPH_SHADER_DIR;
    rd.name = "scene_viewer";
    // A tile's instances, cells and streamed levels are its own.
    const SceneDef *def = &app->def;
    const u32 tiles = tile_count(app, NULL, NULL);
    if (def->tile_instances != 0U) {
        rd.max_instances = nph_max_u32(MAX_INSTANCES, tiles * def->tile_instances);
        rd.max_meshes = MAX_MESHES + tiles * def->tile_meshes;
        rd.max_cells = MAX_CELLS + tiles * def->tile_cells;
    } else {
        rd.max_instances = MAX_INSTANCES + (tiles - 1U) * TILE_INSTANCES;
        rd.max_meshes = MAX_MESHES + (tiles - 1U) * TILE_MESHES;
        rd.max_cells = MAX_CELLS + (tiles - 1U) * TILE_CELLS;
    }
    rd.max_draws = MAX_INSTANCES;
    rd.max_lights = MAX_LIGHTS;
    rd.voxel_pool_bytes = voxel_pool_bytes;
    rd.max_deferred = (1U << 20U) * lists;
    if (lists > 1U) {
        rd.max_visible_clusters = 524288U * lists;
        rd.max_cull_meshlets = (1U << 20U) * lists;
        rd.max_lod_nodes = (1U << 18U) * lists;
        // The flat cull's want no more than the city's: voxel meshes, the landscape's,
        // whose lists these scale for, walk their hierarchies.
        rd.max_lod_groups = (1U << 18U) * nph_min_u32(lists, 4U);
        rd.max_vsm_meshlets = (1U << 20U) * lists;
        rd.max_vsm_draws = (1U << 20U) * lists;
        rd.max_vsm_voxel_draws = (1U << 20U) * lists;
    }
    if (app->opt.texture_budget_mb != 0U) {
        // The pool holds the budget and the textures' small levels.
        rd.texture_page_bytes = app->opt.texture_budget_mb * 1000000ULL + NPH_MIB(512);
    }
    NPH_TRY(nph_renderer_create(&rd, &app->renderer));
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    ed.intensity = app->def.sky;
    NPH_TRY(nph_render_set_environment(app->renderer, &ed));
    NphRenderGpuProfilerDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.alloc = app->alloc;
    pd.max_passes = 256U;
    NPH_TRY(nph_render_gpu_profiler_create(&pd, &app->profiler));
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = app->alloc;
    gd.jobs = app->jobs;
    gd.name = "scene_viewer";
    return nph_gpu_render_graph_create(app->dev, &gd, &app->graph);
}

// The scene, its proxies streamed where there is a pool for them.
static NphResult load_scene(App *app, u64 voxel_pool_bytes)
{
    const SceneDef *def = &app->def;
    CookedSceneDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = app->alloc;
    cd.renderer = app->renderer;
    cd.loader = app->loader;
    cd.jobs = app->jobs;
    cd.scene = nph_guid_from_cstr(def->scene);
    (void)tile_count(app, &cd.tiles_x, &cd.tiles_z);
    cd.tile_pitch = def->tile_pitch;
    cd.stream = voxel_pool_bytes != 0U ? 1 : 0;
    if (app->opt.proxy_budget_mb > 0) {
        cd.stream_budget = (u64)app->opt.proxy_budget_mb * 1000000ULL;
    } else if (app->opt.proxy_budget_mb < 0 && voxel_pool_bytes != 0U) {
        const u64 slack = (u64)PROXY_SLACK * 1000000ULL;
        cd.stream_budget = voxel_pool_bytes > slack ? voxel_pool_bytes - slack : 0U;
    }
    cd.paged_textures = app->opt.texture_budget_mb != 0U ? 1 : 0;
    cd.texture_budget = app->opt.texture_budget_mb * 1000000ULL;
    const u64 t0 = nph_time_now_ns();
    NPH_TRY(cooked_scene_load(&cd, &app->scene));
    NPH_LOG_INFO(&LOG_SAMPLE,
                 "%s: loaded in %.1f s: %u instances, %u lights, %u cells with proxies", def->name,
                 (f64)(nph_time_now_ns() - t0) / 1.0e9, app->scene.instance_count,
                 app->scene.light_count, app->scene.proxy_count);
    if (def->sun_shadows) {
        // glTF has no shadows to cast; the scene's sun casts them, as a
        // game's would (the production path: its virtual shadow map).
        for (u32 i = 0; i < app->scene.light_count; ++i) {
            NphRenderLightDesc d = app->scene.light_descs[i];
            if (d.type == NPH_RENDER_LIGHT_TYPE_DIRECTIONAL) {
                d.shadows = 1;
                NPH_TRY(nph_render_light_update(app->renderer, app->scene.lights[i], &d));
                break;
            }
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// A frame
// ---------------------------------------------------------------------------

static void blit_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    const App *app = (const App *)user;
    nph_gpu_cmd_blit_image(cmd, app->image, 0U, NPH_GPU_LAYOUT_TRANSFER_SRC, app->backbuffer, 0U,
                           NPH_GPU_LAYOUT_TRANSFER_DST, NPH_GPU_FILTER_LINEAR);
}

// The image into the window, and the window to the display.
static NphResult declare_present(App *app, NphGpuRgImage image)
{
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = "into the window";
    pd.type = NPH_GPU_RG_PASS_TRANSFER;
    pd.execute = blit_execute;
    pd.user = app;
    NphGpuRgPass *pass = nph_gpu_rg_add_pass(app->graph, &pd);
    if (pass == NULL) {
        return NPH_RESULT_FULL;
    }
    const NphGpuRgImage back = nph_gpu_rg_import_image(app->graph, app->backbuffer);
    nph_gpu_rg_read_image(pass, image, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, 1U);
    nph_gpu_rg_write_image(pass, back, NPH_GPU_RG_USE_TRANSFER_DST, 0U, 1U);
    nph_gpu_rg_present(app->graph, back, app->swapchain);
    return NPH_RESULT_OK;
}

// One frame: returns false to stop (with *status set on failure).
static bool frame(App *app, const NphRenderView *view, u64 index, int *status)
{
    if (nph_gpu_frame_begin(app->dev) != NPH_RESULT_OK) {
        *status = 1;
        return false;
    }
    NPH_IGNORE_RESULT(nph_render_gpu_profiler_update(app->profiler, app->graph));
    const NphResult ar = nph_gpu_swapchain_acquire(app->swapchain, &app->backbuffer);
    if (ar == NPH_RESULT_EMPTY) { // minimised
        NPH_IGNORE_RESULT(nph_gpu_frame_end(app->dev));
        nph_thread_sleep_ns(16U * NPH_TIME_NS_PER_MS);
        return true;
    }
    if (ar != NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(nph_gpu_frame_end(app->dev));
        *status = 1;
        return false;
    }
    nph_gpu_rg_begin(app->graph);
    // The streams' levels and pages, after the frame began (their uploads go
    // with it) and before it is drawn.
    NphResult r = cooked_scene_update(&app->scene, view, app->opt.height);
    NphGpuRgImage image = NPH_GPU_RG_IMAGE_NULL;
    if (r == NPH_RESULT_OK) {
        r = nph_render_frame(app->renderer, view, app->graph, app->image, &image, &app->stats);
    }
    if (r == NPH_RESULT_OK) {
        r = declare_present(app, image);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_rg_compile(app->graph);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_rg_execute(app->graph);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_swapchain_present(app->swapchain);
    }
    const NphResult er = nph_gpu_frame_end(app->dev);
    if (r != NPH_RESULT_OK || er != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "frame %llu failed: %s", (unsigned long long)index,
                      nph_result_string(r != NPH_RESULT_OK ? r : er));
        *status = 1;
        return false;
    }
    return true;
}

static u32 first_view(const App *app)
{
    if (app->opt.view == NULL) {
        return 0U;
    }
    for (u32 i = 0; i < app->def.view_count; ++i) {
        if (strcmp(app->def.views[i].name, app->opt.view) == 0) {
            return i;
        }
    }
    NPH_LOG_WARN(&LOG_SAMPLE, "no view %s: starting at %s", app->opt.view, app->def.views[0].name);
    return 0U;
}

static void run(App *app, int *status)
{
    app->fly.speed = FLY_SPEED;
    go_to_view(app, first_view(app));
    NphRenderView view;
    nph_render_view_default(&view, fly_camera(&app->fly));
    view.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    view.ev100 = app->def.ev100;
    view.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
    view.bloom = 0.04F;    // r.bloom's default
    view.ao_radius = 0.5F; // r.ao's default
    view.gi = 1;
    const u64 max = app->opt.max_frames;
    const u64 start_ns = nph_time_now_ns();
    u64 last_ns = start_ns;
    u64 index = 0U;
    while (!nph_window_should_close(app->window) && (max == 0U || index < max)) {
        handle_input(app);
        const u64 now = nph_time_now_ns();
        const f32 dt = nph_f32_min((f32)((f64)(now - last_ns) / 1.0e9), MAX_STEP);
        last_ns = now;
        fly_step(&app->fly, app->opt.walk, dt);
        view.camera = fly_camera(&app->fly);
        if (!frame(app, &view, index, status)) {
            return;
        }
        title_frame(app);
        ++index;
    }
    const f64 seconds = (f64)(nph_time_now_ns() - start_ns) / 1.0e9;
    NPH_LOG_INFO(&LOG_SAMPLE,
                 "%llu frames in %.1f s: %.1f fps; the last frame: %u visible clusters, "
                 "%u triangles%s",
                 (unsigned long long)index, seconds, seconds > 0.0 ? (f64)index / seconds : 0.0,
                 app->stats.vis_clusters, app->stats.vis_triangles,
                 app->stats.vis_overflow != 0U ? ", LISTS OVERFLOWED" : "");
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
    if (!define_scene(&app) || load_content(&app) != NPH_RESULT_OK) {
        status = 1;
    } else if (open_device(&app, &status) == NPH_RESULT_OK) {
        const i64 pool_mb = app.opt.voxel_pool_mb >= 0 ? app.opt.voxel_pool_mb
                                                       : (app.def.voxels ? VOXEL_POOL_MB : 0);
        const u64 pool_bytes = (u64)pool_mb * 1000000ULL;
        if (create_image(&app) == NPH_RESULT_OK &&
            create_renderer(&app, pool_bytes) == NPH_RESULT_OK &&
            load_scene(&app, pool_bytes) == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_SAMPLE,
                         "keys: W/A/S/D move, Q/E down and up, Shift faster, the wheel changes "
                         "the speed, hold the right mouse button to look, 1 to %u the views, "
                         "G the passes' GPU times, Escape quits",
                         app.def.view_count);
            run(&app, &status);
        } else {
            status = 1;
        }
    }

    if (app.dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(app.dev));
        cooked_scene_unload(&app.scene);
        nph_gpu_image_destroy(app.dev, app.image);
        nph_gpu_render_graph_destroy(app.graph);
        nph_render_gpu_profiler_destroy(app.profiler);
        nph_renderer_destroy(app.renderer);
        nph_gpu_swapchain_destroy(app.swapchain);
        const NphGpuValidationStats vs = nph_gpu_device_validation_stats(app.dev);
        if (vs.errors != 0U) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "%llu validation errors", (unsigned long long)vs.errors);
            status = 1;
        }
        nph_gpu_device_destroy(app.dev);
    } else {
        cooked_scene_unload(&app.scene);
    }
    nph_window_destroy(app.window);
    nph_window_system_shutdown();
    nph_asset_loader_destroy(app.loader);
    nph_vfs_destroy(app.vfs);
    nph_job_system_destroy(app.jobs);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}

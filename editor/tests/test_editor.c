// SPDX-License-Identifier: Apache-2.0
// The editor on an offscreen engine (editor/src/editor.h): it draws its
// interface and the scene under the validation gate, its camera follows
// mouse and key events on the viewport, Play hands the input to the game
// and Escape takes it back, and the console panel receives the log. Skipped
// without a Vulkan device.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/guid.h>
#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/device.h>
#include <nepheline/gpu/resources.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/scene/transform.h>

#include <ntest/ntest.h>

#include "editor_internal.h"
#include "test_cook_support.h"

#include <cook/cooker.h>
#include <cook/material.h>
#include <stdio.h>
#include <string.h>

#define WIDTH  960U
#define HEIGHT 600U

typedef struct EditorFixture {
    NphDebugAllocator engine_memory;
    NphDebugAllocator editor_memory;
    char project[900]; // a temporary directory for the project's sources and cooked files
    bool keep_project; // finish() leaves it for another editor
    char assets_assignment[1000];
    char cooked_assignment[1000];
    char autosave_assignment[1000];
    NphEngineConfig config;
    NphEngine *engine;
    Editor *editor;
} EditorFixture;

// `project`: an existing project's directory (another editor's), NULL for a
// new one.
static NphResult start_in(EditorFixture *f, NphEngineMode mode, EditorLogSink *log,
                          const char *project)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->engine_memory, nph_allocator_system());
    nph_debug_allocator_init(&f->editor_memory, nph_allocator_system());
    // No layout file; sound on the null device (real time, no hardware), so
    // the statistics panel shows a live mixer.
    static const char *const assignments[2] = {"editor.layout=", "audio.device=null"};
    // The project in a directory of its own.
    static u32 counter = 0U;
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    if (project != NULL) {
        (void)snprintf(f->project, sizeof f->project, "%s", project);
    } else {
        (void)snprintf(f->project, sizeof f->project, "%s/nph_editor_test_%llu_%u", tmp,
                       (unsigned long long)nph_platform_process_id(), counter++);
    }
    (void)snprintf(f->assets_assignment, sizeof f->assets_assignment, "editor.assets=%s/assets",
                   f->project);
    (void)snprintf(f->cooked_assignment, sizeof f->cooked_assignment, "editor.cooked=%s/cooked",
                   f->project);
    // Autosave sessions in the project too, not the tests' directory.
    (void)snprintf(f->autosave_assignment, sizeof f->autosave_assignment,
                   "editor.autosave_dir=%s/autosave", f->project);
    f->config.assignments[0] = assignments[0];
    f->config.assignments[1] = f->assets_assignment;
    f->config.assignments[2] = f->cooked_assignment;
    f->config.assignments[3] = assignments[1];
    f->config.assignments[4] = f->autosave_assignment;
    f->config.assignment_count = 5U;
    f->config.packs[0] = NPH_TEST_PACK;
    f->config.pack_count = 1U;
    f->config.headless = mode == NPH_ENGINE_MODE_HEADLESS ? 1 : 0;
    EditorDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.alloc = nph_debug_allocator_allocator(&f->editor_memory);
    ed.log = log;
    NTEST_ASSERT_EQ_INT(editor_create(&ed, &f->editor), NPH_RESULT_OK);
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->engine_memory);
    d.config = &f->config;
    d.mode = mode;
    d.app_name = "editor tests";
    d.offscreen_width = WIDTH;
    d.offscreen_height = HEIGHT;
    editor_engine_desc(f->editor, &d);
    NPH_TRY(nph_engine_create(&d, &f->engine));
    return editor_attach(f->editor, f->engine);
}

static NphResult start(EditorFixture *f, NphEngineMode mode, EditorLogSink *log)
{
    return start_in(f, mode, log, NULL);
}

static void finish(EditorFixture *f)
{
    if (f->engine != NULL && nph_engine_device(f->engine) != NULL) {
        NPH_IGNORE_RESULT(nph_engine_wait(f->engine));
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(nph_engine_device(f->engine)));
        const NphGpuValidationStats vs =
            nph_gpu_device_validation_stats(nph_engine_device(f->engine));
        NTEST_EXPECT_EQ_UINT(vs.errors, 0U);
        NTEST_EXPECT_EQ_UINT(vs.warnings, 0U);
    }
    editor_detach(f->editor);
    nph_engine_destroy(f->engine);
    editor_destroy(f->editor);
    f->engine = NULL;
    f->editor = NULL;
    if (!f->keep_project && f->project[0] != '\0' && nph_fs_exists(f->project)) {
        NTEST_EXPECT_EQ_INT(nph_fs_remove_tree(f->project), NPH_RESULT_OK);
    }
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->editor_memory, NULL, NULL), 0U);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->engine_memory, NULL, NULL), 0U);
}

// Starts an offscreen editor, or skips the test without a device.
#define START_OR_SKIP(fx, log)                                                                     \
    do {                                                                                           \
        const NphResult started_ = start((fx), NPH_ENGINE_MODE_OFFSCREEN, (log));                  \
        if (started_ == NPH_RESULT_UNSUPPORTED) {                                                  \
            finish(fx);                                                                            \
            NTEST_SKIP("no Vulkan device available");                                              \
        }                                                                                          \
        NTEST_ASSERT_EQ_INT(started_, NPH_RESULT_OK);                                              \
    } while (0)

static void frames(EditorFixture *f, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
    }
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static bool feed(EditorFixture *f, NphWindowEvent ev)
{
    return editor_feed_event(f->editor, &ev);
}

static NphWindowEvent key(NphKey k, NphKeyAction action)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_KEY;
    ev.u.key.key = (u32)k;
    ev.u.key.action = (u32)action;
    return ev;
}

static NphWindowEvent move_to(f32 x, f32 y)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_MOUSE_MOVE;
    ev.u.mouse_move.x = (f64)x;
    ev.u.mouse_move.y = (f64)y;
    return ev;
}

static NphWindowEvent button(NphMouseButton b, NphKeyAction action)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_MOUSE_BUTTON;
    ev.u.mouse_button.button = (u32)b;
    ev.u.mouse_button.action = (u32)action;
    return ev;
}

static NphWindowEvent wheel(f64 dy)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_MOUSE_SCROLL;
    ev.u.scroll.dy = dy;
    return ev;
}

// The middle of the viewport's image, as the last frame laid it out.
static void viewport_center(EditorFixture *f, f32 *x, f32 *y)
{
    f32 rx = 0.0F;
    f32 ry = 0.0F;
    f32 rw = 0.0F;
    f32 rh = 0.0F;
    NTEST_ASSERT_TRUE(editor_viewport_rect(f->editor, &rx, &ry, &rw, &rh));
    *x = rx + rw * 0.5F;
    *y = ry + rh * 0.5F;
}

// ---------------------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------------------

// Copies an RGBA8 image into `out` through a readback buffer; the device is
// idle before and after.
static void read_image(NphGpuDevice *dev, NphGpuImage image, u8 *out, u64 size)
{
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = size;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.memory = NPH_GPU_MEMORY_READBACK;
    bd.name = "editor test readback";
    NphGpuBuffer rb = NPH_GPU_BUFFER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_buffer_create(dev, &bd, &rb), NPH_RESULT_OK);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "editor readback", &cmd),
                        NPH_RESULT_OK);
    NphGpuImageBarrier b;
    memset(&b, 0, sizeof b);
    b.image = image;
    b.src_stages = (u32)NPH_GPU_STAGE_ALL;
    b.src_access = (u32)NPH_GPU_ACCESS_MEMORY_WRITE;
    b.old_layout = nph_gpu_image_layout(dev, image, 0U);
    b.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
    b.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
    b.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
    b.mip_count = 1U;
    b.layer_count = NPH_GPU_ALL_LAYERS;
    NphGpuBarrierDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.images = &b;
    desc.image_count = 1U;
    nph_gpu_cmd_barrier(cmd, &desc);
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.layer_count = 1U;
    nph_gpu_cmd_copy_image_to_buffer(cmd, image, NPH_GPU_LAYOUT_TRANSFER_SRC, rb, &region, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, rb, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_HOST,
                               (u32)NPH_GPU_ACCESS_HOST_READ);
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.queue = NPH_GPU_QUEUE_GRAPHICS;
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    u64 value = 0U;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(dev, &sd, &value), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_gpu_timeline_wait(dev, nph_gpu_queue_timeline(dev, sd.queue), value, NPH_U64_MAX),
        NPH_RESULT_OK);
    memcpy(out, nph_gpu_buffer_map(dev, rb), (size_t)size);
    nph_gpu_buffer_destroy(dev, rb);
}

// The RGBA pixels of `image` after the frames so far (allocated).
static u8 *pixels_of(EditorFixture *f, NphGpuImage image, u64 *out_size)
{
    NphGpuDevice *dev = nph_engine_device(f->engine);
    NTEST_ASSERT_EQ_INT(nph_engine_wait(f->engine), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
    const NphGpuImageDesc *d = nph_gpu_image_desc(dev, image);
    NTEST_ASSERT_NOT_NULL(d);
    const u64 size = (u64)d->width * d->height * 4U;
    u8 *pixels = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, size);
    NTEST_ASSERT_NOT_NULL(pixels);
    read_image(dev, image, pixels, size);
    *out_size = size;
    return pixels;
}

static void free_pixels(u8 *pixels, u64 size)
{
    NPH_FREE_ARRAY(nph_allocator_system(), u8, pixels, size);
}

// Pixels whose colour differs from `ref`'s at the same place.
static u64 differing(const u8 *a, const u8 *ref, u64 size)
{
    u64 n = 0U;
    for (u64 i = 0; i < size; i += 4U) {
        n += memcmp(a + i, ref + i, 3U) != 0 ? 1U : 0U;
    }
    return n;
}

// Pixels whose colour differs from the first pixel's.
static u64 not_uniform(const u8 *a, u64 size)
{
    u64 n = 0U;
    for (u64 i = 4U; i < size; i += 4U) {
        n += memcmp(a + i, a, 3U) != 0 ? 1U : 0U;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

NTEST(Editor, draws_its_interface_and_the_scene_in_the_viewport)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    NphEngine *e = fx.engine;
    NphGpuDevice *dev = nph_engine_device(e);
    // The first frames dock the panels and size the viewport's image.
    frames(&fx, 3U);
    const NphGpuImage viewport = editor_viewport_image(fx.editor);
    NTEST_ASSERT_NE_UINT(viewport.value, 0U);
    const NphGpuImageDesc *vd = nph_gpu_image_desc(dev, viewport);
    NTEST_ASSERT_NOT_NULL(vd);
    NTEST_EXPECT_GT_UINT(vd->width, WIDTH / 3U); // docked beside the other panels
    NTEST_EXPECT_LT_UINT(vd->width, WIDTH);
    NTEST_EXPECT_GT_UINT(vd->height, HEIGHT / 3U);
    NTEST_EXPECT_LT_UINT(vd->height, HEIGHT);
    f32 rx = 0.0F;
    f32 ry = 0.0F;
    f32 rw = 0.0F;
    f32 rh = 0.0F;
    NTEST_ASSERT_TRUE(editor_viewport_rect(fx.editor, &rx, &ry, &rw, &rh));
    NTEST_EXPECT_NEAR_F32(rw, (f32)vd->width, 1.0F); // a framebuffer scale of 1
    NTEST_EXPECT_NEAR_F32(rh, (f32)vd->height, 1.0F);

    // The sky alone, then the showcase in the edited world, seen through
    // the editor's camera.
    u64 size = 0U;
    u8 *sky = pixels_of(&fx, viewport, &size);
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = nph_engine_edit_world(e);
    d.loader = nph_engine_loader(e);
    d.scene = nph_guid_from_cstr("showcase.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_OK);
    frames(&fx, 1U);
    nph_scene_renderer_wait(nph_engine_scene_renderer(e));
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->scene.instances, 2U);
    u64 size2 = 0U;
    u8 *scene = pixels_of(&fx, viewport, &size2);
    NTEST_ASSERT_EQ_UINT(size2, size);
    NTEST_EXPECT_GT_UINT(differing(scene, sky, size), size / 4U / 50U); // over 2%
    free_pixels(sky, size);
    free_pixels(scene, size2);

    // The interface fills the target around the viewport: menus, panels,
    // text, not one colour.
    u64 ui_size = 0U;
    u8 *ui = pixels_of(&fx, nph_engine_offscreen_target(e), &ui_size);
    NTEST_EXPECT_EQ_UINT(ui_size, (u64)WIDTH * HEIGHT * 4U);
    NTEST_EXPECT_GT_UINT(not_uniform(ui, ui_size), ui_size / 4U / 10U);
    free_pixels(ui, ui_size);
    finish(&fx);
}

NTEST(Editor, the_camera_follows_the_mouse_and_the_keys)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    frames(&fx, 3U);
    EditorCamera *cam = editor_camera(fx.editor);
    f32 cx = 0.0F;
    f32 cy = 0.0F;
    viewport_center(&fx, &cx, &cy);

    // A right drag looks around: 100 pixels to the right turn the view right.
    (void)feed(&fx, move_to(cx, cy));
    frames(&fx, 1U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_RIGHT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 1U);
    const f32 yaw0 = cam->yaw;
    const NphDVec3 eye0 = editor_camera_eye(cam);
    (void)feed(&fx, move_to(cx + 100.0F, cy));
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(cam->yaw, nph_f32_wrap_angle(yaw0 - 0.4F), 1e-4F);
    const NphDVec3 eye1 = editor_camera_eye(cam);
    NTEST_EXPECT_NEAR_F64(eye1.x, eye0.x, 1e-4); // looking keeps the eye
    NTEST_EXPECT_NEAR_F64(eye1.z, eye0.z, 1e-4);

    // W flies forward while the button is held.
    const NphVec3 forward = editor_camera_forward(cam);
    (void)feed(&fx, key(NPH_KEY_W, NPH_KEY_ACTION_PRESS));
    frames(&fx, 4U);
    (void)feed(&fx, key(NPH_KEY_W, NPH_KEY_ACTION_RELEASE));
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_RIGHT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 1U);
    const NphDVec3 eye2 = editor_camera_eye(cam);
    const f64 along = (eye2.x - eye1.x) * (f64)forward.x + (eye2.y - eye1.y) * (f64)forward.y +
                      (eye2.z - eye1.z) * (f64)forward.z;
    NTEST_EXPECT_TRUE(along > 0.0);
    NTEST_EXPECT_FALSE(fx.editor->viewport.fly);

    // Without the button, the wheel moves toward the pivot.
    const f32 distance = cam->distance;
    (void)feed(&fx, wheel(1.0));
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(cam->distance, distance * 0.85F, 1e-4F);

    // The view the engine renders is the editor's camera.
    const NphRenderView *view = nph_engine_view(fx.engine);
    const NphDVec3 eye = editor_camera_eye(cam);
    NTEST_EXPECT_NEAR_F64(view->camera.position.x, eye.x, 1e-4);
    NTEST_EXPECT_NEAR_F64(view->camera.position.z, eye.z, 1e-4);
    finish(&fx);
}

NTEST(Editor, play_hands_the_input_to_the_game_and_escape_takes_it_back)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    NphEngine *e = fx.engine;
    frames(&fx, 3U);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(e));
    NTEST_EXPECT_TRUE(nph_engine_world(e) == nph_engine_edit_world(e));

    // F5 plays a copy of the edited world.
    (void)feed(&fx, key(NPH_KEY_F5, NPH_KEY_ACTION_PRESS));
    (void)feed(&fx, key(NPH_KEY_F5, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_ASSERT_TRUE(nph_engine_is_playing(e));
    NTEST_EXPECT_FALSE(nph_engine_world(e) == nph_engine_edit_world(e));
    // The editor keeps the input until the viewport is clicked. (ImGui
    // applies queued events that interleave a frame apart, so each group
    // gets a few frames.)
    NTEST_EXPECT_TRUE(feed(&fx, key(NPH_KEY_A, NPH_KEY_ACTION_PRESS)));
    NTEST_EXPECT_TRUE(feed(&fx, key(NPH_KEY_A, NPH_KEY_ACTION_RELEASE)));
    frames(&fx, 3U);
    f32 cx = 0.0F;
    f32 cy = 0.0F;
    viewport_center(&fx, &cx, &cy);
    (void)feed(&fx, move_to(cx, cy));
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(fx.editor->viewport.game_input);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    NTEST_ASSERT_TRUE(fx.editor->viewport.game_input);
    // Now the game sees the input, the release of that click included.
    NTEST_EXPECT_FALSE(feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE)));
    NTEST_EXPECT_FALSE(feed(&fx, key(NPH_KEY_A, NPH_KEY_ACTION_PRESS)));
    NTEST_EXPECT_FALSE(feed(&fx, key(NPH_KEY_F5, NPH_KEY_ACTION_PRESS))); // the game's F5
    // Escape takes it back.
    NTEST_EXPECT_TRUE(feed(&fx, key(NPH_KEY_ESCAPE, NPH_KEY_ACTION_PRESS)));
    NTEST_EXPECT_FALSE(fx.editor->viewport.game_input);
    NTEST_EXPECT_TRUE(feed(&fx, key(NPH_KEY_ESCAPE, NPH_KEY_ACTION_RELEASE)));
    frames(&fx, 1U);
    NTEST_EXPECT_TRUE(nph_engine_is_playing(e));

    // F5 again stops, and the edited world is back.
    (void)feed(&fx, key(NPH_KEY_F5, NPH_KEY_ACTION_PRESS));
    (void)feed(&fx, key(NPH_KEY_F5, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(e));
    NTEST_EXPECT_TRUE(nph_engine_world(e) == nph_engine_edit_world(e));
    finish(&fx);
}

NTEST(Editor, the_console_panel_receives_the_log)
{
    NphLogConfig lc;
    memset(&lc, 0, sizeof lc);
    lc.alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_log_init(&lc), NPH_RESULT_OK);
    static EditorLogSink sink;
    NTEST_ASSERT_EQ_INT(editor_log_sink_init(&sink, nph_allocator_system(), 1024U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_log_add_sink(editor_log_sink_fn, &sink, NPH_LOG_LEVEL_TRACE),
                        NPH_RESULT_OK);
    static EditorFixture fx;
    const NphResult started = start(&fx, NPH_ENGINE_MODE_OFFSCREEN, &sink);
    if (started == NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "a line for the console panel");
        (void)nph_log_flush(0U);
        frames(&fx, 1U);
        const EditorLogLines *lines = &fx.editor->console.lines;
        bool found = false;
        for (u32 i = 0; i < editor_log_lines_count(lines); ++i) {
            const EditorLogLine *line = editor_log_lines_at(lines, i);
            if (strstr(line->text, "a line for the console panel") != NULL) {
                found = true;
                NTEST_EXPECT_EQ_UINT(line->level, (u32)NPH_LOG_LEVEL_WARN);
            }
        }
        NTEST_EXPECT_TRUE(found);
    }
    finish(&fx);
    (void)nph_log_remove_sink(editor_log_sink_fn, &sink);
    nph_log_shutdown();
    editor_log_sink_free(&sink);
    if (started == NPH_RESULT_UNSUPPORTED) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_EXPECT_EQ_INT(started, NPH_RESULT_OK);
}

NTEST(Editor, attaching_refuses_a_headless_engine)
{
    static EditorFixture fx;
    NTEST_EXPECT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS, NULL), NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_NOT_NULL(fx.engine);
    NTEST_EXPECT_EQ_UINT(editor_viewport_image(fx.editor).value, 0U);
    finish(&fx); // detaching an editor that never attached is harmless
}

// ---------------------------------------------------------------------------
// Scenes and edits
// ---------------------------------------------------------------------------

static void temp_path(const char *name, char *out, u64 cap)
{
    char dir[400];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(dir, sizeof dir, &len), NPH_RESULT_OK);
    (void)snprintf(out, (size_t)cap, "%s/nph-editor-tests-%llu-%s", dir,
                   (unsigned long long)nph_platform_process_id(), name);
}

// The live entity named `name` in the edited world, or null.
static NphEntity find_named(EditorFixture *f, const char *name)
{
    const NphWorld *w = nph_engine_edit_world(f->engine);
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphName *n = NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_NAME, NphName);
        if (n != NULL && strcmp(n->text.text, name) == 0) {
            return e;
        }
    }
    return NPH_ENTITY_NULL;
}

static void press(EditorFixture *f, NphKey k)
{
    (void)feed(f, key(k, NPH_KEY_ACTION_PRESS));
    frames(f, 2U);
    (void)feed(f, key(k, NPH_KEY_ACTION_RELEASE));
    frames(f, 2U);
}

static void chord(EditorFixture *f, NphKey modifier, NphKey k)
{
    (void)feed(f, key(modifier, NPH_KEY_ACTION_PRESS));
    frames(f, 2U);
    press(f, k);
    (void)feed(f, key(modifier, NPH_KEY_ACTION_RELEASE));
    frames(f, 2U);
}

NTEST(Editor, scenes_save_and_open_and_unsaved_changes_are_tracked)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    NphEngine *e = fx.engine;
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    NTEST_EXPECT_EQ_STR(editor_scene_path(ed), "");

    // Build: a parent, a child with a light.
    EditorHistory *h = ed->history;
    NphLocalTransform t = nph_local_transform_identity();
    t.translation.x = 2.0;
    NphEntity post = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(h, "lamp post", NPH_ENTITY_NULL, &t, &post),
                        NPH_RESULT_OK);
    t.translation = nph_dvec3(0.0, 3.0, 0.0);
    NphEntity bulb = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(h, "bulb", post, &t, &bulb), NPH_RESULT_OK);
    const NphComponentId light = nph_engine_components(e)->light;
    NTEST_ASSERT_EQ_INT(editor_edit_add_component(h, bulb, light), NPH_RESULT_OK);
    frames(&fx, 2U);
    NTEST_EXPECT_TRUE(editor_scene_dirty(ed));

    // Save: clean, with a file.
    char path[600];
    temp_path("scene.nscene", path, sizeof path);
    NTEST_ASSERT_EQ_INT(editor_save_scene(ed, path), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    NTEST_EXPECT_EQ_STR(editor_scene_path(ed), path);
    // An edit and its undo come back to the saved state.
    NTEST_ASSERT_EQ_INT(editor_edit_rename(h, post, "renamed"), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(editor_scene_dirty(ed));
    NTEST_ASSERT_EQ_INT(editor_history_undo(h), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    NTEST_ASSERT_EQ_INT(editor_history_redo(h), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(editor_scene_dirty(ed));
    NTEST_ASSERT_EQ_INT(editor_history_undo(h), NPH_RESULT_OK);

    // A file that does not load leaves the scene and its history alone.
    char bad[600];
    temp_path("bad.nscene", bad, sizeof bad);
    const char garbage[] = "{ this is not a scene";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(bad, garbage, sizeof garbage - 1U), NPH_RESULT_OK);
    NTEST_EXPECT_NE_INT(editor_open_scene(ed, bad), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(nph_engine_edit_world(e)), 2U);
    NTEST_EXPECT_TRUE(editor_history_can_undo(h));
    NTEST_EXPECT_EQ_STR(editor_scene_path(ed), path);

    // New: empty and untitled.
    NTEST_ASSERT_EQ_INT(editor_new_scene(ed), NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(nph_engine_edit_world(e)), 0U);
    NTEST_EXPECT_EQ_STR(editor_scene_path(ed), "");
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    NTEST_EXPECT_FALSE(editor_history_can_undo(h));
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(nph_engine_edit_world(e)), 0U);

    // Open: the scene as it was saved.
    NTEST_ASSERT_EQ_INT(editor_open_scene(ed, path), NPH_RESULT_OK);
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    NTEST_EXPECT_EQ_STR(editor_scene_path(ed), path);
    const NphEntity post2 = find_named(&fx, "lamp post");
    const NphEntity bulb2 = find_named(&fx, "bulb");
    NTEST_ASSERT_FALSE(nph_entity_is_null(post2));
    NTEST_ASSERT_FALSE(nph_entity_is_null(bulb2));
    NphWorld *w = nph_engine_edit_world(e);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, bulb2), post2));
    NTEST_EXPECT_TRUE(nph_world_has(w, bulb2, light));
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(w, post2, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.x,
        2.0, 0.0);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->scene.lights, 1U);

    // Not while playing.
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, true), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(editor_open_scene(ed, path), NPH_RESULT_INVALID_STATE);
    NTEST_EXPECT_EQ_INT(editor_new_scene(ed), NPH_RESULT_INVALID_STATE);
    nph_engine_play_end(e);
    NPH_IGNORE_RESULT(nph_fs_remove_file(path));
    NPH_IGNORE_RESULT(nph_fs_remove_file(bad));
    finish(&fx);
}

NTEST(Editor, shortcuts_duplicate_delete_undo_and_redo)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    const NphWorld *w = nph_engine_edit_world(fx.engine);
    frames(&fx, 2U);
    NphEntity a = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "a", NPH_ENTITY_NULL, NULL, &a),
                        NPH_RESULT_OK);
    editor_selection_set(&ed->selection, a);
    const u32 count = nph_world_entity_count(w);

    chord(&fx, NPH_KEY_LEFT_CONTROL, NPH_KEY_D);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), count + 1U);
    NTEST_ASSERT_EQ_UINT(ed->selection.count, 1U);
    NTEST_EXPECT_FALSE(nph_entity_equals(ed->selection.primary, a)); // the copy
    const NphEntity copy = ed->selection.primary;

    press(&fx, NPH_KEY_DELETE);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), count);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, copy));
    NTEST_EXPECT_EQ_UINT(ed->selection.count, 0U);

    chord(&fx, NPH_KEY_LEFT_CONTROL, NPH_KEY_Z); // the delete
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, copy));
    chord(&fx, NPH_KEY_LEFT_CONTROL, NPH_KEY_Z); // the duplicate
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, copy));
    chord(&fx, NPH_KEY_LEFT_CONTROL, NPH_KEY_Y);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, copy)); // the same handle again
    NTEST_EXPECT_EQ_STR(editor_history_redo_label(ed->history), "Delete a");
    finish(&fx);
}

NTEST(Editor, unsaved_changes_are_asked_about_before_they_go)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    const NphWorld *w = nph_engine_edit_world(fx.engine);
    frames(&fx, 2U);
    NphEntity a = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "precious", NPH_ENTITY_NULL, NULL, &a),
                        NPH_RESULT_OK);
    // New with unsaved changes: the question comes first, and the scene stays.
    chord(&fx, NPH_KEY_LEFT_CONTROL, NPH_KEY_N);
    NTEST_EXPECT_EQ_INT(ed->pending, EDITOR_PENDING_NEW);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, a));
    press(&fx, NPH_KEY_ESCAPE); // cancel
    NTEST_EXPECT_EQ_INT(ed->pending, EDITOR_PENDING_NONE);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, a));
    // Saved, there is nothing to ask.
    char path[600];
    temp_path("asked.nscene", path, sizeof path);
    NTEST_ASSERT_EQ_INT(editor_save_scene(ed, path), NPH_RESULT_OK);
    chord(&fx, NPH_KEY_LEFT_CONTROL, NPH_KEY_N);
    NTEST_EXPECT_EQ_INT(ed->pending, EDITOR_PENDING_NONE);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U);
    NPH_IGNORE_RESULT(nph_fs_remove_file(path));
    finish(&fx);
}

// ---------------------------------------------------------------------------
// Picking and the gizmo, driven by the mouse
// ---------------------------------------------------------------------------

// The editor's view of the viewport as the last frame laid it out.
static EditorView editor_view(EditorFixture *f)
{
    f32 rx = 0.0F;
    f32 ry = 0.0F;
    f32 rw = 0.0F;
    f32 rh = 0.0F;
    NTEST_ASSERT_TRUE(editor_viewport_rect(f->editor, &rx, &ry, &rw, &rh));
    const NphRenderCamera cam = editor_camera_render(editor_camera(f->editor));
    EditorView v;
    editor_view_init(&v, &cam, rx, ry, rw, rh);
    return v;
}

static void screen_of(const EditorView *v, NphDVec3 p, f32 *x, f32 *y)
{
    NTEST_ASSERT_TRUE(editor_view_project(v, p, x, y));
}

// A lamp at the origin, the camera 8 m in front of it, level.
static NphEntity lamp_in_view(EditorFixture *f)
{
    Editor *ed = f->editor;
    NphEntity lamp = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "lamp", NPH_ENTITY_NULL, NULL, &lamp),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        editor_edit_add_component(ed->history, lamp, nph_engine_components(f->engine)->light),
        NPH_RESULT_OK);
    EditorCamera *cam = editor_camera(ed);
    editor_camera_default(cam);
    cam->pitch = 0.0F;
    frames(f, 3U); // the transform system gives it its WorldTransform
    return lamp;
}

NTEST(Editor, a_click_in_the_viewport_selects_what_is_under_it)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 2U);
    const NphEntity lamp = lamp_in_view(&fx);
    // Off the middle of the view, the camera looking down at it: a marker
    // drawn relative to the wrong origin would land elsewhere on screen.
    const NphDVec3 at = nph_dvec3(1.5, 0.5, 0.0);
    NphLocalTransform t = nph_local_transform_identity();
    t.translation = at;
    NTEST_ASSERT_EQ_INT(editor_history_begin(ed->history, "place"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_set(ed->history, lamp, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &t),
                        NPH_RESULT_OK);
    editor_history_end(ed->history);
    editor_camera(ed)->pitch = -0.35F;
    frames(&fx, 3U);
    const EditorView v = editor_view(&fx);
    f32 x = 0.0F;
    f32 y = 0.0F;
    screen_of(&v, at, &x, &y);
    (void)feed(&fx, move_to(x, y));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_EXPECT_TRUE(nph_entity_equals(ed->selection.primary, lamp));
    // Its marker turns the selection's orange, around it on screen: the
    // overlays draw where the entity is (they once drew relative to an
    // origin the frame did not use, and landed metres away).
    u64 size = 0U;
    u8 *px = pixels_of(&fx, editor_viewport_image(ed), &size);
    const NphGpuImageDesc *vd =
        nph_gpu_image_desc(nph_engine_device(fx.engine), editor_viewport_image(ed));
    const i32 cx = (i32)(x - v.x);
    const i32 cy = (i32)(y - v.y);
    u32 orange = 0U;
    for (i32 dy = -24; dy <= 24; ++dy) {
        for (i32 dx = -24; dx <= 24; ++dx) {
            const i32 sx = cx + dx;
            const i32 sy = cy + dy;
            if (sx < 0 || sy < 0 || sx >= (i32)vd->width || sy >= (i32)vd->height) {
                continue;
            }
            const u8 *p = px + ((u64)sy * vd->width + (u64)sx) * 4U;
            orange += p[0] > 200U && p[1] > 110U && p[1] < 210U && p[2] < 110U ? 1U : 0U;
        }
    }
    free_pixels(px, size);
    NTEST_EXPECT_GT_UINT(orange, 20U);
    // A click on empty space clears the selection.
    (void)feed(&fx, move_to(v.x + 5.0F, v.y + 5.0F));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(ed->selection.count, 0U);
    finish(&fx);
}

NTEST(Editor, dragging_the_gizmo_moves_the_selection_in_one_step)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 2U);
    const NphEntity lamp = lamp_in_view(&fx);
    editor_selection_set(&ed->selection, lamp);
    frames(&fx, 2U);
    const u32 steps = editor_history_undo_count(ed->history);
    // The middle of the X arrow, as the gizmo sizes it on screen.
    const EditorView v = editor_view(&fx);
    const f32 size = EDITOR_GIZMO_PIXELS *
                     editor_view_world_per_unit(&v, editor_view_depth(&v, nph_dvec3_zero()));
    f32 x = 0.0F;
    f32 y = 0.0F;
    screen_of(&v, nph_dvec3((f64)size * 0.6, 0.0, 0.0), &x, &y);
    (void)feed(&fx, move_to(x, y));
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_INT(ed->gizmo_hover, EDITOR_GIZMO_X);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    NTEST_ASSERT_TRUE(ed->gizmo_dragging);
    // Two metres to the right, in screen units.
    f32 x2 = 0.0F;
    f32 y2 = 0.0F;
    screen_of(&v, nph_dvec3((f64)size * 0.6 + 2.0, 0.0, 0.0), &x2, &y2);
    for (u32 i = 1; i <= 4U; ++i) {
        (void)feed(&fx, move_to(x + (x2 - x) * (f32)i / 4.0F, y));
        frames(&fx, 1U);
    }
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(ed->gizmo_dragging);
    const NphWorld *w = nph_engine_edit_world(fx.engine);
    const NphLocalTransform *t =
        NPH_WORLD_GET(w, lamp, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_EXPECT_NEAR_F64(t->translation.x, 2.0, 0.05);
    NTEST_EXPECT_NEAR_F64(t->translation.y, 0.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(t->translation.z, 0.0, 1e-6);
    // The whole drag is one step.
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(ed->history), steps + 1U);
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(ed->history), "Move");
    NTEST_ASSERT_EQ_INT(editor_history_undo(ed->history), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(w, lamp, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.x,
        0.0, 0.0);
    // With Ctrl, the move snaps to half metres: about 1.3 m becomes 1.5.
    frames(&fx, 2U);
    screen_of(&v, nph_dvec3((f64)size * 0.6 + 1.3, 0.0, 0.0), &x2, &y2);
    (void)feed(&fx, move_to(x, y));
    frames(&fx, 2U);
    (void)feed(&fx, key(NPH_KEY_LEFT_CONTROL, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    (void)feed(&fx, move_to(x2, y));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    (void)feed(&fx, key(NPH_KEY_LEFT_CONTROL, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(w, lamp, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.x,
        1.5, 1e-4);
    finish(&fx);
}

// ---------------------------------------------------------------------------
// Imports, and a scene from start to play (CLAUDE.md §12, Phase 9)
// ---------------------------------------------------------------------------

// Pixels whose colour differs from `ref`'s at the same place by more than
// `tolerance` in a channel.
// Turns the editor's temporal anti-aliasing off (r.taa).
static void one_sample_per_pixel(EditorFixture *f)
{
    NphCvarRegistry *cvars = nph_engine_cvars(f->engine);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(cvars, nph_cvar_find(cvars, NPH_STR("r.taa")), false),
                        NPH_RESULT_OK);
}

static u64 differing_by(const u8 *a, const u8 *ref, u64 size, u32 tolerance)
{
    u64 n = 0U;
    for (u64 i = 0; i < size; i += 4U) {
        for (u64 c = 0; c < 3U; ++c) {
            const i32 d = (i32)a[i + c] - (i32)ref[i + c];
            if ((u32)(d < 0 ? -d : d) > tolerance) {
                ++n;
                break;
            }
        }
    }
    return n;
}

// The entity of the edited world named `name`, or null.
static NphEntity named(EditorFixture *f, const char *name)
{
    const NphWorld *w = nph_engine_edit_world(f->engine);
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphName *n = NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_NAME, NphName);
        if (n != NULL && strcmp(n->text.text, name) == 0) {
            return e;
        }
    }
    return NPH_ENTITY_NULL;
}

// Imports the showcase into `folder` of the sources, dropped at (x, y), and
// runs frames until its scene is placed.
static void drop_showcase(EditorFixture *f, const char *folder, f32 x, f32 y)
{
    Editor *ed = f->editor;
    (void)snprintf(ed->assets.folder, sizeof ed->assets.folder, "%s", folder);
    // The showcase's source, imported as a user's file would be.
    const char *paths[1] = {NPH_TEST_SHOWCASE_GLTF};
    NTEST_ASSERT_EQ_INT(editor_import(ed, paths, 1U, true, x, y), NPH_RESULT_OK);
    frames(f, 1U); // where it fell is worked out
    editor_project_wait(editor_project(ed));
    frames(f, 1U); // the scene is placed
    nph_scene_renderer_wait(nph_engine_scene_renderer(f->engine));
    frames(f, 2U);
}

NTEST(Editor, a_dropped_scene_is_cooked_and_placed_where_it_fell)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    NphEngine *e = fx.engine;
    frames(&fx, 3U);
    const NphGpuImage viewport = editor_viewport_image(ed);
    u64 size = 0U;
    u8 *sky = pixels_of(&fx, viewport, &size);
    // Off-centre, on the ground: where the ray through that point meets y = 0.
    const EditorView v = editor_view(&fx);
    const f32 x = v.x + v.w * 0.7F;
    const f32 y = v.y + v.h * 0.65F;
    const NphVec3 dir = editor_view_ray(&v, x, y);
    NTEST_ASSERT_TRUE(dir.y < 0.0F);
    const f64 t = -v.eye.y / (f64)dir.y;
    const NphDVec3 ground = nph_dvec3(v.eye.x + (f64)dir.x * t, 0.0, v.eye.z + (f64)dir.z * t);
    const NphWorld *w = nph_engine_edit_world(e);
    const u32 entities = nph_world_entity_count(w);
    const u32 steps = editor_history_undo_count(ed->history);
    // Into a folder of its own: the tests' pack holds the same scene at the
    // top of its sources, with the GUIDs that key gives. The browser shows
    // the folder before the import makes it, and keeps it.
    (void)snprintf(ed->assets.folder, sizeof ed->assets.folder, "imported");
    ed->assets.stale = true;
    ed->assets.reveal = true;
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_STR(ed->assets.folder, "imported");
    NTEST_EXPECT_EQ_UINT(ed->assets.entry_count, 0U);
    drop_showcase(&fx, "imported", x, y);

    // One undoable step, selected, at the point it fell on.
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(ed->history), steps + 1U);
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(ed->history), "Import showcase");
    const NphEntity root = ed->selection.primary;
    NTEST_ASSERT_TRUE(nph_world_is_alive(w, root));
    NTEST_EXPECT_EQ_STR(NPH_WORLD_GET(w, root, NPH_COMPONENT_ID_NAME, NphName)->text.text,
                        "showcase");
    const NphLocalTransform *lt =
        NPH_WORLD_GET(w, root, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_EXPECT_NEAR_F64(lt->translation.x, ground.x, 1e-3);
    NTEST_EXPECT_NEAR_F64(lt->translation.y, 0.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(lt->translation.z, ground.z, 1e-3);
    NTEST_EXPECT_GT_UINT(nph_world_entity_count(w), entities + 2U);
    // Its meshes come through the project's database, and are drawn.
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->scene.instances, 2U);
    u64 size2 = 0U;
    u8 *placed = pixels_of(&fx, viewport, &size2);
    NTEST_EXPECT_GT_UINT(differing(placed, sky, size), size / 4U / 50U); // over 2%
    free_pixels(placed, size2);
    free_pixels(sky, size);
    // The browser shows the source, cooked.
    bool listed = false;
    bool cooked = false;
    for (u32 i = 0; i < ed->assets.entry_count; ++i) {
        if (strcmp(ed->assets.entries[i].name, "showcase.gltf") == 0) {
            listed = true;
            cooked = ed->assets.entries[i].cooked;
        }
    }
    NTEST_EXPECT_TRUE(listed);
    NTEST_EXPECT_TRUE(cooked);

    // Undo takes the whole scene away; redo brings it back.
    NTEST_ASSERT_EQ_INT(editor_history_undo(ed->history), NPH_RESULT_OK);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), entities);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->scene.instances, 0U);
    NTEST_ASSERT_EQ_INT(editor_history_redo(ed->history), NPH_RESULT_OK);
    // Undo let the meshes go: they load again, and a frame never waits.
    frames(&fx, 1U);
    nph_scene_renderer_wait(nph_engine_scene_renderer(e));
    frames(&fx, 2U);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, root));
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->scene.instances, 2U);

    // Dropped outside the viewport, a scene is imported but not placed.
    const u32 after = nph_world_entity_count(w);
    drop_showcase(&fx, "elsewhere", 1.0F, 1.0F);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), after);
    NTEST_EXPECT_TRUE(editor_project_is_cooked(editor_project(ed), "elsewhere/showcase.gltf"));
    finish(&fx);
}

// CLAUDE.md §12, Phase 9's exit criterion: a scene is built, saved,
// reloaded, and played in the editor. It is built from an imported scene,
// a move, a light, and a camera; saved; opened by a second editor on the
// same project, as after a restart, where it looks the same; and played
// from that camera, with the edited scene back unchanged after Stop.
NTEST(Editor, a_scene_is_built_saved_reloaded_and_played)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    // One sample per pixel (r.taa off) in both editors: the two images are
    // compared pixel for pixel, and a temporal history's edges depend on
    // the jitter phase each run happens to stop at.
    one_sample_per_pixel(&fx);
    frames(&fx, 3U);
    const NphRuntimeComponents *rc = nph_engine_components(fx.engine);

    // Build.
    const EditorView v = editor_view(&fx);
    drop_showcase(&fx, "level", v.x + v.w * 0.5F, v.y + v.h * 0.5F);
    const NphEntity showcase = ed->selection.primary;
    NphWorld *w = nph_engine_edit_world(fx.engine);
    NTEST_ASSERT_TRUE(nph_world_is_alive(w, showcase));
    NphLocalTransform moved =
        *NPH_WORLD_GET(w, showcase, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    moved.translation.x = 1.5;
    NTEST_ASSERT_EQ_INT(editor_history_begin(ed->history, "Move"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        editor_history_set(ed->history, showcase, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &moved),
        NPH_RESULT_OK);
    editor_history_end(ed->history);
    NphEntity lamp = NPH_ENTITY_NULL;
    NphLocalTransform lamp_at = nph_local_transform_identity();
    lamp_at.translation = nph_dvec3(0.0, 3.0, 0.0);
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "lamp", showcase, &lamp_at, &lamp),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_edit_add_component(ed->history, lamp, rc->light), NPH_RESULT_OK);
    NphEntity eye = NPH_ENTITY_NULL;
    NphLocalTransform eye_at = nph_local_transform_identity();
    eye_at.translation = nph_dvec3(0.0, 2.0, 9.0);
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "eye", NPH_ENTITY_NULL, &eye_at, &eye),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_edit_add_component(ed->history, eye, rc->camera), NPH_RESULT_OK);
    NphCamera camera = *NPH_WORLD_GET(w, eye, rc->camera, NphCamera);
    camera.active = true;
    NTEST_ASSERT_EQ_INT(editor_history_begin(ed->history, "Activate"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_set(ed->history, eye, rc->camera, &camera), NPH_RESULT_OK);
    editor_history_end(ed->history);
    frames(&fx, 2U);
    nph_scene_renderer_wait(nph_engine_scene_renderer(fx.engine));
    frames(&fx, 3U);
    const u32 entities = nph_world_entity_count(w);
    u64 size = 0U;
    u8 *built = pixels_of(&fx, editor_viewport_image(ed), &size);
    NTEST_EXPECT_TRUE(editor_scene_dirty(ed));

    // Save.
    char path[1100];
    (void)snprintf(path, sizeof path, "%s/level.nscene", fx.project);
    NTEST_ASSERT_EQ_INT(editor_save_scene(ed, path), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    char project[900];
    (void)snprintf(project, sizeof project, "%s", fx.project);
    fx.keep_project = true;
    finish(&fx);

    // Reload, in another editor on the same project: the entities come
    // from the file and their meshes through the project's database.
    static EditorFixture again;
    const NphResult started = start_in(&again, NPH_ENGINE_MODE_OFFSCREEN, NULL, project);
    NTEST_ASSERT_EQ_INT(started, NPH_RESULT_OK);
    one_sample_per_pixel(&again);
    ed = again.editor;
    frames(&again, 3U);
    NTEST_ASSERT_EQ_INT(editor_open_scene(ed, path), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    w = nph_engine_edit_world(again.engine);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), entities);
    const NphEntity showcase2 = named(&again, "showcase");
    NTEST_ASSERT_FALSE(nph_entity_is_null(showcase2));
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(w, showcase2, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
            ->translation.x,
        1.5, 0.0);
    const NphEntity lamp2 = named(&again, "lamp");
    NTEST_ASSERT_FALSE(nph_entity_is_null(lamp2));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, lamp2), showcase2));
    frames(&again, 2U);
    nph_scene_renderer_wait(nph_engine_scene_renderer(again.engine));
    frames(&again, 3U);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(again.engine)->scene.instances, 2U);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(again.engine)->scene.failed, 0U);
    u64 size2 = 0U;
    u8 *reloaded = pixels_of(&again, editor_viewport_image(ed), &size2);
    NTEST_ASSERT_EQ_UINT(size2, size);
    // The same scene through the same camera (temporal filtering aside).
    NTEST_EXPECT_LT_UINT(differing_by(reloaded, built, size, 24U), size / 4U / 100U);
    free_pixels(reloaded, size2);
    free_pixels(built, size);

    // Play, from the scene's own camera, a step a frame whatever the time
    // (a fast build renders 10 frames in less than one step of real time).
    NphCvarRegistry *cvars = nph_engine_cvars(again.engine);
    NTEST_ASSERT_EQ_INT(
        nph_cvar_set_bool(cvars, nph_cvar_find(cvars, NPH_STR("rt.lockstep")), true),
        NPH_RESULT_OK);
    editor_play_toggle(ed);
    frames(&again, 10U);
    NTEST_ASSERT_TRUE(nph_engine_is_playing(again.engine));
    NTEST_EXPECT_GT_UINT(nph_engine_frame_stats(again.engine)->total_steps, 0U);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(nph_engine_world(again.engine)), entities);
    const NphDVec3 from = nph_engine_view(again.engine)->camera.position;
    NTEST_EXPECT_NEAR_F64(from.x, 0.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(from.y, 2.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(from.z, 9.0, 1e-6);
    // Stop: the edited scene, as it was.
    editor_play_toggle(ed);
    frames(&again, 2U);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(again.engine));
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(nph_engine_edit_world(again.engine)), entities);
    NTEST_EXPECT_FALSE(editor_scene_dirty(ed));
    finish(&again);
}

// ---------------------------------------------------------------------------
// Model creation and reloads
// ---------------------------------------------------------------------------

// Runs frames until the project's requests are done and what they placed
// is drawn.
static void settle(EditorFixture *f)
{
    frames(f, 1U);
    editor_project_wait(editor_project(f->editor));
    frames(f, 1U);
    nph_scene_renderer_wait(nph_engine_scene_renderer(f->engine));
    frames(f, 2U);
}

static NphGuid mesh_of(EditorFixture *f, NphEntity e)
{
    const NphMeshRenderer *m = (const NphMeshRenderer *)nph_world_get(
        nph_engine_edit_world(f->engine), e, nph_engine_components(f->engine)->mesh_renderer);
    NTEST_ASSERT_NOT_NULL(m);
    return m->mesh;
}

static u32 drawn_vertices(EditorFixture *f, NphEntity e)
{
    const NphAsset *mesh = nph_scene_renderer_mesh_asset(nph_engine_scene_renderer(f->engine), e);
    NTEST_ASSERT_NOT_NULL(mesh);
    NphMeshView view;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(mesh, &view), NPH_RESULT_OK);
    return view.header->vertex_count;
}

NTEST(Editor, a_primitive_is_created_drawn_and_undone)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    const NphWorld *w = nph_engine_edit_world(fx.engine);
    const u32 steps = editor_history_undo_count(ed->history);
    NTEST_ASSERT_EQ_INT(editor_create_primitive(ed, EDITOR_PRIMITIVE_CUBE), NPH_RESULT_OK);
    settle(&fx);
    // One step: an entity named after it, drawing the mesh cooked from a
    // source of its own, resting on the ground in front of the camera.
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(ed->history), steps + 1U);
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(ed->history), "Create Cube");
    const NphEntity cube = ed->selection.primary;
    NTEST_ASSERT_TRUE(nph_world_is_alive(w, cube));
    NTEST_EXPECT_EQ_STR(NPH_WORLD_GET(w, cube, NPH_COMPONENT_ID_NAME, NphName)->text.text, "Cube");
    NTEST_EXPECT_TRUE(
        nph_guid_equals(mesh_of(&fx, cube), nph_guid_from_cstr("meshes/cube.nmesh#mesh")));
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(w, cube, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.y,
        0.5, 1e-3);
    char source[1200];
    (void)snprintf(source, sizeof source, "%s/meshes/cube.nmesh",
                   editor_project_assets_dir(editor_project(ed)));
    NTEST_EXPECT_TRUE(nph_fs_exists(source));
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(fx.engine)->scene.instances, 1U);
    NTEST_EXPECT_EQ_UINT(drawn_vertices(&fx, cube), 24U);
    // Another is a source of its own.
    NTEST_ASSERT_EQ_INT(editor_create_primitive(ed, EDITOR_PRIMITIVE_CUBE), NPH_RESULT_OK);
    settle(&fx);
    NTEST_EXPECT_TRUE(nph_guid_equals(mesh_of(&fx, ed->selection.primary),
                                      nph_guid_from_cstr("meshes/cube_2.nmesh#mesh")));
    // Every kind is drawn.
    for (u32 k = 1; k < (u32)EDITOR_PRIMITIVE_COUNT; ++k) {
        NTEST_ASSERT_EQ_INT(editor_create_primitive(ed, (EditorPrimitive)k), NPH_RESULT_OK);
    }
    settle(&fx);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(fx.engine)->scene.instances, 5U);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(fx.engine)->scene.failed, 0U);
    // Undo and redo, as any entity.
    const u32 entities = nph_world_entity_count(w);
    NTEST_ASSERT_EQ_INT(editor_history_undo(ed->history), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), entities - 1U);
    NTEST_ASSERT_EQ_INT(editor_history_redo(ed->history), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), entities);
    // Not while playing.
    NphCvarRegistry *cvars = nph_engine_cvars(fx.engine);
    NTEST_ASSERT_EQ_INT(
        nph_cvar_set_bool(cvars, nph_cvar_find(cvars, NPH_STR("rt.lockstep")), true),
        NPH_RESULT_OK);
    editor_play_toggle(ed);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_INT(editor_create_primitive(ed, EDITOR_PRIMITIVE_CUBE),
                        NPH_RESULT_INVALID_STATE);
    editor_play_toggle(ed);
    frames(&fx, 2U);
    finish(&fx);
}

NTEST(Editor, a_source_imported_again_is_drawn_anew_where_it_is_used)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    const EditorView v = editor_view(&fx);
    drop_showcase(&fx, "again", v.x + v.w * 0.5F, v.y + v.h * 0.5F);
    const NphEntity torus = named(&fx, "torus");
    NTEST_ASSERT_FALSE(nph_entity_is_null(torus));
    const u32 before = drawn_vertices(&fx, torus);
    // A different document under the same name: its first mesh is a cube,
    // and that is what the torus's entity, which names the first mesh,
    // draws once the new one is cooked, while its file was mapped.
    TempDir dir;
    test_temp_dir_make(&dir, "editor_reimport");
    test_write_gltf(&dir, "showcase", false);
    char path[1200];
    test_path_in(&dir, "showcase.gltf", path, sizeof path);
    const char *paths[1] = {path};
    (void)snprintf(ed->assets.folder, sizeof ed->assets.folder, "again");
    NTEST_ASSERT_EQ_INT(editor_import(ed, paths, 1U, false, 0.0F, 0.0F), NPH_RESULT_OK);
    settle(&fx);
    nph_scene_renderer_wait(nph_engine_scene_renderer(fx.engine));
    frames(&fx, 2U);
    NTEST_EXPECT_TRUE(nph_world_is_alive(nph_engine_edit_world(fx.engine), torus));
    const u32 after = drawn_vertices(&fx, torus);
    NTEST_EXPECT_NE_UINT(after, before);
    NTEST_EXPECT_EQ_UINT(after, 24U); // the generated cube
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(fx.engine)->scene.failed, 0U);
    test_temp_dir_remove(&dir);
    finish(&fx);
}

// ---------------------------------------------------------------------------
// Mesh editing
// ---------------------------------------------------------------------------

// A cube made in the editor, selected, its cook done and drawn.
static NphEntity made_cube(EditorFixture *f)
{
    NTEST_ASSERT_EQ_INT(editor_create_primitive(f->editor, EDITOR_PRIMITIVE_CUBE), NPH_RESULT_OK);
    settle(f);
    const NphEntity cube = f->editor->selection.primary;
    NTEST_ASSERT_TRUE(nph_world_is_alive(nph_engine_edit_world(f->engine), cube));
    return cube;
}

static NphVec3 position(EditorFixture *f, u32 index)
{
    const CookPolyMesh *m = editor_mesh_edit_mesh(f->editor);
    NTEST_ASSERT_NOT_NULL(m);
    NTEST_ASSERT_LT_UINT(index, m->positions.len);
    return m->positions.data[index];
}

NTEST(Editor, a_mesh_is_edited_in_place_saved_and_cooked_again)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    const NphEntity cube = made_cube(&fx);
    const u32 scene_steps = editor_history_undo_count(ed->history);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_begin(ed), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(editor_mesh_editing(ed));
    NTEST_EXPECT_EQ_UINT(editor_mesh_edit_mesh(ed)->faces.len, 6U);
    frames(&fx, 2U);
    // The edited version is drawn and the cooked one hidden.
    NphRenderFrameStats rs = nph_engine_frame_stats(fx.engine)->render;
    NTEST_EXPECT_EQ_UINT(rs.instances, 2U);
    NTEST_EXPECT_EQ_UINT(rs.culled, 1U);

    // The top face (+Y), extruded half a metre and moved a quarter more.
    const u32 top = 4U;
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_FACES, &top, 1U, false),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_extrude(ed, 0.5F), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(editor_mesh_edit_mesh(ed)->faces.len, 10U);
    NTEST_EXPECT_EQ_UINT(editor_mesh_edit_mesh(ed)->positions.len, 12U);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_move(ed, nph_vec3(0.0F, 0.25F, 0.0F)), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(position(&fx, 8U).y, 1.25F, 1e-6F);
    // Its own history: undo and redo the move.
    NTEST_ASSERT_TRUE(editor_mesh_edit_undo(ed));
    NTEST_EXPECT_NEAR_F32(position(&fx, 8U).y, 1.0F, 1e-6F);
    NTEST_ASSERT_TRUE(editor_mesh_edit_redo(ed));
    NTEST_EXPECT_NEAR_F32(position(&fx, 8U).y, 1.25F, 1e-6F);
    NTEST_EXPECT_FALSE(editor_mesh_edit_redo(ed));
    // A vertex, nudged.
    const u32 corner = 0U;
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_VERTICES, &corner, 1U, false),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_move(ed, nph_vec3(-0.25F, 0.0F, 0.0F)), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(position(&fx, 0U).x, -0.75F, 1e-6F);
    const u32 beyond = 99U;
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_VERTICES, &beyond, 1U, false),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_extrude(ed, 0.5F), NPH_RESULT_INVALID_STATE); // vertices
    frames(&fx, 2U);

    // Done: saved, cooked again, and drawn again from the cooked mesh, with
    // no frame in between where the cube is missing.
    editor_mesh_edit_end(ed);
    NTEST_EXPECT_FALSE(editor_mesh_editing(ed));
    char source[1200];
    (void)snprintf(source, sizeof source, "%s/meshes/cube.nmesh",
                   editor_project_assets_dir(editor_project(ed)));
    CookPolyMesh saved;
    NTEST_ASSERT_EQ_INT(cook_polymesh_load(nph_allocator_system(), source, &saved), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(saved.faces.len, 10U);
    NTEST_EXPECT_NEAR_F32(saved.positions.data[0].x, -0.75F, 1e-6F);
    cook_polymesh_free(&saved);
    for (u32 i = 0; i < 40U; ++i) {
        frames(&fx, 1U);
        rs = nph_engine_frame_stats(fx.engine)->render;
        // The preview or the cooked mesh, never neither: one instance shown
        // (on either pipeline; the production one's draws are the GPU's).
        NTEST_ASSERT_EQ_UINT(rs.instances - rs.culled, 1U);
        if (!ed->mesh_edit.committing) {
            break;
        }
        editor_project_wait(editor_project(ed));
        nph_scene_renderer_wait(nph_engine_scene_renderer(fx.engine));
    }
    NTEST_EXPECT_FALSE(ed->mesh_edit.committing);
    frames(&fx, 1U);
    rs = nph_engine_frame_stats(fx.engine)->render;
    NTEST_EXPECT_EQ_UINT(rs.instances, 1U); // the preview is gone
    NTEST_EXPECT_EQ_UINT(rs.culled, 0U);
    NTEST_EXPECT_GT_UINT(drawn_vertices(&fx, cube), 24U); // the extruded cube, cooked
    // The scene's history did not see any of it.
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(ed->history), scene_steps);

    // An entity that draws no editable mesh cannot be edited.
    NphEntity plain = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "plain", NPH_ENTITY_NULL, NULL, &plain),
                        NPH_RESULT_OK);
    editor_selection_set(&ed->selection, plain);
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_begin(ed), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_FALSE(editor_mesh_editing(ed));
    // Nor can nothing.
    editor_selection_clear(&ed->selection);
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_begin(ed), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_FALSE(editor_mesh_editing(ed));
    finish(&fx);
}

NTEST(Editor, mesh_editing_picks_and_drags_with_the_mouse)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    const NphEntity cube = made_cube(&fx);
    // The camera level with the cube, in front of it: +X to the right.
    EditorCamera *cam = editor_camera(ed);
    editor_camera_default(cam);
    cam->pivot = editor_world_transform_of(nph_engine_edit_world(fx.engine), cube).translation;
    cam->pitch = -0.3F;
    cam->distance = 4.0F;
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_begin(ed), NPH_RESULT_OK);
    frames(&fx, 2U);
    const EditorView v = editor_view(&fx);
    const NphWorldTransform wt = editor_world_transform_of(nph_engine_edit_world(fx.engine), cube);
    // Vertex 7 is the (+, +, +) corner, the one nearest the camera.
    f32 x = 0.0F;
    f32 y = 0.0F;
    screen_of(&v, nph_world_transform_point(&wt, position(&fx, 7U)), &x, &y);
    (void)feed(&fx, move_to(x, y));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_ASSERT_EQ_UINT(ed->mesh_edit.selected.len, 8U);
    NTEST_EXPECT_EQ_UINT(ed->mesh_edit.selected.data[7], 1U);
    u32 picked = 0U;
    for (u32 i = 0; i < 8U; ++i) {
        picked += ed->mesh_edit.selected.data[i];
    }
    NTEST_EXPECT_EQ_UINT(picked, 1U);
    // The gizmo stands on it: its X arrow drags it one metre to the right.
    const NphDVec3 at = nph_world_transform_point(&wt, position(&fx, 7U));
    const f32 size =
        EDITOR_GIZMO_PIXELS * editor_view_world_per_unit(&v, editor_view_depth(&v, at));
    f32 ax = 0.0F;
    f32 ay = 0.0F;
    f32 bx = 0.0F;
    f32 by = 0.0F;
    screen_of(&v, nph_dvec3(at.x + (f64)size * 0.6, at.y, at.z), &ax, &ay);
    screen_of(&v, nph_dvec3(at.x + (f64)size * 0.6 + 1.0, at.y, at.z), &bx, &by);
    (void)feed(&fx, move_to(ax, ay));
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_INT(ed->gizmo_hover, EDITOR_GIZMO_X);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    NTEST_ASSERT_TRUE(ed->gizmo_dragging);
    for (u32 i = 1; i <= 4U; ++i) {
        (void)feed(&fx, move_to(ax + (bx - ax) * (f32)i / 4.0F, ay + (by - ay) * (f32)i / 4.0F));
        frames(&fx, 1U);
    }
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(ed->gizmo_dragging);
    NTEST_EXPECT_NEAR_F32(position(&fx, 7U).x, 1.5F, 0.05F);
    NTEST_EXPECT_NEAR_F32(position(&fx, 7U).y, 0.5F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(position(&fx, 6U).x, -0.5F, 0.0F); // the others stay
    // One change: undone in one step.
    NTEST_ASSERT_TRUE(editor_mesh_edit_undo(ed));
    NTEST_EXPECT_NEAR_F32(position(&fx, 7U).x, 0.5F, 0.0F);
    // Faces: a click on the top picks it.
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_FACES, NULL, 0U, false),
                        NPH_RESULT_OK);
    const NphVec3 top = nph_vec3(0.1F, 0.5F, 0.1F); // on the top face, nearer the camera's side
    screen_of(&v, nph_world_transform_point(&wt, top), &x, &y);
    (void)feed(&fx, move_to(x, y));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_PRESS));
    frames(&fx, 2U);
    (void)feed(&fx, button(NPH_MOUSE_BUTTON_LEFT, NPH_KEY_ACTION_RELEASE));
    frames(&fx, 2U);
    NTEST_ASSERT_EQ_UINT(ed->mesh_edit.selected.len, 6U);
    NTEST_EXPECT_EQ_UINT(ed->mesh_edit.selected.data[4], 1U);
    // Selecting another entity ends editing.
    editor_selection_set(&ed->selection, NPH_ENTITY_NULL);
    frames(&fx, 1U);
    NTEST_EXPECT_FALSE(editor_mesh_editing(ed));
    finish(&fx);
}

// Physics in the editor (docs/editor.md "Physics"): a Collider's outline in
// the viewport (the selection's, or every one's), and a scene played with
// its bodies falling, the edited world untouched.
NTEST(Editor, colliders_show_and_bodies_fall_in_play)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    NphEngine *e = fx.engine;
    NphCvarRegistry *cvars = nph_engine_cvars(e);
    NTEST_ASSERT_EQ_INT(
        nph_cvar_set_bool(cvars, nph_cvar_find(cvars, NPH_STR("rt.lockstep")), true),
        NPH_RESULT_OK);
    frames(&fx, 3U);
    NTEST_ASSERT_EQ_INT(editor_create_primitive(ed, EDITOR_PRIMITIVE_CUBE), NPH_RESULT_OK);
    settle(&fx);
    const NphEntity cube = ed->selection.primary;
    NphWorld *w = nph_engine_edit_world(e);
    const NphRuntimeComponents *rc = nph_engine_components(e);
    const u32 lines_before = nph_engine_frame_stats(e)->render.debug_lines;
    // A body, three metres up, and a floor whose top is the ground.
    NTEST_ASSERT_EQ_INT(nph_world_add(w, cube, rc->rigid_body, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_add(w, cube, rc->collider, NULL), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, cube, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.y =
        3.0;
    const NphComponentId floor_comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                           NPH_COMPONENT_ID_WORLD_TRANSFORM, rc->collider};
    NphEntity floor;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, floor_comps, 3U, &floor), NPH_RESULT_OK);
    NphLocalTransform *ft =
        NPH_WORLD_GET_MUT(w, floor, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    ft->translation = nph_dvec3(0.0, -0.5, 0.0);
    ft->scale = nph_vec3(20.0F, 1.0F, 20.0F); // the unit cube scaled
    frames(&fx, 2U);
    // The selected cube's collider adds its twelve edges.
    const u32 lines_selected = nph_engine_frame_stats(e)->render.debug_lines;
    NTEST_EXPECT_EQ_UINT(lines_selected, lines_before + 12U);
    ed->show_colliders = true;
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->render.debug_lines, lines_selected + 12U);
    ed->show_colliders = false;

    // Play: the cube falls onto the floor and rests there.
    editor_play_toggle(ed);
    NTEST_ASSERT_TRUE(nph_engine_is_playing(e));
    NTEST_ASSERT_NOT_NULL(nph_engine_physics(e));
    frames(&fx, 480U);
    NTEST_ASSERT_EQ_INT(nph_engine_wait(e), NPH_RESULT_OK);
    const NphWorld *play = nph_engine_world(e);
    NTEST_EXPECT_TRUE(play != w);
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(play, cube, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
            ->translation.y,
        0.5, 0.005);
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_stats(e)->bodies, 2U);
    // The physics world's own lines, through phys.debug_draw.
    const u32 lines_playing = nph_engine_frame_stats(e)->render.debug_lines;
    NTEST_ASSERT_EQ_INT(
        nph_cvar_set_int(cvars, nph_cvar_find(cvars, NPH_STR("phys.debug_draw")), 1),
        NPH_RESULT_OK);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->render.debug_lines, lines_playing + 24U);
    editor_play_toggle(ed);
    frames(&fx, 2U);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(e));
    NTEST_EXPECT_NULL(nph_engine_physics(e));
    NTEST_EXPECT_NEAR_F64(
        NPH_WORLD_GET(w, cube, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.y,
        3.0, 0.0);
    finish(&fx);
}

// ---------------------------------------------------------------------------
// Autosave and recovery (autosave.c)
// ---------------------------------------------------------------------------

static bool session_has(const Editor *ed, const char *name)
{
    char path[1200];
    (void)snprintf(path, sizeof path, "%s/%s", ed->autosave.dir, name);
    return nph_fs_exists(path);
}

// What a crash leaves: a copy of `ed`'s session directory, named `name`,
// whose lock no process holds.
static void leave_crashed_copy(const Editor *ed, const char *name, char *out, u64 cap)
{
    const char *slash = strrchr(ed->autosave.dir, '/');
    NTEST_ASSERT_NOT_NULL(slash);
    (void)snprintf(out, (size_t)cap, "%.*s/%s", (int)(slash - ed->autosave.dir), ed->autosave.dir,
                   name);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(out), NPH_RESULT_OK);
    static const char *const files[3] = {"session.json", "scene.nscene", "mesh.nmesh"};
    for (u32 i = 0; i < 3U; ++i) {
        char from[1200];
        char to[1200];
        (void)snprintf(from, sizeof from, "%s/%s", ed->autosave.dir, files[i]);
        (void)snprintf(to, sizeof to, "%s/%s", out, files[i]);
        if (nph_fs_exists(from)) {
            NTEST_ASSERT_EQ_INT(nph_fs_copy_file(from, to), NPH_RESULT_OK);
        }
    }
    char lock[1200];
    (void)snprintf(lock, sizeof lock, "%s/lock", out);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(lock, "", 0U), NPH_RESULT_OK);
}

static u32 faces_in(const char *path)
{
    CookPolyMesh m;
    NTEST_ASSERT_EQ_INT(cook_polymesh_load(nph_allocator_system(), path, &m), NPH_RESULT_OK);
    const u32 faces = m.faces.len;
    cook_polymesh_free(&m);
    return faces;
}

NTEST(Editor, autosave_keeps_unsaved_changes_after_its_interval_and_before_play)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    EditorAutosave *as = &ed->autosave;
    NTEST_ASSERT_NE_STR(as->dir, "");
    NTEST_EXPECT_TRUE(session_has(ed, "lock"));
    NTEST_EXPECT_TRUE(session_has(ed, "session.json"));
    NTEST_EXPECT_FALSE(ed->recovery.offered);

    // Nothing unsaved: nothing to write, however long it waits.
    editor_autosave_update(ed, as->last_ns + 3600U * NPH_TIME_NS_PER_S);
    NTEST_EXPECT_EQ_UINT(as->count, 0U);

    NphEntity crate = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "crate", NPH_ENTITY_NULL, NULL, &crate),
                        NPH_RESULT_OK);
    const u64 interval = (u64)nph_cvar_get_int(&ed->cv_autosave_interval) * NPH_TIME_NS_PER_S;
    NTEST_EXPECT_EQ_UINT(interval, 60U * NPH_TIME_NS_PER_S); // the default
    editor_autosave_update(ed, as->last_ns + interval - 1U);
    NTEST_EXPECT_EQ_UINT(as->count, 0U); // not yet
    editor_autosave_update(ed, as->last_ns + interval);
    NTEST_ASSERT_EQ_UINT(as->count, 1U);
    NTEST_EXPECT_TRUE(session_has(ed, "scene.nscene"));
    NTEST_EXPECT_EQ_UINT(as->last_entities,
                         nph_world_entity_count(nph_engine_edit_world(fx.engine)));
    editor_autosave_update(ed, as->last_ns + interval); // held already: no second one
    NTEST_EXPECT_EQ_UINT(as->count, 1U);

    // Before Play, whatever the interval, keeping the one before.
    NTEST_ASSERT_EQ_INT(editor_edit_rename(ed->history, crate, "box"), NPH_RESULT_OK);
    editor_play_toggle(ed);
    NTEST_EXPECT_EQ_UINT(as->count, 2U);
    NTEST_EXPECT_TRUE(session_has(ed, "scene.previous.nscene"));
    frames(&fx, 2U);
    editor_autosave_update(ed, as->last_ns + interval * 2U); // nothing while playing
    NTEST_EXPECT_EQ_UINT(as->count, 2U);
    editor_play_toggle(ed); // stop
    frames(&fx, 1U);

    // Saved: the file holds what the autosave did, which goes.
    char scene[1100];
    (void)snprintf(scene, sizeof scene, "%s/level.nscene", fx.project);
    NTEST_ASSERT_EQ_INT(editor_save_scene(ed, scene), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(as->has_scene);
    NTEST_EXPECT_FALSE(session_has(ed, "scene.nscene"));
    NTEST_EXPECT_FALSE(session_has(ed, "scene.previous.nscene"));

    // A clean exit takes the session away, and leaves the project.
    char dir[EDITOR_PATH_MAX];
    memcpy(dir, as->dir, sizeof dir);
    char project[900];
    memcpy(project, fx.project, sizeof project);
    fx.keep_project = true;
    finish(&fx);
    NTEST_EXPECT_FALSE(nph_fs_exists(dir));
    NTEST_EXPECT_TRUE(nph_fs_exists(scene));
    NTEST_EXPECT_EQ_INT(nph_fs_remove_tree(project), NPH_RESULT_OK);
}

NTEST(Editor, a_crashed_sessions_work_is_offered_and_recovered_unsaved)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    char scene[1100];
    (void)snprintf(scene, sizeof scene, "%s/level.nscene", fx.project);
    NphEntity kept = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "kept", NPH_ENTITY_NULL, NULL, &kept),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_save_scene(ed, scene), NPH_RESULT_OK);
    // What the file does not hold.
    NphEntity unsaved = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "unsaved", NPH_ENTITY_NULL, NULL, &unsaved),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_autosave_now(ed, "test"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(ed->autosave.count, 1U);
    const u32 entities = nph_world_entity_count(nph_engine_edit_world(fx.engine));
    char crashed[1100];
    leave_crashed_copy(ed, "1-1", crashed, sizeof crashed);
    ed->quit_confirmed = true; // this one ends as asked; the copy is what a crash leaves
    fx.keep_project = true;
    char project[900];
    memcpy(project, fx.project, sizeof project);
    finish(&fx);
    NTEST_ASSERT_TRUE(nph_fs_exists(crashed));

    static EditorFixture again;
    NTEST_ASSERT_EQ_INT(start_in(&again, NPH_ENGINE_MODE_OFFSCREEN, NULL, project), NPH_RESULT_OK);
    Editor *next = again.editor;
    const EditorRecovery *rc = &next->recovery;
    NTEST_ASSERT_TRUE(rc->offered);
    NTEST_EXPECT_TRUE(rc->has_scene);
    NTEST_EXPECT_FALSE(rc->has_mesh);
    NTEST_EXPECT_EQ_STR(rc->scene, scene);
    NTEST_EXPECT_FALSE(rc->scene_newer);
    NTEST_EXPECT_EQ_INT((int)next->popup_request, (int)EDITOR_POPUP_RECOVER);
    frames(&again, 2U); // the popup, drawn under the validation gate
    NTEST_ASSERT_EQ_INT(editor_recovery_recover(next), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(rc->offered);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(nph_engine_edit_world(again.engine)), entities);
    NTEST_EXPECT_EQ_STR(editor_scene_path(next), scene); // under its own file,
    NTEST_EXPECT_TRUE(editor_scene_dirty(next));         // and not saved yet
    NTEST_EXPECT_FALSE(nph_fs_exists(crashed));
    NTEST_EXPECT_EQ_UINT(next->autosave.count, 1U); // in this session's autosave at once
    NTEST_EXPECT_EQ_INT(editor_recovery_recover(next), NPH_RESULT_INVALID_STATE); // once
    frames(&again, 2U);
    next->quit_confirmed = true;
    finish(&again);
}

NTEST(Editor, a_running_sessions_work_is_not_offered_and_later_and_discard_answer_a_crashed_one)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 2U);
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "work", NPH_ENTITY_NULL, NULL, &e),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_autosave_now(ed, "test"), NPH_RESULT_OK);
    char dead[1100];
    char live[1100];
    leave_crashed_copy(ed, "1-1", dead, sizeof dead);
    leave_crashed_copy(ed, "2-2", live, sizeof live);
    // One still running: its lock is held.
    char live_lock[1200];
    (void)snprintf(live_lock, sizeof live_lock, "%s/lock", live);
    NphFileLock held;
    NTEST_ASSERT_EQ_INT(nph_file_lock_acquire(live_lock, &held), NPH_RESULT_OK);
    ed->quit_confirmed = true;
    fx.keep_project = true;
    char project[900];
    memcpy(project, fx.project, sizeof project);
    finish(&fx);

    static EditorFixture second;
    NTEST_ASSERT_EQ_INT(start_in(&second, NPH_ENGINE_MODE_OFFSCREEN, NULL, project), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(second.editor->recovery.offered);
    NTEST_EXPECT_EQ_STR(second.editor->recovery.dir, dead); // not the running one's
    editor_recovery_later(second.editor);
    NTEST_EXPECT_FALSE(second.editor->recovery.offered);
    NTEST_EXPECT_TRUE(nph_fs_exists(dead)); // kept for the next start
    second.keep_project = true;
    finish(&second);

    static EditorFixture third;
    NTEST_ASSERT_EQ_INT(start_in(&third, NPH_ENGINE_MODE_OFFSCREEN, NULL, project), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(third.editor->recovery.offered); // offered again
    editor_recovery_discard(third.editor);
    NTEST_EXPECT_FALSE(nph_fs_exists(dead));
    NTEST_EXPECT_TRUE(nph_fs_exists(live)); // a running editor's: untouched
    nph_file_lock_release(&held);
    finish(&third);
}

NTEST(Editor, an_exit_nobody_confirmed_keeps_unsaved_changes_for_the_next_start)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 2U);
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(ed->history, "late", NPH_ENTITY_NULL, NULL, &e),
                        NPH_RESULT_OK);
    // No autosave yet: the exit writes it, as after the console's quit.
    NTEST_EXPECT_EQ_UINT(ed->autosave.count, 0U);
    char dir[EDITOR_PATH_MAX];
    memcpy(dir, ed->autosave.dir, sizeof dir);
    fx.keep_project = true;
    char project[900];
    memcpy(project, fx.project, sizeof project);
    finish(&fx);
    NTEST_ASSERT_TRUE(nph_fs_exists(dir));

    static EditorFixture again;
    NTEST_ASSERT_EQ_INT(start_in(&again, NPH_ENGINE_MODE_OFFSCREEN, NULL, project), NPH_RESULT_OK);
    // Its own directory, though it may have begun in the same second as the
    // last one, in the same process.
    NTEST_EXPECT_NE_STR(again.editor->autosave.dir, dir);
    NTEST_ASSERT_TRUE(again.editor->recovery.offered);
    NTEST_EXPECT_EQ_STR(again.editor->recovery.dir, dir);
    NTEST_EXPECT_TRUE(again.editor->recovery.has_scene);
    editor_recovery_discard(again.editor);
    NTEST_EXPECT_FALSE(nph_fs_exists(dir));
    finish(&again);
}

NTEST(Editor, a_mesh_being_edited_is_autosaved_and_recovered_into_its_source)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    (void)made_cube(&fx);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_begin(ed), NPH_RESULT_OK);
    const u32 top = 4U;
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_FACES, &top, 1U, false),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_extrude(ed, 0.5F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_autosave_now(ed, "test"), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(ed->autosave.has_mesh);
    char saved[1200];
    (void)snprintf(saved, sizeof saved, "%s/mesh.nmesh", ed->autosave.dir);
    NTEST_EXPECT_EQ_UINT(faces_in(saved), 10U); // the edited mesh
    // Nothing new: the next autosave writes nothing.
    const u32 count = ed->autosave.count;
    NTEST_ASSERT_EQ_INT(editor_autosave_now(ed, "test"), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(ed->autosave.count, count);
    char key[EDITOR_PROJECT_PATH_MAX];
    memcpy(key, ed->mesh_edit.key, sizeof key);
    char crashed[1100];
    leave_crashed_copy(ed, "1-1", crashed, sizeof crashed);
    // The edit dropped unsaved, as a crash would have: the source keeps the cube.
    editor_mesh_edit_free(ed);
    editor_mesh_edit_init(ed);
    ed->quit_confirmed = true;
    fx.keep_project = true;
    char project[900];
    memcpy(project, fx.project, sizeof project);
    finish(&fx);
    char source[2048];
    (void)snprintf(source, sizeof source, "%s/assets/%s", project, key);
    NTEST_ASSERT_EQ_UINT(faces_in(source), 6U);

    static EditorFixture again;
    NTEST_ASSERT_EQ_INT(start_in(&again, NPH_ENGINE_MODE_OFFSCREEN, NULL, project), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(again.editor->recovery.offered);
    NTEST_EXPECT_TRUE(again.editor->recovery.has_mesh);
    NTEST_EXPECT_EQ_STR(again.editor->recovery.mesh_key, key);
    NTEST_ASSERT_EQ_INT(editor_recovery_recover(again.editor), NPH_RESULT_OK);
    settle(&again); // the source cooks again
    NTEST_EXPECT_EQ_UINT(faces_in(source), 10U);
    again.editor->quit_confirmed = true;
    finish(&again);
}

NTEST(EditorAutosave, a_new_sessions_directory_takes_a_name_no_directory_holds)
{
    TempDir root;
    test_temp_dir_make(&root, "autosave_names");
    char name[64];
    NTEST_ASSERT_EQ_INT(editor_autosave_session_name(root.dir, 5U, 7U, name, sizeof name),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(name, "5-7");
    // Earlier sessions of this process, in the same second, hold the name and
    // the next: one of them may be a crashed one's, to be offered back.
    char dir[1200];
    (void)snprintf(dir, sizeof dir, "%s/5-7", root.dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    (void)snprintf(dir, sizeof dir, "%s/5-7-1", root.dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_autosave_session_name(root.dir, 5U, 7U, name, sizeof name),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(name, "5-7-2");
    NTEST_ASSERT_EQ_INT(editor_autosave_session_name(root.dir, 6U, 7U, name, sizeof name),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(name, "6-7"); // another second
    NTEST_ASSERT_EQ_INT(editor_autosave_session_name(root.dir, 5U, 8U, name, sizeof name),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(name, "5-8"); // another process
    char small[3];
    NTEST_EXPECT_EQ_INT(editor_autosave_session_name(root.dir, 5U, 7U, small, sizeof small),
                        NPH_RESULT_OVERFLOW);
    NTEST_EXPECT_EQ_STR(small, "");
    test_temp_dir_remove(&root);
}

// ---------------------------------------------------------------------------
// A material cooked again changes in place (ADR-0105)
// ---------------------------------------------------------------------------

// The mean colour of the viewport's centre, a square of 16 pixels a side (R8G8B8A8_UNORM).
static void centre_colour(EditorFixture *f, f32 out[3])
{
    const NphGpuImage image = editor_viewport_image(f->editor);
    u64 size = 0U;
    u8 *px = pixels_of(f, image, &size);
    const NphGpuImageDesc *d = nph_gpu_image_desc(nph_engine_device(f->engine), image);
    NTEST_ASSERT_NOT_NULL(d);
    NTEST_ASSERT_EQ_INT(d->format, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    f64 sum[3] = {0.0, 0.0, 0.0};
    for (u32 y = d->height / 2U - 8U; y < d->height / 2U + 8U; ++y) {
        for (u32 x = d->width / 2U - 8U; x < d->width / 2U + 8U; ++x) {
            const u8 *p = px + ((u64)y * d->width + x) * 4U;
            for (u32 c = 0; c < 3U; ++c) {
                sum[c] += (f64)p[c];
            }
        }
    }
    for (u32 c = 0; c < 3U; ++c) {
        out[c] = (f32)(sum[c] / 256.0);
    }
    free_pixels(px, size);
}

// A plain material of `colour` at the project's `materials/paint.nmat`, cooked.
static void paint(EditorFixture *f, f32 r, f32 g, f32 b)
{
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.base_color = nph_vec4(r, g, b, 1.0F);
    m.roughness = 1.0F;
    char dir[1100];
    (void)snprintf(dir, sizeof dir, "%s/assets/materials", f->project);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    char path[1200];
    (void)snprintf(path, sizeof path, "%s/paint.nmat", dir);
    NTEST_ASSERT_EQ_INT(cook_material_source_save(&m, nph_allocator_system(), path), NPH_RESULT_OK);
    u32 id = 0U;
    NTEST_ASSERT_EQ_INT(
        editor_project_cook(editor_project(f->editor), "materials/paint.nmat", false, 0U, &id),
        NPH_RESULT_OK);
}

NTEST(Editor, a_material_cooked_again_changes_in_place)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    paint(&fx, 1.0F, 0.0F, 0.0F);
    const NphEntity cube = made_cube(&fx);
    // The cube's first slot names the material.
    const NphComponentId id = nph_engine_components(fx.engine)->mesh_renderer;
    NphMeshRenderer mr =
        *(const NphMeshRenderer *)nph_world_get(nph_engine_edit_world(fx.engine), cube, id);
    mr.material0 = nph_guid_from_cstr("materials/paint.nmat#material");
    NTEST_ASSERT_EQ_INT(editor_history_begin(ed->history, "Paint"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_set(ed->history, cube, id, &mr), NPH_RESULT_OK);
    editor_history_end(ed->history);
    settle(&fx);
    f32 red[3];
    centre_colour(&fx, red);
    NTEST_EXPECT_TRUE(red[0] > red[1] + 30.0F);
    NTEST_EXPECT_TRUE(red[0] > red[2] + 30.0F);

    // Cooked again green: the cube keeps its instance on every frame, and turns green.
    NphSceneRenderer *sr = nph_engine_scene_renderer(fx.engine);
    const NphRenderInstance kept = nph_scene_renderer_instance(sr, cube);
    NTEST_ASSERT_NE_UINT(kept.value, 0U);
    paint(&fx, 0.0F, 1.0F, 0.0F);
    for (u32 i = 0; i < 6U; ++i) {
        frames(&fx, 1U);
        NTEST_EXPECT_EQ_UINT(nph_scene_renderer_instance(sr, cube).value, kept.value);
    }
    settle(&fx);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_instance(sr, cube).value, kept.value);
    // Every frame since the update draws it green; the image shows it all after some frames,
    // as the resolve weighs a still pixel's new frame at 1/32 (docs/render.md "Temporal").
    f32 green[3];
    for (u32 waited = 0U; waited < 192U; waited += 8U) {
        centre_colour(&fx, green);
        if (green[1] > green[0] + 30.0F && green[1] > green[2] + 30.0F) {
            break;
        }
        frames(&fx, 8U);
    }
    NTEST_EXPECT_TRUE(green[1] > green[0] + 30.0F);
    NTEST_EXPECT_TRUE(green[1] > green[2] + 30.0F);
    finish(&fx);
}

// ---------------------------------------------------------------------------
// The material panel, and materials given to entities (ADR-0105 decisions 5, 6)
// ---------------------------------------------------------------------------

// The material source of the project's `key`, read from its file.
static CookMaterialSource source_on_disk(EditorFixture *f, const char *key)
{
    char path[1300];
    (void)snprintf(path, sizeof path, "%s/assets/%s", f->project, key);
    CookMaterialSource s;
    memset(&s, 0, sizeof s);
    NTEST_EXPECT_EQ_INT(cook_material_source_load(nph_allocator_system(), path, &s), NPH_RESULT_OK);
    return s;
}

// The viewport's centre: red, green, or blue by far (`channel`).
static bool centre_is(EditorFixture *f, u32 channel)
{
    f32 c[3];
    centre_colour(f, c);
    return c[channel] > c[(channel + 1U) % 3U] + 30.0F &&
           c[channel] > c[(channel + 2U) % 3U] + 30.0F;
}

NTEST(Editor, a_new_material_is_written_with_the_defaults_cooked_and_opened)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 2U);
    char key[256];
    NTEST_ASSERT_EQ_INT(editor_material_new(ed, "materials", key, sizeof key), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(key, "materials/New Material.nmat");
    NTEST_ASSERT_NOT_NULL(editor_material_key(ed));
    NTEST_EXPECT_EQ_STR(editor_material_key(ed), key);
    CookMaterialSource fresh;
    cook_material_source_default(&fresh);
    NTEST_EXPECT_TRUE(cook_material_source_equal(editor_material_source(ed), &fresh));
    const CookMaterialSource written = source_on_disk(&fx, key);
    NTEST_EXPECT_TRUE(cook_material_source_equal(&written, &fresh));
    // The name is taken now: the next one is numbered, and opens in its place.
    char second[256];
    NTEST_ASSERT_EQ_INT(editor_material_new(ed, "materials", second, sizeof second), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(second, "materials/New Material 2.nmat");
    NTEST_EXPECT_EQ_STR(editor_material_key(ed), second);
    // Both cooked, so either can be given to entities at once.
    settle(&fx);
    NTEST_EXPECT_TRUE(editor_project_is_cooked(editor_project(ed), key));
    NTEST_EXPECT_TRUE(editor_project_is_cooked(editor_project(ed), second));
    // Only a material opens.
    NTEST_EXPECT_EQ_INT(editor_material_open(ed, "materials/paint.png"),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_STR(editor_material_key(ed), second);
    editor_material_close(ed);
    NTEST_EXPECT_NULL(editor_material_source(ed));
    finish(&fx);
}

NTEST(Editor, a_material_is_drawn_as_it_changes_written_as_it_settles_and_undone_in_the_panel)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    paint(&fx, 1.0F, 0.0F, 0.0F);
    const NphEntity cube = made_cube(&fx);
    NTEST_ASSERT_EQ_INT(editor_material_assign(ed, cube, 0U, "materials/paint.nmat"),
                        NPH_RESULT_OK);
    settle(&fx);
    NTEST_EXPECT_TRUE(centre_is(&fx, 0U));
    const u64 scene = editor_history_state(ed->history);
    NphSceneRenderer *sr = nph_engine_scene_renderer(fx.engine);
    const NphRenderInstance kept = nph_scene_renderer_instance(sr, cube);
    NTEST_ASSERT_NE_UINT(kept.value, 0U);

    NTEST_ASSERT_EQ_INT(editor_material_open(ed, "materials/paint.nmat"), NPH_RESULT_OK);
    CookMaterialSource s = *editor_material_source(ed);
    s.base_color = nph_vec4(0.0F, 1.0F, 0.0F, 1.0F);
    // Being dragged: drawn from the next frame, written nowhere. Its pixels take no history on
    // the frames it changes, so it shows at once.
    NTEST_ASSERT_EQ_INT(editor_material_change(ed, &s, false), NPH_RESULT_OK);
    frames(&fx, 3U);
    NTEST_EXPECT_TRUE(centre_is(&fx, 1U));
    CookMaterialSource disk = source_on_disk(&fx, "materials/paint.nmat");
    NTEST_EXPECT_NEAR_F32(disk.base_color.x, 1.0F, 1e-6F);
    // Let go: written and cooked; the cube keeps its instance, and stays green.
    NTEST_ASSERT_EQ_INT(editor_material_change(ed, &s, true), NPH_RESULT_OK);
    disk = source_on_disk(&fx, "materials/paint.nmat");
    NTEST_EXPECT_NEAR_F32(disk.base_color.x, 0.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(disk.base_color.y, 1.0F, 1e-6F);
    settle(&fx);
    NTEST_EXPECT_TRUE(centre_is(&fx, 1U));
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_instance(sr, cube).value, kept.value);
    // The panel's history, not the scene's: the scene did not change.
    NTEST_EXPECT_EQ_UINT(editor_history_state(ed->history), scene);
    NTEST_ASSERT_TRUE(editor_material_undo(ed));
    disk = source_on_disk(&fx, "materials/paint.nmat");
    NTEST_EXPECT_NEAR_F32(disk.base_color.x, 1.0F, 1e-6F);
    settle(&fx);
    NTEST_EXPECT_TRUE(centre_is(&fx, 0U));
    NTEST_ASSERT_TRUE(editor_material_redo(ed));
    disk = source_on_disk(&fx, "materials/paint.nmat");
    NTEST_EXPECT_NEAR_F32(disk.base_color.y, 1.0F, 1e-6F);
    NTEST_EXPECT_FALSE(editor_material_redo(ed));
    settle(&fx);
    NTEST_EXPECT_TRUE(centre_is(&fx, 1U));
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_instance(sr, cube).value, kept.value);
    NTEST_EXPECT_EQ_UINT(editor_history_state(ed->history), scene);
    // A source that does not validate changes nothing.
    s.roughness = 2.0F;
    NTEST_EXPECT_EQ_INT(editor_material_change(ed, &s, true), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NEAR_F32(editor_material_source(ed)->roughness, 1.0F, 1e-6F);
    finish(&fx);
}

NTEST(Editor, a_material_dropped_on_a_surface_takes_its_slot_as_one_step_of_the_scene)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    paint(&fx, 0.0F, 0.0F, 1.0F);
    const NphEntity cube = made_cube(&fx);
    const NphRuntimeComponents *rc = nph_engine_components(fx.engine);
    const NphWorld *w = nph_engine_edit_world(fx.engine);
    const NphGuid blue = cooker_asset_guid("materials/paint.nmat", "material");
    const NphMeshRenderer *mr = NPH_WORLD_GET(w, cube, rc->mesh_renderer, NphMeshRenderer);
    NTEST_ASSERT_NOT_NULL(mr);
    const NphGuid before = mr->material0;
    NTEST_EXPECT_FALSE(nph_guid_equals(before, blue));
    // Dropped on the cube, which fills the viewport's middle: its one slot.
    f32 x = 0.0F;
    f32 y = 0.0F;
    viewport_center(&fx, &x, &y);
    NTEST_ASSERT_EQ_INT(editor_viewport_drop_material(ed, "materials/paint.nmat", x, y),
                        NPH_RESULT_OK);
    mr = NPH_WORLD_GET(w, cube, rc->mesh_renderer, NphMeshRenderer);
    NTEST_EXPECT_TRUE(nph_guid_equals(mr->material0, blue));
    NTEST_ASSERT_NOT_NULL(editor_history_undo_label(ed->history));
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(ed->history), "Set Material");
    settle(&fx);
    NTEST_EXPECT_TRUE(centre_is(&fx, 2U));
    // One step of the scene's history.
    NTEST_ASSERT_EQ_INT(editor_history_undo(ed->history), NPH_RESULT_OK);
    mr = NPH_WORLD_GET(w, cube, rc->mesh_renderer, NphMeshRenderer);
    NTEST_EXPECT_TRUE(nph_guid_equals(mr->material0, before));
    NTEST_ASSERT_EQ_INT(editor_history_redo(ed->history), NPH_RESULT_OK);
    mr = NPH_WORLD_GET(w, cube, rc->mesh_renderer, NphMeshRenderer);
    NTEST_EXPECT_TRUE(nph_guid_equals(mr->material0, blue));
    // Nowhere over the image, nothing.
    NTEST_EXPECT_EQ_INT(editor_viewport_drop_material(ed, "materials/paint.nmat", -10.0F, -10.0F),
                        NPH_RESULT_NOT_FOUND);
    // The outliner's drop: every slot the mesh has (the cube's one).
    NTEST_ASSERT_EQ_INT(editor_history_undo(ed->history), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        editor_material_assign(ed, cube, EDITOR_MATERIAL_EVERY_SLOT, "materials/paint.nmat"),
        NPH_RESULT_OK);
    mr = NPH_WORLD_GET(w, cube, rc->mesh_renderer, NphMeshRenderer);
    NTEST_EXPECT_TRUE(nph_guid_equals(mr->material0, blue));
    NTEST_EXPECT_TRUE(nph_guid_is_null(mr->material1));
    // A slot past the eighth goes in MeshMaterials.
    NTEST_ASSERT_EQ_INT(editor_material_assign(ed, cube, 9U, "materials/paint.nmat"),
                        NPH_RESULT_OK);
    const NphMeshMaterials *mm = NPH_WORLD_GET(w, cube, rc->mesh_materials, NphMeshMaterials);
    NTEST_ASSERT_NOT_NULL(mm);
    NTEST_EXPECT_TRUE(nph_guid_equals(nph_mesh_materials_get(mm, 1U), blue));
    // What is refused.
    NTEST_EXPECT_EQ_INT(editor_material_assign(ed, cube, 0U, "materials/paint.png"),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(
        editor_material_assign(ed, cube, NPH_MESH_RENDERER_MATERIALS + NPH_MESH_MATERIALS_EXTRA,
                               "materials/paint.nmat"),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_material_assign(ed, NPH_ENTITY_NULL, 0U, "materials/paint.nmat"),
                        NPH_RESULT_NOT_FOUND);
    finish(&fx);
}

// Waits for the viewport's centre to be of `channel` (the resolve weighs a still pixel's new
// frame at 1/32: a new material shows over some frames).
static bool centre_turns(EditorFixture *f, u32 channel)
{
    for (u32 waited = 0U; waited < 192U; waited += 8U) {
        if (centre_is(f, channel)) {
            return true;
        }
        frames(f, 8U);
    }
    return centre_is(f, channel);
}

NTEST(Editor, a_material_dropped_on_a_face_names_a_slot_of_the_mesh)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    paint(&fx, 0.0F, 0.0F, 1.0F);
    const NphEntity cube = made_cube(&fx);
    const u32 scene_steps = editor_history_undo_count(ed->history);
    // Not while not editing.
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_face_material(ed, 0U, "materials/paint.nmat", NULL),
                        NPH_RESULT_INVALID_STATE);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_begin(ed), NPH_RESULT_OK);
    frames(&fx, 2U);
    // Dropped on the face in the viewport's middle: that face alone goes to a slot no other
    // face keeps, named for the material, as one change of the editing's history.
    f32 x = 0.0F;
    f32 y = 0.0F;
    viewport_center(&fx, &x, &y);
    const EditorView v = editor_view(&fx);
    u32 face = 0U;
    NTEST_ASSERT_TRUE(editor_mesh_edit_pick_face(ed, &v, x, y, &face));
    const u32 changes = ed->mesh_edit.undo.len;
    NTEST_ASSERT_EQ_INT(editor_viewport_drop_material(ed, "materials/paint.nmat", x, y),
                        NPH_RESULT_OK);
    const CookPolyMesh *m = editor_mesh_edit_mesh(ed);
    NTEST_ASSERT_NOT_NULL(m);
    NTEST_EXPECT_EQ_UINT(m->faces.data[face].material, 1U);
    NTEST_EXPECT_EQ_STR(m->materials[1], "materials/paint.nmat");
    for (u32 f = 0; f < m->faces.len; ++f) {
        NTEST_EXPECT_TRUE(f == face || m->faces.data[f].material == 0U);
    }
    NTEST_EXPECT_EQ_UINT(ed->mesh_edit.undo.len, changes + 1U);
    // Again: nothing to change, no step.
    u32 slot = 99U;
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_face_material(ed, face, "materials/paint.nmat", &slot),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(slot, 1U);
    NTEST_EXPECT_EQ_UINT(ed->mesh_edit.undo.len, changes + 1U);
    // On a selected face, every selected face: all of them, to the slot that names it.
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_FACES, NULL, 0U, false),
                        NPH_RESULT_OK);
    editor_mesh_edit_select_all(ed);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_face_material(ed, face, "materials/paint.nmat", &slot),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(slot, 1U);
    for (u32 f = 0; f < m->faces.len; ++f) {
        NTEST_EXPECT_EQ_UINT(m->faces.data[f].material, 1U);
    }
    // The preview draws the slot's material: the cube turns blue.
    NTEST_EXPECT_TRUE(centre_turns(&fx, 2U));
    NTEST_EXPECT_TRUE(nph_guid_equals(ed->mesh_edit.preview_guids[1],
                                      cooker_asset_guid("materials/paint.nmat", "material")));
    // The editing's own history undoes and redoes it.
    NTEST_ASSERT_TRUE(editor_mesh_edit_undo(ed));
    m = editor_mesh_edit_mesh(ed);
    NTEST_EXPECT_EQ_UINT(m->faces.data[face == 0U ? 1U : 0U].material, 0U);
    NTEST_ASSERT_TRUE(editor_mesh_edit_redo(ed));
    m = editor_mesh_edit_mesh(ed);
    NTEST_EXPECT_EQ_UINT(m->faces.data[face == 0U ? 1U : 0U].material, 1U);
    // What is refused: a face out of range, a key that is not a material's, nowhere under the
    // mouse; and when other faces keep every slot.
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_face_material(ed, 99U, "materials/paint.nmat", NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_face_material(ed, face, "materials/paint.png", NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(
        editor_viewport_drop_material(ed, "materials/paint.nmat", v.x + 1.0F, v.y + 1.0F),
        NPH_RESULT_NOT_FOUND);
    const u32 top = 4U;
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_FACES, &top, 1U, false),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_extrude(ed, 0.25F), NPH_RESULT_OK); // ten faces
    CookPolyMesh *full = &ed->mesh_edit.mesh;
    for (u32 f = 0; f < full->faces.len; ++f) {
        full->faces.data[f].material = f % COOK_POLYMESH_MATERIALS;
    }
    NTEST_ASSERT_EQ_INT(editor_mesh_edit_select(ed, EDITOR_MESH_FACES, NULL, 0U, false),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(editor_mesh_edit_face_material(ed, 0U, "materials/glass.nmat", NULL),
                        NPH_RESULT_FULL);
    NTEST_ASSERT_TRUE(editor_mesh_edit_undo(ed)); // back before the extrusion
    frames(&fx, 1U);

    // Done: saved with its slots' materials and cooked; the entity, naming none for the slot,
    // draws the cooked mesh's.
    editor_mesh_edit_end(ed);
    char source[1200];
    (void)snprintf(source, sizeof source, "%s/meshes/cube.nmesh",
                   editor_project_assets_dir(editor_project(ed)));
    CookPolyMesh saved;
    NTEST_ASSERT_EQ_INT(cook_polymesh_load(nph_allocator_system(), source, &saved), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cook_polymesh_material_count(&saved), 2U);
    NTEST_EXPECT_EQ_STR(saved.materials[1], "materials/paint.nmat");
    cook_polymesh_free(&saved);
    for (u32 i = 0; i < 40U && ed->mesh_edit.committing; ++i) {
        frames(&fx, 1U);
        editor_project_wait(editor_project(ed));
        nph_scene_renderer_wait(nph_engine_scene_renderer(fx.engine));
    }
    NTEST_EXPECT_FALSE(ed->mesh_edit.committing);
    settle(&fx);
    NTEST_EXPECT_TRUE(centre_turns(&fx, 2U));
    const NphRenderFrameStats rs = nph_engine_frame_stats(fx.engine)->render;
    NTEST_EXPECT_EQ_UINT(rs.instances, 1U); // the cooked mesh's, the preview gone
    NTEST_EXPECT_EQ_UINT(ed->mesh_edit.preview_refs[1], NPH_SCENE_MATERIAL_NONE); // let go
    NTEST_EXPECT_TRUE(nph_world_is_alive(nph_engine_edit_world(fx.engine), cube));
    // The scene's history did not see any of it.
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(ed->history), scene_steps);
    finish(&fx);
}

// ---------------------------------------------------------------------------
// Material previews (ADR-0105 decision 7)
// ---------------------------------------------------------------------------

static EditorPreview *preview_of(Editor *ed, const char *key)
{
    for (u32 i = 0; ed->previews.items != NULL && i < EDITOR_PREVIEWS_MAX; ++i) {
        if (strcmp(ed->previews.items[i].key, key) == 0) {
            return &ed->previews.items[i];
        }
    }
    return NULL;
}

// Runs frames, asking for the preview of `key` as the browser does, until its picture is up
// for its content as it is now (and not for `before`).
static EditorPreview *preview_made(EditorFixture *f, const char *key, u64 before)
{
    for (u32 i = 0; i < 600U; ++i) {
        (void)editor_material_preview(f->editor, key);
        frames(f, 1U);
        EditorPreview *p = preview_of(f->editor, key);
        if (p != NULL && p->job == NULL && p->made != 0U && p->made != before &&
            p->made == p->wanted) {
            return p;
        }
    }
    NTEST_ASSERT_TRUE(false); // not made in 600 frames
    return NULL;
}

// The cached picture of the preview of `key`, made for `made`, into `out`.
static void cached_picture(EditorFixture *f, const char *key, u64 made, u8 *out)
{
    char path[1400];
    (void)snprintf(path, sizeof path, "%s/previews/%s.nph",
                   editor_project_cooked_dir(editor_project(f->editor)), key);
    void *bytes = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &bytes, &size),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cook_preview_read(bytes, size, made, out), NPH_RESULT_OK);
    nph_file_free_all(nph_allocator_system(), bytes, size);
}

// The middle of a preview: red, green, or blue by far (`channel`).
static bool middle_is(const u8 *picture, u32 channel)
{
    const u8 *p =
        picture + ((u64)(COOK_PREVIEW_SIZE / 2U) * COOK_PREVIEW_SIZE + COOK_PREVIEW_SIZE / 2U) * 4U;
    return p[3] == 255U && p[channel] > p[(channel + 1U) % 3U] + 40U &&
           p[channel] > p[(channel + 2U) % 3U] + 40U;
}

NTEST(Editor, the_browser_shows_a_material_as_a_lit_sphere_made_on_jobs_and_cached)
{
    static EditorFixture fx;
    START_OR_SKIP(&fx, NULL);
    Editor *ed = fx.editor;
    frames(&fx, 3U);
    static const char key[] = "materials/paint.nmat";
    paint(&fx, 1.0F, 0.0F, 0.0F);
    editor_project_wait(editor_project(ed));
    // The browser in the material's folder asks for its preview as it draws its row.
    (void)snprintf(ed->assets.folder, sizeof ed->assets.folder, "materials");
    ed->assets.stale = true;
    NTEST_EXPECT_EQ_UINT(editor_material_preview(ed, key), 0U); // none yet
    EditorPreview *p = preview_made(&fx, key, 0U);
    NTEST_EXPECT_NE_UINT(editor_material_preview(ed, key), 0U);
    NTEST_EXPECT_EQ_UINT(ed->previews.drawn, 1U);
    NTEST_EXPECT_EQ_UINT(ed->previews.loaded, 0U);
    static u8 picture[COOK_PREVIEW_BYTES];
    cached_picture(&fx, key, p->made, picture);
    NTEST_EXPECT_TRUE(middle_is(picture, 0U));
    // Painted green and cooked: drawn again, the red one shown until it is.
    const u64 red = p->made;
    paint(&fx, 0.0F, 1.0F, 0.0F);
    editor_project_wait(editor_project(ed));
    p = preview_made(&fx, key, red);
    NTEST_EXPECT_EQ_UINT(ed->previews.drawn, 2U);
    cached_picture(&fx, key, p->made, picture);
    NTEST_EXPECT_TRUE(middle_is(picture, 1U));
    // Its panel shows it too.
    NTEST_ASSERT_EQ_INT(editor_material_open(ed, key), NPH_RESULT_OK);
    frames(&fx, 2U);
    // Let go and asked for again (the editor started anew): read from the cache, not drawn.
    editor_material_previews_free(ed);
    editor_material_previews_init(ed);
    (void)preview_made(&fx, key, 0U);
    NTEST_EXPECT_EQ_UINT(ed->previews.loaded, 1U);
    NTEST_EXPECT_EQ_UINT(ed->previews.drawn, 0U);
    // A material that is not cooked has none, and nothing is made for it.
    NTEST_EXPECT_EQ_UINT(editor_material_preview(ed, "materials/nothing.nmat"), 0U);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(ed->previews.in_flight, 0U);
    // The picture is a sampled image of its own, which the frames above drew in the browser
    // and the panel under the validation gate.
    p = preview_of(ed, key);
    NTEST_ASSERT_NOT_NULL(p);
    const NphGpuImageDesc *d = nph_gpu_image_desc(nph_engine_device(fx.engine), p->image);
    NTEST_ASSERT_NOT_NULL(d);
    NTEST_EXPECT_EQ_UINT(d->width, COOK_PREVIEW_SIZE);
    NTEST_EXPECT_EQ_INT(d->format, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    finish(&fx);
}

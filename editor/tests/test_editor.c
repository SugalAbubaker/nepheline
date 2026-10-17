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
#include <nepheline/runtime/engine.h>
#include <nepheline/scene/transform.h>

#include <ntest/ntest.h>

#include "editor_internal.h"
#include "test_cook_support.h"

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
    static const char *const assignments[1] = {"editor.layout="}; // no layout file
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
    f->config.assignments[0] = assignments[0];
    f->config.assignments[1] = f->assets_assignment;
    f->config.assignments[2] = f->cooked_assignment;
    f->config.assignment_count = 3U;
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

// SPDX-License-Identifier: Apache-2.0
// The viewport panel (docs/editor.md "Viewport"): the scene's image, sized
// to the panel; the play toolbar; and the editor camera's controls:
//
//   right drag      look around, with W A S D to move, Q E down and up,
//                   Shift to go faster, the wheel to change the speed
//   Alt left drag   orbit the pivot
//   middle drag     pan
//   wheel           move toward or away from the pivot
//
// While playing, a click on the image gives the game the input, and
// Escape takes it back.
#include <nepheline/core/assert.h>
#include <nepheline/gpu/resources.h>
#include <nepheline/math/scalar.h>

#include "editor_internal.h"

#include <string.h>

#define LOOK_RADIANS_PER_PIXEL 0.004F
#define PAN_PER_PIXEL          0.0015F // of the distance to the pivot
#define FLY_BOOST              4.0F
#define FLY_SPEED_MIN          0.1F
#define FLY_SPEED_MAX          1000.0F

static void capture_cursor(Editor *ed, bool capture)
{
    NphWindow *w = nph_engine_window(ed->engine);
    if (w == NULL || ed->viewport.cursor_captured == capture) {
        return;
    }
    // Disabled: hidden, and unlimited motion while looking around.
    nph_window_set_cursor_mode(w, capture ? NPH_CURSOR_MODE_DISABLED : NPH_CURSOR_MODE_NORMAL);
    ed->viewport.cursor_captured = capture;
}

void editor_viewport_free(Editor *ed)
{
    if (ed->engine != NULL) {
        capture_cursor(ed, false);
    }
    if (ed->viewport.image.value != 0U && ed->dev != NULL) {
        nph_gpu_image_destroy(ed->dev, ed->viewport.image);
    }
    memset(&ed->viewport, 0, sizeof ed->viewport);
}

static NphResult resize(Editor *ed, u32 width, u32 height)
{
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.type = NPH_GPU_IMAGE_2D;
    d.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM; // the renderer writes sRGB values itself
    d.width = width;
    d.height = height;
    d.mip_count = 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED |
              (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    d.name = "editor viewport";
    NphGpuImage image = NPH_GPU_IMAGE_NULL;
    NPH_TRY(nph_gpu_image_create(ed->dev, &d, &image));
    if (ed->viewport.image.value != 0U) {
        nph_gpu_image_destroy(ed->dev, ed->viewport.image); // after the frames that use it
    }
    ed->viewport.image = image;
    ed->viewport.width = width;
    ed->viewport.height = height;
    return NPH_RESULT_OK;
}

static void toolbar(Editor *ed)
{
    NphEngine *e = ed->engine;
    const bool playing = nph_engine_is_playing(e);
    const bool paused = nph_engine_is_paused(e);
    if (igButton(playing ? "Stop" : "Play", ui_vec2(0.0F, 0.0F))) {
        editor_play_toggle(ed);
    }
    igSameLine(0.0F, -1.0F);
    igBeginDisabled(!playing);
    if (igButton(paused ? "Resume" : "Pause", ui_vec2(0.0F, 0.0F))) {
        nph_engine_set_paused(e, !paused);
    }
    igSameLine(0.0F, -1.0F);
    igBeginDisabled(!paused);
    if (igButton("Step", ui_vec2(0.0F, 0.0F))) {
        nph_engine_step_once(e);
    }
    igEndDisabled();
    igEndDisabled();
    igSameLine(0.0F, -1.0F);
    if (!playing) {
        igTextDisabled("right drag: look, WASD: move   Alt drag: orbit   middle drag: pan");
    } else if (ed->viewport.game_input) {
        igTextDisabled("the game has the input: Escape to take it back");
    } else {
        igTextDisabled("click the scene to give the game the input");
    }
}

static void camera_input(Editor *ed, bool hovered)
{
    EditorViewport *vp = &ed->viewport;
    EditorCamera *cam = &ed->camera;
    const ImGuiIO *io = igGetIO_Nil();
    if (nph_engine_is_playing(ed->engine)) {
        if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            vp->game_input = true;
        }
        vp->fly = false;
        vp->orbit = false;
        vp->pan = false;
        capture_cursor(ed, false);
        return;
    }
    if (hovered) {
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) {
            vp->fly = true;
        }
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Middle, false)) {
            vp->pan = true;
        }
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && io->KeyAlt) {
            vp->orbit = true;
        }
        if (!vp->fly && io->MouseWheel != 0.0F) {
            editor_camera_dolly(cam, io->MouseWheel);
        }
    }
    vp->fly = vp->fly && igIsMouseDown_Nil(ImGuiMouseButton_Right);
    vp->pan = vp->pan && igIsMouseDown_Nil(ImGuiMouseButton_Middle);
    vp->orbit = vp->orbit && igIsMouseDown_Nil(ImGuiMouseButton_Left);
    capture_cursor(ed, vp->fly);
    const f32 dx = io->MouseDelta.x;
    const f32 dy = io->MouseDelta.y;
    if (vp->fly) {
        editor_camera_look(cam, -dx * LOOK_RADIANS_PER_PIXEL, -dy * LOOK_RADIANS_PER_PIXEL);
        if (io->MouseWheel != 0.0F) {
            cam->fly_speed = nph_f32_clamp(cam->fly_speed * nph_f32_pow(1.25F, io->MouseWheel),
                                           FLY_SPEED_MIN, FLY_SPEED_MAX);
        }
        NphVec3 dir = nph_vec3_zero();
        dir.z += igIsKeyDown_Nil(ImGuiKey_W) ? 1.0F : 0.0F;
        dir.z -= igIsKeyDown_Nil(ImGuiKey_S) ? 1.0F : 0.0F;
        dir.x += igIsKeyDown_Nil(ImGuiKey_D) ? 1.0F : 0.0F;
        dir.x -= igIsKeyDown_Nil(ImGuiKey_A) ? 1.0F : 0.0F;
        dir.y += igIsKeyDown_Nil(ImGuiKey_E) ? 1.0F : 0.0F;
        dir.y -= igIsKeyDown_Nil(ImGuiKey_Q) ? 1.0F : 0.0F;
        editor_camera_fly(cam, nph_vec3_scale(dir, io->KeyShift ? FLY_BOOST : 1.0F), io->DeltaTime);
    } else if (vp->orbit) {
        editor_camera_orbit(cam, -dx * LOOK_RADIANS_PER_PIXEL, -dy * LOOK_RADIANS_PER_PIXEL);
    } else if (vp->pan) {
        const f32 k = cam->distance * PAN_PER_PIXEL;
        editor_camera_pan(cam, -dx * k, dy * k);
    }
}

void editor_panel_viewport(Editor *ed)
{
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, ui_vec2(0.0F, 0.0F));
    const bool open = igBegin(EDITOR_WINDOW_VIEWPORT, &ed->show.viewport,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    igPopStyleVar(1);
    if (!open) {
        igEnd();
        capture_cursor(ed, false);
        return;
    }
    toolbar(ed);
    const ImVec2_c avail = igGetContentRegionAvail();
    const ImGuiIO *io = igGetIO_Nil();
    const f32 px_w = avail.x * io->DisplayFramebufferScale.x;
    const f32 px_h = avail.y * io->DisplayFramebufferScale.y;
    if (px_w >= (f32)EDITOR_VIEWPORT_MIN && px_h >= (f32)EDITOR_VIEWPORT_MIN) {
        const u32 w = (u32)px_w;
        const u32 h = (u32)px_h;
        if (w != ed->viewport.width || h != ed->viewport.height) {
            const NphResult r = resize(ed, w, h);
            if (r != NPH_RESULT_OK && !ed->viewport.resize_failed) {
                NPH_LOG_ERROR(&LOG_EDITOR, "the viewport's %ux%u image: %s", w, h,
                              nph_result_string(r));
            }
            ed->viewport.resize_failed = r != NPH_RESULT_OK;
        }
    }
    bool hovered = false;
    ed->viewport.shown = false;
    if (ed->viewport.image.value != 0U && avail.x > 0.0F && avail.y > 0.0F) {
        const ImTextureID tex = ui_renderer_texture_id(ed->renderer, ed->viewport.image, true);
        igImage(ui_texture_ref(tex), avail, ui_vec2(0.0F, 0.0F), ui_vec2(1.0F, 1.0F));
        hovered = igIsItemHovered(ImGuiHoveredFlags_None);
        const ImVec2_c min = igGetItemRectMin();
        ed->viewport.rect[0] = min.x;
        ed->viewport.rect[1] = min.y;
        ed->viewport.rect[2] = avail.x;
        ed->viewport.rect[3] = avail.y;
        ed->viewport.shown = true;
    }
    camera_input(ed, hovered);
    igEnd();
}

void editor_viewport_apply_view(Editor *ed)
{
    NphEngine *e = ed->engine;
    if (nph_engine_is_playing(e)) {
        nph_engine_set_view(e, NULL); // the world's camera
        return;
    }
    // The world's exposure and tonemapper (its active camera's), seen
    // through the editor's camera.
    NphRenderView view;
    const NphRenderCamera cam = editor_camera_render(&ed->camera);
    nph_render_view_default(&view, cam);
    const NphSceneRenderer *sr = nph_engine_scene_renderer(e);
    if (sr != NULL) {
        (void)nph_scene_renderer_view(sr, &view);
    }
    view.camera = cam;
    nph_engine_set_view(e, &view);
}

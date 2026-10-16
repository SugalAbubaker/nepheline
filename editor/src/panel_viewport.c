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
#include <nepheline/asset/mesh.h>
#include <nepheline/core/assert.h>
#include <nepheline/gpu/resources.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/debug_draw.h>

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
    if (!playing) {
        static const char *const modes[3] = {"Move", "Rotate", "Scale"};
        igSameLine(0.0F, 16.0F);
        for (u32 i = 0; i < 3U; ++i) {
            const bool on = (u32)ed->gizmo_mode == i;
            if (on) {
                igPushStyleColor_U32(ImGuiCol_Button,
                                     igGetColorU32_Col(ImGuiCol_ButtonActive, 1.0F));
            }
            if (igButton(modes[i], ui_vec2(0.0F, 0.0F))) {
                ed->gizmo_mode = (EditorGizmoMode)i;
            }
            if (on) {
                igPopStyleColor(1);
            }
            igSameLine(0.0F, -1.0F);
        }
        if (igButton(ed->gizmo_local ? "Local" : "World", ui_vec2(0.0F, 0.0F))) {
            ed->gizmo_local = !ed->gizmo_local;
        }
        igSameLine(0.0F, 16.0F);
        igTextDisabled("click: select  W E R: gizmo  X: axes  F: frame  right drag: look");
    } else if (ed->viewport.game_input) {
        igTextDisabled("the game has the input: Escape to take it back");
    } else {
        igTextDisabled("click the scene to give the game the input");
    }
}

// ---------------------------------------------------------------------------
// Picking and overlays
// ---------------------------------------------------------------------------

static bool lookup_mesh(void *user, NphEntity e, NphMeshView *out)
{
    const NphAsset *mesh = nph_scene_renderer_mesh_asset((const NphSceneRenderer *)user, e);
    return mesh != NULL && nph_mesh_view(mesh, out) == NPH_RESULT_OK;
}

// Selects what is under the mouse (Ctrl adds or removes); nothing clears.
static void pick(Editor *ed, const EditorView *v, f32 mx, f32 my)
{
    NphSceneRenderer *sr = nph_engine_scene_renderer(ed->engine);
    EditorPick hit;
    const bool any =
        editor_pick(nph_engine_edit_world(ed->engine), nph_engine_components(ed->engine),
                    sr != NULL ? lookup_mesh : NULL, sr, v->eye, editor_view_ray(v, mx, my), &hit);
    if (igGetIO_Nil()->KeyCtrl) {
        if (any) {
            editor_selection_toggle(&ed->selection, hit.entity);
        }
    } else {
        editor_selection_set(&ed->selection, any ? hit.entity : NPH_ENTITY_NULL);
    }
}

#define COLOR_SELECTED NPH_RENDER_RGBA(255, 160, 40, 255)
#define COLOR_MARKER   NPH_RENDER_RGBA(230, 220, 120, 255)
#define COLOR_GRID     NPH_RENDER_RGBA(90, 90, 96, 255)

// The ground grid, markers for lights and cameras (which have no mesh to
// see), and boxes around the selection, as the renderer's debug lines.
static void overlays(Editor *ed)
{
    NphEngine *engine = ed->engine;
    NphRenderer *r = nph_engine_renderer(engine);
    if (r == NULL) {
        return;
    }
    const bool playing = nph_engine_is_playing(engine);
    const NphWorld *w = nph_engine_world(engine);
    // The frame's lines share one origin, which the editor does not own:
    // positions are relative to it as it is.
    const NphDVec3 eye = nph_render_debug_origin(r);
    if (ed->show_grid && !playing) {
        // A metre a cell, on the ground under the pivot.
        const NphDVec3 pivot = ed->camera.pivot;
        const NphDVec3 center =
            nph_dvec3(nph_f64_floor(pivot.x + 0.5), 0.0, nph_f64_floor(pivot.z + 0.5));
        nph_render_debug_grid(r, nph_dvec3_relative(center, eye), 1.0F, 40U, COLOR_GRID,
                              NPH_RENDER_DEBUG_DEPTH_TESTED);
    }
    const NphRuntimeComponents *rc = nph_engine_components(engine);
    const NphSceneRenderer *sr = nph_engine_scene_renderer(engine);
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphWorldTransform *t =
            NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        if (t == NULL) {
            continue;
        }
        const bool selected = editor_selection_contains(&ed->selection, e);
        const bool marker = nph_world_has(w, e, rc->light) || nph_world_has(w, e, rc->camera);
        if (!selected && !marker) {
            continue;
        }
        const NphVec3 at = nph_dvec3_relative(t->translation, eye);
        const NphAsset *mesh = sr != NULL ? nph_scene_renderer_mesh_asset(sr, e) : NULL;
        NphMeshView view;
        if (selected && mesh != NULL && nph_mesh_view(mesh, &view) == NPH_RESULT_OK) {
            const NphAffine a = nph_world_transform_relative(t, eye);
            nph_render_debug_box(r, &a, view.header->bounds, COLOR_SELECTED,
                                 NPH_RENDER_DEBUG_OVERLAY);
        } else if (marker) {
            const u32 c = selected ? COLOR_SELECTED : COLOR_MARKER;
            nph_render_debug_sphere(r, at, EDITOR_PICK_MARKER_RADIUS, c, NPH_RENDER_DEBUG_OVERLAY,
                                    16U);
            // Lights and cameras face their -Z.
            const NphVec3 forward = nph_vec3_normalize_or(nph_vec3_neg(t->z_axis), nph_vec3_zero());
            nph_render_debug_arrow(r, at, nph_vec3_mul_add(at, forward, 1.0F), c,
                                   NPH_RENDER_DEBUG_OVERLAY);
        } else {
            nph_render_debug_cross(r, at, 0.3F, COLOR_SELECTED, NPH_RENDER_DEBUG_OVERLAY);
        }
    }
}

// ---------------------------------------------------------------------------
// The camera
// ---------------------------------------------------------------------------

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
    // The gizmo first: a click on a handle drags it rather than picking.
    if (ed->viewport.shown && !nph_engine_is_playing(ed->engine) && !ed->viewport.fly) {
        EditorView view;
        const NphRenderCamera cam = editor_camera_render(&ed->camera);
        editor_view_init(&view, &cam, ed->viewport.rect[0], ed->viewport.rect[1],
                         ed->viewport.rect[2], ed->viewport.rect[3]);
        const bool gizmo = editor_gizmo_update(ed, &view, hovered, igGetWindowDrawList());
        if (hovered && !gizmo && !io->KeyAlt &&
            igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            pick(ed, &view, io->MousePos.x, io->MousePos.y);
        }
    }
    camera_input(ed, hovered && !ed->gizmo_dragging);
    overlays(ed);
    igEnd();
}

// A sphere around entity `e` in the world: its mesh's bounding sphere when
// the scene renderer has its mesh loaded, else a small one at its origin.
static void bounding_sphere(Editor *ed, const NphWorld *w, NphEntity e, NphDVec3 *out_center,
                            f64 *out_radius)
{
    const NphWorldTransform t = editor_world_transform_of(w, e);
    NphVec3 center = nph_vec3_zero();
    f32 radius = 0.5F;
    const NphSceneRenderer *sr = nph_engine_scene_renderer(ed->engine);
    const NphAsset *mesh = sr != NULL ? nph_scene_renderer_mesh_asset(sr, e) : NULL;
    NphMeshView view;
    if (mesh != NULL && nph_mesh_view(mesh, &view) == NPH_RESULT_OK) {
        center = view.header->sphere.center;
        radius = view.header->sphere.radius;
    }
    const f32 scale = nph_f32_max3(nph_vec3_length(t.x_axis), nph_vec3_length(t.y_axis),
                                   nph_vec3_length(t.z_axis));
    *out_center = nph_world_transform_point(&t, center);
    *out_radius = (f64)(radius * scale);
}

void editor_viewport_frame_selection(Editor *ed)
{
    const NphWorld *w = nph_engine_world(ed->engine);
    bool any = false;
    NphDVec3 lo = nph_dvec3_zero();
    NphDVec3 hi = nph_dvec3_zero();
    for (u32 i = 0; i < ed->selection.count; ++i) {
        const NphEntity e = ed->selection.items[i];
        if (!nph_world_is_alive(w, e)) {
            continue;
        }
        NphDVec3 c;
        f64 r = 0.0;
        bounding_sphere(ed, w, e, &c, &r);
        const NphDVec3 a = nph_dvec3(c.x - r, c.y - r, c.z - r);
        const NphDVec3 b = nph_dvec3(c.x + r, c.y + r, c.z + r);
        lo = any ? nph_dvec3_min(lo, a) : a;
        hi = any ? nph_dvec3_max(hi, b) : b;
        any = true;
    }
    if (!any) {
        return;
    }
    const NphDVec3 center = nph_dvec3_scale(nph_dvec3_add(lo, hi), 0.5);
    const NphDVec3 half = nph_dvec3_scale(nph_dvec3_sub(hi, lo), 0.5);
    const f64 radius = nph_f64_sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
    editor_camera_focus(&ed->camera, center, radius > 0.25 ? (f32)radius : 0.25F);
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

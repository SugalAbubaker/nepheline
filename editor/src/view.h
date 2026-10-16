// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport's camera as the screen sees it (docs/editor.md "Viewport"):
// world points projected onto the viewport's image, and rays from the eye
// through screen points, for picking and the gizmo. The projection is the
// renderer's (render/renderer.h; frame.c): a camera looking down its -Z,
// a vertical field of view, Vulkan's clip space with Y down. Points are
// taken relative to the eye first, so f32 is precise at any distance from
// the origin (CLAUDE.md §8.2).
//
// Thread-safety: plain data; no shared state.

#include <nepheline/render/renderer.h>

typedef struct EditorView {
    NphDVec3 eye;
    NphQuat orientation;
    f32 tan_half_fov; // tan(yfov / 2)
    f32 aspect;       // width / height
    f32 x;            // the image's rectangle on screen
    f32 y;
    f32 w;
    f32 h;
} EditorView;

// The view of `camera` drawn into the screen rectangle (x, y, w, h).
void editor_view_init(EditorView *v, const NphRenderCamera *camera, f32 x, f32 y, f32 w, f32 h);

// Where world point `p` lands on screen. False when it is behind the eye
// (or at it), where nothing projects.
bool editor_view_project(const EditorView *v, NphDVec3 p, f32 *out_sx, f32 *out_sy);

// The unit direction from the eye through screen point (sx, sy).
NphVec3 editor_view_ray(const EditorView *v, f32 sx, f32 sy);

// The world length one screen unit covers at `distance` from the eye along
// the view direction: gizmos keep a constant size on screen with it.
f32 editor_view_world_per_unit(const EditorView *v, f32 distance);

// The distance from `p` to the eye along the view direction (negative
// behind it).
f32 editor_view_depth(const EditorView *v, NphDVec3 p);

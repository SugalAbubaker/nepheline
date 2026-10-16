// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's camera (docs/editor.md "Viewport"): an orbit camera around a
// pivot that also flies. The eye sits `distance` behind the pivot along the
// view direction, which yaw and pitch give (ADR-0008: right-handed, Y up,
// a camera looks down its -Z):
//
//   orientation = rotation_y(yaw) * rotation_x(pitch)
//   eye         = pivot - forward * distance
//
// Orbiting turns the eye around the pivot; looking turns the view around
// the eye (the pivot follows); panning and flying move both. The pivot is
// a 64-bit position (§8.2 large-world coordinates).
//
// Thread-safety: plain data; no shared state.

#include <nepheline/render/renderer.h>

// Pitch stays inside +-EDITOR_CAMERA_MAX_PITCH: just short of straight up
// or down, where yaw would lose its meaning.
#define EDITOR_CAMERA_MAX_PITCH    1.55F
#define EDITOR_CAMERA_MIN_DISTANCE 0.05F
#define EDITOR_CAMERA_MAX_DISTANCE 100000.0F

typedef struct EditorCamera {
    NphDVec3 pivot;
    f32 yaw;       // radians about +Y; 0 looks down -Z
    f32 pitch;     // radians; positive looks up
    f32 distance;  // eye to pivot, metres
    f32 yfov;      // radians
    f32 znear;     // metres
    f32 fly_speed; // metres per second
} EditorCamera;

// A camera 8 m from the origin, looking down at it from the front.
void editor_camera_default(EditorCamera *c);

NphQuat editor_camera_orientation(const EditorCamera *c);
NphVec3 editor_camera_forward(const EditorCamera *c);
NphVec3 editor_camera_right(const EditorCamera *c);
NphVec3 editor_camera_up(const EditorCamera *c);
NphDVec3 editor_camera_eye(const EditorCamera *c);

// The camera the renderer draws from.
NphRenderCamera editor_camera_render(const EditorCamera *c);

// Turns the eye around the pivot by the angles (radians).
void editor_camera_orbit(EditorCamera *c, f32 dyaw, f32 dpitch);

// Turns the view around the eye by the angles (radians): mouse look.
void editor_camera_look(EditorCamera *c, f32 dyaw, f32 dpitch);

// Moves the eye and the pivot `right` and `up` metres in the view plane.
void editor_camera_pan(EditorCamera *c, f32 right, f32 up);

// Moves the eye toward the pivot (steps > 0) or away from it: each step
// scales the distance by 0.85, within the distance limits.
void editor_camera_dolly(EditorCamera *c, f32 steps);

// Flies for `dt` seconds at fly_speed along `dir`: x to the right, y up
// (the world's), z forward. The direction is not normalised: a diagonal
// is as fast as its components.
void editor_camera_fly(EditorCamera *c, NphVec3 dir, f32 dt);

// Puts the pivot at `center` and backs off until a sphere of `radius` fills
// the vertical field of view.
void editor_camera_focus(EditorCamera *c, NphDVec3 center, f32 radius);

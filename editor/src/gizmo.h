// SPDX-License-Identifier: Apache-2.0
#pragma once

// The transform gizmo's geometry (docs/editor.md "Gizmos"; CLAUDE.md §8.13
// "Viewport with gizmos (our own translate/rotate/scale)"):
//   - translate: arrows along three axes, and squares for the three
//     planes between them;
//   - rotate: a ring around each axis;
//   - scale: boxes on the three axes, and one in the middle for all three.
// Translate and rotate use world or local axes; scale always uses the
// entity's own.
//
// A drag starts where the mouse ray meets the handle's axis (the point of
// the axis nearest to the ray) or its plane, and follows that axis or plane
// from there. Positions are relative to the entity's origin, so f32 holds
// them anywhere in the world (§8.2). This file is the math; drawing and
// picking handles on screen are in gizmo_ui.c.
//
// Thread-safety: plain data; no shared state.

#include <nepheline/scene/transform.h>

typedef enum EditorGizmoMode {
    EDITOR_GIZMO_TRANSLATE = 0,
    EDITOR_GIZMO_ROTATE,
    EDITOR_GIZMO_SCALE,
} EditorGizmoMode;

// A handle: an axis (a ring when rotating), the plane facing an axis
// (translate), or all axes at once (scale).
typedef enum EditorGizmoHandle {
    EDITOR_GIZMO_NONE = 0,
    EDITOR_GIZMO_X,
    EDITOR_GIZMO_Y,
    EDITOR_GIZMO_Z,
    EDITOR_GIZMO_PLANE_X, // the YZ plane
    EDITOR_GIZMO_PLANE_Y, // the XZ plane
    EDITOR_GIZMO_PLANE_Z, // the XY plane
    EDITOR_GIZMO_ALL,
} EditorGizmoHandle;

#define EDITOR_GIZMO_PIXELS         90.0F   // the axes' length on screen
#define EDITOR_GIZMO_SNAP_TRANSLATE 0.5F    // metres
#define EDITOR_GIZMO_SNAP_ROTATE    0.2618F // 15 degrees
#define EDITOR_GIZMO_SNAP_SCALE     0.1F    // of the scale
#define EDITOR_GIZMO_SCALE_PER_UNIT 0.01F   // uniform scale per screen unit dragged

typedef struct EditorGizmoDrag {
    EditorGizmoMode mode;
    EditorGizmoHandle handle;
    NphWorldTransform start; // the entity's world transform when the drag began
    NphVec3 axes[3];         // the gizmo's axes then (unit)
    NphVec3 grab;            // where the ray met the handle's plane, relative to the origin
    f32 grab_param;          // where it met the handle's axis, along it
    f32 grab_x;              // the mouse, for the uniform scale
} EditorGizmoDrag;

// The axes the gizmo shows for an entity at `t`: the world's, or (`local`)
// the entity's own, normalised.
void editor_gizmo_axes(const NphWorldTransform *t, bool local, NphVec3 out[3]);

// The parameter along `axis` (unit, through the origin) of the axis point
// nearest to the ray origin + s * dir. False when the ray runs (nearly)
// along the axis.
bool editor_gizmo_axis_param(NphVec3 ray_origin, NphVec3 ray_dir, NphVec3 axis, f32 *out);

// Where the ray meets the plane through the origin facing `normal`. False
// when it runs (nearly) along the plane or points away from it.
bool editor_gizmo_plane_hit(NphVec3 ray_origin, NphVec3 ray_dir, NphVec3 normal, NphVec3 *out);

// The signed angle (radians) that turns `a` onto `b` around `axis`, both
// projected onto the plane facing the axis.
f32 editor_gizmo_angle(NphVec3 a, NphVec3 b, NphVec3 axis);

// `value` rounded to a multiple of `step`.
f32 editor_gizmo_snap(f32 value, f32 step);

// Begins a drag of `handle` on an entity at `start`, with the mouse ray
// (relative to the entity's origin) and the mouse's x on screen. False when
// the ray misses the handle's axis or plane (seen edge-on).
bool editor_gizmo_begin(EditorGizmoDrag *d, EditorGizmoMode mode, EditorGizmoHandle handle,
                        const NphWorldTransform *start, bool local, NphVec3 ray_origin,
                        NphVec3 ray_dir, f32 mouse_x);

// The world transform the drag leads to with the mouse ray now (relative to
// the start's origin) and the mouse's x. Snapped to whole steps of movement,
// turn, or scale with `snap`. The start, when the ray misses.
NphWorldTransform editor_gizmo_drag(const EditorGizmoDrag *d, NphVec3 ray_origin, NphVec3 ray_dir,
                                    f32 mouse_x, bool snap);

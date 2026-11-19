// SPDX-License-Identifier: Apache-2.0
#pragma once

// Picking (docs/editor.md "Viewport"): the entity under a ray from the
// viewport's eye.
//   - An entity with a visible MeshRenderer is hit by its mesh's triangles,
//     tested in the entity's own space after a test against the mesh's
//     bounds. The hit names the material slot of the triangle's submesh, so
//     a material dropped there sets that slot (ADR-0105).
//   - An entity without a mesh but with a Light or a Camera is hit by a
//     small sphere around its origin, so lights and cameras can be clicked.
// The nearest hit wins. Meshes come through a lookup, so the scene renderer
// supplies the loaded ones in the editor and tests supply their own.
//
// Costs: no allocation; every triangle of every mesh whose bounds the ray
// crosses is tested once per pick.
//
// Thread-safety: reads the world; the world's owner calls it.

#include <nepheline/asset/mesh.h>
#include <nepheline/runtime/components.h>

// The mesh entity `e` draws, or false when it has none loaded.
typedef bool (*EditorMeshLookup)(void *user, NphEntity e, NphMeshView *out);

typedef struct EditorPick {
    NphEntity entity;
    f64 distance; // from the eye, along the ray
    u32 slot;     // the material slot of the surface hit: its submesh's (0 for a marker)
} EditorPick;

// The radius of the sphere a light or a camera is picked by, in metres.
#define EDITOR_PICK_MARKER_RADIUS 0.25F

// The nearest entity of `w` the ray from `eye` along the unit `dir` hits.
// False when it hits none.
bool editor_pick(const NphWorld *w, const NphRuntimeComponents *rc, EditorMeshLookup lookup,
                 void *user, NphDVec3 eye, NphVec3 dir, EditorPick *out);

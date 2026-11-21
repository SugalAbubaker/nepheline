// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked physics materials (CLAUDE.md §8.8, §8.9; docs/asset.md "Physics
// materials"; docs/adr/0108-physics-in-levels.md decision 7): a surface made
// once and named by every Collider that has it (runtime/components.h
// `physics_material`), as the cooker makes it from a `.npmat` source
// (cook/physics_material.h).
//
// A physics material asset (NPH_ASSET_TYPE_PHYSICS_MATERIAL) holds one blob:
//
//     HEAD  NphPhysicsMaterialHeader
//
// Its combine modes are the physics module's (physics/shape.h
// NphPhysicsCombine), numbered here because the asset module does not depend
// on physics (CLAUDE.md §4); the runtime checks that the two agree.
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_PHYSICS_MATERIAL_TYPE_VERSION 1U

#define NPH_PHYSICS_MATERIAL_TAG_HEADER NPH_FOURCC('H', 'E', 'A', 'D')

// The largest friction and density a material may have: bounds against
// nonsense, far past any surface's (rubber on dry concrete is about 1, osmium
// 22,590 kg/m^3).
#define NPH_PHYSICS_MATERIAL_MAX_FRICTION 100.0F
#define NPH_PHYSICS_MATERIAL_MAX_DENSITY  1.0e6F

// How a surface's friction, or its restitution, combines with another's: the
// values of physics/shape.h NphPhysicsCombine. Of two touching surfaces'
// modes the higher one decides. Values are part of the file format and never
// renumbered.
typedef enum NphPhysicsMaterialCombine {
    // Friction by the geometric mean, restitution by the larger.
    NPH_PHYSICS_MATERIAL_COMBINE_DEFAULT = 0,
    NPH_PHYSICS_MATERIAL_COMBINE_AVERAGE = 1,  // the arithmetic mean
    NPH_PHYSICS_MATERIAL_COMBINE_MIN = 2,      // the smaller
    NPH_PHYSICS_MATERIAL_COMBINE_MULTIPLY = 3, // the product
    NPH_PHYSICS_MATERIAL_COMBINE_MAX = 4,      // the larger
    NPH_PHYSICS_MATERIAL_COMBINE_COUNT = 5,
} NphPhysicsMaterialCombine;

typedef struct NphPhysicsMaterialHeader {
    f32 friction;            // 0 .. NPH_PHYSICS_MATERIAL_MAX_FRICTION
    f32 restitution;         // 0 .. 1
    f32 density;             // kg/m^3, 0 .. NPH_PHYSICS_MATERIAL_MAX_DENSITY (0: no mass)
    u32 friction_combine;    // NphPhysicsMaterialCombine
    u32 restitution_combine; // NphPhysicsMaterialCombine
    u32 flags;               // zero
    u32 reserved[10];        // zero
} NphPhysicsMaterialHeader;

NPH_STATIC_ASSERT(sizeof(NphPhysicsMaterialHeader) == 64U,
                  "the physics material header is 64 bytes");

typedef struct NphPhysicsMaterialView {
    const NphPhysicsMaterialHeader *header;
} NphPhysicsMaterialView;

// Resolves and validates a READY physics material asset (above): its header,
// one of its size, whose values are in their ranges (finite), whose modes
// are known, and whose flags and reserved words are zero.
// NPH_RESULT_TYPE_MISMATCH and NPH_RESULT_VERSION_MISMATCH for another type
// or schema; NPH_RESULT_CORRUPT for anything out of range.
NPH_MUST_USE NphResult nph_physics_material_view(const NphAsset *asset,
                                                 NphPhysicsMaterialView *out);

#ifdef __cplusplus
}
#endif

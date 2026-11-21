// SPDX-License-Identifier: Apache-2.0
#pragma once

// Physics material sources (docs/cook.md "Physics materials";
// docs/adr/0108-physics-in-levels.md decision 7): a surface the editor makes
// and edits, kept as a `.npmat` source that the cooker cooks into a physics
// material asset (asset/physics_material.h), which Colliders name
// (runtime/components.h `physics_material`) over their own values.
//
// The `.npmat` file is JSON (core/json.h), written in one canonical form so it
// diffs cleanly in git:
//
//     {
//       "npmat": 1,
//       "friction": 0.6,
//       "restitution": 0,
//       "density": 1000,
//       "friction_combine": "default",
//       "restitution_combine": "default"
//     }
//
// Every key but "npmat" may be left out, taking the value above
// (cook_physics_material_source_default): a Collider's own defaults. Density
// is in kg/m^3. A combine mode (physics/shape.h NphPhysicsCombine) is
// "default" (friction by the geometric mean, restitution by the larger),
// "average", "minimum", "multiply" or "maximum"; of two touching surfaces'
// modes the later in that list decides.
//
// Reading checks every value: the version, the keys (an unknown or repeated
// one is refused), and the ranges of the asset (asset/physics_material.h):
// friction 0..100, restitution 0..1, density 0..10^6. Malformed input gives
// NPH_RESULT_PARSE_ERROR (NPH_RESULT_VERSION_MISMATCH for another version),
// never a crash: the reader is a fuzz target (tests/fuzz).
//
// Thread-safety: pure; a source belongs to one thread.

#include <nepheline/asset/physics_material.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_PHYSICS_MATERIAL_SOURCE_VERSION 1U

typedef struct CookPhysicsMaterialSource {
    f32 friction;            // 0..NPH_PHYSICS_MATERIAL_MAX_FRICTION
    f32 restitution;         // 0..1
    f32 density;             // kg/m^3, 0..NPH_PHYSICS_MATERIAL_MAX_DENSITY
    u32 friction_combine;    // NphPhysicsMaterialCombine
    u32 restitution_combine; // NphPhysicsMaterialCombine
} CookPhysicsMaterialSource;

// A new physics material: a Collider's own values (friction 0.6, restitution
// 0, density 1000, both combined by default).
void cook_physics_material_source_default(CookPhysicsMaterialSource *out);

// A combine mode's name in the file ("default", "average", "minimum",
// "multiply", "maximum"), NULL for none.
const char *cook_physics_combine_name(u32 mode);

// Checks what the reader checks. NPH_RESULT_CORRUPT on the first problem.
NPH_MUST_USE NphResult cook_physics_material_source_validate(const CookPhysicsMaterialSource *m);

// True when the two hold the same material.
bool cook_physics_material_source_equal(const CookPhysicsMaterialSource *a,
                                        const CookPhysicsMaterialSource *b);

// The cooked header for it: its values, nothing in the flags or reserved.
void cook_physics_material_source_header(const CookPhysicsMaterialSource *m,
                                         NphPhysicsMaterialHeader *out);

// ---------------------------------------------------------------------------
// The `.npmat` file
// ---------------------------------------------------------------------------

// Reads a document from `text` into `out`. `alloc` holds the parse only.
NPH_MUST_USE NphResult cook_physics_material_source_read(NphAllocator *alloc, NphStr text,
                                                         CookPhysicsMaterialSource *out);
// As cook_physics_material_source_read from the file at `path`; the file's
// errors too.
NPH_MUST_USE NphResult cook_physics_material_source_load(NphAllocator *alloc, const char *path,
                                                         CookPhysicsMaterialSource *out);
// Appends the document to `out`, numbers in their shortest exact form.
// NPH_RESULT_CORRUPT for a source that does not validate.
NPH_MUST_USE NphResult cook_physics_material_source_write(const CookPhysicsMaterialSource *m,
                                                          NphStrBuilder *out);
// Writes the document to `path`, replacing it whole (`path` + ".part" renamed
// over it).
NPH_MUST_USE NphResult cook_physics_material_source_save(const CookPhysicsMaterialSource *m,
                                                         NphAllocator *alloc, const char *path);

#ifdef __cplusplus
}
#endif

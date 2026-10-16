// SPDX-License-Identifier: Apache-2.0
#pragma once

// Transforms and the hierarchy (CLAUDE.md §8.7 "Transform hierarchy:
// local→world updated in parallel by hierarchy depth"; §8.2 "Large-world
// coordinates"; docs/scene.md "Transforms").
//
// Built-in components (registered by every world, ids in world.h):
//
//   Name            NphName            a label for the editor and for lookups
//   LocalTransform  NphLocalTransform  translation (64-bit), rotation, scale,
//                                      relative to the parent (or the world)
//   WorldTransform  NphWorldTransform  the composed result: a 3x3 linear part
//                                      (f32) and a 64-bit translation;
//                                      transient, recomputed, never saved
//   Hierarchy       NphHierarchy       parent and child links and the depth;
//                                      internal, changed only through
//                                      nph_world_set_parent
//
// Positions are 64-bit on the CPU (§8.2): a translation of 10 000 km keeps
// sub-millimetre precision, and the GPU receives camera-relative 32-bit
// transforms from nph_world_transform_relative.
//
// The transform system computes WorldTransform from LocalTransform. Roots
// (no Hierarchy, or no parent) are computed per chunk in parallel. Children
// are bucketed by depth and each depth level is computed in parallel, a
// level after its parent's level. Only chunks whose LocalTransform or
// Hierarchy changed since the system's previous run are recomputed, plus
// the descendants of anything recomputed, so a static scene costs one
// version check per chunk.
//
// Thread-safety: nph_world_set_parent is a change to the world (one
// thread). The transform system is documented with its functions.

#include <nepheline/jobs/jobs.h>
#include <nepheline/math/affine.h>
#include <nepheline/scene/schedule.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

#define NPH_NAME_FIELDS(F) F(TEXT, text, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphName, NPH_NAME_FIELDS);
NPH_COMPONENT_DECLARE(NphName);

#define NPH_LOCAL_TRANSFORM_FIELDS(F)                                                              \
    F(DVEC3, translation, NPH_FIELD_NO_HINTS)                                                      \
    F(QUAT, rotation, NPH_FIELD_NO_HINTS)                                                          \
    F(VEC3, scale, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphLocalTransform, NPH_LOCAL_TRANSFORM_FIELDS);
NPH_COMPONENT_DECLARE(NphLocalTransform);

// The world transform: p_world = translation + linear * p_local, with the
// linear part stored as its three columns (the local axes in world space).
#define NPH_WORLD_TRANSFORM_FIELDS(F)                                                              \
    F(DVEC3, translation, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))                                \
    F(VEC3, x_axis, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))                                      \
    F(VEC3, y_axis, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))                                      \
    F(VEC3, z_axis, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))
NPH_COMPONENT_STRUCT(NphWorldTransform, NPH_WORLD_TRANSFORM_FIELDS);
NPH_COMPONENT_DECLARE(NphWorldTransform);

// Intrusive links: the parent, the first and last child, and the siblings,
// so reparenting is O(1) plus the subtree's depth update and children stay
// in the order they were attached. `depth` is 0 for a root.
#define NPH_HIERARCHY_FIELDS(F)                                                                    \
    F(ENTITY, parent, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))                                    \
    F(ENTITY, first_child, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_HIDDEN))                                 \
    F(ENTITY, last_child, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_HIDDEN))                                  \
    F(ENTITY, next_sibling, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_HIDDEN))                                \
    F(ENTITY, prev_sibling, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_HIDDEN))                                \
    F(U32, depth, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))
NPH_COMPONENT_STRUCT(NphHierarchy, NPH_HIERARCHY_FIELDS);
NPH_COMPONENT_DECLARE(NphHierarchy);

NPH_STATIC_ASSERT(sizeof(NphLocalTransform) == 56U, "LocalTransform is 56 bytes");
NPH_STATIC_ASSERT(sizeof(NphWorldTransform) == 64U, "WorldTransform is one cache line");
NPH_STATIC_ASSERT(sizeof(NphHierarchy) == 24U, "Hierarchy is 24 bytes");

// The deepest hierarchy nph_world_set_parent builds (depth of the deepest
// node). Deeper chains are almost certainly a content error, and the
// transform system's level loop is bounded by it.
#define NPH_HIERARCHY_MAX_DEPTH 1024U

NPH_INLINE NphLocalTransform nph_local_transform_identity(void)
{
    NphLocalTransform t;
    t.translation = nph_dvec3_zero();
    t.rotation = nph_quat_identity();
    t.scale = nph_vec3_one();
    return t;
}

NPH_INLINE NphLocalTransform nph_local_transform(NphDVec3 translation, NphQuat rotation,
                                                 NphVec3 scale)
{
    NphLocalTransform t;
    t.translation = translation;
    t.rotation = rotation;
    t.scale = scale;
    return t;
}

NPH_INLINE NphWorldTransform nph_world_transform_identity(void)
{
    NphWorldTransform t;
    t.translation = nph_dvec3_zero();
    t.x_axis = nph_vec3_unit_x();
    t.y_axis = nph_vec3_unit_y();
    t.z_axis = nph_vec3_unit_z();
    return t;
}

// The world transform of a root: TRS with no parent.
NphWorldTransform nph_world_transform_from_local(const NphLocalTransform *local);

// parent ∘ local: the world transform of a child. The linear parts are
// composed in f32, the translation in f64 (parent.translation +
// parent.linear * local.translation), so a child keeps full precision far
// from the origin.
NphWorldTransform nph_world_transform_compose(const NphWorldTransform *parent,
                                              const NphLocalTransform *local);

// The world transform relative to `origin` (the camera position) as a
// 32-bit affine transform: what the GPU receives (§8.2).
NphAffine nph_world_transform_relative(const NphWorldTransform *t, NphDVec3 origin);

// The 3x3 linear part.
NPH_INLINE NphMat3 nph_world_transform_linear(const NphWorldTransform *t)
{
    return nph_mat3_from_cols(t->x_axis, t->y_axis, t->z_axis);
}

// Transforms a point in the entity's local space to world space (f64).
NphDVec3 nph_world_transform_point(const NphWorldTransform *t, NphVec3 p);

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

// Makes `child` a child of `parent` (appended after its existing
// children), or a root when `parent` is null. Adds the Hierarchy component
// to either entity when it lacks one (a structural change). The depths of
// the child's subtree are updated. NPH_RESULT_NOT_FOUND for a dead entity,
// NPH_RESULT_INVALID_ARGUMENT when `parent` is `child` or one of its
// descendants (a cycle), NPH_RESULT_OVERFLOW when the subtree would end
// deeper than NPH_HIERARCHY_MAX_DEPTH. Nothing changes on failure.
NPH_MUST_USE NphResult nph_world_set_parent(NphWorld *world, NphEntity child, NphEntity parent);

// The parent of `e`, or NPH_ENTITY_NULL (also for a dead entity).
NphEntity nph_world_parent(const NphWorld *world, NphEntity e);

// The first child of `e` and the sibling after `e`, for walking children:
//
//     for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
//          c = nph_world_next_sibling(w, c)) { ... }
NphEntity nph_world_first_child(const NphWorld *world, NphEntity e);
NphEntity nph_world_next_sibling(const NphWorld *world, NphEntity e);

// The depth of `e` (0 for a root or an entity without Hierarchy).
u32 nph_world_depth(const NphWorld *world, NphEntity e);

// Despawns `e` and all of its descendants. NPH_RESULT_NOT_FOUND for a
// dead entity.
NPH_MUST_USE NphResult nph_world_despawn_recursive(NphWorld *world, NphEntity e);

// ---------------------------------------------------------------------------
// The transform system
// ---------------------------------------------------------------------------

typedef struct NphTransformSystem NphTransformSystem;

typedef struct NphTransformStats {
    u32 roots_updated;    // root entities recomputed in the last update
    u32 children_updated; // hierarchy entities recomputed
    u32 chunks_skipped;   // root chunks skipped by the change filter
    u32 levels;           // depth levels processed
} NphTransformStats;

// Creates the system's state for `world`: its queries and a per-entity
// stamp array (4 bytes per entity of capacity, from `alloc`). Bucket
// arrays for the hierarchy pass grow on demand from `alloc` (amortised;
// docs/scene.md). NPH_RESULT_OUT_OF_MEMORY on failure.
// Thread-safety: a change to the world (creates queries).
NPH_MUST_USE NphResult nph_transform_system_create(NphWorld *world, NphAllocator *alloc,
                                                   NphTransformSystem **out);

// Destroys the state. Safe on NULL.
void nph_transform_system_destroy(NphTransformSystem *ts);

// The transform system's name in a schedule: systems that need this
// step's world transforms run after it.
#define NPH_TRANSFORM_SYSTEM_NAME "nph.transform"

// The system's descriptor for nph_schedule_add_system, named
// NPH_TRANSFORM_SYSTEM_NAME: it reads LocalTransform and Hierarchy and
// writes WorldTransform. `ts` must outlive the schedule.
NphSystemDesc nph_transform_system_desc(NphTransformSystem *ts);

// Runs one update outside a schedule: advances the world tick and updates
// every changed world transform, in parallel on `jobs` (NULL: on the
// calling thread). The world's structure must not change meanwhile.
NPH_MUST_USE NphResult nph_transform_system_update(NphTransformSystem *ts, NphJobSystem *jobs);

// Statistics of the most recent update.
void nph_transform_system_stats(const NphTransformSystem *ts, NphTransformStats *out);

#ifdef __cplusplus
}
#endif

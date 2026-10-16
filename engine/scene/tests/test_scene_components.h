// SPDX-License-Identifier: Apache-2.0
#pragma once

// The component types of the scene tests, defined in test_components.c.
// Shared with the scene fuzz targets (tests/fuzz/fuzz_scene_*.c), whose
// seed corpora name them (scripts/fuzz_seeds_scene.py); so the header
// depends on nothing but the scene module.

#include <nepheline/scene/component.h>

#define TEST_POSITION_FIELDS(F) F(VEC3, value, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(TestPosition, TEST_POSITION_FIELDS);
NPH_COMPONENT_DECLARE(TestPosition);

#define TEST_VELOCITY_FIELDS(F) F(VEC3, value, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(TestVelocity, TEST_VELOCITY_FIELDS);
NPH_COMPONENT_DECLARE(TestVelocity);

#define TEST_HEALTH_FIELDS(F)                                                                      \
    F(F32, current, NPH_FIELD_NO_HINTS)                                                            \
    F(F32, maximum, NPH_FIELD_RANGE(1.0F, 1000.0F))                                                \
    F(ENTITY, attacker, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))
NPH_COMPONENT_STRUCT(TestHealth, TEST_HEALTH_FIELDS);
NPH_COMPONENT_DECLARE(TestHealth);

// One field of every kind (the serialization tests round-trip it).
#define TEST_ALL_KINDS_FIELDS(F)                                                                   \
    F(BOOL, flag, NPH_FIELD_NO_HINTS)                                                              \
    F(U8, u8v, NPH_FIELD_NO_HINTS)                                                                 \
    F(U16, u16v, NPH_FIELD_NO_HINTS)                                                               \
    F(U32, u32v, NPH_FIELD_NO_HINTS)                                                               \
    F(U64, u64v, NPH_FIELD_NO_HINTS)                                                               \
    F(I8, i8v, NPH_FIELD_NO_HINTS)                                                                 \
    F(I16, i16v, NPH_FIELD_NO_HINTS)                                                               \
    F(I32, i32v, NPH_FIELD_NO_HINTS)                                                               \
    F(I64, i64v, NPH_FIELD_NO_HINTS)                                                               \
    F(F32, f32v, NPH_FIELD_NO_HINTS)                                                               \
    F(F64, f64v, NPH_FIELD_NO_HINTS)                                                               \
    F(VEC2, v2, NPH_FIELD_NO_HINTS)                                                                \
    F(VEC3, v3, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_COLOR))                                             \
    F(VEC4, v4, NPH_FIELD_NO_HINTS)                                                                \
    F(QUAT, q, NPH_FIELD_NO_HINTS)                                                                 \
    F(DVEC3, d3, NPH_FIELD_NO_HINTS)                                                               \
    F(ENTITY, target, NPH_FIELD_NO_HINTS)                                                          \
    F(STR_ID, sid, NPH_FIELD_NO_HINTS)                                                             \
    F(TEXT, label, NPH_FIELD_NO_HINTS)                                                             \
    F(GUID, asset, NPH_FIELD_NO_HINTS)                                                             \
    F(F32, scratch, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_TRANSIENT))
NPH_COMPONENT_STRUCT(TestAllKinds, TEST_ALL_KINDS_FIELDS);
NPH_COMPONENT_DECLARE(TestAllKinds);

NPH_TAG_DECLARE(TestTagA);
NPH_TAG_DECLARE(TestTagB);

// A 48-byte component (a different chunk capacity from the others).
#define TEST_BIG_FIELDS(F)                                                                         \
    F(VEC4, a0, NPH_FIELD_NO_HINTS) F(VEC4, a1, NPH_FIELD_NO_HINTS) F(VEC4, a2, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(TestBig, TEST_BIG_FIELDS);
NPH_COMPONENT_DECLARE(TestBig);

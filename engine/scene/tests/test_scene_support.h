// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers and component types shared by the scene tests.

#include <nepheline/core/assert.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/scene/command.h>
#include <nepheline/scene/prefab.h>
#include <nepheline/scene/query.h>
#include <nepheline/scene/schedule.h>
#include <nepheline/scene/transform.h>
#include <nepheline/scene/world.h>

#include <ntest/ntest.h>

#include <string.h>

// Work counts scale down where the sanitizers make everything slower.
#if NPH_TSAN_ENABLED
#define TEST_SCALE 1U
#elif NPH_CONFIG_DEBUG
#define TEST_SCALE 2U
#else
#define TEST_SCALE 8U
#endif

// ---------------------------------------------------------------------------
// Test components (defined in test_components.c)
// ---------------------------------------------------------------------------

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

// Ids of the test components in a world made by test_world_create.
typedef struct TestIds {
    NphComponentId position;
    NphComponentId velocity;
    NphComponentId health;
    NphComponentId all;
    NphComponentId tag_a;
    NphComponentId tag_b;
    NphComponentId big;
} TestIds;

static inline NphWorld *test_world_create_desc(const NphWorldDesc *in, TestIds *ids)
{
    NphWorldDesc desc = *in;
    if (desc.alloc == NULL) {
        desc.alloc = nph_allocator_system();
    }
    NphWorld *w = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&desc, &w), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(w);
    if (ids != NULL && desc.registry_from == NULL) {
        NTEST_ASSERT_EQ_INT(
            nph_world_register_component(w, TestPosition_component(), &ids->position),
            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(
            nph_world_register_component(w, TestVelocity_component(), &ids->velocity),
            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_world_register_component(w, TestHealth_component(), &ids->health),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_world_register_component(w, TestAllKinds_component(), &ids->all),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_world_register_component(w, TestTagA_component(), &ids->tag_a),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_world_register_component(w, TestTagB_component(), &ids->tag_b),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_world_register_component(w, TestBig_component(), &ids->big),
                            NPH_RESULT_OK);
    }
    return w;
}

static inline NphWorld *test_world_create(u32 max_entities, TestIds *ids)
{
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.max_entities = max_entities;
    return test_world_create_desc(&desc, ids);
}

static inline NphJobSystem *test_jobs_create(u32 workers)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.worker_count = workers;
    NphJobSystem *js = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_OK);
    return js;
}

// Between 2 and `max` workers, so the parallel paths are exercised on a
// small CI runner without oversubscribing it.
static inline u32 test_worker_count(u32 max)
{
    const u32 cores = nph_cpu_logical_count();
    const u32 n = cores < 2U ? 2U : cores;
    return n > max ? max : n;
}

static inline NphVec3 test_vec3(f32 x, f32 y, f32 z)
{
    return nph_vec3(x, y, z);
}

// Deterministic PRNG (splitmix64).
static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline u32 test_rng_below(u64 *state, u32 bound)
{
    return (u32)(test_rng_next(state) % bound);
}

// ---------------------------------------------------------------------------
// Expected assertions
// ---------------------------------------------------------------------------

typedef struct TestAssertRecorder {
    u32 count;
} TestAssertRecorder;

static inline NphAssertAction test_assert_record(const NphAssertInfo *info, void *user)
{
    NTEST_UNUSED(info);
    ((TestAssertRecorder *)user)->count += 1U;
    return NPH_ASSERT_ACTION_CONTINUE;
}

// Runs `stmt` and checks that at least one assertion fired (none are
// compiled in configurations without asserts; the statement then just
// runs).
#if NPH_ASSERTS_ENABLED
#define TEST_EXPECT_ASSERT(stmt)                                                                   \
    do {                                                                                           \
        TestAssertRecorder rec_ = {0U};                                                            \
        NphAssertHandler prev_ = NULL;                                                             \
        void *prev_user_ = NULL;                                                                   \
        nph_assert_set_handler(test_assert_record, &rec_, &prev_, &prev_user_);                    \
        stmt;                                                                                      \
        nph_assert_set_handler(prev_, prev_user_, NULL, NULL);                                     \
        NTEST_EXPECT_GE_UINT(rec_.count, 1U);                                                      \
    } while (0)
#else
#define TEST_EXPECT_ASSERT(stmt)                                                                   \
    do {                                                                                           \
        stmt;                                                                                      \
    } while (0)
#endif

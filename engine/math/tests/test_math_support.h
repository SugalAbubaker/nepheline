// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the math tests: deterministic random inputs for the
// property tests and assertions over vectors, quaternions, and matrices.
// Every random generator is seeded explicitly so a failing case can be
// reproduced from the log, and draws in order, each draw a statement of its
// own: as one call's arguments, the draws would be made in an order C leaves
// unspecified and compilers differ in, and each would test other inputs.

#include <nepheline/core/assert.h>
#include <nepheline/math/affine.h>

#include <ntest/ntest.h>

// splitmix64 (Steele, Lea & Flood, OOPSLA 2014): good enough to drive
// property tests and independent of the engine's own PRNG, which is itself
// under test.
static inline u64 tm_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// Uniform in [0, 1).
static inline f32 tm_rng_f32(u64 *state)
{
    return (f32)(tm_rng_next(state) >> 40U) * (1.0F / 16777216.0F);
}

// Uniform in [lo, hi).
static inline f32 tm_rng_range(u64 *state, f32 lo, f32 hi)
{
    return lo + (hi - lo) * tm_rng_f32(state);
}

static inline NphVec2 tm_rng_vec2(u64 *state, f32 lo, f32 hi)
{
    const f32 x = tm_rng_range(state, lo, hi);
    const f32 y = tm_rng_range(state, lo, hi);
    return nph_vec2(x, y);
}

static inline NphVec3 tm_rng_vec3(u64 *state, f32 lo, f32 hi)
{
    const f32 x = tm_rng_range(state, lo, hi);
    const f32 y = tm_rng_range(state, lo, hi);
    const f32 z = tm_rng_range(state, lo, hi);
    return nph_vec3(x, y, z);
}

static inline NphVec4 tm_rng_vec4(u64 *state, f32 lo, f32 hi)
{
    const f32 x = tm_rng_range(state, lo, hi);
    const f32 y = tm_rng_range(state, lo, hi);
    const f32 z = tm_rng_range(state, lo, hi);
    const f32 w = tm_rng_range(state, lo, hi);
    return nph_vec4(x, y, z, w);
}

// Uniform in the box [lo, hi).
static inline NphVec3 tm_rng_box(u64 *state, NphVec3 lo, NphVec3 hi)
{
    const f32 x = tm_rng_range(state, lo.x, hi.x);
    const f32 y = tm_rng_range(state, lo.y, hi.y);
    const f32 z = tm_rng_range(state, lo.z, hi.z);
    return nph_vec3(x, y, z);
}

// Uniformly distributed unit vector (Marsaglia 1972).
static inline NphVec3 tm_rng_unit_vec3(u64 *state)
{
    for (;;) {
        const f32 x = tm_rng_range(state, -1.0F, 1.0F);
        const f32 y = tm_rng_range(state, -1.0F, 1.0F);
        const f32 s = x * x + y * y;
        if (s > 0.0F && s < 1.0F) {
            const f32 k = 2.0F * nph_f32_sqrt(1.0F - s);
            return nph_vec3(x * k, y * k, 1.0F - 2.0F * s);
        }
    }
}

// Uniformly distributed unit quaternion (Shoemake, "Uniform Random
// Rotations", Graphics Gems III).
static inline NphQuat tm_rng_unit_quat(u64 *state)
{
    const f32 u1 = tm_rng_f32(state);
    const f32 u2 = tm_rng_f32(state) * NPH_TAU;
    const f32 u3 = tm_rng_f32(state) * NPH_TAU;
    const f32 a = nph_f32_sqrt(1.0F - u1);
    const f32 b = nph_f32_sqrt(u1);
    return nph_quat(a * nph_f32_sin(u2), a * nph_f32_cos(u2), b * nph_f32_sin(u3),
                    b * nph_f32_cos(u3));
}

// A TRS transform with scale in [0.5, 2] and translation in [-10, 10]:
// well conditioned, so inverses and decompositions are testable at tight
// tolerances.
static inline NphMat4 tm_rng_trs_mat4(u64 *state, NphVec3 *out_t, NphQuat *out_r, NphVec3 *out_s)
{
    const NphVec3 t = tm_rng_vec3(state, -10.0F, 10.0F);
    const NphQuat r = tm_rng_unit_quat(state);
    const NphVec3 s = tm_rng_vec3(state, 0.5F, 2.0F);
    if (out_t != NULL) {
        *out_t = t;
    }
    if (out_r != NULL) {
        *out_r = r;
    }
    if (out_s != NULL) {
        *out_s = s;
    }
    return nph_mat4_from_trs(t, r, s);
}

// A general 4x4 with entries in [-1, 1], re-drawn until it is comfortably
// non-singular (|det| >= 0.05).
static inline NphMat4 tm_rng_general_mat4(u64 *state)
{
    for (;;) {
        const NphVec4 c0 = tm_rng_vec4(state, -1.0F, 1.0F);
        const NphVec4 c1 = tm_rng_vec4(state, -1.0F, 1.0F);
        const NphVec4 c2 = tm_rng_vec4(state, -1.0F, 1.0F);
        const NphVec4 c3 = tm_rng_vec4(state, -1.0F, 1.0F);
        const NphMat4 m = nph_mat4_from_cols(c0, c1, c2, c3);
        if (nph_f32_abs(nph_mat4_determinant(m)) >= 0.05F) {
            return m;
        }
    }
}

// ---------------------------------------------------------------------------
// Assertions. Each logs the operands before aborting the test.
// ---------------------------------------------------------------------------

#define TM_ASSERT_VEC2_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        const NphVec2 tm_a_ = (a);                                                                 \
        const NphVec2 tm_b_ = (b);                                                                 \
        if (!nph_vec2_near(tm_a_, tm_b_, (tol))) {                                                 \
            NTEST_FAIL("%s != %s: (%g, %g) vs (%g, %g)", #a, #b, (f64)tm_a_.x, (f64)tm_a_.y,       \
                       (f64)tm_b_.x, (f64)tm_b_.y);                                                \
        }                                                                                          \
    } while (0)

#define TM_ASSERT_VEC3_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        const NphVec3 tm_a_ = (a);                                                                 \
        const NphVec3 tm_b_ = (b);                                                                 \
        if (!nph_vec3_near(tm_a_, tm_b_, (tol))) {                                                 \
            NTEST_FAIL("%s != %s: (%g, %g, %g) vs (%g, %g, %g)", #a, #b, (f64)tm_a_.x,             \
                       (f64)tm_a_.y, (f64)tm_a_.z, (f64)tm_b_.x, (f64)tm_b_.y, (f64)tm_b_.z);      \
        }                                                                                          \
    } while (0)

#define TM_ASSERT_VEC4_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        const NphVec4 tm_a_ = (a);                                                                 \
        const NphVec4 tm_b_ = (b);                                                                 \
        if (!nph_vec4_near(tm_a_, tm_b_, (tol))) {                                                 \
            NTEST_FAIL("%s != %s: (%g, %g, %g, %g) vs (%g, %g, %g, %g)", #a, #b, (f64)tm_a_.x,     \
                       (f64)tm_a_.y, (f64)tm_a_.z, (f64)tm_a_.w, (f64)tm_b_.x, (f64)tm_b_.y,       \
                       (f64)tm_b_.z, (f64)tm_b_.w);                                                \
        }                                                                                          \
    } while (0)

#define TM_ASSERT_QUAT_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        const NphQuat tm_a_ = (a);                                                                 \
        const NphQuat tm_b_ = (b);                                                                 \
        if (!nph_quat_near(tm_a_, tm_b_, (tol))) {                                                 \
            NTEST_FAIL("%s != %s: (%g, %g, %g, %g) vs (%g, %g, %g, %g)", #a, #b, (f64)tm_a_.x,     \
                       (f64)tm_a_.y, (f64)tm_a_.z, (f64)tm_a_.w, (f64)tm_b_.x, (f64)tm_b_.y,       \
                       (f64)tm_b_.z, (f64)tm_b_.w);                                                \
        }                                                                                          \
    } while (0)

// Same rotation: q and -q are equal.
#define TM_ASSERT_ROTATION_NEAR(a, b, tol)                                                         \
    do {                                                                                           \
        const NphQuat tm_a_ = (a);                                                                 \
        const NphQuat tm_b_ = (b);                                                                 \
        if (!nph_quat_near_rotation(tm_a_, tm_b_, (tol))) {                                        \
            NTEST_FAIL("%s !~ %s as rotations: (%g, %g, %g, %g) vs (%g, %g, %g, %g)", #a, #b,      \
                       (f64)tm_a_.x, (f64)tm_a_.y, (f64)tm_a_.z, (f64)tm_a_.w, (f64)tm_b_.x,       \
                       (f64)tm_b_.y, (f64)tm_b_.z, (f64)tm_b_.w);                                  \
        }                                                                                          \
    } while (0)

static inline void tm_log_mat4(const char *name, NphMat4 m)
{
    for (u32 r = 0; r < 4U; ++r) {
        const NphVec4 row = nph_mat4_row(m, r);
        NTEST_LOG("%s row %u: % .6f % .6f % .6f % .6f", name, r, (f64)row.x, (f64)row.y, (f64)row.z,
                  (f64)row.w);
    }
}

#define TM_ASSERT_MAT4_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        const NphMat4 tm_a_ = (a);                                                                 \
        const NphMat4 tm_b_ = (b);                                                                 \
        if (!nph_mat4_near(tm_a_, tm_b_, (tol))) {                                                 \
            tm_log_mat4(#a, tm_a_);                                                                \
            tm_log_mat4(#b, tm_b_);                                                                \
            NTEST_FAIL("%s != %s (tolerance %g)", #a, #b, (f64)(tol));                             \
        }                                                                                          \
    } while (0)

#define TM_ASSERT_MAT3_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        const NphMat3 tm_a_ = (a);                                                                 \
        const NphMat3 tm_b_ = (b);                                                                 \
        if (!nph_mat3_near(tm_a_, tm_b_, (tol))) {                                                 \
            tm_log_mat4(#a, nph_mat4_from_mat3(tm_a_));                                            \
            tm_log_mat4(#b, nph_mat4_from_mat3(tm_b_));                                            \
            NTEST_FAIL("%s != %s (tolerance %g)", #a, #b, (f64)(tol));                             \
        }                                                                                          \
    } while (0)

#define TM_ASSERT_AFFINE_NEAR(a, b, tol)                                                           \
    do {                                                                                           \
        const NphAffine tm_a_ = (a);                                                               \
        const NphAffine tm_b_ = (b);                                                               \
        if (!nph_affine_near(tm_a_, tm_b_, (tol))) {                                               \
            tm_log_mat4(#a, nph_affine_to_mat4(tm_a_));                                            \
            tm_log_mat4(#b, nph_affine_to_mat4(tm_b_));                                            \
            NTEST_FAIL("%s != %s (tolerance %g)", #a, #b, (f64)(tol));                             \
        }                                                                                          \
    } while (0)

// Observes an expected assertion failure (mirrors core's test_support.h).
typedef struct TmAssertRecorder {
    u32 count;
} TmAssertRecorder;

static inline NphAssertAction tm_assert_record(const NphAssertInfo *info, void *user)
{
    NPH_UNUSED(info);
    ((TmAssertRecorder *)user)->count += 1U;
    return NPH_ASSERT_ACTION_CONTINUE;
}

#if NPH_ASSERTS_ENABLED
#define TM_EXPECT_ASSERT(stmt)                                                                     \
    do {                                                                                           \
        TmAssertRecorder rec_ = {0U};                                                              \
        NphAssertHandler prev_ = NULL;                                                             \
        void *prev_user_ = NULL;                                                                   \
        nph_assert_set_handler(tm_assert_record, &rec_, &prev_, &prev_user_);                      \
        stmt;                                                                                      \
        nph_assert_set_handler(prev_, prev_user_, NULL, NULL);                                     \
        NTEST_ASSERT_EQ_UINT(rec_.count, 1U);                                                      \
    } while (0)
#else
#define TM_EXPECT_ASSERT(stmt)                                                                     \
    do {                                                                                           \
        stmt;                                                                                      \
    } while (0)
#endif

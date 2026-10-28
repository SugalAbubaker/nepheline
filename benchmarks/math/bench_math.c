// SPDX-License-Identifier: Apache-2.0
// Math benchmarks (docs/math.md): the scalar reference implementations
// against the SIMD register layer and the batch kernels at each ISA level.
//
//   mat4_mul_scalar / mat4_mul_simd            one 4x4 product
//   mat4_inverse_scalar / mat4_inverse_simd    one general 4x4 inverse
//   quat_mul_scalar / quat_mul_simd            one quaternion product
//   affine_point_scalar / affine_point_simd    one point through an affine
//   transform_points_<level>                   65536 points, batch kernel (ns per point)
//   cull_aabbs_<level>                         65536 boxes against a frustum (ns per box)
//   cull_spheres_<level>                       65536 spheres (ns per sphere)
//   f32_to_f16_<level>                         65536 floats (ns per float)
//   pcg32_next_u32, pcg32_bounded, halton_2d, r2, oct_encode_snorm16
//   blue_noise_32x32                           one full void-and-cluster run
//
// <level> is scalar, sse4, or avx2. When the CPU lacks AVX2 the avx2 entries
// run the sse4 kernels and say so in their label, so the benchmark still
// completes on every machine.

#include <nepheline/math/kernels.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/random.h>
#include <nepheline/math/simd.h>

#include <nbench/nbench.h>

#include <stdlib.h>

#define BATCH 65536U

static NphAABB g_boxes[BATCH];
static NphSphere g_spheres[BATCH];
static NphVec3 g_points[BATCH];
static NphVec3 g_out_points[BATCH];
static f32 g_floats[BATCH];
static u16 g_halves[BATCH];
static u64 g_bits[(BATCH + 63U) / 64U];
static NphFrustum g_frustum;
static NphAffine g_affine;
static bool g_ready;

static void prepare(void)
{
    if (g_ready) {
        return;
    }
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 7U, 11U);
    for (u32 i = 0; i < BATCH; ++i) {
        // Each draw a statement of its own, so every compiler measures the
        // same inputs (as one call's arguments, the draws would be made in an
        // order C leaves unspecified).
        const f32 cx = nph_pcg32_next_range_f32(&rng, -100.0F, 100.0F);
        const f32 cy = nph_pcg32_next_range_f32(&rng, -100.0F, 100.0F);
        const f32 cz = nph_pcg32_next_range_f32(&rng, -100.0F, 100.0F);
        const NphVec3 c = nph_vec3(cx, cy, cz);
        const f32 ex = nph_pcg32_next_range_f32(&rng, 0.1F, 3.0F);
        const f32 ey = nph_pcg32_next_range_f32(&rng, 0.1F, 3.0F);
        const f32 ez = nph_pcg32_next_range_f32(&rng, 0.1F, 3.0F);
        const NphVec3 e = nph_vec3(ex, ey, ez);
        g_boxes[i] = nph_aabb_from_center_extents(c, e);
        g_spheres[i] = nph_sphere(c, e.x);
        g_points[i] = c;
        g_floats[i] = nph_pcg32_next_range_f32(&rng, -70000.0F, 70000.0F);
    }
    const NphMat4 view =
        nph_mat4_look_at(nph_vec3(0.0F, 20.0F, 120.0F), nph_vec3_zero(), nph_vec3_unit_y());
    const NphMat4 proj = nph_mat4_perspective_infinite(1.2F, 16.0F / 9.0F, 0.1F);
    const NphMat4 clip = nph_mat4_mul(proj, view);
    g_frustum = nph_frustum_from_matrix(&clip);
    g_affine = nph_affine_from_trs(
        nph_vec3(1.0F, 2.0F, 3.0F),
        nph_quat_from_axis_angle(nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F)), 0.7F),
        nph_vec3(1.5F, 0.5F, 2.0F));
    g_ready = true;
}

// Picks the table for a requested level, falling back to the best available
// and labelling the fallback.
static const NphMathKernels *kernels_for(NbenchState *state, NphSimdLevel wanted)
{
    const NphSimdLevel have = nph_simd_detect_level();
    if (wanted > have) {
        nbench_set_label(state,
                         wanted == NPH_SIMD_LEVEL_AVX2 ? "avx2 unavailable: ran sse4" : "fallback");
        return nph_math_kernels(have);
    }
    nbench_set_label(state, nph_simd_level_name(wanted));
    return nph_math_kernels(wanted);
}

// ---------------------------------------------------------------------------
// Register layer vs scalar
// ---------------------------------------------------------------------------

NBENCH(mat4_mul_scalar)
{
    const NphMat4 a = nph_mat4_from_trs(nph_vec3(1.0F, 2.0F, 3.0F), nph_quat_from_rotation_y(0.3F),
                                        nph_vec3_one());
    NphMat4 acc = nph_mat4_identity();
    while (nbench_keep_running(state)) {
        acc = nph_mat4_mul(acc, a);
        nbench_do_not_optimize(&acc);
    }
}

NBENCH(mat4_mul_simd)
{
    const NphMat4 a = nph_mat4_from_trs(nph_vec3(1.0F, 2.0F, 3.0F), nph_quat_from_rotation_y(0.3F),
                                        nph_vec3_one());
    const NphSimdMat4 sa = nph_simd_mat4_load(&a);
    NphSimdMat4 acc = nph_simd_mat4_identity();
    while (nbench_keep_running(state)) {
        acc = nph_simd_mat4_mul(acc, sa);
        nbench_do_not_optimize(&acc);
    }
}

NBENCH(mat4_inverse_scalar)
{
    NphMat4 m = nph_mat4_from_trs(nph_vec3(1.0F, 2.0F, 3.0F), nph_quat_from_rotation_y(0.3F),
                                  nph_vec3(1.0F, 2.0F, 0.5F));
    NphMat4 inv;
    while (nbench_keep_running(state)) {
        (void)nph_mat4_inverse(m, &inv);
        nbench_do_not_optimize(&inv);
        m.c[3].x += 1e-3F;
    }
}

NBENCH(mat4_inverse_simd)
{
    NphMat4 m = nph_mat4_from_trs(nph_vec3(1.0F, 2.0F, 3.0F), nph_quat_from_rotation_y(0.3F),
                                  nph_vec3(1.0F, 2.0F, 0.5F));
    NphSimdMat4 inv;
    while (nbench_keep_running(state)) {
        (void)nph_simd_mat4_inverse(nph_simd_mat4_load(&m), &inv);
        nbench_do_not_optimize(&inv);
        m.c[3].x += 1e-3F;
    }
}

NBENCH(quat_mul_scalar)
{
    const NphQuat a = nph_quat_from_rotation_y(0.01F);
    NphQuat acc = nph_quat_identity();
    while (nbench_keep_running(state)) {
        acc = nph_quat_mul(acc, a);
        nbench_do_not_optimize(&acc);
    }
}

NBENCH(quat_mul_simd)
{
    const NphF32x4 a = nph_f32x4_load_quat(nph_quat_from_rotation_y(0.01F));
    NphF32x4 acc = nph_f32x4_quat_identity();
    while (nbench_keep_running(state)) {
        acc = nph_f32x4_quat_mul(acc, a);
        nbench_do_not_optimize(&acc);
    }
}

NBENCH(affine_point_scalar)
{
    prepare();
    NphVec3 p = nph_vec3(1.0F, 2.0F, 3.0F);
    while (nbench_keep_running(state)) {
        p = nph_affine_transform_point(g_affine, p);
        nbench_do_not_optimize(&p);
    }
}

NBENCH(affine_point_simd)
{
    prepare();
    const NphSimdAffine a = nph_simd_affine_load(&g_affine);
    NphF32x4 p = nph_f32x4_load_point(nph_vec3(1.0F, 2.0F, 3.0F));
    while (nbench_keep_running(state)) {
        p = nph_simd_affine_transform_point(a, p);
        nbench_do_not_optimize(&p);
    }
}

// ---------------------------------------------------------------------------
// Batch kernels per level
// ---------------------------------------------------------------------------

#define DEFINE_KERNEL_BENCHES(suffix, level)                                                       \
    NBENCH(transform_points_##suffix)                                                              \
    {                                                                                              \
        prepare();                                                                                 \
        const NphMathKernels *k = kernels_for(state, level);                                       \
        nbench_set_items_per_iteration(state, BATCH);                                              \
        while (nbench_keep_running(state)) {                                                       \
            k->transform_points(&g_affine, g_points, g_out_points, BATCH);                         \
            nbench_do_not_optimize(g_out_points);                                                  \
        }                                                                                          \
    }                                                                                              \
    NBENCH(cull_aabbs_##suffix)                                                                    \
    {                                                                                              \
        prepare();                                                                                 \
        const NphMathKernels *k = kernels_for(state, level);                                       \
        nbench_set_items_per_iteration(state, BATCH);                                              \
        while (nbench_keep_running(state)) {                                                       \
            k->frustum_cull_aabbs(&g_frustum, g_boxes, BATCH, g_bits);                             \
            nbench_do_not_optimize(g_bits);                                                        \
        }                                                                                          \
    }                                                                                              \
    NBENCH(cull_spheres_##suffix)                                                                  \
    {                                                                                              \
        prepare();                                                                                 \
        const NphMathKernels *k = kernels_for(state, level);                                       \
        nbench_set_items_per_iteration(state, BATCH);                                              \
        while (nbench_keep_running(state)) {                                                       \
            k->frustum_cull_spheres(&g_frustum, g_spheres, BATCH, g_bits);                         \
            nbench_do_not_optimize(g_bits);                                                        \
        }                                                                                          \
    }                                                                                              \
    NBENCH(f32_to_f16_##suffix)                                                                    \
    {                                                                                              \
        prepare();                                                                                 \
        const NphMathKernels *k = kernels_for(state, level);                                       \
        nbench_set_items_per_iteration(state, BATCH);                                              \
        while (nbench_keep_running(state)) {                                                       \
            k->f32_to_f16(g_floats, g_halves, BATCH);                                              \
            nbench_do_not_optimize(g_halves);                                                      \
        }                                                                                          \
    }

DEFINE_KERNEL_BENCHES(scalar, NPH_SIMD_LEVEL_SCALAR)
DEFINE_KERNEL_BENCHES(sse4, NPH_SIMD_LEVEL_SSE4)
DEFINE_KERNEL_BENCHES(avx2, NPH_SIMD_LEVEL_AVX2)

// ---------------------------------------------------------------------------
// Random and packing
// ---------------------------------------------------------------------------

NBENCH(pcg32_next_u32)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 1U, 2U);
    u32 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_pcg32_next_u32(&rng);
    }
    nbench_do_not_optimize_u64(acc);
}

NBENCH(pcg32_bounded)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 1U, 2U);
    u32 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_pcg32_next_bounded(&rng, 1000U);
    }
    nbench_do_not_optimize_u64(acc);
}

NBENCH(halton_2d)
{
    u32 i = 0U;
    f32 acc = 0.0F;
    while (nbench_keep_running(state)) {
        const NphVec2 p = nph_halton_2d(i++);
        acc += p.x + p.y;
    }
    nbench_do_not_optimize(&acc);
}

NBENCH(r2)
{
    u32 i = 0U;
    f32 acc = 0.0F;
    while (nbench_keep_running(state)) {
        const NphVec2 p = nph_r2(i++);
        acc += p.x + p.y;
    }
    nbench_do_not_optimize(&acc);
}

NBENCH(oct_encode_snorm16)
{
    prepare();
    u32 i = 0U;
    u32 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_oct_encode_snorm16(nph_vec3_normalize(g_points[i & (BATCH - 1U)]));
        i += 1U;
    }
    nbench_do_not_optimize_u64(acc);
}

NBENCH(blue_noise_32x32)
{
    static u32 ranks[32U * 32U];
    u64 seed = 0U;
    while (nbench_keep_running(state)) {
        if (nph_blue_noise_generate(nph_allocator_system(), 32U, 32U, 1.5F, seed++, ranks) !=
            NPH_RESULT_OK) {
            abort();
        }
        nbench_do_not_optimize(ranks);
    }
}

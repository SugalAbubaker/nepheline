// SPDX-License-Identifier: Apache-2.0
// math_demo: exercises the math module end to end (CLAUDE.md §9 "Sample app
// demonstrating the feature"). It builds a camera, extracts its frustum,
// culls a field of random boxes with every available kernel level and
// checks that they agree, shows reversed-Z depth and large-world
// camera-relative positions, and prints a blue-noise tile. The exit code is
// non-zero if any level disagrees with the scalar reference.

#include <nepheline/math/kernels.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/random.h>
#include <nepheline/math/simd.h>

#include <stdio.h>
#include <stdlib.h>

#define BOX_COUNT 4096U

static NphAABB g_boxes[BOX_COUNT];
static u64 g_bits[NPH_SIMD_LEVEL_COUNT][(BOX_COUNT + 63U) / 64U];

static u32 count_bits(const u64 *words, u32 count)
{
    u32 n = 0U;
    for (u32 i = 0; i < (count + 63U) / 64U; ++i) {
        n += nph_popcount_u64(words[i]);
    }
    return n;
}

int main(void)
{
    const NphSimdLevel level = nph_simd_detect_level();
    printf("SIMD level: %s\n", nph_simd_level_name(level));

    // Camera: reversed-Z infinite perspective, Vulkan clip space.
    const NphVec3 eye = nph_vec3(0.0F, 15.0F, 60.0F);
    const NphMat4 view = nph_mat4_look_at(eye, nph_vec3_zero(), nph_vec3_unit_y());
    const NphMat4 proj = nph_mat4_perspective_infinite(nph_f32_radians(70.0F), 16.0F / 9.0F, 0.1F);
    const NphMat4 clip = nph_mat4_mul(proj, view);
    const NphFrustum frustum = nph_frustum_from_matrix(&clip);
    printf("frustum planes: %u (far plane at infinity)\n", frustum.plane_count);
    const f32 depths[3] = {0.1F, 1.0F, 1000.0F};
    for (u32 i = 0; i < 3U; ++i) {
        const NphVec3 p = nph_vec3_mul_add(eye, nph_vec3_normalize(nph_vec3_neg(eye)), depths[i]);
        const NphVec3 ndc = nph_mat4_transform_point_perspective(clip, p);
        printf("  distance %8.1f -> depth %.6f\n", (f64)depths[i], (f64)ndc.z);
    }

    // A field of boxes, culled by each level; all must agree bit for bit.
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 2026U, 10U);
    for (u32 i = 0; i < BOX_COUNT; ++i) {
        const NphVec3 c = nph_vec3(nph_pcg32_next_range_f32(&rng, -80.0F, 80.0F),
                                   nph_pcg32_next_range_f32(&rng, -10.0F, 30.0F),
                                   nph_pcg32_next_range_f32(&rng, -80.0F, 80.0F));
        const NphVec3 e = nph_vec3(nph_pcg32_next_range_f32(&rng, 0.2F, 3.0F),
                                   nph_pcg32_next_range_f32(&rng, 0.2F, 3.0F),
                                   nph_pcg32_next_range_f32(&rng, 0.2F, 3.0F));
        g_boxes[i] = nph_aabb_from_center_extents(c, e);
    }
    int status = 0;
    for (int l = 0; l <= (int)level; ++l) {
        const NphMathKernels *k = nph_math_kernels((NphSimdLevel)l);
        k->frustum_cull_aabbs(&frustum, g_boxes, BOX_COUNT, g_bits[l]);
        const u32 visible = count_bits(g_bits[l], BOX_COUNT);
        bool same = true;
        for (u32 w = 0; w < (BOX_COUNT + 63U) / 64U; ++w) {
            same = same && g_bits[l][w] == g_bits[0][w];
        }
        printf("  %-6s: %u of %u boxes visible%s\n", k->name, visible, (u32)BOX_COUNT,
               same ? "" : "  MISMATCH");
        if (!same) {
            status = 1;
        }
    }

    // Large world: positions 100 km out keep millimetre precision relative
    // to the camera.
    const NphDVec3 world_camera = nph_dvec3(100000.0, 0.0, 100000.0);
    const NphDVec3 world_object = nph_dvec3_add_vec3(world_camera, nph_vec3(1.5F, 0.001F, -2.0F));
    const NphVec3 relative = nph_dvec3_relative(world_object, world_camera);
    printf("camera-relative position: (%.6f, %.6f, %.6f)\n", (f64)relative.x, (f64)relative.y,
           (f64)relative.z);

    // Packing: a normal through the vertex format and back.
    const NphVec3 n = nph_vec3_normalize(nph_vec3(0.3F, -0.8F, 0.5F));
    const NphVec3 back = nph_oct_decode_snorm16(nph_oct_encode_snorm16(n));
    printf("oct16 normal error: %.5f degrees; f16(3.14159) = 0x%04X\n",
           (f64)nph_f32_degrees(nph_vec3_angle(n, back)), nph_f32_to_f16(3.14159F));

    // Blue noise: a 16x16 tile of ranks as a thresholded pattern.
    u32 ranks[16U * 16U];
    if (nph_blue_noise_generate(nph_allocator_system(), 16U, 16U, 1.5F, 1U, ranks) !=
        NPH_RESULT_OK) {
        return 2;
    }
    printf("blue noise 16x16, ranks below 64:\n");
    for (u32 y = 0; y < 16U; ++y) {
        char line[17];
        for (u32 x = 0; x < 16U; ++x) {
            line[x] = ranks[y * 16U + x] < 64U ? '#' : '.';
        }
        line[16] = '\0';
        printf("  %s\n", line);
    }
    printf(status == 0 ? "ok\n" : "FAILED\n");
    return status;
}

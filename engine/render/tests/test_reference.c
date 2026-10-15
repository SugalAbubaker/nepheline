// SPDX-License-Identifier: Apache-2.0
// The CPU reference math (render/reference.h) against the properties the
// cited papers state: GGX is a normalised distribution, Smith's
// visibility is symmetric and bounded, the split-sum table never creates
// energy, the tonemap curves stay in range and keep greys grey, the cube
// texels tile the sphere, and the SH projection of a constant environment
// gives irradiance pi * L in every direction. The GPU kernels are then
// compared with these functions (test_frame.c).
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>

#include <ntest/ntest.h>

#include <math.h>

NTEST(reference, ggx_distribution_is_normalised)
{
    // The projected area of the microfacets equals the macro surface's:
    // the integral of D(h) (n.h) over the hemisphere is 1 (Walter et al.,
    // "Microfacet Models for Refraction through Rough Surfaces", EGSR 2007).
    static const f32 alphas[4] = {0.1F, 0.3F, 0.6F, 1.0F};
    for (u32 k = 0; k < 4U; ++k) {
        const u32 steps = 200000U;
        f64 sum = 0.0;
        for (u32 i = 0; i < steps; ++i) {
            const f64 theta = ((f64)i + 0.5) / (f64)steps * 0.5 * 3.14159265358979323846;
            const f64 c = cos(theta);
            sum += (f64)nph_render_ref_d_ggx((f32)c, alphas[k]) * c * sin(theta);
        }
        sum *= 2.0 * 3.14159265358979323846 * (0.5 * 3.14159265358979323846 / (f64)steps);
        NTEST_EXPECT_NEAR_F64(sum, 1.0, 2e-3);
    }
}

NTEST(reference, smith_visibility_is_symmetric_and_bounded)
{
    for (u32 i = 1U; i <= 10U; ++i) {
        for (u32 j = 1U; j <= 10U; ++j) {
            const f32 nov = (f32)i / 10.0F;
            const f32 nol = (f32)j / 10.0F;
            for (u32 k = 1U; k <= 4U; ++k) {
                const f32 a = (f32)k / 4.0F;
                const f32 v = nph_render_ref_v_smith(nov, nol, a);
                // Swapped on purpose: the visibility is symmetric in NoV and NoL.
                // NOLINTNEXTLINE(readability-suspicious-call-argument)
                NTEST_EXPECT_NEAR_F32(v, nph_render_ref_v_smith(nol, nov, a), 1e-6F);
                // G2 = 4 NoV NoL V lies in (0, 1].
                const f32 g2 = 4.0F * nov * nol * v;
                NTEST_EXPECT_TRUE(g2 > 0.0F && g2 <= 1.0F + 1e-5F);
            }
        }
    }
    // A perfectly smooth surface does not mask: G2 = 1.
    NTEST_EXPECT_NEAR_F32(4.0F * 0.5F * 0.7F * nph_render_ref_v_smith(0.5F, 0.7F, 0.0F), 1.0F,
                          1e-5F);
}

NTEST(reference, schlick_fresnel_endpoints)
{
    NTEST_EXPECT_NEAR_F32(nph_render_ref_f_schlick(1.0F, 0.04F), 0.04F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(nph_render_ref_f_schlick(0.0F, 0.04F), 1.0F, 1e-6F);
}

NTEST(reference, hammersley_points)
{
    const NphVec2 p0 = nph_render_ref_hammersley(0U, 8U);
    const NphVec2 p1 = nph_render_ref_hammersley(1U, 8U);
    const NphVec2 p3 = nph_render_ref_hammersley(3U, 8U);
    NTEST_EXPECT_NEAR_F32(p0.x, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(p0.y, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(p1.x, 0.125F, 0.0F);
    NTEST_EXPECT_NEAR_F32(p1.y, 0.5F, 1e-7F);  // radical inverse of 1 (binary 0.1)
    NTEST_EXPECT_NEAR_F32(p3.y, 0.75F, 1e-7F); // of 3 (binary 0.11)
}

NTEST(reference, ggx_samples_are_unit_and_upward)
{
    for (u32 i = 0; i < 64U; ++i) {
        const NphVec3 h =
            nph_render_ref_importance_sample_ggx(nph_render_ref_hammersley(i, 64U), 0.4F);
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(h), 1.0F, 1e-5F);
        NTEST_EXPECT_TRUE(h.z >= 0.0F);
    }
}

NTEST(reference, dfg_table_conserves_energy)
{
    for (u32 i = 0; i < 8U; ++i) {
        for (u32 j = 0; j < 8U; ++j) {
            const f32 nov = ((f32)i + 0.5F) / 8.0F;
            const f32 rough = ((f32)j + 0.5F) / 8.0F;
            const NphVec2 dfg = nph_render_ref_dfg(nov, rough, 512U);
            NTEST_EXPECT_TRUE(dfg.x >= 0.0F && dfg.y >= 0.0F);
            NTEST_EXPECT_TRUE(dfg.x + dfg.y <= 1.0F + 1e-3F);
        }
    }
    // A smooth surface seen head-on reflects everything at F0: scale 1,
    // bias 0 (the integral reduces to Schlick at VoH = 1).
    const NphVec2 smooth = nph_render_ref_dfg(0.999F, 0.02F, 1024U);
    NTEST_EXPECT_NEAR_F32(smooth.x, 1.0F, 1e-2F);
    NTEST_EXPECT_NEAR_F32(smooth.y, 0.0F, 1e-2F);
    // Single scattering loses more energy as roughness grows.
    f32 prev = 2.0F;
    for (u32 j = 1U; j <= 8U; ++j) {
        const NphVec2 dfg = nph_render_ref_dfg(0.5F, (f32)j / 8.0F, 1024U);
        NTEST_EXPECT_TRUE(dfg.x + dfg.y < prev + 1e-3F);
        prev = dfg.x + dfg.y;
    }
}

NTEST(reference, tonemap_curves_stay_in_range_and_keep_grey)
{
    static const NphRenderTonemapper ops[3] = {
        NPH_RENDER_TONEMAPPER_NONE, NPH_RENDER_TONEMAPPER_ACES, NPH_RENDER_TONEMAPPER_AGX};
    for (u32 o = 0; o < 3U; ++o) {
        f32 prev = -1.0F;
        for (u32 i = 0; i <= 64U; ++i) {
            const f32 x = nph_f32_pow(2.0F, -12.0F + (f32)i * 0.25F); // 2^-12 .. 2^4
            const NphVec3 c = nph_render_ref_tonemap(nph_vec3(x, x, x), ops[o]);
            NTEST_EXPECT_TRUE(c.x >= 0.0F && c.x <= 1.0F);
            // Grey in, grey out (both fits' matrices have unit row sums).
            NTEST_EXPECT_NEAR_F32(c.x, c.y, 2e-3F);
            NTEST_EXPECT_NEAR_F32(c.y, c.z, 2e-3F);
            NTEST_EXPECT_TRUE(c.x >= prev - 1e-6F); // monotonic
            prev = c.x;
        }
        const NphVec3 black = nph_render_ref_tonemap(nph_vec3_zero(), ops[o]);
        NTEST_EXPECT_NEAR_F32(black.x, 0.0F, 1e-3F);
    }
    // Hill's fit maps middle grey 0.18 to 0.1056 (worked by hand from the
    // rational curve: (0.18 * 0.2045786 - 0.000090537) /
    // (0.18 * 0.6100224 + 0.238081)).
    NTEST_EXPECT_NEAR_F32(
        nph_render_ref_tonemap(nph_vec3(0.18F, 0.18F, 0.18F), NPH_RENDER_TONEMAPPER_ACES).x,
        0.10559F, 5e-4F);
    // Both curves approach white for bright input.
    NTEST_EXPECT_TRUE(
        nph_render_ref_tonemap(nph_vec3(64.0F, 64.0F, 64.0F), NPH_RENDER_TONEMAPPER_ACES).x >
        0.98F);
    NTEST_EXPECT_TRUE(
        nph_render_ref_tonemap(nph_vec3(16.0F, 16.0F, 16.0F), NPH_RENDER_TONEMAPPER_AGX).x > 0.98F);
    // NONE clamps.
    const NphVec3 none =
        nph_render_ref_tonemap(nph_vec3(-1.0F, 0.25F, 3.0F), NPH_RENDER_TONEMAPPER_NONE);
    NTEST_EXPECT_NEAR_F32(none.x, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(none.y, 0.25F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(none.z, 1.0F, 0.0F);
}

NTEST(reference, srgb_transfer_round_trips)
{
    NTEST_EXPECT_NEAR_F32(nph_render_ref_linear_to_srgb(0.5F), 0.735357F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(nph_render_ref_linear_to_srgb(0.001F), 0.01292F, 1e-6F);
    for (u32 i = 0; i <= 100U; ++i) {
        const f32 x = (f32)i / 100.0F;
        NTEST_EXPECT_NEAR_F32(nph_render_ref_srgb_to_linear(nph_render_ref_linear_to_srgb(x)), x,
                              1e-5F);
    }
}

NTEST(reference, cube_faces_follow_the_vulkan_layout)
{
    static const f32 axes[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (u32 f = 0; f < 6U; ++f) {
        const NphVec3 c = nph_render_ref_cube_direction(f, 0.5F, 0.5F);
        NTEST_EXPECT_NEAR_F32(c.x, axes[f][0], 1e-6F);
        NTEST_EXPECT_NEAR_F32(c.y, axes[f][1], 1e-6F);
        NTEST_EXPECT_NEAR_F32(c.z, axes[f][2], 1e-6F);
    }
    // +X: s grows towards -Z, t grows towards -Y (Vulkan spec table 16).
    const NphVec3 right = nph_render_ref_cube_direction(0U, 1.0F, 0.5F);
    NTEST_EXPECT_TRUE(right.z < -0.7F);
    const NphVec3 down = nph_render_ref_cube_direction(0U, 0.5F, 1.0F);
    NTEST_EXPECT_TRUE(down.y < -0.7F);
    // +Y: t grows towards +Z.
    NTEST_EXPECT_TRUE(nph_render_ref_cube_direction(2U, 0.5F, 1.0F).z > 0.7F);
}

NTEST(reference, cube_texels_tile_the_sphere)
{
    static const u32 sizes[3] = {1U, 8U, 32U};
    for (u32 k = 0; k < 3U; ++k) {
        const u32 n = sizes[k];
        const f32 h = 1.0F / (f32)n;
        f64 sum = 0.0;
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                const f32 u = 2.0F * ((f32)x + 0.5F) / (f32)n - 1.0F;
                const f32 v = 2.0F * ((f32)y + 0.5F) / (f32)n - 1.0F;
                sum += (f64)nph_render_ref_cube_texel_solid_angle(u, v, h);
            }
        }
        NTEST_EXPECT_NEAR_F64(sum * 6.0, 4.0 * 3.14159265358979323846, 1e-4);
    }
}

NTEST(reference, sky_gradient)
{
    NphRenderSkyDesc sky;
    nph_render_sky_desc_default(&sky);
    const NphVec3 up = nph_render_ref_sky(&sky, nph_vec3(0.0F, 1.0F, 0.0F));
    const NphVec3 side = nph_render_ref_sky(&sky, nph_vec3(1.0F, 0.0F, 0.0F));
    const NphVec3 down = nph_render_ref_sky(&sky, nph_vec3(0.0F, -1.0F, 0.0F));
    NTEST_EXPECT_TRUE(nph_vec3_near(up, sky.zenith, 1e-2F));
    NTEST_EXPECT_TRUE(nph_vec3_near(side, sky.horizon, 1e-2F));
    NTEST_EXPECT_TRUE(nph_vec3_near(down, sky.ground, 1e-2F));
}

NTEST(reference, sh_of_a_constant_environment_is_pi_l)
{
    // A uniform environment of radiance L gives irradiance pi L facing
    // any direction; band 2 represents it exactly.
    NphRenderSkyDesc sky;
    sky.zenith = nph_vec3(1.0F, 2.0F, 3.0F);
    sky.horizon = sky.zenith;
    sky.ground = sky.zenith;
    sky.zenith_exponent = 0.5F;
    sky.ground_sharpness = 8.0F;
    NphVec3 sh[9];
    nph_render_ref_sh9_sky(&sky, 16U, sh);
    for (u32 i = 0; i < 16U; ++i) {
        const f32 a = (f32)i * 0.7F;
        const NphVec3 n = nph_vec3_normalize(nph_vec3(nph_f32_cos(a), nph_f32_sin(a * 1.3F), 0.4F));
        const NphVec3 e = nph_render_ref_sh9_irradiance(sh, n);
        NTEST_EXPECT_NEAR_F32(e.x, NPH_PI * 1.0F, 2e-3F);
        NTEST_EXPECT_NEAR_F32(e.y, NPH_PI * 2.0F, 4e-3F);
        NTEST_EXPECT_NEAR_F32(e.z, NPH_PI * 3.0F, 6e-3F);
    }
    // A sky brighter above lights upward-facing surfaces more.
    NphRenderSkyDesc grad;
    nph_render_sky_desc_default(&grad);
    nph_render_ref_sh9_sky(&grad, 16U, sh);
    const NphVec3 e_up = nph_render_ref_sh9_irradiance(sh, nph_vec3(0.0F, 1.0F, 0.0F));
    const NphVec3 e_down = nph_render_ref_sh9_irradiance(sh, nph_vec3(0.0F, -1.0F, 0.0F));
    NTEST_EXPECT_TRUE(e_up.z > e_down.z);
}

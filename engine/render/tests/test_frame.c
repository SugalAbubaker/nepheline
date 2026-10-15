// SPDX-License-Identifier: Apache-2.0
// Frames of the reference renderer (render/renderer.h nph_render_frame),
// checked against the CPU reference math (render/reference.h): each test
// sets up a scene whose correct image can be computed by hand, renders it
// on the device (lavapipe in CI, the RTX locally), reads the result back,
// and compares. The fixture's teardown asserts zero validation errors
// (including synchronisation validation) and no leaked GPU objects.
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture Frame;

NTEST_SETUP(Frame)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Frame)
{
    test_render_teardown(f);
}

// The 8-bit value the tonemap pass writes for pre-exposed colour `c` into
// a UNORM target (the shader applies the sRGB curve).
static u32 expected_byte(f32 c)
{
    const f32 e = nph_render_ref_linear_to_srgb(nph_f32_saturate(c));
    return (u32)(e * 255.0F + 0.5F);
}

static bool byte_near(u32 actual, u32 expected, u32 tol)
{
    return (actual > expected ? actual - expected : expected - actual) <= tol;
}

// A view with no image-based lighting, no tonemapping, and an exposure of
// exactly 1 / 1.2 (EV100 0), over a solid black background.
static void plain_view(NphRenderView *v)
{
    nph_render_view_default(v, test_camera());
    v->ev100 = 0.0F;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
    v->disable_ibl = 1;
}

// A 4 m plane facing the camera (+Z) through the origin.
static NphRenderMesh facing_plane(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 4.0F, 1U, 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

static NphRenderTransform facing_transform(void)
{
    // The plane faces +Y; a quarter turn about X turns +Y into +Z.
    return nph_render_transform_from_trs(nph_dvec3_zero(), nph_quat_from_rotation_x(0.5F * NPH_PI),
                                         nph_vec3_one());
}

static NphRenderInstance add_instance(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                                      NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = t;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

static NphRenderMaterial grey_material(RenderFixture *f, f32 albedo, f32 roughness)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(albedo, albedo, albedo, 1.0F);
    md.roughness = roughness;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

// ---------------------------------------------------------------------------
// Frame contract
// ---------------------------------------------------------------------------

NTEST_F(Frame, needs_an_active_frame_and_a_writable_target)
{
    REQUIRE_RENDERER(f);
    NphRenderView v;
    plain_view(&v);
    nph_gpu_rg_begin(f->graph);
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                        NPH_RESULT_INVALID_STATE);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    nph_gpu_rg_begin(f->graph);
    NphRenderView bad = v;
    bad.camera.yfov = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &bad, f->graph, f->target, NULL, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    bad = v;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    bad.tonemapper = (NphRenderTonemapper)9;
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &bad, f->graph, f->target, NULL, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, NPH_GPU_IMAGE_NULL, NULL, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    const NphGpuImage hdr_target = test_make_target(f->dev, NPH_GPU_FORMAT_R16G16B16A16_SFLOAT, 8U);
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, hdr_target, NULL, NULL),
                        NPH_RESULT_UNSUPPORTED);
    NphGpuRgImage out = NPH_GPU_RG_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, &out, NULL),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(!nph_gpu_rg_image_is_null(out));
    // Once per frame.
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                        NPH_RESULT_INVALID_STATE);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    nph_gpu_image_destroy(f->dev, hdr_target);
}

NTEST_F(Frame, first_frame_builds_the_lighting_tables_once)
{
    REQUIRE_RENDERER(f);
    NphRenderView v;
    plain_view(&v);
    NphRenderFrameStats s = test_render_frame(f, &v, f->target);
    NTEST_EXPECT_TRUE(s.environment_updated);
    s = test_render_frame(f, &v, f->target);
    NTEST_EXPECT_TRUE(!s.environment_updated);
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    s = test_render_frame(f, &v, f->target);
    NTEST_EXPECT_TRUE(s.environment_updated);
    NTEST_EXPECT_EQ_UINT(s.draws, 0U);
}

// ---------------------------------------------------------------------------
// Tonemapping
// ---------------------------------------------------------------------------

static void check_tonemap(RenderFixture *f, NphGpuImage target, NphRenderTonemapper op,
                          NphVec3 pre_exposed)
{
    NphRenderView v;
    plain_view(&v);
    v.tonemapper = op;
    // EV100 0 multiplies by 1 / 1.2: the background is pre-scaled so the
    // HDR target holds `pre_exposed`.
    v.background_color = nph_vec3_scale(pre_exposed, 1.2F);
    test_render_frame(f, &v, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, f->pixels, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U);
    const NphVec3 ldr = nph_render_ref_tonemap(pre_exposed, op);
    const u8 *p = test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 3U);
    NTEST_EXPECT_TRUE(byte_near(p[0], expected_byte(ldr.x), 1U));
    NTEST_EXPECT_TRUE(byte_near(p[1], expected_byte(ldr.y), 1U));
    NTEST_EXPECT_TRUE(byte_near(p[2], expected_byte(ldr.z), 1U));
    NTEST_EXPECT_EQ_UINT(p[3], 255U);
}

NTEST_F(Frame, tonemappers_match_the_reference)
{
    REQUIRE_RENDERER(f);
    static const NphRenderTonemapper ops[3] = {
        NPH_RENDER_TONEMAPPER_NONE, NPH_RENDER_TONEMAPPER_ACES, NPH_RENDER_TONEMAPPER_AGX};
    // Distinct channels catch a transposed matrix.
    static const f32 colors[4][3] = {
        {0.05F, 0.10F, 0.20F}, {0.18F, 0.18F, 0.18F}, {0.9F, 0.4F, 0.1F}, {6.0F, 2.5F, 0.7F}};
    for (u32 o = 0; o < 3U; ++o) {
        for (u32 c = 0; c < 4U; ++c) {
            check_tonemap(f, f->target, ops[o], nph_vec3(colors[c][0], colors[c][1], colors[c][2]));
        }
    }
}

NTEST_F(Frame, srgb_targets_are_encoded_by_the_hardware)
{
    REQUIRE_RENDERER(f);
    const NphGpuImage srgb =
        test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_SRGB, TEST_TARGET_SIZE);
    check_tonemap(f, srgb, NPH_RENDER_TONEMAPPER_ACES, nph_vec3(0.3F, 0.6F, 1.2F));
    check_tonemap(f, srgb, NPH_RENDER_TONEMAPPER_NONE, nph_vec3(0.02F, 0.5F, 0.9F));
    nph_gpu_image_destroy(f->dev, srgb);
}

// ---------------------------------------------------------------------------
// Image-based lighting tables
// ---------------------------------------------------------------------------

NTEST_F(Frame, brdf_lut_matches_the_reference_integral)
{
    REQUIRE_RENDERER(f);
    NphRenderView v;
    plain_view(&v);
    test_render_frame(f, &v, f->target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u32 n = 32U; // test_renderer_desc
    u16 texels[32U * 32U * 4U];
    test_read_image(f->dev, f->renderer->brdf_lut, 0U, texels, sizeof texels);
    for (u32 y = 0; y < n; y += 3U) {
        for (u32 x = 0; x < n; x += 3U) {
            const f32 nov = ((f32)x + 0.5F) / (f32)n;
            const f32 rough = ((f32)y + 0.5F) / (f32)n;
            const NphVec2 ref = nph_render_ref_dfg(nov, rough, 1024U);
            const u16 *t = &texels[((u64)y * n + x) * 4U];
            NTEST_EXPECT_NEAR_F32(nph_f16_to_f32(t[0]), ref.x, 3e-3F);
            NTEST_EXPECT_NEAR_F32(nph_f16_to_f32(t[1]), ref.y, 3e-3F);
        }
    }
}

NTEST_F(Frame, irradiance_sh_matches_the_reference_projection)
{
    REQUIRE_RENDERER(f);
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    NphRenderView v;
    plain_view(&v);
    test_render_frame(f, &v, f->target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NphVec4 gpu[9];
    test_read_buffer(f->dev, f->renderer->sh_buffer, 0U, sizeof gpu, gpu);
    NphVec3 ref[9];
    nph_render_ref_sh9_sky(&ed.sky, 32U, ref);
    // The GPU projects box-filtered half-float texels; the reference point
    // samples the analytic sky. Compare the irradiance they predict.
    NphVec3 gpu_sh[9];
    for (u32 k = 0; k < 9U; ++k) {
        gpu_sh[k] = nph_vec4_xyz(gpu[k]);
    }
    for (u32 i = 0; i < 12U; ++i) {
        const f32 a = (f32)i * 0.9F;
        const NphVec3 n =
            nph_vec3_normalize(nph_vec3(nph_f32_cos(a), nph_f32_sin(a * 0.7F), nph_f32_sin(a)));
        const NphVec3 e_gpu = nph_render_ref_sh9_irradiance(gpu_sh, n);
        const NphVec3 e_ref = nph_render_ref_sh9_irradiance(ref, n);
        NTEST_EXPECT_NEAR_F32(e_gpu.x, e_ref.x, 0.01F * e_ref.x + 1.0F);
        NTEST_EXPECT_NEAR_F32(e_gpu.y, e_ref.y, 0.01F * e_ref.y + 1.0F);
        NTEST_EXPECT_NEAR_F32(e_gpu.z, e_ref.z, 0.01F * e_ref.z + 1.0F);
    }
}

// A uniform environment of radiance L stays L through every filter: the
// box-filtered chain, each prefiltered level (a weighted mean), and the
// SH irradiance (pi L).
static void check_constant_environment(RenderFixture *f, NphVec3 l)
{
    NphRenderView v;
    plain_view(&v);
    test_render_frame(f, &v, f->target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NphVec4 sh[9];
    test_read_buffer(f->dev, f->renderer->sh_buffer, 0U, sizeof sh, sh);
    NphVec3 coeffs[9];
    for (u32 k = 0; k < 9U; ++k) {
        coeffs[k] = nph_vec4_xyz(sh[k]);
    }
    const NphVec3 e = nph_render_ref_sh9_irradiance(coeffs, nph_vec3_normalize(nph_vec3(1, 2, 3)));
    NTEST_EXPECT_NEAR_F32(e.x, NPH_PI * l.x, 0.01F * NPH_PI * l.x);
    NTEST_EXPECT_NEAR_F32(e.y, NPH_PI * l.y, 0.01F * NPH_PI * l.y);
    NTEST_EXPECT_NEAR_F32(e.z, NPH_PI * l.z, 0.01F * NPH_PI * l.z);
    for (u32 mip = 0; mip < f->renderer->desc.specular_mips; ++mip) {
        const u32 size = f->renderer->desc.specular_size >> mip;
        const u64 bytes = (u64)size * size * 6U * 8U;
        u16 *texels = NPH_ALLOC_ARRAY(nph_allocator_system(), u16, bytes / 2U);
        NTEST_ASSERT_NOT_NULL(texels);
        test_read_image(f->dev, f->renderer->specular_cube, mip, texels, bytes);
        for (u64 t = 0; t < bytes / 8U; t += 7U) {
            NTEST_EXPECT_NEAR_F32(nph_f16_to_f32(texels[t * 4U]), l.x, 0.005F * l.x);
            NTEST_EXPECT_NEAR_F32(nph_f16_to_f32(texels[t * 4U + 2U]), l.z, 0.005F * l.z);
        }
        NPH_FREE_ARRAY(nph_allocator_system(), u16, texels, bytes / 2U);
    }
}

NTEST_F(Frame, a_constant_sky_survives_every_filter)
{
    REQUIRE_RENDERER(f);
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    ed.sky.zenith = nph_vec3(2.0F, 3.0F, 4.0F);
    ed.sky.horizon = ed.sky.zenith;
    ed.sky.ground = ed.sky.zenith;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    check_constant_environment(f, ed.sky.zenith);
}

NTEST_F(Frame, equirect_and_cube_textures_seed_the_environment)
{
    REQUIRE_RENDERER(f);
    // A constant 8 x 4 equirectangular image of (0.5, 1, 1.5).
    u16 equirect[8U * 4U * 4U];
    for (u64 i = 0; i < 32U; ++i) {
        equirect[i * 4U] = nph_f32_to_f16(0.5F);
        equirect[i * 4U + 1U] = nph_f32_to_f16(1.0F);
        equirect[i * 4U + 2U] = nph_f32_to_f16(1.5F);
        equirect[i * 4U + 3U] = nph_f32_to_f16(1.0F);
    }
    const void *mips[1] = {equirect};
    const u64 sizes[1] = {sizeof equirect};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    td.width = 8U;
    td.height = 4U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture tex = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_EQUIRECT;
    ed.texture = tex;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    check_constant_environment(f, nph_vec3(0.5F, 1.0F, 1.5F));

    // The same as a cube.
    u16 faces[6U * 2U * 2U * 4U];
    for (u64 i = 0; i < 24U; ++i) {
        faces[i * 4U] = nph_f32_to_f16(0.25F);
        faces[i * 4U + 1U] = nph_f32_to_f16(0.75F);
        faces[i * 4U + 2U] = nph_f32_to_f16(2.0F);
        faces[i * 4U + 3U] = nph_f32_to_f16(1.0F);
    }
    const void *cube_mips[1] = {faces};
    const u64 cube_sizes[1] = {sizeof faces};
    td.kind = NPH_RENDER_TEXTURE_CUBE;
    td.width = 2U;
    td.height = 2U;
    td.mips = cube_mips;
    td.mip_sizes = cube_sizes;
    NphRenderTexture cube = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &cube), NPH_RESULT_OK);
    ed.source = NPH_RENDER_ENV_CUBE;
    ed.texture = cube;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    check_constant_environment(f, nph_vec3(0.25F, 0.75F, 2.0F));
    nph_render_texture_destroy(f->renderer, tex);
    nph_render_texture_destroy(f->renderer, cube);
}

// ---------------------------------------------------------------------------
// Punctual lights and materials
// ---------------------------------------------------------------------------

// The pre-exposed radiance of a plane seen and lit head-on (N = V = L) by
// illuminance `e`, for the forward shader's BRDF.
static f32 head_on_radiance(f32 albedo, f32 roughness, f32 e, u32 lut_size)
{
    const f32 a = roughness * roughness;
    // The DFG table is sampled at NoV = 1, roughness: the nearest texel
    // centres under clamping.
    const f32 c = ((f32)lut_size - 0.5F) / (f32)lut_size;
    const NphVec2 dfg =
        nph_render_ref_dfg(c, nph_f32_min(roughness, c), 1024U); // clamped to the last centre
    const f32 ess = dfg.x + dfg.y;
    const f32 f0 = 0.04F;
    const f32 energy = 1.0F + f0 * (1.0F / ess - 1.0F);
    const f32 spec = nph_render_ref_d_ggx(1.0F, a) * nph_render_ref_v_smith(1.0F, 1.0F, a) *
                     nph_render_ref_f_schlick(1.0F, f0) * energy;
    const f32 diffuse = albedo / NPH_PI;
    return (diffuse + spec) * e / 1.2F; // EV100 0
}

NTEST_F(Frame, a_directional_light_matches_the_brdf)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderMaterial mat = grey_material(f, 0.5F, 0.6F);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 0.5F, 0.25F);
    ld.intensity = 2.0F;
    ld.direction = nph_vec3(0.0F, 0.0F, -1.0F); // travels away from the camera, onto the plane
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    NphRenderView v;
    plain_view(&v);
    const NphRenderFrameStats s = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(s.draws, 1U);
    NTEST_EXPECT_EQ_UINT(s.lights, 1U);
    NTEST_EXPECT_EQ_UINT(s.triangles, 2U);
    const f32 l = head_on_radiance(0.5F, 0.6F, 2.0F, 32U);
    const u8 *p = test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U);
    NTEST_EXPECT_TRUE(byte_near(p[0], expected_byte(l * 1.0F), 2U));
    NTEST_EXPECT_TRUE(byte_near(p[1], expected_byte(l * 0.5F), 2U));
    NTEST_EXPECT_TRUE(byte_near(p[2], expected_byte(l * 0.25F), 2U));
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, point_lights_fall_off_with_the_inverse_square)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderMaterial mat = grey_material(f, 0.8F, 1.0F);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3_one();
    ld.intensity = 8.0F;                    // candela
    ld.position = nph_dvec3(0.0, 0.0, 2.0); // 2 m in front of the plane's centre
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    NphRenderView v;
    plain_view(&v);
    test_render_read(f, &v);
    // At the centre the light is head-on: E = I / d^2 = 2 lux. The camera
    // is on the same axis, so N = V = L there too.
    const f32 l = head_on_radiance(0.8F, 1.0F, 8.0F / 4.0F, 32U);
    const u8 *p = test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U);
    NTEST_EXPECT_TRUE(byte_near(p[0], expected_byte(l), 2U));
    // The corners are farther and lit at a grazing angle: darker.
    const u8 *corner = test_pixel(f, 2U, 2U);
    NTEST_EXPECT_TRUE(corner[0] + 10U < p[0]);

    // A range just past the plane's centre windows it almost to zero.
    ld.range = 2.05F;
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, light, &ld), NPH_RESULT_OK);
    test_render_read(f, &v);
    NTEST_EXPECT_TRUE(test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U)[0] < 40U);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, 2U, 2U)[0], 0U); // beyond the range
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, spot_lights_light_only_their_cone)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderMaterial mat = grey_material(f, 0.8F, 1.0F);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_SPOT;
    ld.color = nph_vec3_one();
    ld.intensity = 8.0F;
    ld.position = nph_dvec3(0.0, 0.0, 2.0);
    ld.direction = nph_vec3(0.0F, 0.0F, -1.0F);
    ld.inner_cone = 0.1F;
    ld.outer_cone = 0.2F; // about 0.4 m of radius on the plane
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    NphRenderView v;
    plain_view(&v);
    test_render_read(f, &v);
    // Inside the inner cone the spot equals the point light.
    const f32 l = head_on_radiance(0.8F, 1.0F, 2.0F, 32U);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U)[0],
                                expected_byte(l), 2U));
    NTEST_EXPECT_EQ_UINT(test_pixel(f, 4U, TEST_TARGET_SIZE / 2U)[0], 0U);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, image_based_lighting_of_a_constant_sky)
{
    REQUIRE_RENDERER(f);
    // A white, fully rough, non-metallic plane under a uniform sky of
    // radiance L reflects (Fss Ess + Fms Ems + kD) L: with the multiple-
    // scattering compensation of Fdez-Agüera 2019 a white material
    // conserves energy, so the plane is as bright as the sky (within the
    // single-sample tables' error).
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderMaterial mat = grey_material(f, 1.0F, 1.0F);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    ed.sky.zenith = nph_vec3(0.48F, 0.48F, 0.48F);
    ed.sky.horizon = ed.sky.zenith;
    ed.sky.ground = ed.sky.zenith;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    NphRenderView v;
    plain_view(&v);
    v.disable_ibl = 0;
    test_render_read(f, &v);
    const u8 *p = test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U);
    NTEST_EXPECT_TRUE(byte_near(p[0], expected_byte(0.48F / 1.2F), 3U));
    NTEST_EXPECT_TRUE(byte_near(p[2], expected_byte(0.48F / 1.2F), 3U));
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, textures_reach_the_material)
{
    REQUIRE_RENDERER(f);
    // A 2 x 2 sRGB texture with four colours; the base-colour debug view
    // shows them unlit, so each quadrant of the plane reads back exactly.
    static const u8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    const void *mips[1] = {texels};
    const u64 sizes[1] = {sizeof texels};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.width = 2U;
    td.height = 2U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture tex = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = tex;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].nearest = 1;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].wrap_u = NPH_RENDER_WRAP_CLAMP;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].wrap_v = NPH_RENDER_WRAP_CLAMP;
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderView v;
    plain_view(&v);
    v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
    test_render_read(f, &v);
    // UV (0, 0) is the image's top-left; the plane's v follows -Y on
    // screen after the quarter turn, so the image appears upright.
    const u8 *tl = test_pixel(f, TEST_TARGET_SIZE / 4U, TEST_TARGET_SIZE / 4U);
    const u8 *tr = test_pixel(f, 3U * TEST_TARGET_SIZE / 4U, TEST_TARGET_SIZE / 4U);
    const u8 *bl = test_pixel(f, TEST_TARGET_SIZE / 4U, 3U * TEST_TARGET_SIZE / 4U);
    const u8 *br = test_pixel(f, 3U * TEST_TARGET_SIZE / 4U, 3U * TEST_TARGET_SIZE / 4U);
    NTEST_EXPECT_TRUE(tl[0] == 255U && tl[1] == 0U && tl[2] == 0U);
    NTEST_EXPECT_TRUE(tr[0] == 0U && tr[1] == 255U && tr[2] == 0U);
    NTEST_EXPECT_TRUE(bl[0] == 0U && bl[1] == 0U && bl[2] == 255U);
    NTEST_EXPECT_TRUE(br[0] == 255U && br[1] == 255U && br[2] == 0U);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_mesh_destroy(f->renderer, mesh);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_texture_destroy(f->renderer, tex);
}

NTEST_F(Frame, debug_views_show_material_channels)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.roughness = 0.5F;
    md.metallic = 0.25F;
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderView v;
    plain_view(&v);
    const u32 c = TEST_TARGET_SIZE / 2U;
    v.debug_view = NPH_RENDER_VIEW_ROUGHNESS;
    test_render_read(f, &v);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[0], expected_byte(0.5F), 1U));
    v.debug_view = NPH_RENDER_VIEW_METALLIC;
    test_render_read(f, &v);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[0], expected_byte(0.25F), 1U));
    v.debug_view = NPH_RENDER_VIEW_NORMAL; // +Z: (0.5, 0.5, 1)
    test_render_read(f, &v);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[0], expected_byte(0.5F), 2U));
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[2], 255U, 1U));
    v.debug_view = NPH_RENDER_VIEW_OCCLUSION;
    test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, c, c)[0], 255U);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

// ---------------------------------------------------------------------------
// Geometry: sidedness, mirroring, alpha, culling
// ---------------------------------------------------------------------------

static NphRenderLight head_on_light(RenderFixture *f)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 2.0F;
    ld.direction = nph_vec3(0.0F, 0.0F, -1.0F);
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    return light;
}

NTEST_F(Frame, back_faces_are_culled_unless_double_sided)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    // Turned the other way: the camera sees the plane's back.
    const NphRenderTransform away = nph_render_transform_from_trs(
        nph_dvec3_zero(), nph_quat_from_rotation_x(-0.5F * NPH_PI), nph_vec3_one());
    const NphRenderInstance inst = add_instance(f, mesh, mat, away);
    const NphRenderLight light = head_on_light(f);
    NphRenderView v;
    plain_view(&v);
    const u32 c = TEST_TARGET_SIZE / 2U;
    test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, c, c)[0], 0U);
    // Double-sided: the back face is drawn with its normal flipped towards
    // the viewer, so the head-on light reaches it.
    md.double_sided = 1;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, mat, &md), NPH_RESULT_OK);
    test_render_read(f, &v);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[0],
                                expected_byte(head_on_radiance(1.0F, 1.0F, 2.0F, 32U)), 2U));
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, mirrored_instances_keep_their_front_faces)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderMaterial mat = grey_material(f, 1.0F, 1.0F);
    // A negative scale on X mirrors the plane; it still faces +Z, and its
    // triangles now wind clockwise on screen.
    const NphRenderTransform mirrored = nph_render_transform_from_trs(
        nph_dvec3_zero(), nph_quat_from_rotation_x(0.5F * NPH_PI), nph_vec3(-1.0F, 1.0F, 1.0F));
    const NphRenderInstance inst = add_instance(f, mesh, mat, mirrored);
    const NphRenderLight light = head_on_light(f);
    NphRenderView v;
    plain_view(&v);
    test_render_read(f, &v);
    const u32 c = TEST_TARGET_SIZE / 2U;
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[0],
                                expected_byte(head_on_radiance(1.0F, 1.0F, 2.0F, 32U)), 2U));
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, alpha_modes)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.unlit = 1; // the colour as authored: easy to predict
    md.base_color = nph_vec4(1.0F, 0.0F, 0.0F, 0.25F);
    md.alpha_mode = NPH_RENDER_ALPHA_MASK;
    md.alpha_cutoff = 0.5F;
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderView v;
    plain_view(&v);
    v.background_color = nph_vec3(0.0F, 0.0F, 0.6F); // pre-exposed 0.5 blue
    const u32 c = TEST_TARGET_SIZE / 2U;
    // Masked below the cutoff: the background shows.
    test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, c, c)[0], 0U);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[2], expected_byte(0.5F), 1U));
    // Above the cutoff: opaque red.
    md.base_color.w = 0.75F;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, mat, &md), NPH_RESULT_OK);
    test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, c, c)[0], 255U);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, c, c)[2], 0U);
    // Blended: alpha over the background.
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    md.base_color.w = 0.25F;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, mat, &md), NPH_RESULT_OK);
    const NphRenderFrameStats s = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[0], expected_byte(0.25F), 1U));
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, c, c)[2], expected_byte(0.75F * 0.5F), 1U));
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, blended_draws_sort_back_to_front)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.unlit = 1;
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    md.base_color = nph_vec4(1.0F, 0.0F, 0.0F, 0.5F);
    NphRenderMaterial red = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &red), NPH_RESULT_OK);
    md.base_color = nph_vec4(0.0F, 1.0F, 0.0F, 0.5F);
    NphRenderMaterial green = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &green), NPH_RESULT_OK);
    // Green is nearer the camera; created first so that only the sort puts
    // it last.
    NphRenderTransform near_t = facing_transform();
    near_t.translation.z = 1.0;
    const NphRenderInstance a = add_instance(f, mesh, green, near_t);
    const NphRenderInstance b = add_instance(f, mesh, red, facing_transform());
    NphRenderView v;
    plain_view(&v);
    test_render_read(f, &v);
    // Over black: red at 0.5, then green at 0.5 over it.
    const u8 *p = test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U);
    NTEST_EXPECT_TRUE(byte_near(p[0], expected_byte(0.25F), 1U));
    NTEST_EXPECT_TRUE(byte_near(p[1], expected_byte(0.5F), 1U));
    nph_render_instance_destroy(f->renderer, a);
    nph_render_instance_destroy(f->renderer, b);
    nph_render_material_destroy(f->renderer, red);
    nph_render_material_destroy(f->renderer, green);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, culling_and_hidden_instances)
{
    REQUIRE_RENDERER(f);
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    const NphRenderMaterial mat = grey_material(f, 0.5F, 0.5F);
    const NphRenderInstance visible = add_instance(f, mesh, mat, nph_render_transform_identity());
    NphRenderTransform behind = nph_render_transform_identity();
    behind.translation = nph_dvec3(0.0, 0.0, 10.0); // behind the camera at z = 4
    const NphRenderInstance culled = add_instance(f, mesh, mat, behind);
    const NphRenderInstance hidden = add_instance(f, mesh, mat, nph_render_transform_identity());
    nph_render_instance_set_hidden(f->renderer, hidden, true);
    NphRenderView v;
    plain_view(&v);
    const NphRenderFrameStats s = test_render_frame(f, &v, f->target);
    NTEST_EXPECT_EQ_UINT(s.instances, 3U);
    NTEST_EXPECT_EQ_UINT(s.culled, 2U);
    NTEST_EXPECT_EQ_UINT(s.draws, 1U);
    NTEST_EXPECT_EQ_UINT(s.triangles, 12U);
    nph_render_instance_destroy(f->renderer, visible);
    nph_render_instance_destroy(f->renderer, culled);
    nph_render_instance_destroy(f->renderer, hidden);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, positions_are_camera_relative_far_from_the_origin)
{
    REQUIRE_RENDERER(f);
    // The scene of a_directional_light_matches_the_brdf moved 10,000 km
    // away: in 32-bit world coordinates the plane and the camera would
    // collapse to the same few representable points; camera-relative they
    // render the same image.
    const NphRenderMesh mesh = facing_plane(f);
    const NphRenderMaterial mat = grey_material(f, 0.5F, 0.6F);
    const NphDVec3 far_away = nph_dvec3(1.0e7, -3.0e6, 5.0e6);
    NphRenderTransform t = facing_transform();
    t.translation = far_away;
    const NphRenderInstance inst = add_instance(f, mesh, mat, t);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 2.0F;
    ld.direction = nph_vec3(0.0F, 0.0F, -1.0F);
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    NphRenderView v;
    plain_view(&v);
    v.camera.position = nph_dvec3_add(far_away, nph_dvec3(0.0, 0.0, 4.0));
    test_render_read(f, &v);
    const f32 l = head_on_radiance(0.5F, 0.6F, 2.0F, 32U);
    NTEST_EXPECT_TRUE(byte_near(test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U)[0],
                                expected_byte(l), 2U));
    // The plane still fills the view to its corners (lit, if a little
    // darker where the view is off-axis and the highlight weaker).
    const u8 corner = test_pixel(f, 1U, 1U)[0];
    NTEST_EXPECT_TRUE(corner > 60U && corner <= expected_byte(l) + 2U);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

// ---------------------------------------------------------------------------
// Debug lines
// ---------------------------------------------------------------------------

NTEST_F(Frame, debug_lines_draw_over_the_image)
{
    REQUIRE_RENDERER(f);
    NphRenderView v;
    plain_view(&v);
    // An overlay line across the middle row, in the plane z = 0 (the
    // camera at z = 4 sees about +-1.2 m there).
    NTEST_EXPECT_TRUE(
        nph_render_debug_line(f->renderer, nph_vec3(-5.0F, 0.0F, 0.0F), nph_vec3(5.0F, 0.0F, 0.0F),
                              NPH_RENDER_RGBA(0, 255, 0, 255), NPH_RENDER_DEBUG_OVERLAY));
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(f->renderer), 1U);
    const NphRenderFrameStats s = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(s.debug_lines, 1U);
    u32 green = 0U;
    for (u32 y = TEST_TARGET_SIZE / 2U - 1U; y <= TEST_TARGET_SIZE / 2U; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            const u8 *p = test_pixel(f, x, y);
            green += p[1] == 255U && p[0] == 0U ? 1U : 0U;
        }
    }
    NTEST_EXPECT_TRUE(green >= TEST_TARGET_SIZE - 2U);
    // The lines last one frame.
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(f->renderer), 0U);
    test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, TEST_TARGET_SIZE / 2U, TEST_TARGET_SIZE / 2U)[1], 0U);
}

NTEST_F(Frame, depth_tested_lines_hide_behind_geometry)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = facing_plane(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.unlit = 1;
    md.base_color = nph_vec4(0.0F, 0.0F, 0.0F, 1.0F);
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    const NphRenderInstance inst = add_instance(f, mesh, mat, facing_transform());
    NphRenderView v;
    plain_view(&v);
    // A line behind the plane (z = -1) and one in front (z = 1).
    nph_render_debug_line(f->renderer, nph_vec3(-5.0F, 0.0F, -1.0F), nph_vec3(5.0F, 0.0F, -1.0F),
                          NPH_RENDER_COLOR_WHITE, NPH_RENDER_DEBUG_DEPTH_TESTED);
    nph_render_debug_line(f->renderer, nph_vec3(-5.0F, 0.6F, 1.0F), nph_vec3(5.0F, 0.6F, 1.0F),
                          NPH_RENDER_COLOR_WHITE, NPH_RENDER_DEBUG_DEPTH_TESTED);
    test_render_read(f, &v);
    u32 lit_middle = 0U;
    u32 lit_upper = 0U;
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 8U; x < TEST_TARGET_SIZE - 8U; ++x) {
            if (test_pixel(f, x, y)[0] == 255U) {
                if (y >= TEST_TARGET_SIZE / 2U - 2U && y <= TEST_TARGET_SIZE / 2U + 1U) {
                    ++lit_middle;
                } else {
                    ++lit_upper;
                }
            }
        }
    }
    NTEST_EXPECT_EQ_UINT(lit_middle, 0U);
    NTEST_EXPECT_TRUE(lit_upper >= TEST_TARGET_SIZE - 18U);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
}

NTEST_F(Frame, debug_shapes_and_capacity)
{
    REQUIRE_RENDERER(f);
    NphRenderer *r = f->renderer;
    nph_render_debug_set_origin(r, nph_dvec3(100.0, 0.0, 0.0));
    NTEST_EXPECT_NEAR_F64(nph_render_debug_origin(r).x, 100.0, 0.0);
    const NphAffine id = nph_affine_identity();
    nph_render_debug_aabb(r, nph_aabb(nph_vec3_splat(-1.0F), nph_vec3_one()), NPH_RENDER_COLOR_RED,
                          0U);
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(r), 12U);
    nph_render_debug_box(r, &id, nph_aabb(nph_vec3_splat(-1.0F), nph_vec3_one()),
                         NPH_RENDER_COLOR_RED, 0U);
    nph_render_debug_axes(r, &id, 1.0F, NPH_RENDER_DEBUG_OVERLAY);
    nph_render_debug_arrow(r, nph_vec3_zero(), nph_vec3_unit_x(), NPH_RENDER_COLOR_CYAN, 0U);
    nph_render_debug_cross(r, nph_vec3_zero(), 1.0F, NPH_RENDER_COLOR_YELLOW, 0U);
    // 32 depth-tested lines and 3 overlay lines so far.
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(r), 12U + 12U + 3U + 5U + 3U);
    nph_render_debug_sphere(r, nph_vec3_zero(), 1.0F, NPH_RENDER_COLOR_GRAY, 0U, 8U); // 24
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(r), 35U + 24U);
    nph_render_debug_grid(r, nph_vec3_zero(), 1.0F, 2U, NPH_RENDER_COLOR_GRAY, 0U); // 6
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(r), 35U + 24U + 6U);
    // The depth-tested list holds 64 lines (test_renderer_desc): the
    // frustum's twelve edges fill its last two slots and the rest are
    // dropped, as is everything after.
    const NphRenderCamera cam = test_camera();
    nph_render_debug_frustum(r, &cam, 1.0F, 3.0F, NPH_RENDER_COLOR_MAGENTA, 0U);
    NTEST_EXPECT_EQ_UINT(nph_render_debug_line_count(r), 64U + 3U);
    NTEST_EXPECT_TRUE(
        !nph_render_debug_line(r, nph_vec3_zero(), nph_vec3_one(), NPH_RENDER_COLOR_WHITE, 0U));
    // The overlay list is separate and still has room.
    NTEST_EXPECT_TRUE(nph_render_debug_line(r, nph_vec3_zero(), nph_vec3_one(),
                                            NPH_RENDER_COLOR_WHITE, NPH_RENDER_DEBUG_OVERLAY));
    NphRenderView v;
    plain_view(&v);
    const NphRenderFrameStats s = test_render_frame(f, &v, f->target);
    NTEST_EXPECT_EQ_UINT(s.debug_lines, 64U + 4U);
}

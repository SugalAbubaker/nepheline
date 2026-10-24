// SPDX-License-Identifier: Apache-2.0
// The renderer's resource API (render/renderer.h): creation and its
// argument checks, every resource kind's validation, capacities, stale
// handles, and destruction (the fixture's teardown checks that nothing
// leaked on the device and that validation stayed silent).
#include <nepheline/math/scalar.h>

#include "test_render_support.h"

typedef RenderFixture Api;

NTEST_SETUP(Api)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Api)
{
    test_render_teardown(f);
}

NTEST_F(Api, create_checks_its_description)
{
    REQUIRE_RENDERER(f);
    NphRendererDesc d;
    NphRenderer *r = NULL;
    test_renderer_desc(&d, f->dev);
    d.alloc = NULL;
    NTEST_EXPECT_EQ_INT(nph_renderer_create(&d, &r), NPH_RESULT_INVALID_ARGUMENT);
    test_renderer_desc(&d, f->dev);
    d.shader_dir = NULL;
    NTEST_EXPECT_EQ_INT(nph_renderer_create(&d, &r), NPH_RESULT_INVALID_ARGUMENT);
    test_renderer_desc(&d, f->dev);
    d.environment_size = 48U; // not a power of two
    NTEST_EXPECT_EQ_INT(nph_renderer_create(&d, &r), NPH_RESULT_INVALID_ARGUMENT);
    test_renderer_desc(&d, f->dev);
    d.specular_mips = 7U; // more levels than a 32-texel cube has
    NTEST_EXPECT_EQ_INT(nph_renderer_create(&d, &r), NPH_RESULT_INVALID_ARGUMENT);
    test_renderer_desc(&d, f->dev);
    d.shader_dir = "this/directory/does/not/exist";
    const NphResult missing = nph_renderer_create(&d, &r);
    NTEST_EXPECT_TRUE(missing == NPH_RESULT_NOT_FOUND || missing == NPH_RESULT_IO_ERROR);
    NTEST_EXPECT_NULL(r);
    nph_renderer_destroy(NULL);
    NTEST_EXPECT_EQ_PTR(nph_renderer_device(f->renderer), f->dev);
}

NTEST_F(Api, textures)
{
    REQUIRE_RENDERER(f);
    u8 level0[4U * 4U * 4U];
    u8 level1[2U * 2U * 4U];
    memset(level0, 200, sizeof level0);
    memset(level1, 100, sizeof level1);
    const void *mips[2] = {level0, level1};
    const u64 sizes[2] = {sizeof level0, sizeof level1};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.width = 4U;
    td.height = 4U;
    td.mip_count = 2U;
    td.mips = mips;
    td.mip_sizes = sizes;
    td.name = "test texture";
    NphRenderTexture t = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &t), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_render_texture_is_valid(f->renderer, t));

    NphRenderTexture bad = NPH_RENDER_TEXTURE_NULL;
    const u64 wrong[2] = {sizeof level0 - 1U, sizeof level1};
    td.mip_sizes = wrong;
    NTEST_EXPECT_EQ_INT(nph_render_texture_create(f->renderer, &td, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    td.mip_sizes = sizes;
    td.mip_count = 4U; // a 4 x 4 image has 3 levels
    NTEST_EXPECT_EQ_INT(nph_render_texture_create(f->renderer, &td, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    td.mip_count = 2U;
    td.format = NPH_GPU_FORMAT_D32_SFLOAT;
    NTEST_EXPECT_EQ_INT(nph_render_texture_create(f->renderer, &td, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.kind = NPH_RENDER_TEXTURE_CUBE; // one layer of data for six faces
    NTEST_EXPECT_EQ_INT(nph_render_texture_create(f->renderer, &td, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_TRUE(!nph_render_texture_is_valid(f->renderer, bad));

    // A cube: six faces of one level.
    u8 faces[6U * 2U * 2U * 4U];
    memset(faces, 50, sizeof faces);
    const void *cube_mips[1] = {faces};
    const u64 cube_sizes[1] = {sizeof faces};
    td.kind = NPH_RENDER_TEXTURE_CUBE;
    td.width = 2U;
    td.height = 2U;
    td.mip_count = 1U;
    td.mips = cube_mips;
    td.mip_sizes = cube_sizes;
    NphRenderTexture cube = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &cube), NPH_RESULT_OK);

    nph_render_texture_destroy(f->renderer, t);
    NTEST_EXPECT_TRUE(!nph_render_texture_is_valid(f->renderer, t));
    nph_render_texture_destroy(f->renderer, NPH_RENDER_TEXTURE_NULL);
    NPH_TEST_EXPECT_ASSERT(nph_render_texture_destroy(f->renderer, t)); // stale
    nph_render_texture_destroy(f->renderer, cube);
}

NTEST_F(Api, meshes)
{
    REQUIRE_RENDERER(f);
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, "cube", &sub, &md);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_render_mesh_is_valid(f->renderer, mesh));

    NphRenderMesh bad = NPH_RENDER_MESH_NULL;
    NphRenderMeshDesc broken = md;
    broken.index_count = 35U;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &broken, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    broken = md;
    broken.vertex_count = 7U; // indices reach vertex 23
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &broken, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphRenderSubmesh past = sub;
    past.index_offset = 30U; // 30 + 36 > 36
    broken = md;
    broken.submeshes = &past;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &broken, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    broken = md;
    broken.submesh_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &broken, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);

    // A mesh in use cannot be destroyed.
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.transform = nph_render_transform_identity();
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(nph_render_mesh_destroy(f->renderer, mesh));
    NTEST_EXPECT_TRUE(nph_render_mesh_is_valid(f->renderer, mesh));
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_mesh_destroy(f->renderer, mesh);
    NTEST_EXPECT_TRUE(!nph_render_mesh_is_valid(f->renderer, mesh));
    nph_render_mesh_destroy(f->renderer, NPH_RENDER_MESH_NULL);
    nph_render_mesh_data_free(&data);
}

NTEST_F(Api, materials)
{
    REQUIRE_RENDERER(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    NTEST_EXPECT_NEAR_F32(md.roughness, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(md.base_color.w, 1.0F, 0.0F);
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    md.roughness = 0.25F;
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, m, &md), NPH_RESULT_OK);

    NphRenderMaterial bad = NPH_RENDER_MATERIAL_NULL;
    NphRenderMaterialDesc broken = md;
    broken.textures[NPH_RENDER_SLOT_NORMAL].texture.value = 12345U; // not a texture
    NTEST_EXPECT_EQ_INT(nph_render_material_create(f->renderer, &broken, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    broken = md;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    broken.alpha_mode = (NphRenderAlphaMode)7;
    NTEST_EXPECT_EQ_INT(nph_render_material_create(f->renderer, &broken, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    broken = md;
    broken.metallic = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, m, &broken),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, bad, &md),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_material_destroy(f->renderer, m);
    NTEST_EXPECT_TRUE(!nph_render_material_is_valid(f->renderer, m));
    nph_render_material_destroy(f->renderer, NPH_RENDER_MATERIAL_NULL);
}

NTEST_F(Api, instances_and_transforms)
{
    REQUIRE_RENDERER(f);
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);

    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(1.0e7, 0.0, -3.0), nph_quat_identity(),
                                                 nph_vec3(2.0F, 2.0F, 2.0F));
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_render_instance_is_valid(f->renderer, inst));

    NphRenderTransform flat = nph_render_transform_identity();
    flat.linear = nph_mat3_from_scale(nph_vec3(1.0F, 0.0F, 1.0F)); // no volume
    NTEST_EXPECT_EQ_INT(nph_render_instance_set_transform(f->renderer, inst, &flat),
                        NPH_RESULT_INVALID_ARGUMENT);
    const NphMat4 m4 = nph_mat4_from_translation(nph_vec3(1.0F, 2.0F, 3.0F));
    const NphRenderTransform from4 = nph_render_transform_from_mat4(&m4);
    NTEST_EXPECT_NEAR_F64(from4.translation.z, 3.0, 0.0);
    NTEST_EXPECT_EQ_INT(nph_render_instance_set_transform(f->renderer, inst, &from4),
                        NPH_RESULT_OK);
    NphRenderTransform got;
    NTEST_ASSERT_EQ_INT(nph_render_instance_transform(f->renderer, inst, &got), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F64(got.translation.y, 2.0, 0.0);
    NTEST_EXPECT_NEAR_F32(got.linear.c[1].y, 1.0F, 0.0F);
    nph_render_instance_set_hidden(f->renderer, inst, true);

    NphRenderInstance bad = NPH_RENDER_INSTANCE_NULL;
    id.transform = flat;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    id.transform = nph_render_transform_identity();
    NphRenderMaterial stale = mat;
    stale.value ^= 0x80000000U;
    id.materials = &stale;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);
    id.materials = NULL; // a count without an array
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &bad),
                        NPH_RESULT_INVALID_ARGUMENT);

    nph_render_instance_destroy(f->renderer, inst);
    NTEST_EXPECT_TRUE(!nph_render_instance_is_valid(f->renderer, inst));
    NTEST_EXPECT_EQ_INT(nph_render_instance_transform(f->renderer, inst, &got),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_instance_destroy(f->renderer, NPH_RENDER_INSTANCE_NULL);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
    nph_render_mesh_data_free(&data);
}

NTEST_F(Api, lights)
{
    REQUIRE_RENDERER(f);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_SPOT;
    ld.color = nph_vec3_one();
    ld.intensity = 100.0F;
    ld.direction = nph_vec3(0.0F, -1.0F, 0.0F);
    ld.inner_cone = 0.2F;
    ld.outer_cone = 0.4F;
    NphRenderLight l = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &l), NPH_RESULT_OK);

    NphRenderLightDesc bad = ld;
    bad.inner_cone = 0.5F; // inner past outer
    NTEST_EXPECT_EQ_INT(nph_render_light_update(f->renderer, l, &bad), NPH_RESULT_INVALID_ARGUMENT);
    bad = ld;
    bad.direction = nph_vec3_zero();
    NTEST_EXPECT_EQ_INT(nph_render_light_update(f->renderer, l, &bad), NPH_RESULT_INVALID_ARGUMENT);
    bad = ld;
    bad.intensity = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_render_light_update(f->renderer, l, &bad), NPH_RESULT_INVALID_ARGUMENT);
    NphRenderLightDesc got;
    NTEST_ASSERT_EQ_INT(nph_render_light_desc(f->renderer, l, &got), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(got.intensity, 100.0F, 0.0F); // the refused updates changed nothing
    bad = ld;
    bad.type = NPH_RENDER_LIGHT_TYPE_POINT; // a point light needs no direction
    bad.direction = nph_vec3_zero();
    NTEST_EXPECT_EQ_INT(nph_render_light_update(f->renderer, l, &bad), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_light_desc(f->renderer, l, &got), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT((int)got.type, (int)NPH_RENDER_LIGHT_TYPE_POINT);

    // Capacity: the fixture allows 8 lights.
    NphRenderLight more[8];
    u32 created = 0U;
    for (u32 i = 0; i < 8U; ++i) {
        const NphResult r = nph_render_light_create(f->renderer, &ld, &more[i]);
        if (r == NPH_RESULT_FULL) {
            break;
        }
        NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
        ++created;
    }
    NTEST_EXPECT_EQ_UINT(created, 7U);
    for (u32 i = 0; i < created; ++i) {
        nph_render_light_destroy(f->renderer, more[i]);
    }
    nph_render_light_destroy(f->renderer, l);
    NTEST_EXPECT_TRUE(!nph_render_light_is_valid(f->renderer, l));
    nph_render_light_destroy(f->renderer, NPH_RENDER_LIGHT_NULL);
}

NTEST_F(Api, environment_checks_its_source)
{
    REQUIRE_RENDERER(f);
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    NTEST_EXPECT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    ed.source = NPH_RENDER_ENV_EQUIRECT; // no texture
    NTEST_EXPECT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_INVALID_ARGUMENT);

    u16 texel[4] = {0x3C00U, 0x3C00U, 0x3C00U, 0x3C00U}; // 1.0 in half floats
    const void *mips[1] = {texel};
    const u64 sizes[1] = {sizeof texel};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    td.width = 1U;
    td.height = 1U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture t = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &t), NPH_RESULT_OK);
    ed.texture = t;
    NTEST_EXPECT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    ed.source = NPH_RENDER_ENV_CUBE; // a 2D texture is not a cube
    NTEST_EXPECT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_INVALID_ARGUMENT);
    ed.source = NPH_RENDER_ENV_SKY;
    ed.intensity = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_INVALID_ARGUMENT);
    ed.source = NPH_RENDER_ENV_NONE;
    ed.intensity = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, t);
}

NTEST_F(Api, destroy_releases_what_the_caller_left)
{
    REQUIRE_RENDERER(f);
    // Leave a mesh, an instance, a material, a light, and a texture alive;
    // the renderer's destruction releases them (the teardown's leak check
    // on the device would fail otherwise).
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_sphere(nph_allocator_system(), 1.0F, 8U, 4U, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = nph_render_transform_identity();
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 1.0F;
    ld.direction = nph_vec3(0.0F, -1.0F, 0.0F);
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    nph_renderer_destroy(f->renderer);
    f->renderer = NULL;
}

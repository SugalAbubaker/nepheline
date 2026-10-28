// SPDX-License-Identifier: Apache-2.0
// The cooker's voxel materials (cook/voxel.h; ADR-0061): the images' mip
// chains and lookups, the materials' surfaces, and a foliage mesh cooked
// with voxel levels whose voxels take their colour from its texture.
#include <nepheline/asset/mesh.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/voxel.h>

#include "test_cook_support.h"

#include <cook/mesh.h>
#include <cook/voxel.h>

// An RGBA8 image of `w` x `h` texels from `texels` (4 bytes each).
static void make_image(CookImage *img, u32 w, u32 h, const u8 *texels)
{
    NTEST_ASSERT_EQ_INT(cook_image_create(nph_allocator_system(), w, h, COOK_IMAGE_RGBA8, img),
                        NPH_RESULT_OK);
    memcpy(img->pixels, texels, (size_t)w * h * 4U);
}

NTEST(CookVoxel, an_images_chain_filters_and_wraps)
{
    // Red, green / blue, white.
    const u8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    CookImage img;
    make_image(&img, 2U, 2U, texels);
    CookVoxelImage chain;
    NTEST_ASSERT_EQ_INT(cook_voxel_image_init(&chain, nph_allocator_system(), &img, false),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(chain.width, 2U);
    NTEST_EXPECT_EQ_UINT(chain.levels, 2U);
    // A texel's centre, with no footprint: the texel.
    NphVec4 c = cook_voxel_image_sample(&chain, nph_vec2(0.25F, 0.25F), 0.0F,
                                        NPH_MATERIAL_WRAP_REPEAT, NPH_MATERIAL_WRAP_REPEAT);
    NTEST_EXPECT_TRUE(nph_vec4_near(c, nph_vec4(1.0F, 0.0F, 0.0F, 1.0F), 1e-6F));
    c = cook_voxel_image_sample(&chain, nph_vec2(0.75F, 0.75F), 0.0F, NPH_MATERIAL_WRAP_REPEAT,
                                NPH_MATERIAL_WRAP_REPEAT);
    NTEST_EXPECT_TRUE(nph_vec4_near(c, nph_vec4(1.0F, 1.0F, 1.0F, 1.0F), 1e-6F));
    // A footprint of the whole image: the last level, the mean.
    c = cook_voxel_image_sample(&chain, nph_vec2(0.25F, 0.25F), 1.0F, NPH_MATERIAL_WRAP_REPEAT,
                                NPH_MATERIAL_WRAP_REPEAT);
    NTEST_EXPECT_TRUE(nph_vec4_near(c, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), 1.0F / 255.0F));
    // Past the edge: repeat wraps to red, clamp holds green, mirror folds
    // back to green.
    const NphVec2 past = nph_vec2(1.25F, 0.25F);
    c = cook_voxel_image_sample(&chain, past, 0.0F, NPH_MATERIAL_WRAP_REPEAT,
                                NPH_MATERIAL_WRAP_REPEAT);
    NTEST_EXPECT_TRUE(nph_vec4_near(c, nph_vec4(1.0F, 0.0F, 0.0F, 1.0F), 1e-6F));
    c = cook_voxel_image_sample(&chain, past, 0.0F, NPH_MATERIAL_WRAP_CLAMP,
                                NPH_MATERIAL_WRAP_CLAMP);
    NTEST_EXPECT_TRUE(nph_vec4_near(c, nph_vec4(0.0F, 1.0F, 0.0F, 1.0F), 1e-6F));
    c = cook_voxel_image_sample(&chain, past, 0.0F, NPH_MATERIAL_WRAP_MIRROR,
                                NPH_MATERIAL_WRAP_MIRROR);
    NTEST_EXPECT_TRUE(nph_vec4_near(c, nph_vec4(0.0F, 1.0F, 0.0F, 1.0F), 1e-6F));
    cook_voxel_image_free(&chain);
    cook_image_free(&img);
}

NTEST(CookVoxel, an_srgb_chain_filters_in_linear_light_and_large_images_shrink)
{
    // Black and white, sRGB-encoded: their mean in linear light is a half,
    // which sRGB encodes as 188 (a mean of the bytes would be 128, 0.22).
    const u8 texels[8] = {0, 0, 0, 255, 255, 255, 255, 255};
    CookImage img;
    make_image(&img, 2U, 1U, texels);
    CookVoxelImage chain;
    NTEST_ASSERT_EQ_INT(cook_voxel_image_init(&chain, nph_allocator_system(), &img, true),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(chain.texels[chain.offset[1]], 188U);
    const NphVec4 c = cook_voxel_image_sample(&chain, nph_vec2(0.5F, 0.5F), 1.0F,
                                              NPH_MATERIAL_WRAP_REPEAT, NPH_MATERIAL_WRAP_REPEAT);
    NTEST_EXPECT_NEAR_F32(c.x, 0.5F, 0.004F);
    cook_voxel_image_free(&chain);
    cook_image_free(&img);

    // 3000 x 1000: halved twice, to 750 x 250, each texel the mean of 16.
    NTEST_ASSERT_EQ_INT(
        cook_image_create(nph_allocator_system(), 3000U, 1000U, COOK_IMAGE_RGBA8, &img),
        NPH_RESULT_OK);
    u8 *p = (u8 *)img.pixels;
    for (u64 i = 0; i < (u64)3000U * 1000U; ++i) {
        p[i * 4U] = (u8)((i % 3000U) < 1500U ? 200U : 100U);
        p[i * 4U + 3U] = 255U;
    }
    NTEST_ASSERT_EQ_INT(cook_voxel_image_init(&chain, nph_allocator_system(), &img, false),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(chain.width, 750U);
    NTEST_EXPECT_EQ_UINT(chain.height, 250U);
    NTEST_EXPECT_EQ_UINT(chain.texels[0], 200U);
    NTEST_EXPECT_EQ_UINT(chain.texels[(u64)749U * 4U], 100U);
    cook_voxel_image_free(&chain);
    cook_image_free(&img);
}

NTEST(CookVoxel, a_materials_surface_follows_its_alpha_and_light)
{
    NphMaterialHeader h;
    nph_material_header_default(&h);
    CookVoxelMaterial m;
    cook_voxel_material_init(&m, &h);
    NTEST_EXPECT_EQ_UINT(m.surface, NPH_RENDER_VOXEL_SURFACE_OPAQUE);
    h.alpha_mode = NPH_MATERIAL_ALPHA_MASK;
    cook_voxel_material_init(&m, &h);
    NTEST_EXPECT_EQ_UINT(m.surface, NPH_RENDER_VOXEL_SURFACE_MASKED);
    h.alpha_mode = NPH_MATERIAL_ALPHA_BLEND;
    cook_voxel_material_init(&m, &h);
    NTEST_EXPECT_EQ_UINT(m.surface, NPH_RENDER_VOXEL_SURFACE_NONE);
    nph_material_header_default(&h);
    h.emissive_factor = nph_vec3(1.0F, 0.5F, 0.0F);
    cook_voxel_material_init(&m, &h);
    NTEST_EXPECT_EQ_UINT(m.surface, NPH_RENDER_VOXEL_SURFACE_NONE);
    nph_material_header_default(&h);
    h.flags |= (u32)NPH_MATERIAL_FLAG_UNLIT;
    cook_voxel_material_init(&m, &h);
    NTEST_EXPECT_EQ_UINT(m.surface, NPH_RENDER_VOXEL_SURFACE_NONE);
}

// A foliage mesh, one submesh: `count` closed thin needles (triangular
// prisms) in a cube of 4, each with the texture coordinate u = 0.25 where
// its centre lies at x < 2 and 0.75 beyond, v = 0.5.
static void make_foliage(CookMesh *mesh, u32 count)
{
    NphAllocator *alloc = nph_allocator_system();
    CookVertex *v = NPH_ALLOC_ARRAY(alloc, CookVertex, (u64)count * 6U);
    u32 *idx = NPH_ALLOC_ARRAY(alloc, u32, (u64)count * 24U);
    NTEST_ASSERT_TRUE(v != NULL && idx != NULL);
    static const u32 prism[24] = {0, 2, 1, 3, 4, 5, 0, 1, 4, 0, 4, 3,
                                  1, 2, 5, 1, 5, 4, 2, 0, 3, 2, 3, 5};
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 77U, 1U);
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c = nph_vec3_scale(nph_pcg32_next_vec3(&rng), 4.0F);
        const f32 z = 2.0F * nph_pcg32_next_f32(&rng) - 1.0F;
        const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(&rng);
        const f32 rr = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
        const NphVec3 d = nph_vec3(rr * nph_f32_cos(phi), rr * nph_f32_sin(phi), z);
        const NphVec3 up = nph_f32_abs(d.z) < 0.9F ? nph_vec3_unit_z() : nph_vec3_unit_x();
        const NphVec3 t = nph_vec3_normalize(nph_vec3_cross(up, d));
        const NphVec3 b = nph_vec3_cross(d, t);
        const f32 u = c.x < 2.0F ? 0.25F : 0.75F;
        for (u32 k = 0; k < 6U; ++k) {
            const f32 a = 2.0F * NPH_PI * (f32)(k % 3U) / 3.0F;
            const NphVec3 off = nph_vec3_add(nph_vec3_scale(t, 0.008F * nph_f32_cos(a)),
                                             nph_vec3_scale(b, 0.008F * nph_f32_sin(a)));
            const NphVec3 end = nph_vec3_scale(d, k < 3U ? -0.2F : 0.2F);
            CookVertex *p = &v[i * 6U + k];
            memset(p, 0, sizeof *p);
            p->position = nph_vec3_add(nph_vec3_add(c, end), off);
            p->normal = nph_vec3_normalize(off);
            p->tangent = nph_vec4_from_vec3(d, 1.0F);
            p->uv0 = nph_vec2(u, 0.5F);
            p->color = nph_vec4_one();
        }
        for (u32 k = 0; k < 24U; ++k) {
            idx[i * 24U + k] = i * 6U + prism[k];
        }
    }
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(mesh, v, count * 6U, idx, count * 24U, 0U),
                        NPH_RESULT_OK);
    NPH_FREE_ARRAY(alloc, CookVertex, v, (u64)count * 6U);
    NPH_FREE_ARRAY(alloc, u32, idx, (u64)count * 24U);
    mesh->flags = (u32)COOK_MESH_HAS_NORMALS | (u32)COOK_MESH_HAS_UV0 | (u32)COOK_MESH_HAS_TANGENTS;
    strcpy(mesh->name, "foliage");
}

static void cook_foliage(bool voxels, const CookVoxelMaterials *materials, TestAsset *ta,
                         CookMeshStats *st)
{
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    make_foliage(&m, 1200U);
    CookMeshSettings s;
    cook_mesh_settings_default(&s);
    s.voxels = voxels ? 1 : 0;
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                          nph_guid_from_cstr("foliage"), 1U);
    NTEST_ASSERT_EQ_INT(cook_mesh_process(&m, &s, materials, NULL, &w, st), NPH_RESULT_OK);
    test_asset_from_writer(alloc, &w, ta);
    nph_asset_writer_free(&w);
    cook_mesh_free(&m);
}

NTEST(CookVoxel, foliage_cooks_into_voxels_coloured_by_its_texture)
{
    // Red on the left half of the texture, green on the right.
    const u8 texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    CookImage img;
    make_image(&img, 2U, 1U, texels);
    CookVoxelImage chain;
    NTEST_ASSERT_EQ_INT(cook_voxel_image_init(&chain, nph_allocator_system(), &img, true),
                        NPH_RESULT_OK);
    NphMaterialHeader h;
    nph_material_header_default(&h);
    h.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].wrap_u = NPH_MATERIAL_WRAP_CLAMP;
    h.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].wrap_v = NPH_MATERIAL_WRAP_CLAMP;
    CookVoxelMaterial mat;
    cook_voxel_material_init(&mat, &h);
    mat.base_color_image.image = &chain;
    mat.base_color_image.wrap_u = NPH_MATERIAL_WRAP_CLAMP;
    mat.base_color_image.wrap_v = NPH_MATERIAL_WRAP_CLAMP;
    const CookVoxelMaterials materials = {&mat, 1U};

    TestAsset ta;
    CookMeshStats st;
    memset(&st, 0, sizeof st);
    cook_foliage(true, &materials, &ta, &st);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(v.voxel_bricks);
    NTEST_LOG("%u levels, %u voxel clusters, %u bricks, %u voxels", st.lod_levels,
              st.voxel_clusters, st.voxel_bricks, st.voxels);
    NTEST_EXPECT_GT_UINT(st.voxel_clusters, 0U);
    NTEST_EXPECT_EQ_UINT(v.lod->brick_count, st.voxel_bricks);
    NTEST_EXPECT_EQ_UINT(v.lod->voxel_count, st.voxels);
    // Each voxel's colour is its side's: red where its cell lies left of
    // x = 1.75, green right of 2.25 (a needle reaches 0.21 from its centre,
    // so cells nearer the middle may hold both).
    u32 left = 0U;
    u32 right = 0U;
    u32 wrong = 0U;
    for (u32 k = 0; k < v.lod->brick_count; ++k) {
        const NphMeshVoxelBrick *br = &v.voxel_bricks[k];
        u32 index = br->first_voxel;
        for (u32 bit = 0; bit < 64U; ++bit) {
            if (((br->mask[bit >> 5U] >> (bit & 31U)) & 1U) == 0U) {
                continue;
            }
            NphRenderVoxel rv;
            memcpy(&rv, &v.voxels[index++], sizeof rv);
            NphRenderVoxelDesc d;
            nph_render_voxel_unpack(rv, &d);
            const f32 lo = br->origin.x + (f32)(bit & 3U) * br->cell;
            const f32 hi = lo + br->cell;
            if (hi < 1.75F) {
                left += 1U;
                wrong += d.albedo.x > 0.9F && d.albedo.y < 0.05F ? 0U : 1U;
            } else if (lo > 2.25F) {
                right += 1U;
                wrong += d.albedo.y > 0.9F && d.albedo.x < 0.05F ? 0U : 1U;
            }
        }
    }
    NTEST_LOG("%u voxels on the left, %u on the right, %u of the wrong colour", left, right, wrong);
    NTEST_EXPECT_GT_UINT(left, 10U);
    NTEST_EXPECT_GT_UINT(right, 10U);
    NTEST_EXPECT_EQ_UINT(wrong, 0U);
    test_asset_free(&ta);

    // Without the setting, or without materials, none.
    cook_foliage(false, &materials, &ta, &st);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(v.voxel_bricks == NULL && st.voxels == 0U);
    test_asset_free(&ta);
    cook_foliage(true, NULL, &ta, &st);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(v.voxel_bricks == NULL && st.voxels == 0U);
    test_asset_free(&ta);
    cook_voxel_image_free(&chain);
    cook_image_free(&img);
}

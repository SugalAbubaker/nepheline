// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/pack.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/sound.h>
#include <nepheline/asset/texture.h>
#include <nepheline/math/pack.h>

#include "test_asset_support.h"

// An NphAsset built directly from a file image whose blobs are all
// stored, so the views can be tested without the loader.
typedef struct ImageAsset {
    NphAllocator *alloc;
    void *image;
    u64 size;
    NphAssetBlob blobs[16];
    NphAsset asset;
} ImageAsset;

static void image_asset_finish(ImageAsset *ia, NphAssetWriter *w)
{
    NTEST_ASSERT_EQ_INT(nph_asset_writer_finish_alloc(w, ia->alloc, &ia->image, &ia->size),
                        NPH_RESULT_OK);
    nph_asset_writer_free(w);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(ia->image, ia->size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_LE_UINT(info.blob_count, NPH_ARRAY_COUNT(ia->blobs));
    for (u32 i = 0; i < info.blob_count; ++i) {
        ia->blobs[i].tag = info.blobs[i].tag;
        ia->blobs[i].index = info.blobs[i].index;
        ia->blobs[i].codec = info.blobs[i].codec;
        ia->blobs[i].alignment = info.blobs[i].alignment;
        ia->blobs[i].data = nph_asset_file_blob(ia->image, &info.blobs[i]);
        ia->blobs[i].size = info.blobs[i].raw_size;
        ia->blobs[i].offset = info.blobs[i].offset;
    }
    ia->asset.header = info.header;
    ia->asset.blobs = ia->blobs;
    ia->asset.blob_count = info.blob_count;
    ia->asset.path = NPH_STR("memory");
}

static void image_asset_free(ImageAsset *ia)
{
    if (ia->image != NULL) {
        test_free_asset_image(ia->alloc, ia->image, ia->size);
        ia->image = NULL;
    }
}

// Patches a blob's bytes in place (the image is writable memory the
// fixture owns; the blob's pointer is derived from it).
static void *blob_bytes(ImageAsset *ia, u32 tag, u32 index)
{
    const NphAssetBlob *b = nph_asset_blob(&ia->asset, tag, index);
    NTEST_ASSERT_NOT_NULL(b);
    return (u8 *)ia->image + nph_ptr_diff(ia->image, b->data);
}

static void add(NphAssetWriter *w, u32 tag, u32 index, const void *data, u64 size)
{
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(w, tag, index, data, size, NPH_ASSET_CODEC_NONE, 0U),
        NPH_RESULT_OK);
}

// ---------------------------------------------------------------------------
// Mesh: a cube with one submesh and one meshlet
// ---------------------------------------------------------------------------

static void build_cube(ImageAsset *ia, u32 type_version)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_MESH, type_version,
                          nph_guid_from_cstr("cube"), 1U);
    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.vertex_count = 8U;
    h.index_count = 36U;
    h.submesh_count = 1U;
    h.meshlet_count = 1U;
    h.meshlet_vertex_count = 8U;
    h.meshlet_triangle_count = 12U;
    h.flags = (u32)NPH_MESH_FLAG_HAS_UV0;
    h.position_offset = nph_vec3(-1.0F, -1.0F, -1.0F);
    h.position_scale = nph_vec3(2.0F, 2.0F, 2.0F);
    h.bounds = nph_aabb(nph_vec3(-1.0F, -1.0F, -1.0F), nph_vec3(1.0F, 1.0F, 1.0F));
    h.sphere = nph_sphere(nph_vec3_zero(), nph_f32_sqrt(3.0F));
    add(&w, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h);
    NphMeshPosition pos[8];
    NphMeshAttrib attr[8];
    for (u32 i = 0; i < 8U; ++i) {
        pos[i].x = (i & 1U) != 0U ? 65535U : 0U;
        pos[i].y = (i & 2U) != 0U ? 65535U : 0U;
        pos[i].z = (i & 4U) != 0U ? 65535U : 0U;
        pos[i].reserved = 0U;
        attr[i].normal = nph_oct_encode_snorm16(nph_vec3_normalize(
            nph_vec3((i & 1U) != 0U ? 1.0F : -1.0F, (i & 2U) != 0U ? 1.0F : -1.0F,
                     (i & 4U) != 0U ? 1.0F : -1.0F)));
        attr[i].tangent = nph_pack_snorm8x4(nph_vec4(1.0F, 0.0F, 0.0F, 1.0F));
        attr[i].uv0 = nph_pack_f16x2(nph_vec2((f32)(i & 1U), (f32)((i >> 1U) & 1U)));
        attr[i].color = 0xFFFFFFFFU;
    }
    add(&w, NPH_MESH_TAG_POSITIONS, 0U, pos, sizeof pos);
    add(&w, NPH_MESH_TAG_ATTRIBS, 0U, attr, sizeof attr);
    static const u32 indices[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                    2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    add(&w, NPH_MESH_TAG_INDICES, 0U, indices, sizeof indices);
    NphSubmesh sub;
    memset(&sub, 0, sizeof sub);
    sub.index_count = 36U;
    sub.meshlet_count = 1U;
    sub.bounds = h.bounds;
    add(&w, NPH_MESH_TAG_SUBMESHES, 0U, &sub, sizeof sub);
    NphMeshlet ml;
    memset(&ml, 0, sizeof ml);
    ml.vertex_count = 8U;
    ml.triangle_count = 12U;
    add(&w, NPH_MESH_TAG_MESHLETS, 0U, &ml, sizeof ml);
    NphMeshletBounds mb;
    memset(&mb, 0, sizeof mb);
    mb.radius = nph_f32_sqrt(3.0F);
    mb.cone_cutoff = 127; // never culled: no single normal cone fits a cube
    add(&w, NPH_MESH_TAG_MESHLET_BOUNDS, 0U, &mb, sizeof mb);
    const u32 mlv[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    add(&w, NPH_MESH_TAG_MESHLET_VERTICES, 0U, mlv, sizeof mlv);
    u8 mlt[36];
    for (u32 i = 0; i < 36U; ++i) {
        mlt[i] = (u8)indices[i];
    }
    add(&w, NPH_MESH_TAG_MESHLET_TRIS, 0U, mlt, sizeof mlt);
    image_asset_finish(ia, &w);
}

NTEST(views, mesh_view_resolves_a_valid_cube)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_cube(&ia, NPH_MESH_TYPE_VERSION);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->vertex_count, 8U);
    NTEST_ASSERT_EQ_UINT(v.indices[35], 5U);
    NTEST_ASSERT_EQ_UINT(v.meshlets[0].triangle_count, 12U);
    NTEST_ASSERT_EQ_UINT(v.submeshes[0].index_count, 36U);
    NTEST_ASSERT_EQ_UINT(v.meshlet_vertices[7], 7U);
    NTEST_ASSERT_EQ_UINT(v.meshlet_triangles[1], 2U);
    const NphVec3 p7 = nph_mesh_dequantize_position(v.header, v.positions[7]);
    NTEST_ASSERT_TRUE(nph_vec3_near(p7, nph_vec3(1.0F, 1.0F, 1.0F), 1e-6F));
    const NphVec3 p0 = nph_mesh_dequantize_position(v.header, v.positions[0]);
    NTEST_ASSERT_TRUE(nph_vec3_near(p0, nph_vec3(-1.0F, -1.0F, -1.0F), 1e-6F));
    const NphVec3 n7 = nph_oct_decode_snorm16(v.attribs[7].normal);
    NTEST_ASSERT_TRUE(nph_vec3_near(n7, nph_vec3_normalize(nph_vec3_one()), 1e-3F));
    NTEST_ASSERT_TRUE(
        nph_vec2_near(nph_unpack_f16x2(v.attribs[3].uv0), nph_vec2(1.0F, 1.0F), 0.0F));

    // Wrong kind of asset for the other views.
    NphTextureView tv;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &tv), NPH_RESULT_TYPE_MISMATCH);
    NphMaterialView mv;
    NTEST_ASSERT_EQ_INT(nph_material_view(&ia.asset, &mv), NPH_RESULT_TYPE_MISMATCH);
    NphSceneView sv;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &sv), NPH_RESULT_TYPE_MISMATCH);
    image_asset_free(&ia);

    build_cube(&ia, NPH_MESH_TYPE_VERSION + 1U);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_VERSION_MISMATCH);
    image_asset_free(&ia);
}

NTEST(views, mesh_view_rejects_bad_ranges)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_cube(&ia, NPH_MESH_TYPE_VERSION);
    NphMeshView v;

    u32 *indices = (u32 *)blob_bytes(&ia, NPH_MESH_TAG_INDICES, 0U);
    indices[5] = 8U; // past the vertex count
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    indices[5] = 3U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    NphSubmesh *sub = (NphSubmesh *)blob_bytes(&ia, NPH_MESH_TAG_SUBMESHES, 0U);
    sub->index_offset = 3U; // 3 + 36 > 36
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    sub->index_offset = 0U;
    sub->index_count = 35U; // not a multiple of three
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    sub->index_count = 36U;
    sub->meshlet_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    sub->meshlet_count = 1U;

    NphMeshlet *ml = (NphMeshlet *)blob_bytes(&ia, NPH_MESH_TAG_MESHLETS, 0U);
    ml->vertex_count = 65U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    ml->vertex_count = 8U;
    ml->submesh = 1U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    ml->submesh = 0U;
    ml->triangle_offset = 1U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    ml->triangle_offset = 0U;

    u8 *tris = (u8 *)blob_bytes(&ia, NPH_MESH_TAG_MESHLET_TRIS, 0U);
    tris[0] = 8U; // a corner beyond the meshlet's vertices
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    tris[0] = 0U;
    u32 *mlv = (u32 *)blob_bytes(&ia, NPH_MESH_TAG_MESHLET_VERTICES, 0U);
    mlv[2] = 100U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    mlv[2] = 2U;

    NphMeshHeader *h = (NphMeshHeader *)blob_bytes(&ia, NPH_MESH_TAG_HEADER, 0U);
    h->vertex_count = 9U; // the position blob no longer matches
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->vertex_count = 8U;
    h->index_count = 35U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->index_count = 36U;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    // A missing blob with a non-zero count.
    ia.asset.blob_count -= 1U; // drops the last blob (MLTR)
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
}

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

static void build_texture(ImageAsset *ia, u32 width, u32 height, u32 mips, u32 layers, u32 format,
                          u32 kind)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                          nph_guid_from_cstr("tex"), 1U);
    NphTextureHeader h;
    memset(&h, 0, sizeof h);
    h.width = width;
    h.height = height;
    h.mip_count = mips;
    h.layer_count = layers;
    h.format = format;
    h.kind = kind;
    add(&w, NPH_TEXTURE_TAG_HEADER, 0U, &h, sizeof h);
    for (u32 m = 0; m < mips; ++m) {
        const u64 bytes = nph_texture_level_bytes(format, nph_texture_mip_extent(width, m),
                                                  nph_texture_mip_extent(height, m)) *
                          layers;
        u8 *data = (u8 *)NPH_ALLOC(ia->alloc, nph_max_u64(bytes, 1U), 16U);
        NTEST_ASSERT_NOT_NULL(data);
        test_fill_pattern(data, bytes, m);
        add(&w, NPH_TEXTURE_TAG_MIP, m, data, bytes);
        NPH_FREE(ia->alloc, data, nph_max_u64(bytes, 1U), 16U);
    }
    image_asset_finish(ia, &w);
}

NTEST(views, texture_formats_and_sizes)
{
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM, 7U, 3U), 84U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_BC7_UNORM, 7U, 3U), 32U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_BC4_UNORM, 1U, 1U), 8U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_BC5_UNORM, 8U, 8U), 64U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_BC6H_UFLOAT, 4U, 4U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, 2U, 2U),
                         32U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(NPH_TEXTURE_FORMAT_UNDEFINED, 2U, 2U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_texture_level_bytes(99U, 2U, 2U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_texture_mip_count(1U, 1U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_texture_mip_count(256U, 16U), 9U);
    NTEST_ASSERT_EQ_UINT(nph_texture_mip_count(5U, 300U), 9U);
    NTEST_ASSERT_EQ_UINT(nph_texture_mip_extent(256U, 8U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_texture_mip_extent(256U, 9U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_texture_mip_extent(5U, 1U), 2U);
    NTEST_ASSERT_EQ_STR(nph_texture_format_info(NPH_TEXTURE_FORMAT_BC7_SRGB)->name, "BC7_SRGB");
    NTEST_ASSERT_TRUE(nph_texture_format_info(NPH_TEXTURE_FORMAT_BC7_SRGB)->srgb);
    NTEST_ASSERT_TRUE(nph_texture_format_info(NPH_TEXTURE_FORMAT_BC7_SRGB)->compressed);
    NTEST_ASSERT_FALSE(nph_texture_format_info(NPH_TEXTURE_FORMAT_R8_UNORM)->compressed);
    NTEST_ASSERT_EQ_UINT(nph_texture_format_info(7U)->bytes_per_block, 0U);
}

NTEST(views, texture_view_checks_every_mip)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_texture(&ia, 8U, 4U, 4U, 1U, NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB, NPH_TEXTURE_2D);
    NphTextureView v;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[0], 128U);
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[1], 32U);
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[2], 8U);
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[3], 4U);
    NTEST_ASSERT_TRUE(test_check_pattern(v.mips[2], 8U, 2U));
    NphTextureHeader *h = (NphTextureHeader *)blob_bytes(&ia, NPH_TEXTURE_TAG_HEADER, 0U);
    h->mip_count = 5U; // more than the chain holds
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->mip_count = 3U; // fewer: fine, the fourth blob is just unused
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_OK);
    h->mip_count = 4U;
    h->width = 16U; // mip 0 no longer matches its blob
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->width = 8U;
    h->format = 7U;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->format = NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB;
    h->layer_count = 2U; // a 2D texture has one layer
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->layer_count = 1U;
    h->kind = NPH_TEXTURE_CUBE; // needs six layers
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->kind = NPH_TEXTURE_2D;
    h->reserved[3] = 1U;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->reserved[3] = 0U;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);

    // A BC7 cube map: six layers per mip, whole blocks.
    build_texture(&ia, 6U, 6U, 2U, 6U, NPH_TEXTURE_FORMAT_BC7_UNORM, NPH_TEXTURE_CUBE);
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[0], 4U * 16U * 6U); // 2x2 blocks
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[1], 16U * 6U);      // 3x3 -> 1 block
    image_asset_free(&ia);
}

// ---------------------------------------------------------------------------
// Material
// ---------------------------------------------------------------------------

NTEST(views, material_view_and_defaults)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia.alloc, NPH_ASSET_TYPE_MATERIAL, NPH_MATERIAL_TYPE_VERSION,
                          nph_guid_from_cstr("mat"), 1U);
    NphMaterialHeader h;
    nph_material_header_default(&h);
    NTEST_ASSERT_TRUE(nph_vec4_eq(h.base_color_factor, nph_vec4_one()));
    NTEST_ASSERT_EQ_UINT(h.alpha_mode, NPH_MATERIAL_ALPHA_OPAQUE);
    NTEST_ASSERT_NEAR_F32(h.alpha_cutoff, 0.5F, 0.0F);
    NTEST_ASSERT_NEAR_F32(h.textures[NPH_MATERIAL_TEXTURE_NORMAL].scale, 1.0F, 0.0F);
    h.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture = nph_guid_from_cstr("tex");
    h.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].wrap_u = (u8)NPH_MATERIAL_WRAP_MIRROR;
    add(&w, NPH_MATERIAL_TAG_HEADER, 0U, &h, sizeof h);
    image_asset_finish(&ia, &w);
    NphMaterialView v;
    NTEST_ASSERT_EQ_INT(nph_material_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_guid_equals(v.header->textures[0].texture, nph_guid_from_cstr("tex")));
    NphMaterialHeader *p = (NphMaterialHeader *)blob_bytes(&ia, NPH_MATERIAL_TAG_HEADER, 0U);
    p->alpha_mode = 3U;
    NTEST_ASSERT_EQ_INT(nph_material_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    p->alpha_mode = 1U;
    p->textures[2].wrap_v = 9U;
    NTEST_ASSERT_EQ_INT(nph_material_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    p->textures[2].wrap_v = 0U;
    NTEST_ASSERT_EQ_INT(nph_material_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

static void build_scene(ImageAsset *ia)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_SCENE, NPH_SCENE_TYPE_VERSION,
                          nph_guid_from_cstr("scene"), 1U);
    static const char strings[] = "root\0child\0lamp";
    NphSceneHeader h;
    memset(&h, 0, sizeof h);
    h.node_count = 3U;
    h.instance_count = 1U;
    h.material_count = 2U;
    h.camera_count = 1U;
    h.light_count = 1U;
    h.string_bytes = sizeof strings;
    add(&w, NPH_SCENE_TAG_HEADER, 0U, &h, sizeof h);
    NphSceneNode nodes[3];
    memset(nodes, 0, sizeof nodes);
    for (u32 i = 0; i < 3U; ++i) {
        nodes[i].rotation = nph_quat_identity();
        nodes[i].scale = nph_vec3_one();
        nodes[i].first_child = NPH_SCENE_NO_NODE;
        nodes[i].next_sibling = NPH_SCENE_NO_NODE;
    }
    nodes[0].parent = NPH_SCENE_NO_NODE;
    nodes[0].first_child = 1U;
    nodes[1].parent = 0U;
    nodes[1].name = 5U;
    nodes[1].next_sibling = 2U;
    nodes[1].translation = nph_vec3(1.0F, 2.0F, 3.0F);
    nodes[2].parent = 0U;
    nodes[2].name = 11U;
    add(&w, NPH_SCENE_TAG_NODES, 0U, nodes, sizeof nodes);
    NphSceneMeshInstance inst;
    memset(&inst, 0, sizeof inst);
    inst.mesh = nph_guid_from_cstr("cube");
    inst.node = 1U;
    inst.material_offset = 0U;
    inst.material_count = 2U;
    add(&w, NPH_SCENE_TAG_INSTANCES, 0U, &inst, sizeof inst);
    const NphGuid mats[2] = {nph_guid_from_cstr("m0"), nph_guid_from_cstr("m1")};
    add(&w, NPH_SCENE_TAG_MATERIALS, 0U, mats, sizeof mats);
    NphSceneCamera cam;
    memset(&cam, 0, sizeof cam);
    cam.node = 0U;
    cam.yfov = 1.0F;
    cam.aspect = 1.5F;
    cam.znear = 0.1F;
    add(&w, NPH_SCENE_TAG_CAMERAS, 0U, &cam, sizeof cam);
    NphSceneLight light;
    memset(&light, 0, sizeof light);
    light.node = 2U;
    light.type = (u32)NPH_SCENE_LIGHT_POINT;
    light.color = nph_vec3_one();
    light.intensity = 100.0F;
    add(&w, NPH_SCENE_TAG_LIGHTS, 0U, &light, sizeof light);
    add(&w, NPH_SCENE_TAG_STRINGS, 0U, strings, sizeof strings);
    image_asset_finish(ia, &w);
}

NTEST(views, scene_view_resolves_and_validates)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_scene(&ia);
    NphSceneView v;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->node_count, 3U);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_scene_node_name(&v, 0U), NPH_STR("root")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_scene_node_name(&v, 1U), NPH_STR("child")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_scene_node_name(&v, 2U), NPH_STR("lamp")));
    NTEST_ASSERT_TRUE(nph_guid_equals(v.instances[0].mesh, nph_guid_from_cstr("cube")));
    NTEST_ASSERT_TRUE(nph_guid_equals(v.materials[1], nph_guid_from_cstr("m1")));
    NTEST_ASSERT_EQ_UINT(v.lights[0].node, 2U);
    NTEST_ASSERT_NEAR_F32(v.cameras[0].aspect, 1.5F, 0.0F);
    const NphAffine local = nph_scene_node_local(&v.nodes[1]);
    NTEST_ASSERT_TRUE(
        nph_vec3_near(nph_affine_translation(local), nph_vec3(1.0F, 2.0F, 3.0F), 0.0F));

    NphSceneNode *nodes = (NphSceneNode *)blob_bytes(&ia, NPH_SCENE_TAG_NODES, 0U);
    nodes[1].parent = 1U; // a parent must come before its child
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    nodes[1].parent = 0U;
    nodes[0].first_child = 3U;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    nodes[0].first_child = 1U;
    nodes[2].name = 16U; // past the string table
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    nodes[2].name = 11U;
    NphSceneMeshInstance *inst =
        (NphSceneMeshInstance *)blob_bytes(&ia, NPH_SCENE_TAG_INSTANCES, 0U);
    inst->material_offset = 1U; // 1 + 2 > 2
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    inst->material_offset = 0U;
    inst->node = 3U;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    inst->node = 1U;
    NphSceneLight *light = (NphSceneLight *)blob_bytes(&ia, NPH_SCENE_TAG_LIGHTS, 0U);
    light->type = 3U;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    light->type = 0U;
    char *strings = (char *)blob_bytes(&ia, NPH_SCENE_TAG_STRINGS, 0U);
    strings[15] = 'x'; // the table must end in NUL
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    strings[15] = '\0';
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

// A sound of `codec`, 100 frames of stereo at 48 kHz with `flags`: PCM in
// 16 bits; ADPCM and lossless as made-up bytes, the lossless ones as four
// blocks of 25 frames (the view checks how blocks tile DATA, not what they
// hold). `lz`: DATA compressed, which a sound's may not be.
static void build_sound(ImageAsset *ia, u32 codec, u32 flags, bool lz)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION,
                          nph_guid_from_cstr("snd"), 1U);
    NphSoundHeader h;
    memset(&h, 0, sizeof h);
    h.codec = codec;
    h.channels = 2U;
    h.sample_rate = 48000U;
    h.frames = 100U;
    h.loop_end = 100U;
    h.bits = 16U;
    h.flags = flags;
    u8 data[400];
    test_fill_pattern(data, sizeof data, 7U);
    u64 size = sizeof data; // PCM: 100 frames of 2 channels of 2 bytes
    if (codec == NPH_SOUND_CODEC_PCM) {
        h.format = NPH_SOUND_FORMAT_S16;
    } else if (codec == NPH_SOUND_CODEC_ADPCM) {
        size = 150U;
    } else {
        h.block_frames = 25U;
        h.block_count = 4U;
        const u32 offsets[4] = {0U, 90U, 170U, 260U};
        add(&w, NPH_SOUND_TAG_BLOCKS, 0U, offsets, sizeof offsets);
        size = 330U;
    }
    add(&w, NPH_SOUND_TAG_HEADER, 0U, &h, sizeof h);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_SOUND_TAG_DATA, 0U, data, size,
                                                  lz ? NPH_ASSET_CODEC_LZ : NPH_ASSET_CODEC_NONE,
                                                  0U),
                        NPH_RESULT_OK);
    image_asset_finish(ia, &w);
}

NTEST(views, sound_view_resolves_each_codec)
{
    NTEST_ASSERT_EQ_UINT(nph_sound_format_bytes(NPH_SOUND_FORMAT_S16), 2U);
    NTEST_ASSERT_EQ_UINT(nph_sound_format_bytes(NPH_SOUND_FORMAT_S24), 3U);
    NTEST_ASSERT_EQ_UINT(nph_sound_format_bytes(NPH_SOUND_FORMAT_S32), 4U);
    NTEST_ASSERT_EQ_UINT(nph_sound_format_bytes(NPH_SOUND_FORMAT_F32), 4U);
    NTEST_ASSERT_EQ_UINT(nph_sound_format_bytes(NPH_SOUND_FORMAT_NONE), 0U);
    NTEST_ASSERT_EQ_UINT(nph_sound_format_bytes(5U), 0U);
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    NphSoundView v;
    // PCM, from memory and streamed: DATA's bytes, and where they lie in
    // the file, which is where a stream reads them.
    for (u32 flags = 0U; flags <= (u32)NPH_SOUND_FLAG_STREAM; ++flags) {
        build_sound(&ia, NPH_SOUND_CODEC_PCM, flags, false);
        NTEST_ASSERT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.size, 400U);
        NTEST_ASSERT_TRUE(test_check_pattern(v.data, 400U, 7U));
        NTEST_ASSERT_EQ_UINT(v.offset, nph_ptr_diff(ia.image, v.data));
        NTEST_ASSERT_NULL(v.block_offsets);
        NTEST_ASSERT_EQ_UINT(v.header->frames, 100U);
        image_asset_free(&ia);
    }
    // ADPCM from memory; it never streams.
    build_sound(&ia, NPH_SOUND_CODEC_ADPCM, 0U, false);
    NTEST_ASSERT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.size, 150U);
    image_asset_free(&ia);
    build_sound(&ia, NPH_SOUND_CODEC_ADPCM, NPH_SOUND_FLAG_STREAM, false);
    NTEST_ASSERT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
    // Lossless, streamed: the blocks.
    build_sound(&ia, NPH_SOUND_CODEC_LOSSLESS, NPH_SOUND_FLAG_STREAM, false);
    NTEST_ASSERT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(v.block_offsets);
    NTEST_ASSERT_EQ_UINT(v.block_offsets[3], 260U);
    NTEST_ASSERT_EQ_UINT(v.size, 330U);
    image_asset_free(&ia);
    // Compressed samples could not be read from the file as they are.
    build_sound(&ia, NPH_SOUND_CODEC_PCM, 0U, true);
    NTEST_ASSERT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
}

NTEST(views, sound_view_rejects_what_is_out_of_range)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    NphSoundView v;
    build_sound(&ia, NPH_SOUND_CODEC_PCM, 0U, false);
    NphSoundHeader *h = (NphSoundHeader *)blob_bytes(&ia, NPH_SOUND_TAG_HEADER, 0U);
    const NphSoundHeader good = *h;
    // Each field out of range on its own.
    h->codec = 0U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->codec = 4U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->channels = 0U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->channels = NPH_SOUND_MAX_CHANNELS + 1U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->sample_rate = NPH_SOUND_MIN_RATE - 1U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->sample_rate = NPH_SOUND_MAX_RATE + 1U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->frames = 101U; // DATA no longer holds them
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->frames = 0U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->frames = NPH_SOUND_MAX_FRAMES + 1U; // too long for memory
    h->loop_end = h->frames;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->loop_start = 100U; // an empty loop
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->loop_end = 101U; // past the end
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->loop_end = 0U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->format = NPH_SOUND_FORMAT_NONE;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->format = 5U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->bits = 24U; // not the format's
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->block_frames = 25U; // PCM has no blocks
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->flags = 2U; // unknown
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    h->reserved[1] = 1U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = good;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    ia.asset.header.type_version = NPH_SOUND_TYPE_VERSION + 1U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_VERSION_MISMATCH);
    ia.asset.header.type_version = NPH_SOUND_TYPE_VERSION;
    ia.asset.header.type = NPH_ASSET_TYPE_TEXTURE;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_TYPE_MISMATCH);
    image_asset_free(&ia);

    // The lossless blocks must tile DATA in order.
    build_sound(&ia, NPH_SOUND_CODEC_LOSSLESS, 0U, false);
    h = (NphSoundHeader *)blob_bytes(&ia, NPH_SOUND_TAG_HEADER, 0U);
    u32 *offsets = (u32 *)blob_bytes(&ia, NPH_SOUND_TAG_BLOCKS, 0U);
    const NphSoundHeader lossless = *h;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    offsets[0] = 1U; // bytes before the first block
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    offsets[0] = 0U;
    offsets[2] = 90U; // an empty block
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    offsets[2] = 60U; // out of order
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    offsets[2] = 170U;
    offsets[3] = 330U; // the last block empty
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    offsets[3] = 400U; // past DATA
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    offsets[3] = 260U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    h->block_count = 3U; // not the frames' count, nor BLKS's
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = lossless;
    h->block_frames = 0U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = lossless;
    h->block_frames = 34U; // three blocks' worth, where BLKS holds four
    h->block_count = 3U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = lossless;
    h->bits = 25U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->bits = 3U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = lossless;
    h->format = NPH_SOUND_FORMAT_S16; // lossless has no PCM format
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *h = lossless;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);

    // ADPCM: 16-bit, no format.
    build_sound(&ia, NPH_SOUND_CODEC_ADPCM, 0U, false);
    h = (NphSoundHeader *)blob_bytes(&ia, NPH_SOUND_TAG_HEADER, 0U);
    h->bits = 24U;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->bits = 16U;
    h->format = NPH_SOUND_FORMAT_S16;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->format = NPH_SOUND_FORMAT_NONE;
    NTEST_EXPECT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
}

// ---------------------------------------------------------------------------
// The fuzz seed corpora (scripts/fuzz_seeds.py) are written by an
// independent Python description of the formats; every seed must pass the
// engine's validation and resolve its typed view.
// ---------------------------------------------------------------------------

#ifdef NPH_FUZZ_CORPUS_DIR

static void load_seed(NphAllocator *alloc, const char *name, void **out, u64 *out_size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", NPH_FUZZ_CORPUS_DIR, name);
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, out, out_size), NPH_RESULT_OK);
}

static void check_seed_asset(const char *name, u32 type)
{
    NphAllocator *alloc = nph_allocator_system();
    void *raw = NULL;
    u64 size = 0U;
    load_seed(alloc, name, &raw, &size);
    // read_all gives 16-byte alignment; the views need the blob alignment
    // the entries declare (16 in the seeds), and validation needs 8.
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(raw, size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.header.type, type);
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = alloc;
    void *owned[NPH_ARRAY_COUNT(ia.blobs)];
    NTEST_ASSERT_LE_UINT(info.blob_count, NPH_ARRAY_COUNT(ia.blobs));
    for (u32 i = 0; i < info.blob_count; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        NTEST_ASSERT_TRUE(nph_asset_file_verify_blob(raw, e));
        ia.blobs[i].tag = e->tag;
        ia.blobs[i].index = e->index;
        ia.blobs[i].codec = e->codec;
        ia.blobs[i].alignment = e->alignment;
        ia.blobs[i].size = e->raw_size;
        ia.blobs[i].offset = e->offset;
        void *unpacked = NPH_ALLOC(alloc, nph_max_u64(e->raw_size, 1U), e->alignment);
        NTEST_ASSERT_NOT_NULL(unpacked);
        NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(raw, e, unpacked), NPH_RESULT_OK);
        ia.blobs[i].data = unpacked;
        owned[i] = unpacked;
    }
    ia.asset.header = info.header;
    ia.asset.blobs = ia.blobs;
    ia.asset.blob_count = info.blob_count;
    ia.asset.path = nph_str_from_cstr(name);
    if (type == NPH_ASSET_TYPE_MESH) {
        NphMeshView v;
        NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.header->vertex_count, 3U);
    } else if (type == NPH_ASSET_TYPE_TEXTURE) {
        NphTextureView v;
        NTEST_ASSERT_EQ_INT(nph_texture_view(&ia.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.header->mip_count, 2U);
    } else if (type == NPH_ASSET_TYPE_MATERIAL) {
        NphMaterialView v;
        NTEST_ASSERT_EQ_INT(nph_material_view(&ia.asset, &v), NPH_RESULT_OK);
    } else if (type == NPH_ASSET_TYPE_SCENE) {
        NphSceneView v;
        NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_TRUE(nph_str_equals(nph_scene_node_name(&v, 1U), NPH_STR("child")));
    } else if (type == NPH_ASSET_TYPE_SOUND) {
        NphSoundView v;
        NTEST_ASSERT_EQ_INT(nph_sound_view(&ia.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.header->frames, 4U);
        NTEST_ASSERT_EQ_UINT(v.header->channels, 2U);
    }
    for (u32 i = 0; i < info.blob_count; ++i) {
        NPH_FREE(alloc, owned[i], nph_max_u64(ia.blobs[i].size, 1U), ia.blobs[i].alignment);
    }
    nph_file_free_all(alloc, raw, size);
}

NTEST(views, fuzz_seeds_written_by_the_python_description_validate)
{
    check_seed_asset("nph/blob", NPH_ASSET_TYPE_BLOB);
    check_seed_asset("nph/mesh", NPH_ASSET_TYPE_MESH);
    check_seed_asset("nph/texture", NPH_ASSET_TYPE_TEXTURE);
    check_seed_asset("nph/material", NPH_ASSET_TYPE_MATERIAL);
    check_seed_asset("nph/scene", NPH_ASSET_TYPE_SCENE);
    check_seed_asset("nph/sound", NPH_ASSET_TYPE_SOUND);
    check_seed_asset("nph/sound_streamed", NPH_ASSET_TYPE_SOUND);

    NphAllocator *alloc = nph_allocator_system();
    void *raw = NULL;
    u64 size = 0U;
    load_seed(alloc, "pack/one_entry", &raw, &size);
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(raw, size, &pack), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pack_entry_count(&pack), 1U);
    const NphPackEntry *e = nph_pack_find_path(&pack, NPH_STR("assets/one.nph"));
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_ASSERT_EQ_PTR(nph_pack_find_guid(&pack, e->guid), e);
    nph_file_free_all(alloc, raw, size);
    load_seed(alloc, "pack/empty", &raw, &size);
    NTEST_ASSERT_EQ_INT(nph_pack_validate(raw, size, &pack), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pack_entry_count(&pack), 0U);
    nph_file_free_all(alloc, raw, size);
}

#endif

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
    NphAssetBlob blobs[24]; // a mesh with levels and voxels has 17
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

// The cube with levels of detail: its one meshlet as level 0 under a root
// group, the smallest hierarchy there is (render/cluster_lod.h). `blobs`
// says which of the five to write (all: LOD_ALL).
enum {
    LOD_HEADER = 1U << 0U,
    LOD_CLUSTERS = 1U << 1U,
    LOD_GROUPS = 1U << 2U,
    LOD_MEMBERS = 1U << 3U,
    LOD_NODES = 1U << 4U,
    LOD_ALL = 31U,
};

static void add_cube_lods(NphAssetWriter *w, u32 blobs)
{
    NphMeshLodHeader lh;
    memset(&lh, 0, sizeof lh);
    lh.levels = 1U;
    lh.level0_meshlets = 1U;
    lh.group_count = 1U;
    lh.group_cluster_count = 1U;
    lh.node_count = 1U;
    const NphMeshClusterLod cl = {NPH_MESH_LOD_NONE, 0U, 0U, 0U};
    NphMeshLodGroup g;
    memset(&g, 0, sizeof g);
    g.radius = nph_f32_sqrt(3.0F);
    g.error = NPH_F32_MAX;
    g.cluster_count = 1U;
    const u32 member = 0U;
    NphMeshLodNode n;
    memset(&n, 0, sizeof n);
    for (u32 i = 0; i < NPH_MESH_LOD_CHILDREN; ++i) {
        n.child[i] = NPH_MESH_LOD_NONE;
    }
    n.child[0] = 0U | NPH_MESH_LOD_LEAF;
    n.sphere[0] = nph_vec4(0.0F, 0.0F, 0.0F, g.radius);
    n.error[0] = g.error;
    if ((blobs & LOD_HEADER) != 0U) {
        add(w, NPH_MESH_TAG_LOD_HEADER, 0U, &lh, sizeof lh);
    }
    if ((blobs & LOD_CLUSTERS) != 0U) {
        add(w, NPH_MESH_TAG_LOD_CLUSTERS, 0U, &cl, sizeof cl);
    }
    if ((blobs & LOD_GROUPS) != 0U) {
        add(w, NPH_MESH_TAG_LOD_GROUPS, 0U, &g, sizeof g);
    }
    if ((blobs & LOD_MEMBERS) != 0U) {
        add(w, NPH_MESH_TAG_LOD_MEMBERS, 0U, &member, sizeof member);
    }
    if ((blobs & LOD_NODES) != 0U) {
        add(w, NPH_MESH_TAG_LOD_NODES, 0U, &n, sizeof n);
    }
}

// build_cube's blobs, then the levels' (`lod_blobs`).
static void build_cube_with_lods(ImageAsset *ia, u32 type_version, u32 lod_blobs)
{
    // Rebuild the cube into a writer of our own, adding the levels before
    // it finishes.
    ImageAsset plain;
    memset(&plain, 0, sizeof plain);
    plain.alloc = ia->alloc;
    build_cube(&plain, type_version);
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_MESH, type_version,
                          nph_guid_from_cstr("cube"), 1U);
    for (u32 i = 0; i < plain.asset.blob_count; ++i) {
        const NphAssetBlob *b = &plain.asset.blobs[i];
        add(&w, b->tag, b->index, b->data, b->size);
    }
    image_asset_free(&plain);
    add_cube_lods(&w, lod_blobs);
    image_asset_finish(ia, &w);
}

NTEST(views, mesh_view_resolves_levels_of_detail)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_cube_with_lods(&ia, NPH_MESH_TYPE_VERSION, LOD_ALL);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(v.lod);
    NTEST_EXPECT_EQ_UINT(v.lod->level0_meshlets, 1U);
    NTEST_EXPECT_EQ_UINT(v.lod_clusters[0].group, NPH_MESH_LOD_NONE);
    NTEST_EXPECT_TRUE(v.lod_groups[0].error == NPH_F32_MAX);
    NTEST_EXPECT_EQ_UINT(v.lod_members[0], 0U);
    NTEST_EXPECT_EQ_UINT(v.lod_nodes[0].child[0], NPH_MESH_LOD_LEAF);
    image_asset_free(&ia);

    // Without them (version 2 or 1): every pointer NULL.
    build_cube(&ia, NPH_MESH_TYPE_VERSION);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(v.lod == NULL && v.lod_clusters == NULL && v.lod_groups == NULL &&
                      v.lod_members == NULL && v.lod_nodes == NULL);
    image_asset_free(&ia);
    build_cube(&ia, 1U);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(v.lod == NULL);
    image_asset_free(&ia);

    // Levels in a version-1 mesh, or some of their blobs without the rest.
    build_cube_with_lods(&ia, 1U, LOD_ALL);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
    const u32 partial[3] = {LOD_ALL & ~LOD_HEADER, LOD_ALL & ~LOD_NODES, LOD_HEADER};
    for (u32 i = 0; i < 3U; ++i) {
        build_cube_with_lods(&ia, NPH_MESH_TYPE_VERSION, partial[i]);
        NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
        image_asset_free(&ia);
    }
}

// The cube with a coarser level of voxels (version 3): its meshlet at level
// 0, made by group 0 into one voxel cluster of two bricks, the first holding
// two voxels (a box and a capsule) and the second one (a box), under root
// group 1.
static void build_cube_with_voxels(ImageAsset *ia, u32 type_version)
{
    ImageAsset plain;
    memset(&plain, 0, sizeof plain);
    plain.alloc = ia->alloc;
    build_cube(&plain, type_version);
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_MESH, type_version,
                          nph_guid_from_cstr("cube"), 1U);
    for (u32 i = 0; i < plain.asset.blob_count; ++i) {
        const NphAssetBlob *b = &plain.asset.blobs[i];
        if (b->tag == NPH_MESH_TAG_HEADER) {
            NphMeshHeader h;
            memcpy(&h, b->data, sizeof h);
            h.meshlet_count = 2U;
            add(&w, b->tag, b->index, &h, sizeof h);
        } else if (b->tag == NPH_MESH_TAG_MESHLETS) {
            NphMeshlet ml[2];
            memcpy(&ml[0], b->data, sizeof ml[0]);
            memset(&ml[1], 0, sizeof ml[1]);
            ml[1].triangle_count = 2U; // two bricks from 0
            add(&w, b->tag, b->index, ml, sizeof ml);
        } else if (b->tag == NPH_MESH_TAG_MESHLET_BOUNDS) {
            NphMeshletBounds bd[2];
            memcpy(&bd[0], b->data, sizeof bd[0]);
            bd[1] = bd[0];
            add(&w, b->tag, b->index, bd, sizeof bd);
        } else {
            add(&w, b->tag, b->index, b->data, b->size);
        }
    }
    image_asset_free(&plain);
    NphMeshLodHeader lh;
    memset(&lh, 0, sizeof lh);
    lh.levels = 2U;
    lh.level0_meshlets = 1U;
    lh.group_count = 2U;
    lh.group_cluster_count = 2U;
    lh.node_count = 1U;
    lh.brick_count = 2U;
    lh.voxel_word_count = 3U * NPH_MESH_VOXEL_WORDS;
    lh.voxel_cluster_count = 1U;
    const NphMeshClusterLod cl[2] = {{NPH_MESH_LOD_NONE, 0U, 0U, 0U}, {0U, 1U, 1U, 0U}};
    NphMeshLodGroup g[2];
    memset(g, 0, sizeof g);
    g[0].radius = nph_f32_sqrt(3.0F);
    g[0].error = 0.25F;
    g[0].cluster_count = 1U;
    g[1] = g[0];
    g[1].error = NPH_F32_MAX;
    g[1].level = 1U;
    g[1].cluster_offset = 1U;
    const u32 members[2] = {0U, 1U};
    NphMeshLodNode n;
    memset(&n, 0, sizeof n);
    for (u32 i = 0; i < NPH_MESH_LOD_CHILDREN; ++i) {
        n.child[i] = NPH_MESH_LOD_NONE;
    }
    n.child[0] = 0U | NPH_MESH_LOD_LEAF;
    n.child[1] = 1U | NPH_MESH_LOD_LEAF;
    // The voxel cluster's record: cells of 0.5 (2^-1), its voxels sharing
    // their occlusion and metallic.
    NphMeshVoxelCluster vc;
    memset(&vc, 0, sizeof vc);
    vc.cell_log2 = -1;
    vc.occlusion = 255U;
    NphMeshVoxelBrick bricks[2];
    memset(bricks, 0, sizeof bricks);
    bricks[0].corner[0] = -1;
    bricks[0].corner[1] = -1;
    bricks[0].corner[2] = -1;
    bricks[0].mask[0] = 1U;        // cell (0, 0, 0)
    bricks[0].mask[1] = 1U << 31U; // cell (3, 3, 3)
    // The next brick along x, its voxel's rank following the first brick's.
    bricks[1].corner[1] = -1;
    bricks[1].corner[2] = -1;
    bricks[1].mask[0] = 1U;
    bricks[1].first = 2U;
    // Three voxels of three words (box, lobe, look): a box over the whole
    // cell, then a capsule (shape 2) of radius code 32 (its byte in the
    // lobe's bits 16..23), then a box again.
    u32 words[3U * NPH_MESH_VOXEL_WORDS];
    memset(words, 0, sizeof words);
    words[0] = (31U << 15U) | (31U << 20U) | (31U << 25U);
    words[3] = words[0] | (2U << 30U);
    words[4] = 32U << 18U;
    words[6] = words[0];
    add(&w, NPH_MESH_TAG_LOD_HEADER, 0U, &lh, sizeof lh);
    add(&w, NPH_MESH_TAG_LOD_CLUSTERS, 0U, cl, sizeof cl);
    add(&w, NPH_MESH_TAG_LOD_GROUPS, 0U, g, sizeof g);
    add(&w, NPH_MESH_TAG_LOD_MEMBERS, 0U, members, sizeof members);
    add(&w, NPH_MESH_TAG_LOD_NODES, 0U, &n, sizeof n);
    add(&w, NPH_MESH_TAG_VOXEL_CLUSTERS, 0U, &vc, sizeof vc);
    add(&w, NPH_MESH_TAG_VOXEL_BRICKS, 0U, bricks, sizeof bricks);
    add(&w, NPH_MESH_TAG_VOXELS, 0U, words, sizeof words);
    image_asset_finish(ia, &w);
}

NTEST(views, mesh_view_resolves_and_checks_voxel_clusters)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_cube_with_voxels(&ia, NPH_MESH_TYPE_VERSION);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(v.voxel_clusters);
    NTEST_ASSERT_NOT_NULL(v.voxel_bricks);
    NTEST_ASSERT_NOT_NULL(v.voxel_words);
    NTEST_EXPECT_EQ_UINT(v.lod->brick_count, 2U);
    NTEST_EXPECT_EQ_UINT(v.lod->voxel_word_count, 3U * NPH_MESH_VOXEL_WORDS);
    NTEST_EXPECT_EQ_UINT(v.lod->voxel_cluster_count, 1U);
    NTEST_EXPECT_EQ_UINT(v.meshlets[1].vertex_count, 0U);

    NphMeshlet *ml = (NphMeshlet *)blob_bytes(&ia, NPH_MESH_TAG_MESHLETS, 0U);
    NphMeshVoxelCluster *vc =
        (NphMeshVoxelCluster *)blob_bytes(&ia, NPH_MESH_TAG_VOXEL_CLUSTERS, 0U);
    NphMeshVoxelBrick *b = (NphMeshVoxelBrick *)blob_bytes(&ia, NPH_MESH_TAG_VOXEL_BRICKS, 0U);
    u32 *words = (u32 *)blob_bytes(&ia, NPH_MESH_TAG_VOXELS, 0U);
    // The cluster's bricks: none, too many, past the bricks; its record past
    // the records; its submesh past the submeshes.
    const u16 counts[2] = {0U, NPH_MESH_VOXEL_CLUSTER_BRICKS + 1U};
    for (u32 i = 0; i < 2U; ++i) {
        ml[1].triangle_count = counts[i];
        NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    }
    ml[1].triangle_count = 2U;
    ml[1].triangle_offset = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    ml[1].triangle_offset = 0U;
    ml[1].vertex_offset = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    ml[1].vertex_offset = 0U;
    ml[1].submesh = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    ml[1].submesh = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    // Its record: a cell past the exponents a float holds, an unknown flag,
    // a wide cluster's shared look not zero, voxels' words past the array
    // (wide, three voxels take twelve).
    vc->cell_log2 = -127;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    vc->cell_log2 = -1;
    vc->flags = 2U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    vc->flags = (u8)NPH_MESH_VOXEL_CLUSTER_WIDE;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    vc->occlusion = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    vc->flags = 0U;
    vc->occlusion = 255U;
    vc->first_word = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    vc->first_word = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    // The first brick: an empty mask, its first voxel's rank not 0.
    b->mask[0] = 0U;
    b->mask[1] = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    b->mask[0] = 1U;
    b->mask[1] = 1U << 31U;
    b->first = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    b->first = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    // The second: its first rank not following the first brick's voxels, as
    // the renderer's raster takes a cluster's voxels as one run.
    b[1].first = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    b[1].first = 2U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    // A voxel: a footprint of no extent, an unknown shape, a capsule's
    // radius past its codes.
    const u32 box = words[0];
    words[0] = (3U << 5U) | (2U << 20U); // y from 3 to 3
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    words[0] = box | (3U << 30U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    words[0] = box;
    words[4] = 57U << 18U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    words[4] = 32U << 18U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    // A record without vertices at level 0 is a meshlet's, its corners
    // checked as ever (a corner past its no vertices).
    ml[0].vertex_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);

    // Before version 3 there are no voxels: the same blobs are refused.
    build_cube_with_voxels(&ia, 2U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
    // Before version 5 the voxel clusters' count was reserved; versions 3 and
    // 4 held their voxels in the builders' form, which no longer reads.
    build_cube_with_voxels(&ia, 4U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    NphMeshLodHeader *lh = (NphMeshLodHeader *)blob_bytes(&ia, NPH_MESH_TAG_LOD_HEADER, 0U);
    lh->voxel_cluster_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_VERSION_MISMATCH);
    image_asset_free(&ia);
}

// A mesh of voxels alone (version 4, a cell proxy's): no vertices, no
// indices, one submesh of no indices over level 0's one voxel cluster (a
// brick of cell 0.5), which group 0 makes into level 1's (a brick of cell
// 1), under root group 1. `indices`: three indices and a vertex, which make
// it a mesh of triangles again.
static void build_voxels_alone(ImageAsset *ia, u32 type_version, bool indices)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_MESH, type_version,
                          nph_guid_from_cstr("proxy"), 1U);
    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.vertex_count = indices ? 1U : 0U;
    h.index_count = indices ? 3U : 0U;
    h.submesh_count = 1U;
    h.meshlet_count = 2U;
    h.position_scale = nph_vec3_one();
    h.bounds.max = nph_vec3(2.0F, 2.0F, 2.0F);
    h.sphere = nph_sphere(nph_vec3_one(), nph_f32_sqrt(3.0F));
    add(&w, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h);
    if (indices) {
        const NphMeshPosition pos = {0U, 0U, 0U, 0U};
        NphMeshAttrib attr;
        memset(&attr, 0, sizeof attr);
        const u32 idx[3] = {0U, 0U, 0U};
        add(&w, NPH_MESH_TAG_POSITIONS, 0U, &pos, sizeof pos);
        add(&w, NPH_MESH_TAG_ATTRIBS, 0U, &attr, sizeof attr);
        add(&w, NPH_MESH_TAG_INDICES, 0U, idx, sizeof idx);
    }
    NphSubmesh sub;
    memset(&sub, 0, sizeof sub);
    sub.meshlet_count = 1U;
    sub.index_count = indices ? 3U : 0U;
    add(&w, NPH_MESH_TAG_SUBMESHES, 0U, &sub, sizeof sub);
    NphMeshlet ml[2];
    memset(ml, 0, sizeof ml);
    ml[0].triangle_count = 1U; // brick 0, record 0
    ml[1].vertex_offset = 1U;  // brick 1, record 1
    ml[1].triangle_offset = 1U;
    ml[1].triangle_count = 1U;
    add(&w, NPH_MESH_TAG_MESHLETS, 0U, ml, sizeof ml);
    NphMeshletBounds bd[2];
    memset(bd, 0, sizeof bd);
    bd[0].center = nph_vec3_splat(0.25F);
    bd[0].radius = 0.5F;
    bd[0].cone_cutoff = 127;
    bd[1] = bd[0];
    add(&w, NPH_MESH_TAG_MESHLET_BOUNDS, 0U, bd, sizeof bd);
    NphMeshLodHeader lh;
    memset(&lh, 0, sizeof lh);
    lh.levels = 2U;
    lh.level0_meshlets = 1U;
    lh.group_count = 2U;
    lh.group_cluster_count = 2U;
    lh.node_count = 1U;
    lh.brick_count = 2U;
    lh.voxel_word_count = 2U * NPH_MESH_VOXEL_WORDS;
    lh.voxel_cluster_count = 2U;
    const NphMeshClusterLod cl[2] = {{NPH_MESH_LOD_NONE, 0U, 0U, 0U}, {0U, 1U, 1U, 0U}};
    NphMeshLodGroup g[2];
    memset(g, 0, sizeof g);
    g[0].radius = 1.0F;
    g[0].error = 1.0F;
    g[0].cluster_count = 1U;
    g[1] = g[0];
    g[1].error = NPH_F32_MAX;
    g[1].level = 1U;
    g[1].cluster_offset = 1U;
    const u32 members[2] = {0U, 1U};
    NphMeshLodNode n;
    memset(&n, 0, sizeof n);
    for (u32 i = 0; i < NPH_MESH_LOD_CHILDREN; ++i) {
        n.child[i] = NPH_MESH_LOD_NONE;
    }
    n.child[0] = 0U | NPH_MESH_LOD_LEAF;
    n.child[1] = 1U | NPH_MESH_LOD_LEAF;
    // Two clusters of a brick of one voxel each, of cells 0.5 and 1.
    NphMeshVoxelCluster vc[2];
    memset(vc, 0, sizeof vc);
    vc[0].cell_log2 = -1;
    vc[1].first_word = NPH_MESH_VOXEL_WORDS;
    NphMeshVoxelBrick bricks[2];
    memset(bricks, 0, sizeof bricks);
    bricks[0].mask[0] = 1U;
    bricks[1].mask[0] = 1U;
    u32 words[2U * NPH_MESH_VOXEL_WORDS];
    memset(words, 0, sizeof words);
    words[0] = (31U << 15U) | (31U << 20U) | (31U << 25U);
    words[NPH_MESH_VOXEL_WORDS] = words[0];
    add(&w, NPH_MESH_TAG_LOD_HEADER, 0U, &lh, sizeof lh);
    add(&w, NPH_MESH_TAG_LOD_CLUSTERS, 0U, cl, sizeof cl);
    add(&w, NPH_MESH_TAG_LOD_GROUPS, 0U, g, sizeof g);
    add(&w, NPH_MESH_TAG_LOD_MEMBERS, 0U, members, sizeof members);
    add(&w, NPH_MESH_TAG_LOD_NODES, 0U, &n, sizeof n);
    add(&w, NPH_MESH_TAG_VOXEL_CLUSTERS, 0U, vc, sizeof vc);
    add(&w, NPH_MESH_TAG_VOXEL_BRICKS, 0U, bricks, sizeof bricks);
    add(&w, NPH_MESH_TAG_VOXELS, 0U, words, sizeof words);
    image_asset_finish(ia, &w);
}

NTEST(views, mesh_view_takes_a_mesh_of_voxels_alone)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_voxels_alone(&ia, NPH_MESH_TYPE_VERSION, false);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_NULL(v.positions);
    NTEST_EXPECT_NULL(v.indices);
    NTEST_EXPECT_NULL(v.meshlet_vertices);
    NTEST_EXPECT_NULL(v.meshlet_triangles);
    NTEST_EXPECT_EQ_UINT(v.lod->level0_meshlets, 1U);
    NTEST_EXPECT_EQ_UINT(v.meshlets[0].vertex_count, 0U);
    // Its level-0 cluster's bricks are checked as a voxel cluster's.
    NphMeshVoxelBrick *b = (NphMeshVoxelBrick *)blob_bytes(&ia, NPH_MESH_TAG_VOXEL_BRICKS, 0U);
    b[0].mask[0] = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    b[0].mask[0] = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
    // Before version 4 a record without vertices at level 0 is a meshlet's
    // (here of a triangle past the mesh's no triangles).
    build_voxels_alone(&ia, 3U, false);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
    // With a vertex and indices it is a mesh of triangles, and its level 0 a
    // meshlet's again.
    build_voxels_alone(&ia, NPH_MESH_TYPE_VERSION, true);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
}

NTEST(views, mesh_view_rejects_levels_out_of_range)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_cube_with_lods(&ia, NPH_MESH_TYPE_VERSION, LOD_ALL);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    NphMeshLodHeader *lh = (NphMeshLodHeader *)blob_bytes(&ia, NPH_MESH_TAG_LOD_HEADER, 0U);
    lh->level0_meshlets = 2U; // more than the meshlets
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    lh->level0_meshlets = 1U;
    lh->voxel_cluster_count = 1U; // voxel clusters the mesh does not hold
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    lh->voxel_cluster_count = 0U;
    lh->brick_count = 1U; // bricks the mesh does not hold
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    lh->brick_count = 0U;
    lh->group_count = 2U; // the groups' blob no longer matches
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    lh->group_count = 1U;

    NphMeshClusterLod *cl = (NphMeshClusterLod *)blob_bytes(&ia, NPH_MESH_TAG_LOD_CLUSTERS, 0U);
    cl->group = 0U; // a level-0 cluster with a maker
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    cl->group = NPH_MESH_LOD_NONE;
    cl->parent = 1U; // no such group
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    cl->parent = 0U;

    u32 *member = (u32 *)blob_bytes(&ia, NPH_MESH_TAG_LOD_MEMBERS, 0U);
    *member = 1U; // no such cluster
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    *member = 0U;

    NphMeshLodGroup *g = (NphMeshLodGroup *)blob_bytes(&ia, NPH_MESH_TAG_LOD_GROUPS, 0U);
    g->cluster_offset = 1U; // past the members
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    g->cluster_offset = 0U;
    g->cluster_count = 2U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    g->cluster_count = 1U;

    NphMeshLodNode *n = (NphMeshLodNode *)blob_bytes(&ia, NPH_MESH_TAG_LOD_NODES, 0U);
    n->child[0] = 1U | NPH_MESH_LOD_LEAF; // no such group
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    n->child[1] = 1U; // no such node
    n->child[0] = NPH_MESH_LOD_LEAF;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    n->child[1] = NPH_MESH_LOD_NONE;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);

    // The submesh's meshlets beyond level 0.
    NphSubmesh *sub = (NphSubmesh *)blob_bytes(&ia, NPH_MESH_TAG_SUBMESHES, 0U);
    lh->level0_meshlets = 1U;
    sub->meshlet_offset = 1U;
    sub->meshlet_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK); // empty, at the end
    sub->meshlet_offset = 0U;
    sub->meshlet_count = 1U;
    image_asset_free(&ia);
}

NTEST(views, mesh_view_takes_a_meshlet_without_triangles)
{
    // A meshlet may hold no triangles, and a mesh with none needs no triangle
    // blob: the view takes it without forming a pointer into the blob that
    // is not there (the nightly fuzzer's find, 2026-10-25, as UBSan's
    // "applying zero offset to null pointer"; its input is the nph corpus's
    // regression_meshlet_without_triangles).
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_cube(&ia, NPH_MESH_TYPE_VERSION);
    NphMeshHeader *h = (NphMeshHeader *)blob_bytes(&ia, NPH_MESH_TAG_HEADER, 0U);
    h->meshlet_triangle_count = 0U;
    NphMeshlet *ml = (NphMeshlet *)blob_bytes(&ia, NPH_MESH_TAG_MESHLETS, 0U);
    ml->triangle_count = 0U;
    ia.asset.blob_count -= 1U; // drops the last blob (MLTR)
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_NULL(v.meshlet_triangles);
    NTEST_EXPECT_EQ_UINT(v.meshlets[0].triangle_count, 0U);
    NTEST_EXPECT_EQ_UINT(v.meshlets[0].vertex_count, 8U);
    image_asset_free(&ia);
}

// ---------------------------------------------------------------------------
// Levels apart (version 6; ADR-0063 "Stage 2, a level is a mesh")
// ---------------------------------------------------------------------------

// A level apart (NPH_MESH_FLAG_LEVEL): a mesh of voxels alone of one level,
// its two clusters (bricks of one voxel, cells 0.5 and 1) at its level 0,
// consumed by its root group, their makers (groups of the level below, which
// the mesh does not hold) as records.
static void build_level_apart(ImageAsset *ia, u32 type_version)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_MESH, type_version,
                          nph_guid_from_cstr("level"), 1U);
    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.submesh_count = 1U;
    h.meshlet_count = 2U;
    h.flags = (u32)NPH_MESH_FLAG_LEVEL;
    h.position_scale = nph_vec3_one();
    h.bounds.max = nph_vec3(2.0F, 2.0F, 2.0F);
    h.sphere = nph_sphere(nph_vec3_one(), nph_f32_sqrt(3.0F));
    add(&w, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h);
    NphSubmesh sub;
    memset(&sub, 0, sizeof sub);
    sub.meshlet_count = 2U;
    add(&w, NPH_MESH_TAG_SUBMESHES, 0U, &sub, sizeof sub);
    NphMeshlet ml[2];
    memset(ml, 0, sizeof ml);
    ml[0].triangle_count = 1U;
    ml[1].vertex_offset = 1U;
    ml[1].triangle_offset = 1U;
    ml[1].triangle_count = 1U;
    add(&w, NPH_MESH_TAG_MESHLETS, 0U, ml, sizeof ml);
    NphMeshletBounds bd[2];
    memset(bd, 0, sizeof bd);
    bd[0].center = nph_vec3_splat(0.25F);
    bd[0].radius = 0.5F;
    bd[0].cone_cutoff = 127;
    bd[1] = bd[0];
    add(&w, NPH_MESH_TAG_MESHLET_BOUNDS, 0U, bd, sizeof bd);
    NphMeshLodHeader lh;
    memset(&lh, 0, sizeof lh);
    lh.levels = 1U;
    lh.level0_meshlets = 2U;
    lh.group_count = 1U;
    lh.group_cluster_count = 2U;
    lh.node_count = 1U;
    lh.brick_count = 2U;
    lh.voxel_word_count = 2U * NPH_MESH_VOXEL_WORDS;
    lh.voxel_cluster_count = 2U;
    const NphMeshClusterLod cl[2] = {{NPH_MESH_LOD_NONE, 0U, 0U, 0U},
                                     {NPH_MESH_LOD_NONE, 0U, 0U, 0U}};
    NphMeshLodGroup g;
    memset(&g, 0, sizeof g);
    g.radius = 1.0F;
    g.error = NPH_F32_MAX;
    g.cluster_count = 2U;
    const u32 members[2] = {0U, 1U};
    NphMeshLodNode n;
    memset(&n, 0, sizeof n);
    for (u32 i = 0; i < NPH_MESH_LOD_CHILDREN; ++i) {
        n.child[i] = NPH_MESH_LOD_NONE;
    }
    n.child[0] = 0U | NPH_MESH_LOD_LEAF;
    NphMeshVoxelCluster vc[2];
    memset(vc, 0, sizeof vc);
    vc[0].cell_log2 = -1;
    vc[1].first_word = NPH_MESH_VOXEL_WORDS;
    NphMeshVoxelBrick bricks[2];
    memset(bricks, 0, sizeof bricks);
    bricks[0].mask[0] = 1U;
    bricks[1].mask[0] = 1U;
    u32 words[2U * NPH_MESH_VOXEL_WORDS];
    memset(words, 0, sizeof words);
    words[0] = (31U << 15U) | (31U << 20U) | (31U << 25U);
    words[NPH_MESH_VOXEL_WORDS] = words[0];
    NphMeshLodMaker makers[2];
    memset(makers, 0, sizeof makers);
    makers[0].center = nph_vec3_splat(0.25F);
    makers[0].radius = 0.75F;
    makers[0].error = 0.5F;
    makers[1] = makers[0];
    add(&w, NPH_MESH_TAG_LOD_HEADER, 0U, &lh, sizeof lh);
    add(&w, NPH_MESH_TAG_LOD_CLUSTERS, 0U, cl, sizeof cl);
    add(&w, NPH_MESH_TAG_LOD_GROUPS, 0U, &g, sizeof g);
    add(&w, NPH_MESH_TAG_LOD_MEMBERS, 0U, members, sizeof members);
    add(&w, NPH_MESH_TAG_LOD_NODES, 0U, &n, sizeof n);
    add(&w, NPH_MESH_TAG_VOXEL_CLUSTERS, 0U, vc, sizeof vc);
    add(&w, NPH_MESH_TAG_VOXEL_BRICKS, 0U, bricks, sizeof bricks);
    add(&w, NPH_MESH_TAG_VOXELS, 0U, words, sizeof words);
    add(&w, NPH_MESH_TAG_LOD_MAKERS, 0U, makers, sizeof makers);
    image_asset_finish(ia, &w);
}

// A head of levels apart (NPH_MESH_FLAG_LEVELS): no geometry, one submesh,
// `count` levels (to one past the most a head holds), each naming an asset,
// their errors rising, the coarsest consumed by a root group.
static void build_levels_head(ImageAsset *ia, u32 type_version, u32 count)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_MESH, type_version,
                          nph_guid_from_cstr("head"), 1U);
    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.submesh_count = 1U;
    h.flags = (u32)NPH_MESH_FLAG_LEVELS;
    h.position_scale = nph_vec3_one();
    h.bounds.max = nph_vec3(64.0F, 8.0F, 64.0F);
    h.sphere = nph_sphere(nph_vec3(32.0F, 4.0F, 32.0F), 46.0F);
    add(&w, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h);
    NphSubmesh sub;
    memset(&sub, 0, sizeof sub);
    add(&w, NPH_MESH_TAG_SUBMESHES, 0U, &sub, sizeof sub);
    NphMeshLevel levels[NPH_MESH_MAX_LEVELS + 1U];
    memset(levels, 0, sizeof levels);
    for (u32 k = 0; k < count; ++k) {
        char name[32];
        snprintf(name, sizeof name, "level %u", k);
        levels[k].mesh = nph_guid_from_cstr(name);
        levels[k].maker_error = k == 0U ? 0.0F : 0.25F * (f32)(1U << k) * 0.5F;
        levels[k].error = k + 1U == count ? NPH_F32_MAX : 0.25F * (f32)(1U << k);
        levels[k].cluster_count = 1U + (count - k) * 8U;
    }
    add(&w, NPH_MESH_TAG_LEVELS, 0U, levels, (u64)count * sizeof(NphMeshLevel));
    image_asset_finish(ia, &w);
}

NTEST(views, mesh_view_takes_levels_apart)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    NphMeshView v;

    // A level: its makers, no levels of its own to name.
    build_level_apart(&ia, NPH_MESH_TYPE_VERSION);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(v.lod_makers);
    NTEST_EXPECT_NEAR_F32(v.lod_makers[1].error, 0.5F, 0.0F);
    NTEST_EXPECT_NULL(v.levels);
    NTEST_EXPECT_EQ_UINT(v.level_count, 0U);
    NphMeshLodMaker *mk = (NphMeshLodMaker *)blob_bytes(&ia, NPH_MESH_TAG_LOD_MAKERS, 0U);
    NphMeshHeader *h = (NphMeshHeader *)blob_bytes(&ia, NPH_MESH_TAG_HEADER, 0U);
    NphMeshLodHeader *lh = (NphMeshLodHeader *)blob_bytes(&ia, NPH_MESH_TAG_LOD_HEADER, 0U);
    // A maker's error or radius negative, or not finite.
    mk[1].error = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    mk[1].error = nph_f32_from_bits(0x7F800000U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    mk[1].error = 0.5F;
    mk[0].radius = nph_f32_from_bits(0x7FC00000U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    mk[0].radius = 0.75F;
    mk[0].center.y = nph_f32_from_bits(0xFF800000U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    mk[0].center.y = 0.25F;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    // More than one level, or a cluster past its level 0.
    lh->levels = 2U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    lh->levels = 1U;
    lh->level0_meshlets = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    lh->level0_meshlets = 2U;
    // Makers without the flag; the flag with a head's.
    h->flags = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->flags = (u32)NPH_MESH_FLAG_LEVEL | (u32)NPH_MESH_FLAG_LEVELS;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->flags = (u32)NPH_MESH_FLAG_LEVEL;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
    // Before version 6 there are no levels apart.
    build_level_apart(&ia, 5U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);

    // A head: its levels, no geometry.
    build_levels_head(&ia, NPH_MESH_TYPE_VERSION, 4U);
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(v.levels);
    NTEST_EXPECT_EQ_UINT(v.level_count, 4U);
    NTEST_EXPECT_TRUE(nph_guid_equals(v.levels[2].mesh, nph_guid_from_cstr("level 2")));
    NTEST_EXPECT_NULL(v.lod);
    NTEST_EXPECT_NULL(v.meshlets);
    NphMeshLevel *lv = (NphMeshLevel *)blob_bytes(&ia, NPH_MESH_TAG_LEVELS, 0U);
    h = (NphMeshHeader *)blob_bytes(&ia, NPH_MESH_TAG_HEADER, 0U);
#define EXPECT_HEAD_CORRUPT(field, bad)                                                            \
    do {                                                                                           \
        const NphMeshLevel keep = lv[1];                                                           \
        lv[1].field = (bad);                                                                       \
        NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);                     \
        lv[1] = keep;                                                                              \
    } while (0)
    EXPECT_HEAD_CORRUPT(mesh, NPH_GUID_NULL);
    EXPECT_HEAD_CORRUPT(cluster_count, 0U);
    EXPECT_HEAD_CORRUPT(reserved, 1U);
    EXPECT_HEAD_CORRUPT(maker_error, -1.0F);                          // below the finer level's
    EXPECT_HEAD_CORRUPT(maker_error, 1.0F);                           // past its error
    EXPECT_HEAD_CORRUPT(error, nph_f32_from_bits(0x7FC00000U));       // not a number
    EXPECT_HEAD_CORRUPT(maker_error, nph_f32_from_bits(0x7F800000U)); // infinite
#undef EXPECT_HEAD_CORRUPT
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    // Levels without the flag, or geometry of its own.
    h->flags = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->flags = (u32)NPH_MESH_FLAG_LEVELS;
    h->meshlet_count = 1U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    h->meshlet_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
    // As many levels as a head holds, not one more; and before version 6.
    build_levels_head(&ia, NPH_MESH_TYPE_VERSION, NPH_MESH_MAX_LEVELS);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);
    build_levels_head(&ia, NPH_MESH_TYPE_VERSION, NPH_MESH_MAX_LEVELS + 1U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
    build_levels_head(&ia, 5U, 2U);
    NTEST_EXPECT_EQ_INT(nph_mesh_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
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
    NTEST_EXPECT_EQ_UINT(v.cell_count, 0U);
    NTEST_EXPECT_NULL(v.cells);
    image_asset_free(&ia);
}

// A scene of `type_version` with two instances and, with `cells`, two cells
// (version 2): the first holding instance 0 and a proxy, the second instance
// 1 and none. `cell_word`: what the header's cell count holds.
static void build_scene_cells(ImageAsset *ia, u32 type_version, bool cells, u32 cell_word)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, ia->alloc, NPH_ASSET_TYPE_SCENE, type_version,
                          nph_guid_from_cstr("cells"), 1U);
    static const char strings[] = "root";
    NphSceneHeader h;
    memset(&h, 0, sizeof h);
    h.node_count = 1U;
    h.instance_count = 2U;
    h.material_count = 1U;
    h.string_bytes = sizeof strings;
    h.cell_count = cell_word;
    add(&w, NPH_SCENE_TAG_HEADER, 0U, &h, sizeof h);
    NphSceneNode node;
    memset(&node, 0, sizeof node);
    node.rotation = nph_quat_identity();
    node.scale = nph_vec3_one();
    node.parent = NPH_SCENE_NO_NODE;
    node.first_child = NPH_SCENE_NO_NODE;
    node.next_sibling = NPH_SCENE_NO_NODE;
    add(&w, NPH_SCENE_TAG_NODES, 0U, &node, sizeof node);
    NphSceneMeshInstance inst[2];
    memset(inst, 0, sizeof inst);
    inst[0].mesh = nph_guid_from_cstr("tree");
    inst[0].material_count = 1U;
    inst[1] = inst[0];
    add(&w, NPH_SCENE_TAG_INSTANCES, 0U, inst, sizeof inst);
    const NphGuid mat = nph_guid_from_cstr("bark");
    add(&w, NPH_SCENE_TAG_MATERIALS, 0U, &mat, sizeof mat);
    add(&w, NPH_SCENE_TAG_STRINGS, 0U, strings, sizeof strings);
    if (cells) {
        NphSceneCell c[2];
        memset(c, 0, sizeof c);
        c[0].bounds.max = nph_vec3(64.0F, 20.0F, 64.0F);
        c[0].instance_count = 1U;
        c[0].proxy = nph_guid_from_cstr("proxy");
        c[0].proxy_material = mat;
        c[0].proxy_cell = 0.25F;
        c[0].proxy_distance = 560.0F;
        c[1].bounds = c[0].bounds;
        c[1].instance_offset = 1U;
        c[1].instance_count = 1U;
        c[1].proxy_origin = nph_vec3(64.0F, 0.0F, 0.0F);
        add(&w, NPH_SCENE_TAG_CELLS, 0U, c, sizeof c);
    }
    image_asset_finish(ia, &w);
}

NTEST(views, scene_view_resolves_and_checks_cells)
{
    ImageAsset ia;
    memset(&ia, 0, sizeof ia);
    ia.alloc = nph_allocator_system();
    build_scene_cells(&ia, NPH_SCENE_TYPE_VERSION, true, 2U);
    NphSceneView v;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.cell_count, 2U);
    NTEST_ASSERT_NOT_NULL(v.cells);
    NTEST_EXPECT_TRUE(nph_guid_equals(v.cells[0].proxy, nph_guid_from_cstr("proxy")));
    NTEST_EXPECT_EQ_UINT(v.cells[1].instance_offset, 1U);
    NphSceneCell *c = (NphSceneCell *)blob_bytes(&ia, NPH_SCENE_TAG_CELLS, 0U);
    // Ranges: overlapping, out of order, past the instances.
    c[1].instance_offset = 0U;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    c[1].instance_offset = 1U;
    c[1].instance_count = 2U;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    c[1].instance_count = 1U;
    c[0].instance_offset = 2U;
    c[0].instance_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    c[0].instance_offset = 0U;
    c[0].instance_count = 1U;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
    // A proxy's cell not a positive power of two, its distance not positive
    // and finite; bounds or an origin not finite; a reserved word.
    const f32 bad_cells[3] = {0.3F, -0.25F, 0.0F};
    for (u32 i = 0; i < 3U; ++i) {
        c[0].proxy_cell = bad_cells[i];
        NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    }
    c[0].proxy_cell = 0.25F;
    const f32 bad_distances[2] = {0.0F, NPH_F32_INFINITY};
    for (u32 i = 0; i < 2U; ++i) {
        c[0].proxy_distance = bad_distances[i];
        NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    }
    c[0].proxy_distance = 560.0F;
    c[0].bounds.min.y = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    c[0].bounds.min.y = 0.0F;
    c[1].proxy_origin.z = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    c[1].proxy_origin.z = 0.0F;
    c[1].reserved[2] = 1U;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    c[1].reserved[2] = 0U;
    // A cell without a proxy has no cell or distance to check.
    c[1].proxy_cell = 0.3F;
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
    image_asset_free(&ia);

    // A count the blob does not hold.
    build_scene_cells(&ia, NPH_SCENE_TYPE_VERSION, true, 3U);
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
    image_asset_free(&ia);
    // Version 1: no cells, whatever its reserved word holds; a blob of them
    // is refused.
    build_scene_cells(&ia, 1U, false, 7U);
    NTEST_ASSERT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(v.cell_count, 0U);
    NTEST_EXPECT_NULL(v.cells);
    image_asset_free(&ia);
    build_scene_cells(&ia, 1U, true, 0U);
    NTEST_EXPECT_EQ_INT(nph_scene_view(&ia.asset, &v), NPH_RESULT_CORRUPT);
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
        // The triangle, (the proxy's or its level's) voxels alone, or the
        // head's levels.
        NTEST_ASSERT_TRUE(v.header->vertex_count == 3U ||
                          (v.header->vertex_count == 0U && v.voxel_bricks != NULL) ||
                          (v.header->vertex_count == 0U && v.level_count == 2U));
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
        for (u32 k = 0; k < v.cell_count; ++k) {
            NTEST_ASSERT_NEAR_F32(v.cells[k].proxy_cell, 0.25F, 0.0F);
        }
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
    check_seed_asset("nph/mesh_lod", NPH_ASSET_TYPE_MESH);
    check_seed_asset("nph/mesh_voxels", NPH_ASSET_TYPE_MESH);
    check_seed_asset("nph/mesh_proxy", NPH_ASSET_TYPE_MESH);
    check_seed_asset("nph/mesh_level", NPH_ASSET_TYPE_MESH);
    check_seed_asset("nph/mesh_levels_head", NPH_ASSET_TYPE_MESH);
    check_seed_asset("nph/texture", NPH_ASSET_TYPE_TEXTURE);
    check_seed_asset("nph/material", NPH_ASSET_TYPE_MATERIAL);
    check_seed_asset("nph/scene", NPH_ASSET_TYPE_SCENE);
    check_seed_asset("nph/scene_cells", NPH_ASSET_TYPE_SCENE);
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

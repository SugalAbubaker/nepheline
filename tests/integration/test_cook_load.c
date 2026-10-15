// SPDX-License-Identifier: Apache-2.0
// Phase 6's exit criterion, "a glTF scene cooks and loads" (CLAUDE.md §12),
// end to end across the cooker, the asset runtime, the job system, and the
// RHI:
//
// 1. a generated glTF scene (external buffer and image files, a data-URI
//    normal map, a mesh without normals, a camera, a light) is cooked into
//    a pack with the job system encoding the textures;
// 2. a VFS mounts the pack and a loader with a job system loads the scene
//    by its GUID, asynchronously, then everything it references the way a
//    runtime does: meshes and materials by the GUIDs in the scene, textures
//    by the GUIDs in the materials, all in flight together on one counter;
// 3. every typed view resolves, the hierarchy and the references are what
//    the source says, and with a Vulkan device every texture mip and every
//    mesh stream is uploaded through the RHI under the validation gate, so
//    the cooked sizes are the ones the GPU formats imply.
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/texture.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/platform/cpu.h>

#include "test_cook_support.h"
#include "test_gpu_support.h"

#include <cook/cooker.h>

#define MAX_ASSETS 32U

typedef struct Loaded {
    NphAssetHandle handles[MAX_ASSETS];
    u32 count;
} Loaded;

static NphAssetHandle load_async(NphAssetLoader *loader, NphGuid guid, NphJobCounter *done,
                                 Loaded *loaded)
{
    NTEST_ASSERT_LT_UINT(loaded->count, MAX_ASSETS);
    NphAssetHandle h = NPH_ASSET_HANDLE_NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid_async(loader, guid, done, &h), NPH_RESULT_OK);
    loaded->handles[loaded->count++] = h;
    return h;
}

static NphGpuFormat gpu_format(u32 format)
{
    switch (format) {
    case NPH_TEXTURE_FORMAT_R8_UNORM:
        return NPH_GPU_FORMAT_R8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8_UNORM:
        return NPH_GPU_FORMAT_R8G8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM:
        return NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB:
        return NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    case NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
        return NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    case NPH_TEXTURE_FORMAT_BC4_UNORM:
        return NPH_GPU_FORMAT_BC4_UNORM;
    case NPH_TEXTURE_FORMAT_BC5_UNORM:
        return NPH_GPU_FORMAT_BC5_UNORM;
    case NPH_TEXTURE_FORMAT_BC6H_UFLOAT:
        return NPH_GPU_FORMAT_BC6H_UFLOAT;
    case NPH_TEXTURE_FORMAT_BC7_UNORM:
        return NPH_GPU_FORMAT_BC7_UNORM;
    case NPH_TEXTURE_FORMAT_BC7_SRGB:
        return NPH_GPU_FORMAT_BC7_SRGB;
    default:
        return NPH_GPU_FORMAT_UNDEFINED;
    }
}

static void upload_texture(NphGpuDevice *dev, const NphTextureView *tv)
{
    const NphTextureHeader *h = tv->header;
    NphGpuImage img = test_make_image_2d(
        dev, h->width, h->height, gpu_format(h->format), h->mip_count,
        (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST, "cooked texture");
    for (u32 m = 0; m < h->mip_count; ++m) {
        NTEST_ASSERT_EQ_INT(
            nph_gpu_upload_image(dev, img, m, 0U, h->layer_count, tv->mips[m], tv->mip_sizes[m]),
            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    nph_gpu_image_destroy(dev, img);
}

static void upload_bytes(NphGpuDevice *dev, const void *data, u64 size, const char *name)
{
    if (size == 0U) {
        return;
    }
    NphGpuBuffer b = test_make_buffer(
        dev, size, (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
        NPH_GPU_MEMORY_DEVICE, name);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(dev, b, 0U, data, size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    nph_gpu_buffer_destroy(dev, b);
}

static void upload_mesh(NphGpuDevice *dev, const NphMeshView *mv)
{
    const NphMeshHeader *h = mv->header;
    upload_bytes(dev, mv->positions, (u64)h->vertex_count * sizeof *mv->positions, "positions");
    upload_bytes(dev, mv->attribs, (u64)h->vertex_count * sizeof *mv->attribs, "attributes");
    upload_bytes(dev, mv->indices, (u64)h->index_count * sizeof(u32), "indices");
    upload_bytes(dev, mv->meshlets, (u64)h->meshlet_count * sizeof *mv->meshlets, "meshlets");
    upload_bytes(dev, mv->meshlet_bounds, (u64)h->meshlet_count * sizeof *mv->meshlet_bounds,
                 "meshlet bounds");
    upload_bytes(dev, mv->meshlet_vertices, (u64)h->meshlet_vertex_count * sizeof(u32),
                 "meshlet vertices");
    upload_bytes(dev, mv->meshlet_triangles, (u64)h->meshlet_triangle_count * 3U,
                 "meshlet triangles");
}

NTEST(integration, a_gltf_scene_cooks_and_loads)
{
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "cook_load");
    test_write_gltf(&dir, "scene", true);
    char gltf[1200];
    char out_dir[1200];
    char pack_path[1200];
    test_path_in(&dir, "scene.gltf", gltf, sizeof gltf);
    test_path_in(&dir, "cooked", out_dir, sizeof out_dir);
    test_path_in(&dir, "content.npk", pack_path, sizeof pack_path);

    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = alloc;
    jd.worker_count = nph_clamp_u32(nph_cpu_logical_count(), 2U, 4U);
    NphJobSystem *jobs = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &jobs), NPH_RESULT_OK);

    // 1. Cook and pack.
    CookerDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = alloc;
    cd.out_dir = out_dir;
    cd.source_root = dir.dir;
    cd.jobs = jobs;
    cook_texture_settings_default(&cd.texture);
    cook_mesh_settings_default(&cd.mesh);
    Cooker *cooker = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&cd, &cooker), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(cooker, gltf), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cooker_stats(cooker).cooked, 7U);
    NTEST_ASSERT_EQ_INT(cooker_pack(cooker, pack_path), NPH_RESULT_OK);
    cooker_destroy(cooker);

    // 2. Load the scene by identity, then what it references.
    NphVfs *vfs = NULL;
    NTEST_ASSERT_EQ_INT(nph_vfs_create(alloc, &vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(vfs, NPH_STR("content"), pack_path, NULL),
                        NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = alloc;
    ld.vfs = vfs;
    ld.jobs = jobs;
    ld.verify = NPH_ASSET_VERIFY_ALWAYS;
    NphAssetLoader *loader = NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &loader), NPH_RESULT_OK);
    Loaded loaded;
    memset(&loaded, 0, sizeof loaded);
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    const NphAssetHandle scene_handle =
        load_async(loader, nph_guid_from_cstr("scene.gltf#scene"), &done, &loaded);
    nph_job_wait(jobs, &done);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(loader, scene_handle), NPH_RESULT_OK);
    NphSceneView sv;
    NTEST_ASSERT_EQ_INT(nph_scene_view(nph_asset_get(loader, scene_handle), &sv), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(sv.header->node_count, 5U);
    NTEST_ASSERT_EQ_UINT(sv.header->instance_count, 2U);
    NTEST_ASSERT_EQ_UINT(sv.header->camera_count, 1U);
    NTEST_ASSERT_EQ_UINT(sv.header->light_count, 1U);
    NTEST_ASSERT_TRUE(
        nph_str_equals(nph_scene_node_name(&sv, sv.cameras[0].node), NPH_STR("camera")));
    NTEST_ASSERT_TRUE(
        nph_str_equals(nph_scene_node_name(&sv, sv.lights[0].node), NPH_STR("light")));
    // Meshes and materials, all in flight at once.
    for (u32 i = 0; i < sv.header->instance_count; ++i) {
        load_async(loader, sv.instances[i].mesh, &done, &loaded);
    }
    for (u32 i = 0; i < sv.header->material_count; ++i) {
        load_async(loader, sv.materials[i], &done, &loaded);
    }
    nph_job_wait(jobs, &done);
    // Textures, from the materials now loaded.
    for (u32 i = 0; i < sv.header->material_count; ++i) {
        const NphAssetHandle mh = nph_asset_find_guid(loader, sv.materials[i]);
        NTEST_ASSERT_EQ_INT(nph_asset_wait(loader, mh), NPH_RESULT_OK);
        NphMaterialView matv;
        NTEST_ASSERT_EQ_INT(nph_material_view(nph_asset_get(loader, mh), &matv), NPH_RESULT_OK);
        for (u32 t = 0; t < NPH_MATERIAL_TEXTURE_COUNT; ++t) {
            if (!nph_guid_is_null(matv.header->textures[t].texture)) {
                load_async(loader, matv.header->textures[t].texture, &done, &loaded);
            }
        }
    }
    nph_job_wait(jobs, &done);
    // Every load finished READY (the counter is signalled either way).
    for (u32 i = 0; i < loaded.count; ++i) {
        NTEST_ASSERT_EQ_INT(nph_asset_wait(loader, loaded.handles[i]), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_UINT(loaded.count, 1U + 2U + 2U + 2U); // scene, meshes, materials, textures

    // 3. Views, references, and the GPU.
    NphGpuDevice *dev = test_gpu_create(NULL);
    u32 textures = 0U;
    u32 meshes = 0U;
    for (u32 i = 0; i < loaded.count; ++i) {
        const NphAsset *a = nph_asset_get(loader, loaded.handles[i]);
        NTEST_ASSERT_NOT_NULL(a);
        if (a->header.type == NPH_ASSET_TYPE_TEXTURE) {
            NphTextureView tv;
            NTEST_ASSERT_EQ_INT(nph_texture_view(a, &tv), NPH_RESULT_OK);
            if (dev != NULL && nph_gpu_device_info(dev)->features.texture_compression_bc) {
                upload_texture(dev, &tv);
            }
            ++textures;
        } else if (a->header.type == NPH_ASSET_TYPE_MESH) {
            NphMeshView mv;
            NTEST_ASSERT_EQ_INT(nph_mesh_view(a, &mv), NPH_RESULT_OK);
            NTEST_ASSERT_GT_UINT(mv.header->meshlet_count, 0U);
            if (dev != NULL) {
                upload_mesh(dev, &mv);
            }
            ++meshes;
        }
    }
    NTEST_ASSERT_EQ_UINT(textures, 2U);
    NTEST_ASSERT_EQ_UINT(meshes, 2U);
    if (dev == NULL) {
        NTEST_LOG("no Vulkan device: the GPU uploads were skipped");
    }
    test_gpu_destroy(dev);

    for (u32 i = 0; i < loaded.count; ++i) {
        nph_asset_release(loader, loaded.handles[i]);
    }
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(loader), 0U);
    nph_asset_loader_destroy(loader);
    nph_vfs_destroy(vfs);
    nph_job_system_destroy(jobs);
    test_temp_dir_remove(&dir);
}

// SPDX-License-Identifier: Apache-2.0
// End to end: cook generated glTF scenes, pack them, mount the pack, load
// everything through the loader, and resolve every reference; and the
// cooker's bookkeeping: source keys, incremental decisions, stale assets,
// output paths, directories.
#include <nepheline/asset/loader.h>
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/texture.h>
#include <nepheline/asset/vfs.h>

#include "test_cook_support.h"

#include <cook/cooker.h>
#include <stdlib.h>

typedef struct CookFixture {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
    char gltf[1200];
} CookFixture;

NTEST_SETUP(CookFixture)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "cooker");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
    test_write_gltf(&f->dir, "scene", false);
    test_path_in(&f->dir, "scene.gltf", f->gltf, sizeof f->gltf);
}

NTEST_TEARDOWN(CookFixture)
{
    test_temp_dir_remove(&f->dir);
}

static Cooker *make_cooker(CookFixture *f, bool force)
{
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    desc.force = force;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.texture.lz = NPH_LZ_LEVEL_FAST;
    desc.mesh.lz = NPH_LZ_LEVEL_FAST;
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    return c;
}

// A document of `mesh_count` one-triangle meshes, one node each, no
// materials (so the default material is used).
static void write_triangles_gltf(const char *path, u32 mesh_count)
{
    const f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    char b64[128];
    test_base64((const u8 *)pos, sizeof pos, b64);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                                                "\"scenes\":[{\"nodes\":["),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < mesh_count; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&sb, "%s%u", i == 0U ? "" : ",", i), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, "]}],\"nodes\":["), NPH_RESULT_OK);
    for (u32 i = 0; i < mesh_count; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&sb, "%s{\"mesh\":%u}", i == 0U ? "" : ",", i),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, "],\"meshes\":["), NPH_RESULT_OK);
    for (u32 i = 0; i < mesh_count; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&sb,
                                           "%s{\"name\":\"tri%u\",\"primitives\":[{\"attributes\":"
                                           "{\"POSITION\":0}}]}",
                                           i == 0U ? "" : ",", i),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(
        nph_sb_appendf(&sb,
                       "],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}],\"bufferViews\":[{"
                       "\"buffer\":0,\"byteLength\":36}],\"buffers\":[{\"byteLength\":36,\"uri\":"
                       "\"data:application/octet-stream;base64,%s\"}]}",
                       b64),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, sb.data, sb.len), NPH_RESULT_OK);
    nph_sb_free(&sb);
}

static u32 count_entries_of(const CookDb *db, const char *source)
{
    u32 n = 0U;
    for (u32 i = 0; i < cook_db_count(db); ++i) {
        n += strcmp(cook_db_entry(db, i)->source, source) == 0 ? 1U : 0U;
    }
    return n;
}

// Loads every asset of the pack into the loader and checks the scene's
// references resolve to meshes, materials, and textures.
static void load_everything(CookFixture *f, const char *pack_path)
{
    NphVfs *vfs = NULL;
    NTEST_ASSERT_EQ_INT(nph_vfs_create(f->alloc, &vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(vfs, nph_str_empty(), pack_path, NULL), NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = f->alloc;
    ld.vfs = vfs;
    ld.verify = NPH_ASSET_VERIFY_ALWAYS;
    NphAssetLoader *loader = NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &loader), NPH_RESULT_OK);
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, pack_path, &pack), NPH_RESULT_OK);
    const u32 n = nph_pack_entry_count(&pack);
    NTEST_ASSERT_EQ_UINT(n, 7U); // 2 meshes, 2 materials, 2 textures, the scene
    NphAssetHandle handles[16];
    NphAssetHandle scene_handle = NPH_ASSET_HANDLE_NULL;
    for (u32 i = 0; i < n; ++i) {
        const NphPackEntry *e = nph_pack_entry_at(&pack, i);
        NTEST_ASSERT_EQ_INT(nph_asset_load(loader, nph_pack_entry_path(&pack, e), &handles[i]),
                            NPH_RESULT_OK);
        const NphAsset *a = nph_asset_get(loader, handles[i]);
        NTEST_ASSERT_NOT_NULL(a);
        NTEST_ASSERT_EQ_UINT(a->header.type, e->type);
        NTEST_ASSERT_TRUE(nph_guid_equals(a->header.guid, e->guid));
        if (a->header.type == NPH_ASSET_TYPE_SCENE) {
            scene_handle = handles[i];
        }
    }
    nph_pack_close(&pack);
    NTEST_ASSERT_FALSE(nph_asset_handle_is_null(scene_handle));
    NphSceneView sv;
    NTEST_ASSERT_EQ_INT(nph_scene_view(nph_asset_get(loader, scene_handle), &sv), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(sv.header->node_count, 5U);
    NTEST_ASSERT_EQ_UINT(sv.header->instance_count, 2U);
    NTEST_ASSERT_EQ_UINT(sv.header->camera_count, 1U);
    NTEST_ASSERT_EQ_UINT(sv.header->light_count, 1U);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_scene_node_name(&sv, 0U), NPH_STR("root")));
    NTEST_ASSERT_EQ_UINT(sv.nodes[0].first_child, 1U);
    NTEST_ASSERT_EQ_UINT(sv.nodes[1].next_sibling, 2U);
    NTEST_ASSERT_EQ_UINT(sv.nodes[3].next_sibling, 4U);
    NTEST_ASSERT_EQ_UINT(sv.nodes[4].next_sibling, NPH_SCENE_NO_NODE);
    u32 textured_materials = 0U;
    for (u32 i = 0; i < sv.header->instance_count; ++i) {
        const NphSceneMeshInstance *inst = &sv.instances[i];
        const NphAssetHandle mh = nph_asset_find_guid(loader, inst->mesh);
        NTEST_ASSERT_FALSE(nph_asset_handle_is_null(mh));
        NphMeshView mv;
        NTEST_ASSERT_EQ_INT(nph_mesh_view(nph_asset_get(loader, mh), &mv), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(mv.header->submesh_count, inst->material_count);
        for (u32 s = 0; s < inst->material_count; ++s) {
            const NphAssetHandle mat =
                nph_asset_find_guid(loader, sv.materials[inst->material_offset + s]);
            NTEST_ASSERT_FALSE(nph_asset_handle_is_null(mat));
            NphMaterialView matv;
            NTEST_ASSERT_EQ_INT(nph_material_view(nph_asset_get(loader, mat), &matv),
                                NPH_RESULT_OK);
            for (u32 t = 0; t < NPH_MATERIAL_TEXTURE_COUNT; ++t) {
                const NphGuid tg = matv.header->textures[t].texture;
                if (nph_guid_is_null(tg)) {
                    continue;
                }
                const NphAssetHandle th = nph_asset_find_guid(loader, tg);
                NTEST_ASSERT_FALSE(nph_asset_handle_is_null(th));
                NphTextureView tv;
                NTEST_ASSERT_EQ_INT(nph_texture_view(nph_asset_get(loader, th), &tv),
                                    NPH_RESULT_OK);
                if (t == NPH_MATERIAL_TEXTURE_NORMAL) {
                    NTEST_ASSERT_EQ_UINT(tv.header->format, NPH_TEXTURE_FORMAT_BC5_UNORM);
                    NTEST_ASSERT_TRUE((tv.header->flags & (u32)NPH_TEXTURE_FLAG_NORMAL_MAP) != 0U);
                } else {
                    NTEST_ASSERT_EQ_UINT(tv.header->format, NPH_TEXTURE_FORMAT_BC7_SRGB);
                    NTEST_ASSERT_EQ_UINT(tv.header->mip_count, 4U); // 8x8
                }
                ++textured_materials;
            }
        }
    }
    NTEST_ASSERT_EQ_UINT(textured_materials, 2U); // base colour + normal on the cube's material
    for (u32 i = 0; i < n; ++i) {
        nph_asset_release(loader, handles[i]);
    }
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(loader), 0U);
    nph_asset_loader_destroy(loader);
    nph_vfs_destroy(vfs);
}

NTEST_F(CookFixture, cooks_packs_loads_and_skips_what_is_current)
{
    Cooker *c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    CookerStats st = cooker_stats(c);
    NTEST_ASSERT_EQ_UINT(st.cooked, 7U); // 2 textures, 2 materials, 2 meshes, 1 scene
    NTEST_ASSERT_EQ_UINT(st.skipped, 0U);
    NTEST_ASSERT_EQ_UINT(st.failed, 0U);
    NTEST_ASSERT_EQ_UINT(cook_db_count(cooker_db(c)), 7U);
    char pack_path[1200];
    test_path_in(&f->dir, "content.npk", pack_path, sizeof pack_path);
    NTEST_ASSERT_EQ_INT(cooker_pack(c, pack_path), NPH_RESULT_OK);
    cooker_destroy(c);
    char db_path[1300];
    snprintf(db_path, sizeof db_path, "%s/%s", f->out_dir, COOK_DB_FILE_NAME);
    NTEST_ASSERT_TRUE(nph_fs_exists(db_path));
    char tmp_path[1400];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", db_path);
    NTEST_ASSERT_FALSE(nph_fs_exists(tmp_path)); // the save renamed its temporary file

    // The outputs are where the database says, under the source's
    // directory, and the source is keyed relative to the root.
    CookDb db;
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, f->alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&db, db_path), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 7U);
    for (u32 i = 0; i < cook_db_count(&db); ++i) {
        const CookDbEntry *e = cook_db_entry(&db, i);
        char full[1500];
        snprintf(full, sizeof full, "%s/%s", f->out_dir, e->output);
        NTEST_ASSERT_TRUE(nph_fs_exists(full));
        NTEST_ASSERT_TRUE(
            nph_str_starts_with(nph_str_from_cstr(e->output), NPH_STR("scene_gltf/")));
        NTEST_ASSERT_EQ_STR(e->source, "scene.gltf");
        // The GUID is the key and sub-asset name, hashed.
        char text[300];
        snprintf(text, sizeof text, "scene.gltf#%s", e->sub);
        NTEST_ASSERT_TRUE(nph_guid_equals(e->guid, nph_guid_from_cstr(text)));
    }
    const CookDbEntry *scene = cook_db_find(&db, NPH_STR("scene.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(scene);
    NTEST_ASSERT_EQ_UINT(scene->dep_count, 4U); // 2 meshes + 2 materials
    cook_db_free(&db);

    // A second run finds everything current.
    c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    st = cooker_stats(c);
    NTEST_ASSERT_EQ_UINT(st.cooked, 0U);
    NTEST_ASSERT_EQ_UINT(st.skipped, 7U);
    cooker_destroy(c);
    // Forcing recooks.
    c = make_cooker(f, true);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cooker_stats(c).cooked, 7U);
    cooker_destroy(c);
    // The same content with an external buffer and image: the document
    // changed, so meshes, materials, and the scene recook; the textures
    // depend on their images' bytes only, which did not change.
    test_write_gltf(&f->dir, "scene", true);
    c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    st = cooker_stats(c);
    NTEST_ASSERT_EQ_UINT(st.cooked, 5U);
    NTEST_ASSERT_EQ_UINT(st.skipped, 2U);
    NTEST_ASSERT_EQ_UINT(st.removed, 0U);
    NTEST_ASSERT_EQ_UINT(cook_db_count(cooker_db(c)), 7U); // replaced, not duplicated
    NTEST_ASSERT_EQ_INT(cooker_pack(c, pack_path), NPH_RESULT_OK);
    cooker_destroy(c);

    load_everything(f, pack_path);
}

NTEST_F(CookFixture, assets_a_source_no_longer_produces_are_removed)
{
    char path[1200];
    test_path_in(&f->dir, "multi.gltf", path, sizeof path);
    write_triangles_gltf(path, 3U);
    Cooker *c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    // Three meshes, the default material, the scene.
    NTEST_ASSERT_EQ_UINT(count_entries_of(cooker_db(c), "multi.gltf"), 5U);
    const CookDbEntry *third = cook_db_find(cooker_db(c), NPH_STR("multi.gltf"), NPH_STR("mesh/2"));
    NTEST_ASSERT_NOT_NULL(third);
    char third_file[1500];
    snprintf(third_file, sizeof third_file, "%s/%s", f->out_dir, third->output);
    NTEST_ASSERT_TRUE(nph_fs_exists(third_file));
    cooker_destroy(c);
    // The document loses its third mesh.
    write_triangles_gltf(path, 2U);
    c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cooker_stats(c).removed, 1U);
    NTEST_ASSERT_EQ_UINT(count_entries_of(cooker_db(c), "multi.gltf"), 4U);
    NTEST_ASSERT_NULL(cook_db_find(cooker_db(c), NPH_STR("multi.gltf"), NPH_STR("mesh/2")));
    NTEST_ASSERT_FALSE(nph_fs_exists(third_file));
    // The pack holds what the database holds: the stale mesh is gone.
    char pack_path[1200];
    test_path_in(&f->dir, "multi.npk", pack_path, sizeof pack_path);
    NTEST_ASSERT_EQ_INT(cooker_pack(c, pack_path), NPH_RESULT_OK);
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, pack_path, &pack), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pack_entry_count(&pack), 4U);
    nph_pack_close(&pack);
    // A missing output stops the pack instead of leaving a hole in it.
    const CookDbEntry *first = cook_db_find(cooker_db(c), NPH_STR("multi.gltf"), NPH_STR("mesh/0"));
    NTEST_ASSERT_NOT_NULL(first);
    char first_file[1500];
    snprintf(first_file, sizeof first_file, "%s/%s", f->out_dir, first->output);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_file(first_file), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_pack(c, pack_path), NPH_RESULT_NOT_FOUND);
    cooker_destroy(c);
}

NTEST_F(CookFixture, source_keys_are_relative_to_the_root)
{
    Cooker *c = make_cooker(f, false);
    char key[1200];
    NTEST_ASSERT_EQ_INT(cooker_source_key(c, f->gltf, key, sizeof key), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(key, "scene.gltf");
    // The same file spelled with "." and ".." components, and with the
    // other separator on Windows, has the same key.
    char odd[1400];
    snprintf(odd, sizeof odd, "%s/./sub/../scene.gltf", f->dir.dir);
    NTEST_ASSERT_EQ_INT(cooker_source_key(c, odd, key, sizeof key), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(key, "scene.gltf");
    snprintf(odd, sizeof odd, "%s/models/rock.gltf", f->dir.dir);
    NTEST_ASSERT_EQ_INT(cooker_source_key(c, odd, key, sizeof key), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(key, "models/rock.gltf");
    // Outside the root: refused, including a sibling that shares a prefix.
    snprintf(odd, sizeof odd, "%s/../elsewhere.gltf", f->dir.dir);
    NTEST_ASSERT_EQ_INT(cooker_source_key(c, odd, key, sizeof key), NPH_RESULT_INVALID_ARGUMENT);
    snprintf(odd, sizeof odd, "%sx/scene.gltf", f->dir.dir);
    NTEST_ASSERT_EQ_INT(cooker_source_key(c, odd, key, sizeof key), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, odd), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(cooker_stats(c).failed, 1U);
    cooker_destroy(c);
}

NTEST_F(CookFixture, a_packed_occlusion_map_shares_the_metallic_roughness_texture)
{
    // glTF's "ORM" packing: one image for occlusion (R), roughness (G),
    // and metallic (B), and another for the base colour.
    u8 orm[4 * 4 * 4];
    u8 base[4 * 4 * 4];
    for (u32 i = 0; i < 16U; ++i) {
        orm[(u64)i * 4U] = 200U;
        orm[i * 4U + 1U] = (u8)(i * 10U);
        orm[i * 4U + 2U] = 30U;
        orm[i * 4U + 3U] = 255U;
        base[(u64)i * 4U] = 90U;
        base[i * 4U + 1U] = 90U;
        base[i * 4U + 2U] = (u8)(i * 15U);
        base[i * 4U + 3U] = 255U;
    }
    u8 orm_tga[18 + 64];
    u8 base_tga[18 + 64];
    char orm_b64[256];
    char base_b64[256];
    test_base64(orm_tga, test_write_tga(orm, 4U, 4U, orm_tga), orm_b64);
    test_base64(base_tga, test_write_tga(base, 4U, 4U, base_tga), base_b64);
    const f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const f32 uv[6] = {0, 1, 1, 1, 0, 0};
    u8 buffer[60];
    memcpy(buffer, pos, sizeof pos);
    memcpy(buffer + 36, uv, sizeof uv);
    char buffer_b64[128];
    test_base64(buffer, sizeof buffer, buffer_b64);
    char doc[4096];
    snprintf(doc, sizeof doc,
             "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
             "\"nodes\":[{\"mesh\":0}],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
             "\"material\":0}]}],"
             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1},"
             "\"metallicRoughnessTexture\":{\"index\":0}},\"occlusionTexture\":{\"index\":0,"
             "\"strength\":0.8}}],"
             "\"textures\":[{\"source\":0},{\"source\":1}],"
             "\"images\":[{\"uri\":\"data:image/tga;base64,%s\"},{\"uri\":\"data:image/tga;base64,"
             "%s\"}],"
             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":"
             "\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},{\"bufferView\":1,\"componentType\":5126,"
             "\"count\":3,\"type\":\"VEC2\"}],"
             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,"
             "\"byteLength\":24}],"
             "\"buffers\":[{\"byteLength\":60,\"uri\":\"data:application/octet-stream;base64,%s\"}"
             "]}",
             orm_b64, base_b64, buffer_b64);
    char path[1200];
    test_path_in(&f->dir, "orm.gltf", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, doc, strlen(doc)), NPH_RESULT_OK);
    Cooker *c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    // Two textures (the ORM as BC7 linear, the base colour as BC7 sRGB),
    // one material, one mesh, one scene.
    NTEST_ASSERT_EQ_UINT(cooker_stats(c).cooked, 5U);
    const CookDbEntry *mat = cook_db_find(cooker_db(c), NPH_STR("orm.gltf"), NPH_STR("material/0"));
    NTEST_ASSERT_NOT_NULL(mat);
    NTEST_ASSERT_EQ_UINT(mat->dep_count, 2U);
    NTEST_ASSERT_NOT_NULL(
        cook_db_find(cooker_db(c), NPH_STR("orm.gltf"), NPH_STR("image/0/linear")));
    NTEST_ASSERT_NULL(cook_db_find(cooker_db(c), NPH_STR("orm.gltf"), NPH_STR("image/0/mask")));
    // The material points both slots at the same texture.
    char full[1500];
    snprintf(full, sizeof full, "%s/%s", f->out_dir, mat->output);
    void *bytes = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(full, f->alloc, &bytes, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(bytes, size, &info), NPH_RESULT_OK);
    const NphAssetBlobEntry *head = nph_asset_file_find(&info, NPH_MATERIAL_TAG_HEADER, 0U);
    NTEST_ASSERT_NOT_NULL(head);
    NphMaterialHeader h;
    memcpy(&h, nph_asset_file_blob(bytes, head), sizeof h);
    NTEST_ASSERT_TRUE(nph_guid_equals(h.textures[NPH_MATERIAL_TEXTURE_OCCLUSION].texture,
                                      h.textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].texture));
    NTEST_ASSERT_NEAR_F32(h.textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale, 0.8F, 1e-6F);
    nph_file_free_all(f->alloc, bytes, size);
    cooker_destroy(c);
}

NTEST_F(CookFixture, directories_and_colliding_names)
{
    // a/x.gltf and b/y.gltf under a directory, with a file that is not a
    // scene beside them; and two sources whose names sanitise alike.
    char sub[1200];
    test_path_in(&f->dir, "content/a", sub, sizeof sub);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(sub), NPH_RESULT_OK);
    test_path_in(&f->dir, "content/b", sub, sizeof sub);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(sub), NPH_RESULT_OK);
    char path[1200];
    write_triangles_gltf(test_path_in(&f->dir, "content/a/x.gltf", path, sizeof path), 1U);
    write_triangles_gltf(test_path_in(&f->dir, "content/b/y.gltf", path, sizeof path), 1U);
    write_triangles_gltf(test_path_in(&f->dir, "content/a b.gltf", path, sizeof path), 1U);
    write_triangles_gltf(test_path_in(&f->dir, "content/a_b.gltf", path, sizeof path), 1U);
    test_path_in(&f->dir, "content/readme.txt", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, "notes", 5U), NPH_RESULT_OK);
    Cooker *c = make_cooker(f, false);
    char content[1200];
    test_path_in(&f->dir, "content", content, sizeof content);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, content), NPH_RESULT_OK);
    const CookDb *db = cooker_db(c);
    NTEST_ASSERT_EQ_UINT(count_entries_of(db, "content/a/x.gltf"), 3U); // mesh, default, scene
    NTEST_ASSERT_EQ_UINT(count_entries_of(db, "content/b/y.gltf"), 3U);
    NTEST_ASSERT_EQ_UINT(count_entries_of(db, "content/a b.gltf"), 3U);
    NTEST_ASSERT_EQ_UINT(count_entries_of(db, "content/a_b.gltf"), 3U);
    const CookDbEntry *x = cook_db_find(db, NPH_STR("content/a/x.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(x);
    NTEST_ASSERT_EQ_STR(x->output, "content/a/x_gltf/scene.nph");
    // "a b" and "a_b" both sanitise to "a_b": the second gets a suffix.
    const CookDbEntry *s1 = cook_db_find(db, NPH_STR("content/a b.gltf"), NPH_STR("scene"));
    const CookDbEntry *s2 = cook_db_find(db, NPH_STR("content/a_b.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(s1);
    NTEST_ASSERT_NOT_NULL(s2);
    NTEST_ASSERT_TRUE(strcmp(s1->output, s2->output) != 0);
    // Every output is distinct, so the pack takes them all.
    char pack_path[1200];
    test_path_in(&f->dir, "all.npk", pack_path, sizeof pack_path);
    NTEST_ASSERT_EQ_INT(cooker_pack(c, pack_path), NPH_RESULT_OK);
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, pack_path, &pack), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pack_entry_count(&pack), 12U);
    nph_pack_close(&pack);
    cooker_destroy(c);
}

NTEST_F(CookFixture, cooks_standalone_images_and_rejects_unknown_files)
{
    u8 rgba[4 * 4 * 4];
    for (u32 i = 0; i < 16U; ++i) {
        rgba[(u64)i * 4U] = (u8)(i * 16U);
        rgba[i * 4U + 1U] = 128U;
        rgba[i * 4U + 2U] = 255U;
        rgba[i * 4U + 3U] = 255U;
    }
    u8 tga[18 + 64];
    const u64 len = test_write_tga(rgba, 4U, 4U, tga);
    char path[1200];
    test_path_in(&f->dir, "Rough Metal.tga", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, len), NPH_RESULT_OK);
    Cooker *c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_image(c, path, COOK_TEXTURE_LINEAR), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK); // default usage: colour
    char other[1200];
    test_path_in(&f->dir, "notes.txt", other, sizeof other);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(other, "x", 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, other), NPH_RESULT_UNSUPPORTED);
    test_path_in(&f->dir, "missing.png", other, sizeof other);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, other), NPH_RESULT_NOT_FOUND);
    // A file that claims to be an image but is not.
    test_path_in(&f->dir, "fake.png", other, sizeof other);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(other, "not a png", 9U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, other), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(cooker_cook_image(c, path, (CookTextureUsage)9),
                        NPH_RESULT_INVALID_ARGUMENT);
    const CookerStats st = cooker_stats(c);
    NTEST_ASSERT_EQ_UINT(st.cooked, 2U); // linear and colour usages are distinct assets
    NTEST_ASSERT_EQ_UINT(st.failed, 2U);
    const CookDbEntry *lin =
        cook_db_find(cooker_db(c), NPH_STR("Rough Metal.tga"), NPH_STR("linear"));
    NTEST_ASSERT_NOT_NULL(lin);
    NTEST_ASSERT_EQ_STR(lin->output, "Rough_Metal_tga/tex_linear.nph");
    cooker_destroy(c);
}

#ifdef NPH_COOK_EXE
static int run_command(const char *command)
{
#if NPH_PLATFORM_WINDOWS
    // cmd.exe strips the outer quotes of a command line that begins with
    // one; wrapping the whole thing in another pair keeps every path intact.
    char wrapped[4200];
    snprintf(wrapped, sizeof wrapped, "\"%s\"", command);
    // The tool runs from a shell as a user runs it; the command is built
    // from the test's own paths.
    return system(wrapped); // NOLINT(cert-env33-c)
#else
    const int status = system(command); // NOLINT(cert-env33-c): as above
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

NTEST_F(CookFixture, the_command_line_tool_cooks_and_packs)
{
    char pack_path[1200];
    test_path_in(&f->dir, "cli.npk", pack_path, sizeof pack_path);
    char command[4096];
    snprintf(command, sizeof command,
             "\"%s\" \"%s\" -o \"%s\" --root \"%s\" --pack \"%s\" --lz fast --threads 2 --quiet",
             NPH_COOK_EXE, f->gltf, f->out_dir, f->dir.dir, pack_path);
    NTEST_ASSERT_EQ_INT(run_command(command), 0);
    NTEST_ASSERT_TRUE(nph_fs_exists(pack_path));
    load_everything(f, pack_path);
    // A source outside the root fails the run (exit 1); usage errors exit 2.
    snprintf(command, sizeof command, "\"%s\" \"%s\" -o \"%s\" --root \"%s/out\" --quiet",
             NPH_COOK_EXE, f->gltf, f->out_dir, f->dir.dir);
    NTEST_ASSERT_EQ_INT(run_command(command), 1);
    snprintf(command, sizeof command, "\"%s\" --bogus", NPH_COOK_EXE);
    NTEST_ASSERT_EQ_INT(run_command(command), 2);
}
#endif

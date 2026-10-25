// SPDX-License-Identifier: Apache-2.0
// End to end: cook generated glTF scenes, pack them, mount the pack, load
// everything through the loader, and resolve every reference; and the
// cooker's bookkeeping: source keys, incremental decisions, stale assets,
// output paths, directories.
#include <nepheline/asset/loader.h>
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/sound.h>
#include <nepheline/asset/texture.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/audio/wav.h>

#include "test_cook_support.h"

#include <cook/cooker.h>
#include <cook/polymesh.h>
#include <cook/specgloss.h>
#include <stdlib.h>
#if !NPH_PLATFORM_WINDOWS
#include <sys/wait.h>
#endif

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

NTEST(cooker, canonical_and_relative_paths)
{
    char cwd[1024];
    NTEST_ASSERT_EQ_INT(nph_fs_current_directory(cwd, sizeof cwd, NULL), NPH_RESULT_OK);
    char cwd_canonical[1024];
    NTEST_ASSERT_EQ_INT(cooker_canonical_path(cwd, cwd_canonical, sizeof cwd_canonical),
                        NPH_RESULT_OK);
    char out[1200];
    char expected[1200];
    // Relative paths start at the working directory; "." and ".." go.
    NTEST_ASSERT_EQ_INT(cooker_canonical_path("a/./b/../c/", out, sizeof out), NPH_RESULT_OK);
    snprintf(expected, sizeof expected, "%s/a/c", cwd_canonical);
    NTEST_EXPECT_EQ_STR(out, expected);
#if NPH_PLATFORM_WINDOWS
    NTEST_ASSERT_EQ_INT(cooker_canonical_path("C:\\x\\..\\..\\y\\", out, sizeof out),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "C:/y"); // ".." stops at the drive
    NTEST_ASSERT_EQ_INT(cooker_canonical_path("C:\\", out, sizeof out), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "C:/");
#else
    NTEST_ASSERT_EQ_INT(cooker_canonical_path("/x/../../y/", out, sizeof out), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "/y");
    NTEST_ASSERT_EQ_INT(cooker_canonical_path("/", out, sizeof out), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "/");
#endif
    char tiny[4];
    NTEST_EXPECT_EQ_INT(cooker_canonical_path("abcdef", tiny, sizeof tiny), NPH_RESULT_OVERFLOW);

    // What each kind of file is, by its extension in any case.
    NTEST_EXPECT_EQ_INT(cooker_source_kind("a/b.gltf"), COOKER_SOURCE_SCENE);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("B.GLB"), COOKER_SOURCE_SCENE);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("c.Png"), COOKER_SOURCE_IMAGE);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("sky.hdr"), COOKER_SOURCE_IMAGE);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("d.jpeg"), COOKER_SOURCE_IMAGE);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("buffer.bin"), COOKER_SOURCE_NONE);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("gltf"), COOKER_SOURCE_NONE);

    // Below a root, the root itself, and the paths that are not below it.
    NTEST_ASSERT_EQ_INT(cooker_relative_path("/p/assets", "/p/assets/m/a.gltf", out, sizeof out),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "m/a.gltf");
    NTEST_ASSERT_EQ_INT(cooker_relative_path("/p/assets", "/p/assets", out, sizeof out),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "");
    NTEST_ASSERT_EQ_INT(cooker_relative_path("/", "/p/a.gltf", out, sizeof out), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "p/a.gltf");
    NTEST_EXPECT_EQ_INT(cooker_relative_path("/p/assets", "/p/assets2/a.gltf", out, sizeof out),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cooker_relative_path("/p/assets", "/p", out, sizeof out),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cooker_relative_path("/p/assets", "/p/assets/abcd", tiny, sizeof tiny),
                        NPH_RESULT_OVERFLOW);
#if NPH_PLATFORM_WINDOWS
    NTEST_ASSERT_EQ_INT(cooker_relative_path("C:/P/Assets", "c:/p/assets/A.gltf", out, sizeof out),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(out, "A.gltf"); // the spelling given
#else
    NTEST_EXPECT_EQ_INT(cooker_relative_path("/P/Assets", "/p/assets/a.gltf", out, sizeof out),
                        NPH_RESULT_INVALID_ARGUMENT);
#endif
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

// A texture asset's top level, unpacked (`out` holds `cap` bytes).
static void read_top_level(CookFixture *f, const Cooker *c, NphGuid guid, u8 *out, u64 cap)
{
    const CookDbEntry *e = cook_db_find_guid(cooker_db(c), guid);
    NTEST_ASSERT_NOT_NULL(e);
    char full[1500];
    snprintf(full, sizeof full, "%s/%s", f->out_dir, e->output);
    void *bytes = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(full, f->alloc, &bytes, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(bytes, size, &info), NPH_RESULT_OK);
    const NphAssetBlobEntry *mip = nph_asset_file_find(&info, NPH_TEXTURE_TAG_MIP, 0U);
    NTEST_ASSERT_NOT_NULL(mip);
    NTEST_ASSERT_EQ_UINT(mip->raw_size, cap);
    NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(bytes, mip, out), NPH_RESULT_OK);
    nph_file_free_all(f->alloc, bytes, size);
}

static NphMaterialHeader read_material(CookFixture *f, const Cooker *c, const char *source,
                                       const char *sub)
{
    const CookDbEntry *mat =
        cook_db_find(cooker_db(c), nph_str_from_cstr(source), nph_str_from_cstr(sub));
    NTEST_ASSERT_NOT_NULL(mat);
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
    nph_file_free_all(f->alloc, bytes, size);
    return h;
}

// KHR_materials_pbrSpecularGlossiness: a material with textures gets a
// base colour and a metallic-roughness texture made from its two images
// (once for the materials that share them), carrying the factors; one with
// factors alone is converted to factors; a second cook finds all current.
NTEST_F(CookFixture, specular_glossiness_materials_become_metallic_roughness)
{
    u8 diffuse[4 * 4 * 4];
    u8 specgloss[4 * 4 * 4];
    for (u32 i = 0; i < 16U; ++i) {
        diffuse[(u64)i * 4U] = (u8)(40U + i * 10U);
        diffuse[i * 4U + 1U] = 120U;
        diffuse[i * 4U + 2U] = (u8)(200U - i * 5U);
        diffuse[i * 4U + 3U] = 255U;
        specgloss[(u64)i * 4U] = (u8)(i < 8U ? 59U : 240U); // a dielectric's 0.04, or metal
        specgloss[i * 4U + 1U] = (u8)(i < 8U ? 59U : 200U);
        specgloss[i * 4U + 2U] = (u8)(i < 8U ? 59U : 100U);
        specgloss[i * 4U + 3U] = (u8)(i * 16U);
    }
    u8 diffuse_tga[18 + 64];
    u8 specgloss_tga[18 + 64];
    char diffuse_b64[256];
    char specgloss_b64[256];
    test_base64(diffuse_tga, test_write_tga(diffuse, 4U, 4U, diffuse_tga), diffuse_b64);
    test_base64(specgloss_tga, test_write_tga(specgloss, 4U, 4U, specgloss_tga), specgloss_b64);
    const f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const f32 uv[6] = {0, 1, 1, 1, 0, 0};
    u8 buffer[60];
    memcpy(buffer, pos, sizeof pos);
    memcpy(buffer + 36, uv, sizeof uv);
    char buffer_b64[128];
    test_base64(buffer, sizeof buffer, buffer_b64);
    static const char sg_textures[] =
        "{\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{\"diffuseTexture\":{\"index\":"
        "0},\"specularGlossinessTexture\":{\"index\":1},\"glossinessFactor\":0.5}}}";
    char doc[4096];
    snprintf(doc, sizeof doc,
             "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":["
             "\"KHR_materials_pbrSpecularGlossiness\"],\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
             "\"nodes\":[{\"mesh\":0}],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
             "\"material\":0}]}],"
             "\"materials\":[%s,%s,{\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
             "\"diffuseFactor\":[0.5,0.25,0.125,1],\"specularFactor\":[0.04,0.04,0.04],"
             "\"glossinessFactor\":0.25}}}],"
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
             sg_textures, sg_textures, diffuse_b64, specgloss_b64, buffer_b64);
    char path[1200];
    test_path_in(&f->dir, "sg.gltf", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, doc, strlen(doc)), NPH_RESULT_OK);
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.texture.lz = NPH_LZ_LEVEL_FAST;
    desc.texture.uncompressed = 1; // RGBA8 levels the test can read
    desc.texture.no_mips = 1;
    desc.mesh.lz = NPH_LZ_LEVEL_FAST;
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    // One texture pair for the two materials that share their images, three
    // materials, one mesh, one scene.
    NTEST_ASSERT_EQ_UINT(cooker_stats(c).cooked, 7U);
    const NphMaterialHeader m0 = read_material(f, c, "sg.gltf", "material/0");
    const NphMaterialHeader m1 = read_material(f, c, "sg.gltf", "material/1");
    const NphMaterialHeader m2 = read_material(f, c, "sg.gltf", "material/2");
    const NphGuid base = m0.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture;
    const NphGuid mr = m0.textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].texture;
    NTEST_ASSERT_FALSE(nph_guid_is_null(base));
    NTEST_ASSERT_FALSE(nph_guid_is_null(mr));
    NTEST_ASSERT_FALSE(nph_guid_equals(base, mr));
    NTEST_ASSERT_TRUE(nph_guid_equals(m1.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture, base));
    NTEST_ASSERT_TRUE(
        nph_guid_equals(m1.textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].texture, mr));
    // The textures carry the factors.
    NTEST_EXPECT_NEAR_F32(m0.base_color_factor.x, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m0.metallic_factor, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m0.roughness_factor, 1.0F, 0.0F);
    // Each texel is the conversion of the texels under it.
    u8 base_texels[4 * 4 * 4];
    u8 mr_texels[4 * 4 * 4];
    read_top_level(f, c, base, base_texels, sizeof base_texels);
    read_top_level(f, c, mr, mr_texels, sizeof mr_texels);
    for (u32 i = 0; i < 16U; ++i) {
        CookSpecGloss sg;
        const u8 *d = diffuse + (u64)i * 4U;
        const u8 *s = specgloss + (u64)i * 4U;
        sg.diffuse = nph_vec4(cook_srgb_to_linear((f32)d[0] / 255.0F),
                              cook_srgb_to_linear((f32)d[1] / 255.0F),
                              cook_srgb_to_linear((f32)d[2] / 255.0F), 1.0F);
        sg.specular = nph_vec3(cook_srgb_to_linear((f32)s[0] / 255.0F),
                               cook_srgb_to_linear((f32)s[1] / 255.0F),
                               cook_srgb_to_linear((f32)s[2] / 255.0F));
        sg.glossiness = (f32)s[3] / 255.0F * 0.5F;
        const CookMetalRough m = cook_specgloss_to_metalrough(sg);
        NTEST_EXPECT_NEAR_F32((f32)base_texels[(u64)i * 4U],
                              cook_linear_to_srgb(m.base_color.x) * 255.0F, 0.51F);
        NTEST_EXPECT_NEAR_F32((f32)mr_texels[i * 4U + 1U], m.roughness * 255.0F, 0.51F);
        NTEST_EXPECT_NEAR_F32((f32)mr_texels[i * 4U + 2U], m.metallic * 255.0F, 0.51F);
    }
    // Factors alone: converted on import (a dielectric: the diffuse is the
    // base colour), no textures.
    NTEST_ASSERT_TRUE(nph_guid_is_null(m2.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture));
    NTEST_EXPECT_NEAR_F32(m2.base_color_factor.x, 0.5F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(m2.base_color_factor.y, 0.25F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(m2.metallic_factor, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m2.roughness_factor, 0.75F, 1e-6F);
    cooker_destroy(c);
    // Nothing changed: everything is current.
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 0U);
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

typedef struct CookedLog {
    NphGuid guids[16];
    u32 types[16];
    u32 count;
} CookedLog;

static void note_cooked(void *user, NphGuid guid, u32 type)
{
    CookedLog *log = (CookedLog *)user;
    NTEST_ASSERT_LT_UINT(log->count, 16U);
    log->guids[log->count] = guid;
    log->types[log->count] = type;
    ++log->count;
}

NTEST_F(CookFixture, cooks_editable_meshes_and_tells_what_it_wrote)
{
    CookPolyMesh m;
    cook_polymesh_init(&m, f->alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_cylinder(&m, 0.5F, 1.0F, 12U), NPH_RESULT_OK);
    char dir[1200];
    char path[1300];
    test_path_in(&f->dir, "meshes", dir, sizeof dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    (void)snprintf(path, sizeof path, "%s/drum.nmesh", dir);
    NTEST_ASSERT_EQ_INT(cook_polymesh_save(&m, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_source_kind(path), COOKER_SOURCE_MESH);

    CookedLog log;
    memset(&log, 0, sizeof log);
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.mesh.lz = NPH_LZ_LEVEL_FAST;
    desc.on_cooked = note_cooked;
    desc.on_cooked_user = &log;
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    const NphGuid guid = nph_guid_from_cstr("meshes/drum.nmesh#mesh");
    NTEST_ASSERT_EQ_UINT(log.count, 1U);
    NTEST_EXPECT_TRUE(nph_guid_equals(log.guids[0], guid));
    NTEST_EXPECT_EQ_UINT(log.types[0], (u32)NPH_ASSET_TYPE_MESH);
    const CookDbEntry *e =
        cook_db_find(cooker_db(c), NPH_STR("meshes/drum.nmesh"), NPH_STR("mesh"));
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_EXPECT_TRUE(nph_guid_equals(e->guid, guid));
    NTEST_EXPECT_EQ_STR(e->output, "meshes/drum_nmesh/mesh.nph");
    // The cooked file is a mesh the runtime can view: the cylinder's
    // triangles, 12 per side band and 10 per cap.
    char cooked[1400];
    (void)snprintf(cooked, sizeof cooked, "%s/%s", f->out_dir, e->output);
    void *image = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(cooked, f->alloc, &image, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(image, size, &info), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(info.header.type, (u32)NPH_ASSET_TYPE_MESH);
    nph_file_free_all(f->alloc, image, size);

    // Current: skipped, and nothing told. Changed: cooked again.
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(log.count, 1U);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).skipped, 1U);
    m.positions.data[0].y += 0.25F;
    NTEST_ASSERT_EQ_INT(cook_polymesh_save(&m, path), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(log.count, 2U);

    // A directory's editable meshes cook with its scenes.
    char second[1300];
    (void)snprintf(second, sizeof second, "%s/box.nmesh", dir);
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 1.0F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_polymesh_save(&m, second), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_directory(c, dir), NPH_RESULT_OK);
    NTEST_EXPECT_NOT_NULL(cook_db_find(cooker_db(c), NPH_STR("meshes/box.nmesh"), NPH_STR("mesh")));

    // A mesh without faces, and a file that is not one, are refused.
    char empty[1300];
    (void)snprintf(empty, sizeof empty, "%s/empty.nmesh", dir);
    cook_polymesh_clear(&m);
    NTEST_ASSERT_EQ_INT(cook_polymesh_save(&m, empty), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_mesh(c, empty), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(empty, "{ nope", 6U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_mesh(c, empty), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).failed, 2U);
    cooker_destroy(c);
    cook_polymesh_free(&m);
}

// A WAV file at `path`: `seconds` of a mono 16-bit tone at 8 kHz.
static void write_tone_wav(const char *path, f64 seconds, f64 freq)
{
    NphAllocator *a = nph_allocator_system();
    const u64 frames = (u64)(seconds * 8000.0);
    f32 *x = (f32 *)NPH_ALLOC(a, frames * sizeof(f32), 16U);
    NTEST_ASSERT_NOT_NULL(x);
    for (u64 i = 0U; i < frames; ++i) {
        x[i] = (f32)(0.5 * sin(2.0 * 3.14159265358979323846 * freq * (f64)i / 8000.0));
    }
    const f32 *planes[1] = {x};
    const u64 size = nph_wav_size(NPH_WAV_PCM_S16, 1U, frames);
    void *image = NPH_ALLOC(a, size, 16U);
    NTEST_ASSERT_NOT_NULL(image);
    NTEST_ASSERT_EQ_INT(nph_wav_write(NPH_WAV_PCM_S16, 1U, 8000U, planes, frames, image),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, image, size), NPH_RESULT_OK);
    NPH_FREE(a, image, size, 16U);
    NPH_FREE(a, x, frames * sizeof(f32), 16U);
}

// The sound at `vpath` through the loader: its header, and for one that
// streams, DATA read again through the VFS where the view says it lies.
static NphSoundHeader load_sound(NphVfs *vfs, NphAssetLoader *loader, const char *vpath)
{
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(loader, nph_str_from_cstr(vpath), &h), NPH_RESULT_OK);
    const NphAsset *asset = nph_asset_get(loader, h);
    NphSoundView v;
    NTEST_ASSERT_EQ_INT(nph_sound_view(asset, &v), NPH_RESULT_OK);
    const NphSoundHeader header = *v.header;
    if ((header.flags & NPH_SOUND_FLAG_STREAM) != 0U) {
        NphVfsFile file;
        NTEST_ASSERT_EQ_INT(nph_vfs_open(vfs, asset->path, 0U, &file), NPH_RESULT_OK);
        u8 *bytes = (u8 *)NPH_ALLOC(nph_allocator_system(), v.size, 16U);
        NTEST_ASSERT_NOT_NULL(bytes);
        u64 got = 0U;
        NTEST_EXPECT_EQ_INT(nph_vfs_read_at(&file, v.offset, bytes, v.size, &got), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(got, v.size);
        NTEST_EXPECT_TRUE(memcmp(bytes, v.data, (size_t)v.size) == 0);
        NPH_FREE(nph_allocator_system(), bytes, v.size, 16U);
        nph_vfs_close(vfs, &file);
    }
    nph_asset_release(loader, h);
    return header;
}

NTEST_F(CookFixture, cooks_sounds_with_their_settings_files)
{
    char dir[1200];
    test_path_in(&f->dir, "sounds", dir, sizeof dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    char boom[1300];
    char music[1300];
    char settings[1400];
    (void)snprintf(boom, sizeof boom, "%s/boom.wav", dir);
    (void)snprintf(music, sizeof music, "%s/music.wav", dir);
    (void)snprintf(settings, sizeof settings, "%s.import.json", boom);
    write_tone_wav(boom, 0.5, 440.0);
    write_tone_wav(music, 9.0, 220.0);
    NTEST_EXPECT_EQ_INT(cooker_source_kind(boom), COOKER_SOURCE_SOUND);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("LOUD.WAV"), COOKER_SOURCE_SOUND);

    // A directory's sounds cook with it: the short one to play from memory,
    // the long one to stream.
    Cooker *c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_directory(c, dir), NPH_RESULT_OK);
    const CookDbEntry *e = cook_db_find(cooker_db(c), NPH_STR("sounds/boom.wav"), NPH_STR("sound"));
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_EXPECT_EQ_UINT(e->type, (u32)NPH_ASSET_TYPE_SOUND);
    NTEST_EXPECT_EQ_STR(e->output, "sounds/boom_wav/sound.nph");
    NTEST_EXPECT_TRUE(nph_guid_equals(e->guid, nph_guid_from_cstr("sounds/boom.wav#sound")));
    NTEST_ASSERT_NOT_NULL(
        cook_db_find(cooker_db(c), NPH_STR("sounds/music.wav"), NPH_STR("sound")));
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 2U);

    // Loaded through a VFS and the loader, as the runtime loads them; the
    // streamed one's samples read again from the file where its view says.
    NphVfs *vfs = NULL;
    NTEST_ASSERT_EQ_INT(nph_vfs_create(f->alloc, &vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(vfs, nph_str_empty(), f->out_dir, NULL),
                        NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = f->alloc;
    ld.vfs = vfs;
    ld.verify = NPH_ASSET_VERIFY_ALWAYS;
    NphAssetLoader *loader = NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &loader), NPH_RESULT_OK);
    NphSoundHeader h = load_sound(vfs, loader, "sounds/boom_wav/sound.nph");
    NTEST_EXPECT_EQ_UINT(h.codec, (u32)NPH_SOUND_CODEC_PCM);
    NTEST_EXPECT_EQ_UINT(h.format, (u32)NPH_SOUND_FORMAT_S16);
    NTEST_EXPECT_EQ_UINT(h.flags, 0U);
    NTEST_EXPECT_EQ_UINT(h.frames, 4000U);
    h = load_sound(vfs, loader, "sounds/music_wav/sound.nph");
    NTEST_EXPECT_EQ_UINT(h.codec, (u32)NPH_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_UINT(h.flags, (u32)NPH_SOUND_FLAG_STREAM);
    NTEST_EXPECT_EQ_UINT(h.frames, 72000U);

    // Current: skipped. A settings file beside a sound cooks it again, its
    // way; a changed settings file too.
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, boom), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).skipped, 1U);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(settings, "{\"codec\": \"adpcm\"}", 18U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, boom), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 3U);
    // (load_sound released the first: this loads the new file.)
    h = load_sound(vfs, loader, "sounds/boom_wav/sound.nph");
    NTEST_EXPECT_EQ_UINT(h.codec, (u32)NPH_SOUND_CODEC_ADPCM);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(settings, "{\"stream\": \"always\"}", 20U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, boom), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 4U);

    // A settings file the cooker cannot take, and a file that is not a
    // WAV, fail their sources.
    NTEST_ASSERT_EQ_INT(nph_file_write_all(settings, "{\"codec\": \"mp3\"}", 16U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_sound(c, boom), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(settings, "{ nope", 6U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_sound(c, boom), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_file(settings), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(boom, "RIFX----WAVE", 12U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_sound(c, boom), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).failed, 3U);
    nph_asset_loader_destroy(loader);
    nph_vfs_destroy(vfs);
    cooker_destroy(c);
}

#ifdef NPH_COOK_EXE
static int run_command(const char *command)
{
#if NPH_PLATFORM_WINDOWS
    // cmd.exe strips the outer quotes of a command line that begins with
    // one; wrapping the whole thing in another pair keeps every path intact.
    char wrapped[8200];
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
    char command[8192];
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

NTEST_F(CookFixture, the_command_line_tool_takes_the_sound_options)
{
    char wav[1200];
    test_path_in(&f->dir, "hit.wav", wav, sizeof wav);
    write_tone_wav(wav, 0.25, 880.0);
    char command[8192];
    snprintf(command, sizeof command,
             "\"%s\" \"%s\" -o \"%s\" --root \"%s\" --sound-codec lossless --sound-stream "
             "always --stream-seconds 2 --quiet",
             NPH_COOK_EXE, wav, f->out_dir, f->dir.dir);
    NTEST_ASSERT_EQ_INT(run_command(command), 0);
    char cooked[1400];
    (void)snprintf(cooked, sizeof cooked, "%s/hit_wav/sound.nph", f->out_dir);
    void *image = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(cooked, f->alloc, &image, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(image, size, &info), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(info.header.type, (u32)NPH_ASSET_TYPE_SOUND);
    const NphAssetBlobEntry *head = nph_asset_file_find(&info, NPH_SOUND_TAG_HEADER, 0U);
    NTEST_ASSERT_NOT_NULL(head);
    NphSoundHeader h;
    memcpy(&h, nph_asset_file_blob(image, head), sizeof h);
    NTEST_EXPECT_EQ_UINT(h.codec, (u32)NPH_SOUND_CODEC_LOSSLESS);
    NTEST_EXPECT_EQ_UINT(h.flags, (u32)NPH_SOUND_FLAG_STREAM);
    nph_file_free_all(f->alloc, image, size);
    // Values the tool does not know are usage errors.
    static const char *const bad[] = {"--sound-codec mp3", "--sound-stream maybe",
                                      "--stream-seconds 0", "--stream-seconds x"};
    for (u32 i = 0U; i < NPH_ARRAY_COUNT(bad); ++i) {
        snprintf(command, sizeof command, "\"%s\" \"%s\" -o \"%s\" %s --quiet", NPH_COOK_EXE, wav,
                 f->out_dir, bad[i]);
        NTEST_EXPECT_EQ_INT(run_command(command), 2);
    }
}
#endif

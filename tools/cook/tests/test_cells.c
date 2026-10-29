// SPDX-License-Identifier: Apache-2.0
// The world partition's cells (cook/cells.h; ADR-0062): instances placed by
// their centres; a glTF scene cooked with cells writes each cell's proxy (a
// mesh of voxels alone) and its scene's cells over its instances, finds them
// current the next time, and drops them when cells are no longer asked for.
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/platform/file.h>

#include "test_cook_support.h"

#include <cook/cells.h>
#include <cook/cooker.h>
#include <math.h>

// ---------------------------------------------------------------------------
// The partition
// ---------------------------------------------------------------------------

static CookCellInstance placed_at(f32 x, f32 y, f32 z, f32 radius, bool cellable)
{
    CookCellInstance ci;
    memset(&ci, 0, sizeof ci);
    ci.transform[0][0] = 1.0;
    ci.transform[1][1] = 1.0;
    ci.transform[2][2] = 1.0;
    ci.transform[0][3] = (f64)x;
    ci.transform[1][3] = (f64)y;
    ci.transform[2][3] = (f64)z;
    ci.sphere = nph_sphere(nph_vec3(x, y, z), radius);
    ci.cellable = cellable;
    return ci;
}

NTEST(cells, the_partition_places_instances_by_their_centres)
{
    NphAllocator *a = nph_allocator_system();
    const CookCellInstance instances[6] = {
        placed_at(1.0F, 0.0F, 1.0F, 1.0F, true),  // square (0, 0)
        placed_at(-1.0F, 0.0F, 1.0F, 1.0F, true), // (-1, 0)
        placed_at(9.0F, 5.0F, -3.0F, 2.0F, true), // (1, -1)
        placed_at(3.0F, 0.0F, 3.0F, 1.0F, true),  // (0, 0)
        placed_at(0.0F, 0.0F, 0.0F, 5.0F, true),  // past half the edge: none
        placed_at(2.0F, 0.0F, 2.0F, 1.0F, false), // does not voxelize: none
    };
    NphArrayU32 order;
    CookCellArray cells;
    NPH_ARRAY_INIT(&order, a);
    NPH_ARRAY_INIT(&cells, a);
    NTEST_ASSERT_EQ_INT(cook_cells_partition(instances, 6U, 8.0F, &order, &cells), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cells.len, 3U);
    NTEST_ASSERT_EQ_UINT(order.len, 6U);
    // The cells in order of (z, x), each one's instances together, then
    // those of none in their own order.
    const i32 squares[3][2] = {{1, -1}, {-1, 0}, {0, 0}};
    const u32 counts[3] = {1U, 1U, 2U};
    for (u32 k = 0; k < 3U; ++k) {
        NTEST_EXPECT_EQ_INT(cells.data[k].x, squares[k][0]);
        NTEST_EXPECT_EQ_INT(cells.data[k].z, squares[k][1]);
        NTEST_EXPECT_EQ_UINT(cells.data[k].count, counts[k]);
    }
    const u32 expected[6] = {2U, 1U, 0U, 3U, 4U, 5U};
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_EXPECT_EQ_UINT(order.data[i], expected[i]);
    }
    NTEST_EXPECT_EQ_UINT(cells.data[2].first, 2U);
    // A cell's bounds hold its instances' spheres.
    NTEST_EXPECT_NEAR_F32(cells.data[2].bounds.min.x, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(cells.data[2].bounds.max.x, 4.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(cells.data[2].bounds.min.y, -1.0F, 0.0F);
    // No edge, or one not finite.
    NTEST_EXPECT_EQ_INT(cook_cells_partition(instances, 6U, 0.0F, &order, &cells),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_cells_partition(instances, 6U, NPH_F32_INFINITY, &order, &cells),
                        NPH_RESULT_INVALID_ARGUMENT);
    NPH_ARRAY_FREE(&order);
    NPH_ARRAY_FREE(&cells);
}

NTEST(cells, the_finest_cell_is_a_pixel_at_the_distance)
{
    CookHlodSettings s;
    memset(&s, 0, sizeof s);
    // 560 m at 4.47e-4 m a metre: 0.25 m (a power of two at most 0.2503).
    NTEST_EXPECT_NEAR_F32(cook_hlod_finest_cell(&s), 0.25F, 0.0F);
    s.distance = 1000.0F;
    NTEST_EXPECT_NEAR_F32(cook_hlod_finest_cell(&s), 0.25F, 0.0F); // 0.447
    s.distance = 1200.0F;
    NTEST_EXPECT_NEAR_F32(cook_hlod_finest_cell(&s), 0.5F, 0.0F); // 0.536
}

// ---------------------------------------------------------------------------
// A scene cooked with cells
// ---------------------------------------------------------------------------

typedef struct CellsFixture {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
    char gltf[1200];
} CellsFixture;

NTEST_SETUP(CellsFixture)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "cells");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
    test_path_in(&f->dir, "grove.gltf", f->gltf, sizeof f->gltf);
}

NTEST_TEARDOWN(CellsFixture)
{
    test_temp_dir_remove(&f->dir);
}

// Writes the grove: a tuft of 200 needles (thin closed prisms, 0.3 m long,
// in a cube of 1.5 m round its origin), placed twice in each of four squares
// of 8 m, and a ground of 40 m under them (no cell's: past half an edge).
static void write_grove(const char *path)
{
    NphAllocator *a = nph_allocator_system();
    NPH_ARRAY(f32) pos;
    NphArrayU32 idx;
    NPH_ARRAY_INIT(&pos, a);
    NPH_ARRAY_INIT(&idx, a);
    u64 rng = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < 200U; ++i) {
        f32 c[3];
        f32 d[3];
        for (u32 k = 0; k < 3U; ++k) {
            c[k] = (f32)((f64)(test_rng_next(&rng) >> 11U) / 9007199254740992.0 * 1.5 - 0.75);
            d[k] = (f32)((f64)(test_rng_next(&rng) >> 11U) / 9007199254740992.0 * 2.0 - 1.0);
        }
        const NphVec3 axis = nph_vec3_normalize_or(nph_vec3(d[0], d[1], d[2]), nph_vec3_unit_y());
        const NphVec3 t = nph_vec3_normalize(nph_vec3_perpendicular(axis));
        const NphVec3 b = nph_vec3_cross(axis, t);
        const u32 base = pos.len / 3U;
        for (u32 end = 0; end < 2U; ++end) {
            const NphVec3 at = nph_vec3_add(nph_vec3(c[0], c[1], c[2]),
                                            nph_vec3_scale(axis, end == 0U ? -0.15F : 0.15F));
            for (u32 k = 0; k < 3U; ++k) {
                const f32 phi = 2.0F * NPH_PI * (f32)k / 3.0F;
                const NphVec3 p =
                    nph_vec3_add(at, nph_vec3_add(nph_vec3_scale(t, 0.01F * nph_f32_cos(phi)),
                                                  nph_vec3_scale(b, 0.01F * nph_f32_sin(phi))));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&pos, p.x));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&pos, p.y));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&pos, p.z));
            }
        }
        const u32 q[24] = {base,      base + 2U, base + 1U, base + 3U, base + 4U, base + 5U,
                           base,      base + 1U, base + 4U, base,      base + 4U, base + 3U,
                           base + 1U, base + 2U, base + 5U, base + 1U, base + 5U, base + 4U,
                           base + 2U, base,      base + 3U, base + 2U, base + 3U, base + 5U};
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&idx, q, 24U));
    }
    // The ground: a quad from -12 to 28 on x and z.
    const f32 ground[12] = {-12.0F, 0.0F, -12.0F, 28.0F,  0.0F, -12.0F,
                            28.0F,  0.0F, 28.0F,  -12.0F, 0.0F, 28.0F};
    const u32 ground_idx[6] = {0U, 2U, 1U, 0U, 3U, 2U};
    const u64 tuft_pos = (u64)pos.len * 4U;
    const u64 tuft_idx = (u64)idx.len * 4U;
    const u64 total = tuft_pos + tuft_idx + sizeof ground + sizeof ground_idx;
    u8 *bytes = NPH_ALLOC_ARRAY(a, u8, total);
    NTEST_ASSERT_NOT_NULL(bytes);
    memcpy(bytes, pos.data, (size_t)tuft_pos);
    memcpy(bytes + tuft_pos, idx.data, (size_t)tuft_idx);
    memcpy(bytes + tuft_pos + tuft_idx, ground, sizeof ground);
    memcpy(bytes + tuft_pos + tuft_idx + sizeof ground, ground_idx, sizeof ground_idx);
    char *b64 = NPH_ALLOC_ARRAY(a, char, test_base64_size(total) + 1U);
    NTEST_ASSERT_NOT_NULL(b64);
    test_base64(bytes, total, b64);
    f32 lo[3] = {1e30F, 1e30F, 1e30F};
    f32 hi[3] = {-1e30F, -1e30F, -1e30F};
    for (u32 i = 0; i < pos.len; ++i) {
        lo[i % 3U] = nph_f32_min(lo[i % 3U], pos.data[i]);
        hi[i % 3U] = nph_f32_max(hi[i % 3U], pos.data[i]);
    }
    static const f32 at[8][2] = {{2.0F, 2.0F},  {5.0F, 5.0F},  {10.0F, 2.0F},  {13.0F, 6.0F},
                                 {2.0F, 10.0F}, {6.0F, 13.0F}, {10.0F, 10.0F}, {14.0F, 14.0F}};
    NphStrBuilder sb;
    nph_sb_init(&sb, a);
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                                                "\"scenes\":[{\"nodes\":[0,1,2,3,4,5,6,7,8]}],"
                                                "\"nodes\":["),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&sb,
                                           "{\"mesh\":0,\"translation\":[%g,1,%g],\"rotation\":"
                                           "[0,%g,0,%g]},",
                                           (f64)at[i][0], (f64)at[i][1], sin(0.35 * (f64)i),
                                           cos(0.35 * (f64)i)),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(
        nph_sb_appendf(
            &sb,
            "{\"mesh\":1}],\"meshes\":[{\"name\":\"tuft\",\"primitives\":[{\"attributes\":{"
            "\"POSITION\":0},\"indices\":1}]},{\"name\":\"ground\",\"primitives\":[{\"attributes\":"
            "{"
            "\"POSITION\":2},\"indices\":3}]}],\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%g,%"
            "g,"
            "%g],\"max\":[%g,%g,%g]},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-12,0,"
            "-12],\"max\":[28,0,28]},"
            "{\"bufferView\":3,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":%llu},{\"buffer\":0,\"byteOffset\":%llu,"
            "\"byteLength\":%llu},{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":48},{\"buffer\":"
            "0,"
            "\"byteOffset\":%llu,\"byteLength\":24}],\"buffers\":[{\"byteLength\":%llu,\"uri\":"
            "\"data:application/octet-stream;base64,%s\"}]}",
            pos.len / 3U, (f64)lo[0], (f64)lo[1], (f64)lo[2], (f64)hi[0], (f64)hi[1], (f64)hi[2],
            idx.len, (unsigned long long)tuft_pos, (unsigned long long)tuft_pos,
            (unsigned long long)tuft_idx, (unsigned long long)(tuft_pos + tuft_idx),
            (unsigned long long)(tuft_pos + tuft_idx + sizeof ground), (unsigned long long)total,
            b64),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, sb.data, sb.len), NPH_RESULT_OK);
    nph_sb_free(&sb);
    NPH_FREE_ARRAY(a, char, b64, test_base64_size(total) + 1U);
    NPH_FREE_ARRAY(a, u8, bytes, total);
    NPH_ARRAY_FREE(&pos);
    NPH_ARRAY_FREE(&idx);
}

static Cooker *make_cooker(CellsFixture *f, bool cells)
{
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.texture.lz = NPH_LZ_LEVEL_FAST;
    desc.mesh.lz = NPH_LZ_LEVEL_FAST;
    desc.mesh.voxels = 1;
    desc.hlod.enabled = cells ? 1 : 0;
    desc.hlod.cell = 8.0F;
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    return c;
}

// A cooked asset's file, every blob unpacked, as a loader would.
static void read_asset(CellsFixture *f, const char *relative, TestAsset *out)
{
    char full[1500];
    snprintf(full, sizeof full, "%s/%s", f->out_dir, relative);
    void *bytes = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(full, f->alloc, &bytes, &size), NPH_RESULT_OK);
    memset(out, 0, sizeof *out);
    out->alloc = f->alloc;
    out->image = NPH_ALLOC(f->alloc, nph_max_u64(size, 1U), NPH_ASSET_BLOB_MAX_ALIGN);
    NTEST_ASSERT_NOT_NULL(out->image);
    memcpy(out->image, bytes, (size_t)size);
    out->size = size;
    nph_file_free_all(f->alloc, bytes, size);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(out->image, out->size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_LE_UINT(info.blob_count, NPH_ARRAY_COUNT(out->blobs));
    for (u32 i = 0; i < info.blob_count; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        out->blobs[i].tag = e->tag;
        out->blobs[i].index = e->index;
        out->blobs[i].codec = e->codec;
        out->blobs[i].alignment = e->alignment;
        out->blobs[i].size = e->raw_size;
        out->blobs[i].offset = e->offset;
        void *raw = NPH_ALLOC(f->alloc, nph_max_u64(e->raw_size, 1U), e->alignment);
        NTEST_ASSERT_NOT_NULL(raw);
        NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(out->image, e, raw), NPH_RESULT_OK);
        out->blobs[i].data = raw;
        out->owned[i] = raw;
    }
    out->asset.header = info.header;
    out->asset.blobs = out->blobs;
    out->asset.blob_count = info.blob_count;
    out->asset.path = NPH_STR("file");
}

static u32 count_cells(const CookDb *db)
{
    u32 n = 0U;
    for (u32 i = 0; i < cook_db_count(db); ++i) {
        n += nph_str_starts_with(nph_str_from_cstr(cook_db_entry(db, i)->sub), NPH_STR("cell/"))
                 ? 1U
                 : 0U;
    }
    return n;
}

NTEST_F(CellsFixture, a_scene_cooks_its_cells_and_their_proxies)
{
    write_grove(f->gltf);
    Cooker *c = make_cooker(f, true);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    CookerStats st = cooker_stats(c);
    NTEST_EXPECT_EQ_UINT(st.failed, 0U);
    // The scene: its tufts in the four cells, two each, the ground after them.
    const CookDbEntry *se = cook_db_find(cooker_db(c), NPH_STR("grove.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(se);
    TestAsset scene;
    read_asset(f, se->output, &scene);
    NphSceneView sv;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&scene.asset, &sv), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(sv.header->instance_count, 9U);
    NTEST_ASSERT_EQ_UINT(sv.cell_count, 4U);
    const NphGuid tuft = nph_guid_from_cstr("grove.gltf#mesh/0");
    const NphGuid ground = nph_guid_from_cstr("grove.gltf#mesh/1");
    u32 levels = 0U; // the proxies' levels apart, an asset each
    for (u32 k = 0; k < 4U; ++k) {
        const NphSceneCell *cell = &sv.cells[k];
        NTEST_EXPECT_EQ_UINT(cell->instance_offset, 2U * k);
        NTEST_EXPECT_EQ_UINT(cell->instance_count, 2U);
        NTEST_EXPECT_NEAR_F32(cell->proxy_cell, 0.25F, 0.0F);
        NTEST_EXPECT_NEAR_F32(cell->proxy_distance, 560.0F, 0.0F);
        NTEST_EXPECT_NEAR_F32(cell->proxy_origin.x, 8.0F * (f32)(k % 2U), 0.0F);
        NTEST_EXPECT_NEAR_F32(cell->proxy_origin.z, 8.0F * (f32)(k >> 1U), 0.0F);
        for (u32 i = 0; i < 2U; ++i) {
            NTEST_EXPECT_TRUE(nph_guid_equals(sv.instances[2U * k + i].mesh, tuft));
        }
        // Its proxy: a head of no geometry naming its levels apart (asset/
        // mesh.h version 6; ADR-0063), whose assets depend on it.
        NTEST_ASSERT_FALSE(nph_guid_is_null(cell->proxy));
        const CookDbEntry *pe = cook_db_find_guid(cooker_db(c), cell->proxy);
        NTEST_ASSERT_NOT_NULL(pe);
        TestAsset proxy;
        read_asset(f, pe->output, &proxy);
        NphMeshView mv;
        NTEST_ASSERT_EQ_INT(nph_mesh_view(&proxy.asset, &mv), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(mv.header->vertex_count, 0U);
        NTEST_EXPECT_EQ_UINT(mv.header->meshlet_count, 0U);
        NTEST_EXPECT_NULL(mv.lod);
        NTEST_ASSERT_NOT_NULL(mv.levels);
        NTEST_ASSERT_GE_UINT(mv.level_count, 2U);
        NTEST_ASSERT_EQ_UINT(pe->dep_count, mv.level_count);
        // The proxy's space is the square's: its voxels within a tuft's reach
        // of its two tufts (here x and z from about 0.7 to 7.3).
        NTEST_EXPECT_TRUE(mv.header->bounds.min.x > -1.0F && mv.header->bounds.max.x < 9.0F);
        NTEST_EXPECT_TRUE(mv.header->bounds.min.z > -1.0F && mv.header->bounds.max.z < 9.0F);
        // Each level: a mesh of voxels alone of one level, its makers as
        // records (none at level 0), over a hundred voxels at level 0,
        // compact (render/voxel.h), its errors rising from the finer one's.
        u32 words = 0U;
        for (u32 i = 0; i < mv.level_count; ++i) {
            const NphMeshLevel *lv = &mv.levels[i];
            NTEST_EXPECT_TRUE(nph_guid_equals(pe->deps[i], lv->mesh));
            const CookDbEntry *le = cook_db_find_guid(cooker_db(c), lv->mesh);
            NTEST_ASSERT_NOT_NULL(le);
            TestAsset level;
            read_asset(f, le->output, &level);
            NphMeshView lm;
            NTEST_ASSERT_EQ_INT(nph_mesh_view(&level.asset, &lm), NPH_RESULT_OK);
            NTEST_EXPECT_EQ_UINT(lm.header->vertex_count, 0U);
            NTEST_ASSERT_NOT_NULL(lm.lod);
            NTEST_ASSERT_NOT_NULL(lm.lod_makers);
            NTEST_EXPECT_EQ_UINT(lm.lod->levels, 1U);
            NTEST_EXPECT_EQ_UINT(lm.header->meshlet_count, lv->cluster_count);
            NTEST_EXPECT_GT_UINT(lm.lod->voxel_cluster_count, 0U);
            NTEST_EXPECT_EQ_UINT(lm.meshlets[0].vertex_count, 0U);
            f32 smallest = NPH_F32_MAX;
            for (u32 m = 0; m < lm.header->meshlet_count; ++m) {
                smallest = nph_f32_min(smallest, lm.lod_makers[m].error);
            }
            NTEST_EXPECT_NEAR_F32(lv->maker_error, smallest, 0.0F);
            NTEST_EXPECT_TRUE(i == 0U ? lv->maker_error == 0.0F
                                      : lv->maker_error >= mv.levels[i - 1U].maker_error);
            words += i == 0U ? lm.lod->voxel_word_count : 0U;
            test_asset_free(&level);
        }
        NTEST_EXPECT_GT_UINT(words, 100U * NPH_MESH_VOXEL_WORDS);
        levels += mv.level_count;
        test_asset_free(&proxy);
    }
    // The default material, two meshes, four proxies' heads and levels, the
    // scene.
    NTEST_EXPECT_EQ_UINT(st.cooked, 8U + levels);
    NTEST_EXPECT_EQ_UINT(count_cells(cooker_db(c)), 4U + levels);
    NTEST_EXPECT_TRUE(nph_guid_equals(sv.instances[8].mesh, ground));
    test_asset_free(&scene);
    cooker_destroy(c);

    // A second cook finds all of it current, the proxies too.
    c = make_cooker(f, true);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    st = cooker_stats(c);
    NTEST_EXPECT_EQ_UINT(st.cooked, 0U);
    NTEST_EXPECT_EQ_UINT(st.skipped, 8U + levels);
    NTEST_EXPECT_EQ_UINT(count_cells(cooker_db(c)), 4U + levels);
    cooker_destroy(c);

    // Without cells: the scene recooks without them, its proxies are gone.
    c = make_cooker(f, false);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, f->gltf), NPH_RESULT_OK);
    st = cooker_stats(c);
    NTEST_EXPECT_EQ_UINT(st.removed, 4U + levels);
    NTEST_EXPECT_EQ_UINT(count_cells(cooker_db(c)), 0U);
    se = cook_db_find(cooker_db(c), NPH_STR("grove.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(se);
    read_asset(f, se->output, &scene);
    NTEST_ASSERT_EQ_INT(nph_scene_view(&scene.asset, &sv), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(sv.cell_count, 0U);
    test_asset_free(&scene);
    cooker_destroy(c);
}

NTEST_F(CellsFixture, cells_need_voxels)
{
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    cook_mesh_settings_default(&desc.mesh);
    desc.hlod.enabled = 1;
    Cooker *c = NULL;
    NTEST_EXPECT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(c);
}

// SPDX-License-Identifier: Apache-2.0
// Physics material sources (cook/physics_material.h; ADR-0108): the `.npmat` file read, written,
// and refused where it must be; its header; and the cooker's physics material asset.
#include <nepheline/asset/physics_material.h>

#include "test_cook_support.h"

#include <cook/cooker.h>
#include <cook/physics_material.h>

static NphResult read_text(const char *text, CookPhysicsMaterialSource *out)
{
    return cook_physics_material_source_read(nph_allocator_system(), nph_str_from_cstr(text), out);
}

// Ice: slippery against anything (friction by the smaller), a bounce averaged, ice's density.
static void ice(CookPhysicsMaterialSource *m)
{
    cook_physics_material_source_default(m);
    m->friction = 0.02F;
    m->restitution = 0.125F;
    m->density = 917.0F;
    m->friction_combine = (u32)NPH_PHYSICS_MATERIAL_COMBINE_MIN;
    m->restitution_combine = (u32)NPH_PHYSICS_MATERIAL_COMBINE_AVERAGE;
}

// ---------------------------------------------------------------------------
// The source
// ---------------------------------------------------------------------------

NTEST(physics_material_source, the_default_is_a_colliders_and_writes_every_key)
{
    CookPhysicsMaterialSource m;
    cook_physics_material_source_default(&m);
    NTEST_EXPECT_NEAR_F32(m.friction, 0.6F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m.restitution, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m.density, 1000.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_validate(&m), NPH_RESULT_OK);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_write(&m, &sb), NPH_RESULT_OK);
    static const char expected[] = "{\n"
                                   "  \"npmat\": 1,\n"
                                   "  \"friction\": 0.6,\n"
                                   "  \"restitution\": 0,\n"
                                   "  \"density\": 1000,\n"
                                   "  \"friction_combine\": \"default\",\n"
                                   "  \"restitution_combine\": \"default\"\n"
                                   "}\n";
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&sb), expected);
    // A document of the version alone is the default.
    CookPhysicsMaterialSource back;
    NTEST_ASSERT_EQ_INT(read_text("{\"npmat\": 1}", &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_physics_material_source_equal(&back, &m));
    nph_sb_free(&sb);
}

NTEST(physics_material_source, reads_back_what_it_writes)
{
    CookPhysicsMaterialSource m;
    ice(&m);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_write(&m, &sb), NPH_RESULT_OK);
    const char *text = nph_sb_cstr(&sb);
    NTEST_EXPECT_TRUE(strstr(text, "\"friction_combine\": \"minimum\"") != NULL);
    NTEST_EXPECT_TRUE(strstr(text, "\"restitution_combine\": \"average\"") != NULL);
    CookPhysicsMaterialSource back;
    NTEST_ASSERT_EQ_INT(read_text(text, &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_physics_material_source_equal(&back, &m));
    // Writing what was read gives the same text: one canonical form.
    NphStrBuilder again;
    nph_sb_init(&again, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_write(&back, &again), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&again), text);
    // Each mode by its name, in order; a change is a different material.
    static const char *const names[] = {"default", "average", "minimum", "multiply", "maximum"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(names); ++i) {
        NTEST_EXPECT_EQ_STR(cook_physics_combine_name(i), names[i]);
        char doc[96];
        (void)snprintf(doc, sizeof doc, "{\"npmat\": 1, \"restitution_combine\": \"%s\"}",
                       names[i]);
        NTEST_ASSERT_EQ_INT(read_text(doc, &back), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(back.restitution_combine, i);
    }
    NTEST_EXPECT_NULL(cook_physics_combine_name((u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT));
    back = m;
    back.density = 918.0F;
    NTEST_EXPECT_FALSE(cook_physics_material_source_equal(&back, &m));
    back = m;
    back.friction_combine = (u32)NPH_PHYSICS_MATERIAL_COMBINE_MAX;
    NTEST_EXPECT_FALSE(cook_physics_material_source_equal(&back, &m));
    nph_sb_free(&again);
    nph_sb_free(&sb);
}

NTEST(physics_material_source, the_reader_refuses_what_it_must)
{
    static const char *const bad[] = {
        "",
        "[]",
        "{\"npmat\": \"1\"}",
        "{\"friction\": 0.5}", // no version
        "{\"npmat\": 1, \"npmat\": 1}",
        "{\"npmat\": 1, \"bounciness\": 0.5}",
        "{\"npmat\": 1, \"friction\": 0.5, \"friction\": 0.6}",
        "{\"npmat\": 1, \"friction\": -0.01}",
        "{\"npmat\": 1, \"friction\": 101}",
        "{\"npmat\": 1, \"friction\": \"0.5\"}",
        "{\"npmat\": 1, \"restitution\": 1.5}",
        "{\"npmat\": 1, \"restitution\": -1}",
        "{\"npmat\": 1, \"density\": -1}",
        "{\"npmat\": 1, \"density\": 2e6}",
        "{\"npmat\": 1, \"density\": null}",
        "{\"npmat\": 1, \"friction_combine\": \"min\"}",
        "{\"npmat\": 1, \"friction_combine\": 2}",
        "{\"npmat\": 1, \"restitution_combine\": \"Maximum\"}",
        "{\"npmat\": 1, \"restitution_combine\": [\"maximum\"]}",
        "{\"npmat\": 1, \"friction\": 0.5",
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad); ++i) {
        CookPhysicsMaterialSource m;
        const NphResult r = read_text(bad[i], &m);
        if (r != NPH_RESULT_PARSE_ERROR) {
            NTEST_FAIL("document %u read as %s: %s", i, nph_result_string(r), bad[i]);
        }
        // What failed to read leaves the default.
        CookPhysicsMaterialSource def;
        cook_physics_material_source_default(&def);
        NTEST_EXPECT_TRUE(cook_physics_material_source_equal(&m, &def));
    }
    // Another version, whatever else it holds.
    CookPhysicsMaterialSource m;
    NTEST_EXPECT_EQ_INT(read_text("{\"npmat\": 2, \"rolling\": 0.1}", &m),
                        NPH_RESULT_VERSION_MISMATCH);
    // The bounds themselves read.
    NTEST_EXPECT_EQ_INT(
        read_text("{\"npmat\": 1, \"friction\": 100, \"restitution\": 1, \"density\": 1e6}", &m),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(
        read_text("{\"npmat\": 1, \"friction\": 0, \"restitution\": 0, \"density\": 0}", &m),
        NPH_RESULT_OK);
}

NTEST(physics_material_source, validation_refuses_what_the_reader_would)
{
    CookPhysicsMaterialSource m;
    ice(&m);
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_validate(&m), NPH_RESULT_OK);
    const f32 nan = nph_f32_from_bits(0x7FC00000U);
    CookPhysicsMaterialSource bad = m;
    bad.friction = -0.01F;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_validate(&bad), NPH_RESULT_CORRUPT);
    bad.friction = nan;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_validate(&bad), NPH_RESULT_CORRUPT);
    bad = m;
    bad.restitution = 1.0001F;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_validate(&bad), NPH_RESULT_CORRUPT);
    bad = m;
    bad.density = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_validate(&bad), NPH_RESULT_CORRUPT);
    bad = m;
    bad.friction_combine = (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_validate(&bad), NPH_RESULT_CORRUPT);
    bad = m;
    bad.restitution_combine = 7U;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_validate(&bad), NPH_RESULT_CORRUPT);
    // A source that does not validate does not write.
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_write(&bad, &sb), NPH_RESULT_CORRUPT);
    nph_sb_free(&sb);
}

NTEST(physics_material_source, the_header_holds_every_value)
{
    CookPhysicsMaterialSource m;
    ice(&m);
    NphPhysicsMaterialHeader h;
    memset(&h, 0xFF, sizeof h);
    cook_physics_material_source_header(&m, &h);
    NTEST_EXPECT_NEAR_F32(h.friction, 0.02F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.restitution, 0.125F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.density, 917.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(h.friction_combine, (u32)NPH_PHYSICS_MATERIAL_COMBINE_MIN);
    NTEST_EXPECT_EQ_UINT(h.restitution_combine, (u32)NPH_PHYSICS_MATERIAL_COMBINE_AVERAGE);
    NTEST_EXPECT_EQ_UINT(h.flags, 0U);
    for (u32 i = 0; i < NPH_ARRAY_COUNT(h.reserved); ++i) {
        NTEST_EXPECT_EQ_UINT(h.reserved[i], 0U);
    }
}

NTEST(physics_material_source, saves_whole_and_loads)
{
    TempDir dir;
    test_temp_dir_make(&dir, "physics_material_save");
    char path[1200];
    test_path_in(&dir, "ice.npmat", path, sizeof path);
    CookPhysicsMaterialSource m;
    ice(&m);
    NphAllocator *alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_save(&m, alloc, path), NPH_RESULT_OK);
    CookPhysicsMaterialSource back;
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_load(alloc, path, &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_physics_material_source_equal(&back, &m));
    // A source that does not validate leaves the file as it was, and no part behind.
    m.restitution = 2.0F;
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_save(&m, alloc, path), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_load(alloc, path, &back), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(back.restitution, 0.125F, 0.0F);
    char part[1300];
    (void)snprintf(part, sizeof part, "%s.part", path);
    NTEST_EXPECT_FALSE(nph_fs_exists(part));
    test_path_in(&dir, "missing.npmat", path, sizeof path);
    NTEST_EXPECT_EQ_INT(cook_physics_material_source_load(alloc, path, &back),
                        NPH_RESULT_NOT_FOUND);
    test_temp_dir_remove(&dir);
}

// ---------------------------------------------------------------------------
// The cooker's physics material
// ---------------------------------------------------------------------------

typedef struct PhysicsMaterialCook {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
} PhysicsMaterialCook;

NTEST_SETUP(PhysicsMaterialCook)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "physics_material_cook");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
}

NTEST_TEARDOWN(PhysicsMaterialCook)
{
    test_temp_dir_remove(&f->dir);
}

static Cooker *pm_cooker(PhysicsMaterialCook *f)
{
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    return c;
}

static void write_source(PhysicsMaterialCook *f, const char *key,
                         const CookPhysicsMaterialSource *m, char *path, u64 cap)
{
    (void)snprintf(path, (size_t)cap, "%s/%s", f->dir.dir, key);
    char dir[1300];
    (void)snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    NTEST_ASSERT_NOT_NULL(slash);
    *slash = '\0';
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_physics_material_source_save(m, f->alloc, path), NPH_RESULT_OK);
}

// The cooked physics material of `key` as the database holds it: false when there is none.
static bool cooked_values(PhysicsMaterialCook *f, Cooker *c, const char *key,
                          NphPhysicsMaterialHeader *out)
{
    const CookDbEntry *e =
        cook_db_find(cooker_db(c), nph_str_from_cstr(key), nph_str_from_cstr("physics_material"));
    if (e == NULL) {
        return false;
    }
    NTEST_EXPECT_EQ_UINT(e->type, (u32)NPH_ASSET_TYPE_PHYSICS_MATERIAL);
    NTEST_EXPECT_TRUE(nph_guid_equals(e->guid, cooker_asset_guid(key, "physics_material")));
    char path[1500];
    (void)snprintf(path, sizeof path, "%s/%s", f->out_dir, e->output);
    TestAsset a;
    test_asset_from_file(f->alloc, path, &a);
    NphPhysicsMaterialView v;
    NTEST_ASSERT_EQ_INT(nph_physics_material_view(&a.asset, &v), NPH_RESULT_OK);
    *out = *v.header;
    NTEST_EXPECT_TRUE(nph_guid_equals(a.asset.header.guid, e->guid));
    test_asset_free(&a);
    return true;
}

NTEST_F(PhysicsMaterialCook, cooks_the_source_and_again_when_it_changes)
{
    NTEST_EXPECT_EQ_INT(cooker_source_kind("surfaces/ice.npmat"), COOKER_SOURCE_PHYSICS_MATERIAL);
    NTEST_EXPECT_EQ_INT(cooker_source_kind("surfaces/ICE.NPMAT"), COOKER_SOURCE_PHYSICS_MATERIAL);
    CookPhysicsMaterialSource m;
    ice(&m);
    char path[1300];
    write_source(f, "surfaces/ice.npmat", &m, path, sizeof path);
    Cooker *c = pm_cooker(f);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NphPhysicsMaterialHeader h;
    memset(&h, 0, sizeof h);
    NTEST_ASSERT_TRUE(cooked_values(f, c, "surfaces/ice.npmat", &h));
    NTEST_EXPECT_NEAR_F32(h.friction, 0.02F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.density, 917.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(h.friction_combine, (u32)NPH_PHYSICS_MATERIAL_COMBINE_MIN);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 1U);
    // Current: skipped. Changed: cooked again, under the same GUID.
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).skipped, 1U);
    m.friction = 0.05F;
    write_source(f, "surfaces/ice.npmat", &m, path, sizeof path);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 2U);
    NTEST_ASSERT_TRUE(cooked_values(f, c, "surfaces/ice.npmat", &h));
    NTEST_EXPECT_NEAR_F32(h.friction, 0.05F, 0.0F);
    cooker_destroy(c);
}

NTEST_F(PhysicsMaterialCook, a_directory_cooks_its_physics_materials_and_bad_ones_fail)
{
    CookPhysicsMaterialSource m;
    cook_physics_material_source_default(&m);
    char path[1300];
    write_source(f, "surfaces/rubber.npmat", &m, path, sizeof path);
    ice(&m);
    write_source(f, "surfaces/deep/ice.npmat", &m, path, sizeof path);
    Cooker *c = pm_cooker(f);
    char surfaces[1300];
    test_path_in(&f->dir, "surfaces", surfaces, sizeof surfaces);
    NTEST_ASSERT_EQ_INT(cooker_cook_directory(c, surfaces), NPH_RESULT_OK);
    NphPhysicsMaterialHeader h;
    memset(&h, 0, sizeof h);
    NTEST_EXPECT_TRUE(cooked_values(f, c, "surfaces/rubber.npmat", &h));
    NTEST_EXPECT_TRUE(cooked_values(f, c, "surfaces/deep/ice.npmat", &h));
    // A file that is not one fails, and stays uncooked.
    (void)snprintf(path, sizeof path, "%s/surfaces/bad.npmat", f->dir.dir);
    static const char text[] = "{\"npmat\": 1, \"sticky\": true}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, text, sizeof text - 1U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_FALSE(cooked_values(f, c, "surfaces/bad.npmat", &h));
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).failed, 1U);
    cooker_destroy(c);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The fuzz target's seeds (scripts/fuzz_seeds_cook.py) read, so a seed that stops reading fails
// here rather than quietly in the nightly runs.
NTEST(physics_material_source, the_fuzz_corpus_reads)
{
    static const char *const seeds[] = {"version_only", "default", "ice", "every_mode"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(seeds); ++i) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/npmat/%s", NPH_FUZZ_CORPUS_DIR, seeds[i]);
        CookPhysicsMaterialSource m;
        const NphResult r = cook_physics_material_source_load(nph_allocator_system(), path, &m);
        if (r != NPH_RESULT_OK) {
            NTEST_FAIL("seed %s: %s", seeds[i], nph_result_string(r));
        }
    }
}
#endif

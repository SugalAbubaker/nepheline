// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/cvar.h>

#include "test_support.h"

#include <math.h>
#include <string.h>

static NPH_CVAR_BOOL(cv_vsync, "r.vsync", 1, "Vertical sync", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_INT(cv_shadow, "r.shadow.resolution", 2048, 256, 8192, "Shadow map size",
                    NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_FLOAT(cv_gamma, "r.gamma", 2.2, 1.0, 3.0, "Display gamma", NPH_CVAR_FLAG_NONE);
static NPH_CVAR_STRING(cv_name, "game.player_name", "Player", "Name", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_INT(cv_build, "engine.build", 7, 0, 1000000, "Build number",
                    NPH_CVAR_FLAG_READ_ONLY);
static NPH_CVAR_BOOL(cv_god, "cheat.god", 0, "God mode", NPH_CVAR_FLAG_CHEAT);

typedef struct CvarFixture {
    NphDebugAllocator da;
    NphCvarRegistry registry;
    u32 changes;
} CvarFixture;

static void count_change(NphCvar *cvar, void *user)
{
    NPH_UNUSED(cvar);
    CvarFixture *f = (CvarFixture *)user;
    f->changes += 1U;
}

NTEST_SETUP(CvarFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_cvar_registry_init(&f->registry, nph_debug_allocator_allocator(&f->da)),
                        NPH_RESULT_OK);
    // Static cvars survive across tests: reset the registration state.
    NphCvar *all[] = {&cv_vsync, &cv_shadow, &cv_gamma, &cv_name, &cv_build, &cv_god};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(all); ++i) {
        all[i]->registered = 0U;
        all[i]->on_change = NULL;
        all[i]->change_count = 0U;
    }
}

NTEST_TEARDOWN(CvarFixture)
{
    nph_cvar_registry_free(&f->registry);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static void register_all(CvarFixture *f)
{
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_vsync), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_shadow), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_gamma), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_name), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_build), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_god), NPH_RESULT_OK);
}

NTEST_F(CvarFixture, registration_defaults_and_lookup)
{
    register_all(f);
    NTEST_ASSERT_EQ_UINT(nph_cvar_registry_count(&f->registry), 6U);
    NTEST_ASSERT_EQ_PTR(nph_cvar_registry_at(&f->registry, 1U), &cv_shadow);
    NTEST_ASSERT_EQ_PTR(nph_cvar_find(&f->registry, NPH_STR("r.gamma")), &cv_gamma);
    NTEST_ASSERT_NULL(nph_cvar_find(&f->registry, NPH_STR("r.Gamma")));
    NTEST_ASSERT_TRUE(nph_cvar_get_bool(&cv_vsync));
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 2048);
    NTEST_ASSERT_TRUE(nph_cvar_get_float(&cv_gamma) == 2.2);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_name), NPH_STR("Player")));
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_vsync), NPH_RESULT_INVALID_ARGUMENT);
    static NPH_CVAR_BOOL(duplicate, "r.vsync", 0, "dup", NPH_CVAR_FLAG_NONE);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &duplicate), NPH_RESULT_ALREADY_EXISTS);
    static NPH_CVAR_BOOL(bad_name, "R.VSync", 0, "bad", NPH_CVAR_FLAG_NONE);
    NphResult r = NPH_RESULT_OK;
    NPH_TEST_EXPECT_ASSERT(r = nph_cvar_register(&f->registry, &bad_name));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
    static NPH_CVAR_INT(bad_range, "x.bad", 10, 0, 5, "default out of range", NPH_CVAR_FLAG_NONE);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &bad_range), NPH_RESULT_INVALID_ARGUMENT);
}

NTEST_F(CvarFixture, setters_clamp_type_check_and_notify)
{
    register_all(f);
    cv_shadow.on_change = count_change;
    cv_shadow.on_change_user = f;
    const u64 version = f->registry.version;
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(&f->registry, &cv_shadow, 4096), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 4096);
    NTEST_ASSERT_EQ_UINT(f->changes, 1U);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(&f->registry, &cv_shadow, 4096), NPH_RESULT_OK); // no-op
    NTEST_ASSERT_EQ_UINT(f->changes, 1U);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(&f->registry, &cv_shadow, 100000), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 8192); // clamped
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(&f->registry, &cv_shadow, -1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 256);
    NTEST_ASSERT_EQ_UINT(cv_shadow.change_count, 3U);
    NTEST_ASSERT_EQ_UINT(f->registry.version, version + 3U);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(&f->registry, &cv_shadow, 1), NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_float(&f->registry, &cv_shadow, 1.0),
                        NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_string(&f->registry, &cv_shadow, NPH_STR("x")),
                        NPH_RESULT_TYPE_MISMATCH);

    NTEST_ASSERT_EQ_INT(nph_cvar_set_float(&f->registry, &cv_gamma, 9.0), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_cvar_get_float(&cv_gamma) == 3.0);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_float(&f->registry, &cv_gamma, (f64)NAN),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(&f->registry, &cv_vsync, 7), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_bool(&cv_vsync), 1);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_string(&f->registry, &cv_name, NPH_STR("Ada")), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_name), NPH_STR("Ada")));
    NTEST_ASSERT_EQ_INT(nph_cvar_set_string(&f->registry, &cv_name, NPH_STR("Bob")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_reset(&f->registry, &cv_name), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_name), NPH_STR("Player")));
    NTEST_ASSERT_EQ_INT(nph_cvar_reset(&f->registry, &cv_shadow), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 2048);

    // Read-only and cheat protection.
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(&f->registry, &cv_build, 8), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_build), 7);
    NTEST_ASSERT_EQ_INT(nph_cvar_force_from_string(&f->registry, &cv_build, NPH_STR("9")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_build), 9);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(&f->registry, &cv_god, 1), NPH_RESULT_INVALID_ARGUMENT);
    nph_cvar_registry_set_cheats(&f->registry, true);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(&f->registry, &cv_god, 1), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_cvar_get_bool(&cv_god));
}

NTEST_F(CvarFixture, parsing_and_formatting)
{
    register_all(f);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_vsync, NPH_STR(" off ")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_cvar_get_bool(&cv_vsync));
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_vsync, NPH_STR("maybe")),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_shadow, NPH_STR("512")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 512);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_shadow, NPH_STR("5x")),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_gamma, NPH_STR("1.8")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_cvar_get_float(&cv_gamma) == 1.8);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_gamma, NPH_STR("")),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_from_string(&f->registry, &cv_name, NPH_STR("  Zed  ")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_name), NPH_STR("Zed")));
    char text[64];
    NTEST_ASSERT_EQ_UINT(nph_cvar_format(&cv_vsync, text, sizeof text), 5U);
    NTEST_ASSERT_EQ_STR(text, "false");
    nph_cvar_format(&cv_shadow, text, sizeof text);
    NTEST_ASSERT_EQ_STR(text, "512");
    nph_cvar_format(&cv_gamma, text, sizeof text);
    NTEST_ASSERT_EQ_STR(text, "1.8");
    nph_cvar_format(&cv_name, text, sizeof text);
    NTEST_ASSERT_EQ_STR(text, "Zed");
}

NTEST_F(CvarFixture, config_text_and_pending_values)
{
    // Config applied before registration: values wait for the cvars.
    const NphStr config = NPH_STR("# comment line\n"
                                  "r.vsync = false   # trailing comment\n"
                                  "\n"
                                  "r.shadow.resolution=1024\n"
                                  "game.player_name = \"Grace Hopper\"\n"
                                  "future.setting = 42\n"
                                  "engine.build = 12\n");
    u32 error_line = 0U;
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_config(&f->registry, config, &error_line), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->registry.pending), 5U);
    register_all(f);
    NTEST_ASSERT_FALSE(nph_cvar_get_bool(&cv_vsync));
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 1024);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_name), NPH_STR("Grace Hopper")));
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_build), 12); // config may set read-only values
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->registry.pending), 1U); // future.setting
    // Applied after registration: direct.
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_config(&f->registry, NPH_STR("r.gamma = 2.5"), NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_cvar_get_float(&cv_gamma) == 2.5);
    // Errors: a line without '=', a bad value; good lines still apply.
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_config(&f->registry,
                                              NPH_STR("r.gamma = 1.5\nnonsense\nr.vsync = yes\n"),
                                              &error_line),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_UINT(error_line, 2U);
    NTEST_ASSERT_TRUE(nph_cvar_get_float(&cv_gamma) == 1.5);
    NTEST_ASSERT_TRUE(nph_cvar_get_bool(&cv_vsync));
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_config(&f->registry, NPH_STR("r.gamma = high"), &error_line),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_UINT(error_line, 1U);
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_config(&f->registry, NPH_STR(" = 3"), &error_line),
                        NPH_RESULT_PARSE_ERROR);
    // Command-line assignments.
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_assignment(&f->registry, NPH_STR("r.shadow.resolution=300")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_shadow), 300);
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_assignment(&f->registry, NPH_STR("r.shadow.resolution")),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_assignment(&f->registry, NPH_STR("later.cvar=1")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_assignment(&f->registry, NPH_STR("later.cvar=2")),
                        NPH_RESULT_OK); // replaces the pending value
    NTEST_ASSERT_EQ_UINT(nph_hashmap_len(&f->registry.pending), 2U);
    static NPH_CVAR_INT(later, "later.cvar", 0, 0, 10, "registered late", NPH_CVAR_FLAG_NONE);
    later.registered = 0U;
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &later), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&later), 2);
}

NTEST_F(CvarFixture, write_config_sorted_saved_only)
{
    register_all(f);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_string(&f->registry, &cv_name, NPH_STR("Ada")), NPH_RESULT_OK);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_debug_allocator_allocator(&f->da));
    NTEST_ASSERT_EQ_INT(nph_cvar_write_config(&f->registry, &sb), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&sb), "game.player_name = \"Ada\"\n"
                                          "r.shadow.resolution = 2048\n"
                                          "r.vsync = true\n");
    // Round trip.
    nph_cvar_registry_free(&f->registry);
    NTEST_ASSERT_EQ_INT(nph_cvar_registry_init(&f->registry, nph_debug_allocator_allocator(&f->da)),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_apply_config(&f->registry, nph_sb_str(&sb), NULL), NPH_RESULT_OK);
    cv_name.registered = 0U;
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->registry, &cv_name), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_name), NPH_STR("Ada")));
    nph_sb_free(&sb);
}

NTEST_F(CvarFixture, registry_free_restores_defaults)
{
    register_all(f);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_string(&f->registry, &cv_name, NPH_STR("temp")),
                        NPH_RESULT_OK);
    nph_cvar_registry_free(&f->registry);
    NTEST_ASSERT_EQ_PTR(cv_name.value_string, cv_name.default_string);
    NTEST_ASSERT_EQ_UINT(cv_name.registered, 0U);
    // Re-init so teardown has something to free.
    NTEST_ASSERT_EQ_INT(nph_cvar_registry_init(&f->registry, nph_debug_allocator_allocator(&f->da)),
                        NPH_RESULT_OK);
}

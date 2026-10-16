// SPDX-License-Identifier: Apache-2.0
// The project (editor/src/project.h), without an engine: imports copy what
// lies outside the sources (a document with the files it names, a folder
// whole), cook on the import thread, and leave a snapshot of the database
// that the resolver reads, also while other imports run. Failures arrive
// in the results, and the requests in flight are bounded.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/guid.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/platform/thread.h>

#include "project.h"
#include "test_cook_support.h"

#include <string.h>

typedef struct ProjectFixture {
    NphDebugAllocator da;
    TempDir dir;       // the test's: src/ outside the project, and project/
    TempDir src;       // where files come from
    char project[1200];
    char assets[1200];
    char cooked[1200];
    EditorProject *p;
} ProjectFixture;

static void open_project(ProjectFixture *f, NphJobSystem *jobs)
{
    editor_project_destroy(f->p);
    f->p = NULL;
    EditorProjectDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.assets_dir = f->assets;
    d.cooked_dir = f->cooked;
    d.jobs = jobs;
    NTEST_ASSERT_EQ_INT(editor_project_create(&d, &f->p), NPH_RESULT_OK);
}

NTEST_SETUP(ProjectFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    test_temp_dir_make(&f->dir, "editor_project");
    test_path_in(&f->dir, "src", f->src.dir, sizeof f->src.dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->src.dir), NPH_RESULT_OK);
    test_path_in(&f->dir, "project", f->project, sizeof f->project);
    test_path_in(&f->dir, "project/assets", f->assets, sizeof f->assets);
    test_path_in(&f->dir, "project/cooked", f->cooked, sizeof f->cooked);
    f->p = NULL;
    open_project(f, NULL);
}

NTEST_TEARDOWN(ProjectFixture)
{
    editor_project_destroy(f->p);
    test_temp_dir_remove(&f->dir);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// Imports `paths` into `folder` and waits for the result.
static EditorImportResult import_now(ProjectFixture *f, const char *const *paths, u32 count,
                                     const char *folder)
{
    u32 id = 0U;
    NTEST_ASSERT_EQ_INT(editor_project_import(f->p, paths, count, folder, 42U, &id),
                        NPH_RESULT_OK);
    editor_project_wait(f->p);
    NTEST_EXPECT_EQ_UINT(editor_project_pending(f->p), 0U);
    EditorImportResult res;
    NTEST_ASSERT_TRUE(editor_project_poll(f->p, &res));
    NTEST_EXPECT_EQ_UINT(res.id, id);
    NTEST_EXPECT_EQ_UINT(res.tag, 42U);
    EditorImportResult none;
    NTEST_EXPECT_FALSE(editor_project_poll(f->p, &none));
    return res;
}

static EditorImportResult cook_now(ProjectFixture *f, const char *key, bool force)
{
    NTEST_ASSERT_EQ_INT(editor_project_cook(f->p, key, force, 0U, NULL), NPH_RESULT_OK);
    editor_project_wait(f->p);
    EditorImportResult res;
    NTEST_ASSERT_TRUE(editor_project_poll(f->p, &res));
    return res;
}

static bool exists_in(const char *dir, const char *name)
{
    char path[1600];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    return nph_fs_exists(path);
}

// A 4x4 TGA at `path`.
static void write_tga(const char *path)
{
    u8 rgba[4 * 4 * 4];
    for (u64 i = 0; i < 16U; ++i) {
        rgba[i * 4U] = (u8)(i * 16U);
        rgba[i * 4U + 1U] = 128U;
        rgba[i * 4U + 2U] = (u8)(255U - i * 16U);
        rgba[i * 4U + 3U] = 255U;
    }
    u8 tga[18 + 4 * 4 * 4];
    const u64 n = test_write_tga(rgba, 4U, 4U, tga);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, n), NPH_RESULT_OK);
}

NTEST_F(ProjectFixture, an_import_copies_cooks_and_resolves)
{
    test_write_gltf(&f->src, "crate", true);
    char path[1200];
    test_path_in(&f->src, "crate.gltf", path, sizeof path);
    const char *paths[1] = {path};
    NTEST_EXPECT_EQ_UINT(editor_project_generation(f->p), 0U);
    const EditorImportResult res = import_now(f, paths, 1U, "models");
    NTEST_ASSERT_EQ_INT(res.result, NPH_RESULT_OK);
    NTEST_EXPECT_GT_UINT(res.cooked, 0U);
    NTEST_EXPECT_EQ_UINT(res.failed, 0U);
    NTEST_ASSERT_EQ_UINT(res.scene_count, 1U);
    NTEST_EXPECT_EQ_STR(res.scene_names[0], "crate");
    NTEST_EXPECT_GT_UINT(editor_project_generation(f->p), 0U);
    // The document and the files it names, beside each other in the sources.
    NTEST_EXPECT_TRUE(exists_in(f->assets, "models/crate.gltf"));
    NTEST_EXPECT_TRUE(exists_in(f->assets, "models/crate.bin"));
    NTEST_EXPECT_TRUE(exists_in(f->assets, "models/crate_base.tga"));
    // The snapshot knows the scene; its image was cooked through it, not alone.
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "models/crate.gltf"));
    NTEST_EXPECT_FALSE(editor_project_is_cooked(f->p, "models/crate_base.tga"));
    NTEST_EXPECT_FALSE(editor_project_is_cooked(f->p, "crate.gltf"));
    NphGuid scene;
    NTEST_ASSERT_TRUE(editor_project_scene_of(f->p, "models/crate.gltf", &scene));
    NTEST_EXPECT_TRUE(nph_guid_equals(scene, res.scenes[0]));
    // The GUID follows the key, wherever the project is.
    NTEST_EXPECT_TRUE(nph_guid_equals(scene, nph_guid_from_cstr("models/crate.gltf#scene")));
    // The resolver names its file in the cooked directory.
    char vpath[1024];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(editor_project_resolve(f->p, scene, vpath, sizeof vpath, &len),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(len, strlen(vpath));
    NTEST_EXPECT_TRUE(exists_in(f->cooked, vpath));
    char tiny[4];
    NTEST_EXPECT_EQ_INT(editor_project_resolve(f->p, scene, tiny, sizeof tiny, &len),
                        NPH_RESULT_OVERFLOW);
    NTEST_EXPECT_EQ_INT(
        editor_project_resolve(f->p, nph_guid_from_cstr("nothing"), vpath, sizeof vpath, &len),
        NPH_RESULT_NOT_FOUND);
    // Again, unchanged: the copies replace themselves and nothing cooks.
    const EditorImportResult again = import_now(f, paths, 1U, "models");
    NTEST_EXPECT_EQ_INT(again.result, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(again.cooked, 0U);
    NTEST_EXPECT_GT_UINT(again.skipped, 0U);
    // A project opened again on the directories starts from the database.
    open_project(f, NULL);
    NTEST_EXPECT_EQ_INT(editor_project_resolve(f->p, scene, vpath, sizeof vpath, &len),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "models/crate.gltf"));
}

NTEST_F(ProjectFixture, a_source_already_in_the_project_is_cooked_where_it_is)
{
    TempDir assets;
    test_path_in(&f->dir, "project/assets", assets.dir, sizeof assets.dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(assets.dir), NPH_RESULT_OK);
    test_write_gltf(&assets, "rock", false);
    char path[1200];
    test_path_in(&assets, "rock.gltf", path, sizeof path);
    const char *paths[1] = {path};
    const EditorImportResult res = import_now(f, paths, 1U, "elsewhere");
    NTEST_ASSERT_EQ_INT(res.result, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(res.scene_count, 1U);
    NTEST_EXPECT_FALSE(exists_in(f->assets, "elsewhere")); // not copied
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "rock.gltf"));
}

NTEST_F(ProjectFixture, a_folder_comes_whole_and_its_scenes_cook)
{
    TempDir kit;
    TempDir sub;
    test_path_in(&f->src, "kit", kit.dir, sizeof kit.dir);
    test_path_in(&kit, "sub", sub.dir, sizeof sub.dir);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(sub.dir), NPH_RESULT_OK);
    test_write_gltf(&kit, "a", true);
    test_write_gltf(&sub, "b", false);
    const char *paths[1] = {kit.dir};
    const EditorImportResult res = import_now(f, paths, 1U, "");
    NTEST_ASSERT_EQ_INT(res.result, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(res.scene_count, 0U); // a folder's scenes are cooked, not named
    NTEST_EXPECT_TRUE(exists_in(f->assets, "kit/a.gltf"));
    NTEST_EXPECT_TRUE(exists_in(f->assets, "kit/a.bin"));
    NTEST_EXPECT_TRUE(exists_in(f->assets, "kit/a_base.tga"));
    NTEST_EXPECT_TRUE(exists_in(f->assets, "kit/sub/b.gltf"));
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "kit/a.gltf"));
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "kit/sub/b.gltf"));
}

NTEST_F(ProjectFixture, an_image_alone_cooks_and_one_beside_a_scene_comes_along)
{
    char lone[1200];
    test_path_in(&f->src, "lone.tga", lone, sizeof lone);
    write_tga(lone);
    const char *one[1] = {lone};
    const EditorImportResult res = import_now(f, one, 1U, "textures");
    NTEST_ASSERT_EQ_INT(res.result, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(res.scene_count, 0U);
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "textures/lone.tga"));
    // Dropped with a scene, an image is the scene's to cook.
    char extra[1200];
    char scene[1200];
    test_path_in(&f->src, "extra.tga", extra, sizeof extra);
    write_tga(extra);
    test_write_gltf(&f->src, "c", false);
    test_path_in(&f->src, "c.gltf", scene, sizeof scene);
    const char *two[2] = {extra, scene};
    const EditorImportResult both = import_now(f, two, 2U, "");
    NTEST_ASSERT_EQ_INT(both.result, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(both.scene_count, 1U);
    NTEST_EXPECT_TRUE(exists_in(f->assets, "extra.tga"));
    NTEST_EXPECT_FALSE(editor_project_is_cooked(f->p, "extra.tga"));
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "c.gltf"));
}

NTEST_F(ProjectFixture, what_an_import_refuses_and_how_failures_arrive)
{
    char ok[1200];
    test_write_gltf(&f->src, "ok", false);
    test_path_in(&f->src, "ok.gltf", ok, sizeof ok);
    const char *paths[EDITOR_IMPORT_PATHS_MAX + 1U];
    for (u32 i = 0; i <= EDITOR_IMPORT_PATHS_MAX; ++i) {
        paths[i] = ok;
    }
    // Up front: no paths, too many, and folders outside the sources.
    NTEST_EXPECT_EQ_INT(editor_project_import(f->p, paths, 0U, "", 0U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(
        editor_project_import(f->p, paths, EDITOR_IMPORT_PATHS_MAX + 1U, "", 0U, NULL),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_project_import(f->p, paths, 1U, "../up", 0U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_project_import(f->p, paths, 1U, f->src.dir, 0U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(editor_project_pending(f->p), 0U);

    // A missing file fails its request, and the rest of it still runs.
    char missing[1200];
    test_path_in(&f->src, "missing.gltf", missing, sizeof missing);
    const char *mixed[2] = {missing, ok};
    const EditorImportResult res = import_now(f, mixed, 2U, "");
    NTEST_EXPECT_EQ_INT(res.result, NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_UINT(res.scene_count, 1U);
    NTEST_EXPECT_TRUE(editor_project_is_cooked(f->p, "ok.gltf"));

    // The folder holding the sources is not copied into them.
    const char *project[1] = {f->project};
    const EditorImportResult into = import_now(f, project, 1U, "");
    NTEST_EXPECT_EQ_INT(into.result, NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_FALSE(exists_in(f->assets, "project"));

    // A document naming a file above its folder cannot be copied with it.
    static const char up_json[] = "{\"asset\":{\"version\":\"2.0\"},"
                                  "\"images\":[{\"uri\":\"../outside.png\"}]}";
    char up[1200];
    test_path_in(&f->src, "up.gltf", up, sizeof up);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(up, up_json, sizeof up_json - 1U), NPH_RESULT_OK);
    const char *ups[1] = {up};
    const EditorImportResult refused = import_now(f, ups, 1U, "");
    NTEST_EXPECT_EQ_INT(refused.result, NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_EQ_UINT(refused.scene_count, 0U);
}

NTEST_F(ProjectFixture, requests_in_flight_are_bounded)
{
    char missing[1200];
    test_path_in(&f->src, "missing.png", missing, sizeof missing);
    const char *paths[1] = {missing};
    // Finished requests count until they are taken.
    for (u32 i = 0; i < EDITOR_PROJECT_REQUESTS; ++i) {
        NTEST_ASSERT_EQ_INT(editor_project_import(f->p, paths, 1U, "", i, NULL), NPH_RESULT_OK);
    }
    editor_project_wait(f->p);
    NTEST_EXPECT_EQ_INT(editor_project_import(f->p, paths, 1U, "", 99U, NULL), NPH_RESULT_FULL);
    NTEST_EXPECT_EQ_INT(editor_project_cook(f->p, "x.png", false, 99U, NULL), NPH_RESULT_FULL);
    // In order, each with its tag.
    EditorImportResult res;
    for (u32 i = 0; i < EDITOR_PROJECT_REQUESTS; ++i) {
        NTEST_ASSERT_TRUE(editor_project_poll(f->p, &res));
        NTEST_EXPECT_EQ_UINT(res.tag, i);
        NTEST_EXPECT_EQ_INT(res.result, NPH_RESULT_NOT_FOUND);
    }
    NTEST_EXPECT_FALSE(editor_project_poll(f->p, &res));
    NTEST_EXPECT_EQ_INT(editor_project_import(f->p, paths, 1U, "", 99U, NULL), NPH_RESULT_OK);
    editor_project_wait(f->p);
    NTEST_EXPECT_TRUE(editor_project_poll(f->p, &res));
    // A request still queued when the project goes is dropped with it.
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_EQ_INT(editor_project_import(f->p, paths, 1U, "", i, NULL), NPH_RESULT_OK);
    }
}

NTEST_F(ProjectFixture, cooks_take_sources_by_key)
{
    test_write_gltf(&f->src, "crate", true);
    char path[1200];
    test_path_in(&f->src, "crate.gltf", path, sizeof path);
    const char *paths[1] = {path};
    NTEST_ASSERT_EQ_INT(import_now(f, paths, 1U, "models").result, NPH_RESULT_OK);
    const EditorImportResult current = cook_now(f, "models/crate.gltf", false);
    NTEST_EXPECT_EQ_INT(current.result, NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(current.cooked, 0U);
    NTEST_EXPECT_EQ_UINT(current.scene_count, 1U);
    const EditorImportResult forced = cook_now(f, "models/crate.gltf", true);
    NTEST_EXPECT_EQ_INT(forced.result, NPH_RESULT_OK);
    NTEST_EXPECT_GT_UINT(forced.cooked, 0U);
    // "" is every scene in the sources.
    const EditorImportResult all = cook_now(f, "", false);
    NTEST_EXPECT_EQ_INT(all.result, NPH_RESULT_OK);
    NTEST_EXPECT_GT_UINT(all.skipped, 0U);
    NTEST_EXPECT_EQ_INT(cook_now(f, "models/crate.bin", false).result, NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_EQ_INT(cook_now(f, "models/none.gltf", false).result, NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(editor_project_cook(f->p, "../x.gltf", false, 0U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
}

typedef struct ResolveLoop {
    EditorProject *p;
    NphGuid guid;
    char expected[1024];
    NphAtomicU32 stop;
    u32 calls;
    u32 wrong;
} ResolveLoop;

static void resolve_loop(void *user)
{
    ResolveLoop *l = (ResolveLoop *)user;
    while (nph_atomic_load_u32(&l->stop, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
        char path[1024];
        u64 len = 0U;
        const NphResult r = editor_project_resolve(l->p, l->guid, path, sizeof path, &len);
        ++l->calls;
        if (r != NPH_RESULT_OK || strcmp(path, l->expected) != 0) {
            ++l->wrong;
        }
    }
}

NTEST_F(ProjectFixture, the_resolver_answers_while_cooks_swap_the_snapshot)
{
    // Textures encode on a job system's workers, waited for by the import
    // thread, while another thread resolves without pause.
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = nph_allocator_system();
    jd.worker_count = 2U;
    jd.fiber_count = 16U;
    jd.job_capacity = 1024U;
    jd.pinning = NPH_JOB_PIN_NEVER;
    NphJobSystem *jobs = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &jobs), NPH_RESULT_OK);
    open_project(f, jobs);
    test_write_gltf(&f->src, "crate", true);
    char path[1200];
    test_path_in(&f->src, "crate.gltf", path, sizeof path);
    const char *paths[1] = {path};
    const EditorImportResult res = import_now(f, paths, 1U, "");
    NTEST_ASSERT_EQ_INT(res.result, NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(res.scene_count, 1U);

    static ResolveLoop loop;
    memset(&loop, 0, sizeof loop);
    loop.p = f->p;
    loop.guid = res.scenes[0];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(
        editor_project_resolve(f->p, loop.guid, loop.expected, sizeof loop.expected, &len),
        NPH_RESULT_OK);
    NphThreadDesc td;
    memset(&td, 0, sizeof td);
    td.fn = resolve_loop;
    td.user = &loop;
    td.name = "test-resolve";
    NphThread thread;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&thread, &td), NPH_RESULT_OK);
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_EXPECT_EQ_INT(cook_now(f, "crate.gltf", true).result, NPH_RESULT_OK);
    }
    nph_atomic_store_u32(&loop.stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    nph_thread_join(&thread);
    NTEST_EXPECT_GT_UINT(loop.calls, 0U);
    NTEST_EXPECT_EQ_UINT(loop.wrong, 0U);
    editor_project_destroy(f->p);
    f->p = NULL;
    nph_job_system_destroy(jobs);
    open_project(f, NULL);
}

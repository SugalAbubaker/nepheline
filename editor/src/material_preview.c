// SPDX-License-Identifier: Apache-2.0
// Material previews (docs/editor.md "Assets"; ADR-0105 decision 7): a lit sphere of each material
// the asset browser or the material panel shows (cook/preview.h).
//
//   - What a picture shows is named by a key from the cook database (editor_project_content_key:
//     the material's content hash and its textures'), looked up again whenever the project's
//     snapshot changes. A picture whose key no longer matches is made again; the old one shows
//     until the new one is up.
//   - A picture is made by three jobs on one counter: an I/O job reads the cached picture, or on
//     a miss the material's source and its images; a worker job (low priority) decodes, reduces
//     and draws them; an I/O job writes the picture to the cache
//     (<cooked>/previews/<key>.nph). The main thread looks at the counters at the sync point.
//   - Each picture is a GPU image of its own, made anew for a new picture (the device destroys
//     the old one once the frames using it are done), so nothing is written to an image a frame
//     may be reading.
//   - At most EDITOR_PREVIEWS_MAX pictures are kept: the one asked for longest ago gives way.
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/gpu/resources.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>

#include "editor_internal.h"
#include "ui_renderer.h"

#include <cook/cooker.h>
#include <stdio.h>
#include <string.h>

struct EditorPreviewJob {
    NphAllocator *alloc;
    NphJobSystem *jobs;
    const CookPreviewStudio *studio;
    NphJobCounter done;
    char root[EDITOR_PROJECT_PATH_MAX];        // the sources
    char source[EDITOR_PROJECT_PATH_MAX * 2U]; // the material's file
    char cache[EDITOR_PROJECT_PATH_MAX * 2U];  // its picture's
    NphGuid guid;                              // its picture's, as an asset
    u64 key;
    // Written by the jobs, read by the main thread once the counter is zero.
    NphResult result;
    b32 cached; // read from the cache
    CookMaterialSource material;
    CookPreviewFiles files;
    u8 pixels[COOK_PREVIEW_BYTES];
};

// ---------------------------------------------------------------------------
// The jobs
// ---------------------------------------------------------------------------

// I/O: the picture into the cache. A failure costs a drawing next time, nothing more.
static void write_job(void *user)
{
    EditorPreviewJob *j = (EditorPreviewJob *)user;
    char dir[EDITOR_PROJECT_PATH_MAX * 2U];
    const NphStr d = nph_path_dirname(nph_str_from_cstr(j->cache));
    (void)snprintf(dir, sizeof dir, "%.*s", (int)d.len, d.data);
    NphResult r = nph_fs_create_directories(dir);
    if (r == NPH_RESULT_OK) {
        r = cook_preview_save(j->alloc, j->cache, j->guid, j->key, j->pixels);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "caching the preview %s: %s", j->cache, nph_result_string(r));
    }
}

// A worker: the images decoded, reduced and drawn on the sphere; then the cache's write.
static void draw_job(void *user)
{
    EditorPreviewJob *j = (EditorPreviewJob *)user;
    CookPreviewImages images;
    NphResult r = cook_preview_images_make(j->alloc, &j->files, &images);
    cook_preview_files_free(&j->files);
    if (r == NPH_RESULT_OK) {
        cook_preview_render(j->studio, &j->material, &images, j->pixels);
        cook_preview_images_free(&images);
    }
    j->result = r;
    if (r == NPH_RESULT_OK) {
        // Nothing of `j` is touched after this: the write may run at once.
        const NphJobDecl decl = {write_job, j, "material preview cache"};
        NPH_IGNORE_RESULT(nph_job_run_io(j->jobs, &decl, 1U, &j->done)); // uncached: drawn again
    }
}

// I/O: the cached picture, or the material and its images for drawing.
static void read_job(void *user)
{
    EditorPreviewJob *j = (EditorPreviewJob *)user;
    void *bytes = NULL;
    u64 size = 0U;
    if (nph_file_read_all(j->cache, j->alloc, &bytes, &size) == NPH_RESULT_OK) {
        const NphResult hit = cook_preview_read(bytes, size, j->key, j->pixels);
        nph_file_free_all(j->alloc, bytes, size);
        if (hit == NPH_RESULT_OK) {
            j->cached = 1;
            j->result = NPH_RESULT_OK;
            return;
        }
    }
    NphResult r = cook_material_source_load(j->alloc, j->source, &j->material);
    if (r == NPH_RESULT_OK) {
        r = cook_preview_files_read(j->alloc, j->root, &j->material, &j->files);
    }
    if (r == NPH_RESULT_OK) {
        j->result = NPH_RESULT_OK;
        // Nothing of `j` is touched after this: the drawing may run at once.
        const NphJobDecl decl = {draw_job, j, "material preview"};
        r = nph_job_run(j->jobs, &decl, 1U, NPH_JOB_PRIORITY_LOW, &j->done);
        if (r == NPH_RESULT_OK) {
            return;
        }
        cook_preview_files_free(&j->files);
    }
    j->result = r;
}

// ---------------------------------------------------------------------------
// The pictures
// ---------------------------------------------------------------------------

void editor_material_previews_init(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    memset(&ed->previews, 0, sizeof ed->previews);
}

static void release(Editor *ed, EditorPreview *p)
{
    if (p->job != NULL) {
        nph_job_wait(nph_engine_jobs(ed->engine), &p->job->done);
        cook_preview_files_free(&p->job->files); // a draw that never ran left them
        NPH_FREE_TYPE(ed->alloc, EditorPreviewJob, p->job);
        p->job = NULL;
        ed->previews.in_flight -= 1U;
    }
    NphGpuDevice *dev = ed->engine != NULL ? nph_engine_device(ed->engine) : NULL;
    if (dev != NULL && nph_gpu_image_is_valid(dev, p->image)) {
        nph_gpu_image_destroy(dev, p->image); // deferred past the frames that use it
    }
    memset(p, 0, sizeof *p);
}

void editor_material_previews_free(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorPreviews *pv = &ed->previews;
    if (pv->items != NULL) {
        for (u32 i = 0; i < EDITOR_PREVIEWS_MAX; ++i) {
            release(ed, &pv->items[i]);
        }
        NPH_FREE_ARRAY(ed->alloc, EditorPreview, pv->items, EDITOR_PREVIEWS_MAX);
    }
    if (pv->studio != NULL) {
        NPH_FREE_TYPE(ed->alloc, CookPreviewStudio, pv->studio);
    }
    memset(pv, 0, sizeof *pv);
}

// The picture of `j` as a GPU image of its own (the old one goes once no frame uses it).
static NphResult show(Editor *ed, EditorPreview *p, const EditorPreviewJob *j)
{
    NphGpuDevice *dev = nph_engine_device(ed->engine);
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM; // sRGB bytes, as the UI's textures hold
    d.width = COOK_PREVIEW_SIZE;
    d.height = COOK_PREVIEW_SIZE;
    d.mip_count = 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    d.name = "material preview";
    NphGpuImage image;
    NPH_TRY(nph_gpu_image_create(dev, &d, &image));
    const NphResult r =
        nph_gpu_upload_image_region(dev, image, 0U, 0U, 0U, 0U, COOK_PREVIEW_SIZE,
                                    COOK_PREVIEW_SIZE, j->pixels, COOK_PREVIEW_BYTES);
    if (r != NPH_RESULT_OK) {
        nph_gpu_image_destroy(dev, image);
        return r;
    }
    if (nph_gpu_image_is_valid(dev, p->image)) {
        nph_gpu_image_destroy(dev, p->image); // deferred past the frames that use it
    }
    p->image = image;
    return NPH_RESULT_OK;
}

// A job done: its picture shown, or its failure noted.
static void collect(Editor *ed, EditorPreview *p)
{
    EditorPreviewJob *j = p->job;
    if (nph_job_counter_value(&j->done) != 0U) {
        return;
    }
    nph_job_wait(nph_engine_jobs(ed->engine), &j->done); // returns at once: the jobs' writes seen
    NphResult r = j->result;
    if (r == NPH_RESULT_OK) {
        r = show(ed, p, j);
    }
    if (r == NPH_RESULT_OK) {
        p->made = j->key;
        if (j->cached) {
            ed->previews.loaded += 1U;
        } else {
            ed->previews.drawn += 1U;
        }
    } else {
        p->failed = j->key;
        NPH_LOG_WARN(&LOG_EDITOR, "previewing %s: %s", p->key, nph_result_string(r));
    }
    NPH_FREE_TYPE(ed->alloc, EditorPreviewJob, j);
    p->job = NULL;
    ed->previews.in_flight -= 1U;
}

// Begins making the picture `p` wants.
static void start(Editor *ed, EditorPreview *p)
{
    EditorPreviewJob *j = NPH_ALLOC_TYPE(ed->alloc, EditorPreviewJob);
    if (j == NULL) {
        p->failed = p->wanted;
        return;
    }
    memset(j, 0, sizeof *j);
    j->alloc = ed->alloc;
    j->jobs = nph_engine_jobs(ed->engine);
    j->studio = ed->previews.studio;
    j->key = p->wanted;
    const char *assets = editor_project_assets_dir(ed->project);
    const char *cooked = editor_project_cooked_dir(ed->project);
    const int a = snprintf(j->root, sizeof j->root, "%s", assets);
    const int b = snprintf(j->source, sizeof j->source, "%s/%s", assets, p->key);
    const int c = snprintf(j->cache, sizeof j->cache, "%s/previews/%s.nph", cooked, p->key);
    char name[EDITOR_PROJECT_PATH_MAX + 16U];
    const int n = snprintf(name, sizeof name, "%s#preview", p->key);
    if (a < 0 || (u64)a >= sizeof j->root || b < 0 || (u64)b >= sizeof j->source || c < 0 ||
        (u64)c >= sizeof j->cache || n < 0 || (u64)n >= sizeof name) {
        NPH_FREE_TYPE(ed->alloc, EditorPreviewJob, j);
        p->failed = p->wanted;
        return;
    }
    j->guid = nph_guid_from_cstr(name);
    nph_job_counter_init(&j->done);
    const NphJobDecl decl = {read_job, j, "material preview read"};
    if (nph_job_run_io(j->jobs, &decl, 1U, &j->done) != NPH_RESULT_OK) {
        NPH_FREE_TYPE(ed->alloc, EditorPreviewJob, j);
        return; // the queues are full: tried again next frame
    }
    p->job = j;
    ed->previews.in_flight += 1U;
}

void editor_material_previews_update(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorPreviews *pv = &ed->previews;
    pv->frame += 1U;
    if (pv->items == NULL || ed->engine == NULL || ed->project == NULL) {
        return;
    }
    for (u32 i = 0; i < EDITOR_PREVIEWS_MAX; ++i) {
        if (pv->items[i].job != NULL) {
            collect(ed, &pv->items[i]);
        }
    }
    // The pictures asked for lately whose content changed, the latest asked first.
    while (pv->in_flight < EDITOR_PREVIEWS_IN_FLIGHT) {
        EditorPreview *next = NULL;
        for (u32 i = 0; i < EDITOR_PREVIEWS_MAX; ++i) {
            EditorPreview *p = &pv->items[i];
            const bool due = p->key[0] != '\0' && p->job == NULL && p->wanted != 0U &&
                             p->wanted != p->made && p->wanted != p->failed &&
                             p->used + 2U >= pv->frame;
            if (due && (next == NULL || p->used > next->used)) {
                next = p;
            }
        }
        if (next == NULL) {
            break;
        }
        const u32 before = pv->in_flight;
        start(ed, next);
        if (pv->in_flight == before && next->failed != next->wanted) {
            break; // the job queues are full: next frame
        }
    }
}

// The entry for `key`: its own, else a free one, else the one asked for longest ago that is not
// being made; NULL when every one is being made.
static EditorPreview *entry(Editor *ed, const char *key)
{
    EditorPreviews *pv = &ed->previews;
    EditorPreview *free_entry = NULL;
    EditorPreview *oldest = NULL;
    for (u32 i = 0; i < EDITOR_PREVIEWS_MAX; ++i) {
        EditorPreview *p = &pv->items[i];
        if (p->key[0] == '\0') {
            free_entry = free_entry != NULL ? free_entry : p;
            continue;
        }
        if (strcmp(p->key, key) == 0) {
            return p;
        }
        if (p->job == NULL && (oldest == NULL || p->used < oldest->used)) {
            oldest = p;
        }
    }
    EditorPreview *p = free_entry != NULL ? free_entry : oldest;
    if (p == NULL) {
        return NULL;
    }
    release(ed, p);
    (void)snprintf(p->key, sizeof p->key, "%s", key);
    return p;
}

ImTextureID editor_material_preview(Editor *ed, const char *key)
{
    NPH_ASSERT(ed != NULL && key != NULL);
    EditorPreviews *pv = &ed->previews;
    if (ed->engine == NULL || ed->project == NULL || ed->renderer == NULL || key[0] == '\0' ||
        strlen(key) >= EDITOR_PROJECT_PATH_MAX) {
        return (ImTextureID)0;
    }
    if (pv->items == NULL) {
        pv->studio = NPH_ALLOC_TYPE(ed->alloc, CookPreviewStudio);
        pv->items = NPH_ALLOC_ARRAY(ed->alloc, EditorPreview, EDITOR_PREVIEWS_MAX);
        if (pv->studio == NULL || pv->items == NULL) {
            editor_material_previews_free(ed);
            return (ImTextureID)0;
        }
        cook_preview_studio_init(pv->studio);
        memset(pv->items, 0, sizeof(EditorPreview) * EDITOR_PREVIEWS_MAX);
    }
    EditorPreview *p = entry(ed, key);
    if (p == NULL) {
        return (ImTextureID)0;
    }
    p->used = pv->frame;
    // What it should show, again whenever the project's snapshot changed.
    const u64 generation = editor_project_generation(ed->project) + 1U; // 0 is "never"
    if (p->checked != generation) {
        u64 content = 0U;
        p->wanted = editor_project_content_key(ed->project, key, "material", &content)
                        ? nph_hash_combine(content, COOK_PREVIEW_VERSION) | 1U
                        : 0U;
        p->checked = generation;
    }
    NphGpuDevice *dev = nph_engine_device(ed->engine);
    if (!nph_gpu_image_is_valid(dev, p->image)) {
        return (ImTextureID)0;
    }
    return ui_renderer_texture_id(ed->renderer, p->image, false);
}

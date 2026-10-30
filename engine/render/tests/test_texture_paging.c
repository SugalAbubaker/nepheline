// SPDX-License-Identifier: Apache-2.0
// Paged textures (docs/adr/0065-virtual-texturing.md; paging.c,
// texture_paging.glsl): a texture whose pages are all resident draws as the
// same texture resident whole, byte for byte; one with pages missing never
// samples them, nor a level until it is resident whole; pixels ask for the
// pages they sample; evicted pages stop being sampled and return their
// memory. Devices that page no textures (lavapipe: without sparse
// residency on Windows, with sparse images that sample unlike ordinary ones
// on Linux) skip all but the test of paging switched off.
#include "test_render_support.h"

#include <stdlib.h>

typedef RenderFixture Paging;

NTEST_SETUP(Paging)
{
    // Anisotropic filtering on, as material textures have it: the requests
    // follow the hardware's footprint.
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_anisotropy = 8.0F;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(Paging)
{
    test_render_teardown(f);
}

#define REQUIRE_PAGING(f)                                                                          \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_texture_paging_supported((f)->renderer)) {                               \
            NTEST_SKIP("no texture paging on this device");                                        \
        }                                                                                          \
    } while (0)

#define TEX      1024U // RGBA8: 128 x 128 pages, 8 x 8 at level 0, the tail from level 4
#define PIXELS   ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)
#define MAX_MIPS 11U

// Every level of a TEX x TEX RGBA8 texture.
typedef struct Levels {
    u32 count;
    u8 *data[MAX_MIPS];
    u64 size[MAX_MIPS];
} Levels;

// `uniform`: every texel of every level the same colour, so that any level
// samples alike and only a read of unbound memory (zeros) shows. Otherwise
// a pattern that differs from level to level and texel to texel.
static void levels_make(Levels *l, bool uniform)
{
    memset(l, 0, sizeof *l);
    l->count = nph_gpu_mip_count(TEX, TEX, 1U);
    for (u32 m = 0; m < l->count; ++m) {
        const u32 w = nph_gpu_mip_extent(TEX, m);
        l->size[m] = (u64)w * w * 4U;
        l->data[m] = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, l->size[m]);
        NTEST_ASSERT_NOT_NULL(l->data[m]);
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8 *t = l->data[m] + ((u64)y * w + x) * 4U;
                if (uniform) {
                    t[0] = 200U;
                    t[1] = 120U;
                    t[2] = 40U;
                } else {
                    // A checker of 8-texel squares, tinted by level.
                    const bool on = (((x >> 3U) ^ (y >> 3U)) & 1U) != 0U;
                    t[0] = (u8)(on ? 230U : 30U);
                    t[1] = (u8)(40U + 20U * m);
                    t[2] = (u8)((x * 7U + y * 3U) & 0xFFU);
                }
                t[3] = 255U;
            }
        }
    }
}

static void levels_free(Levels *l)
{
    for (u32 m = 0; m < l->count; ++m) {
        NPH_FREE_ARRAY(nph_allocator_system(), u8, l->data[m], l->size[m]);
    }
}

static NphRenderTexture make_texture(RenderFixture *f, const Levels *l, bool paged)
{
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    td.width = TEX;
    td.height = TEX;
    td.mip_count = l->count;
    td.mips = (const void *const *)l->data;
    td.mip_sizes = l->size;
    td.flags = paged ? (u32)NPH_RENDER_TEXTURE_FLAG_PAGED : 0U;
    td.name = paged ? "paged texture" : "whole texture";
    NphRenderTexture t = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &t), NPH_RESULT_OK);
    return t;
}

// Installs the pages `keep` (level, column, row) chooses; NULL: every page.
typedef bool (*KeepFn)(u32 mip, u32 x, u32 y);

static u32 install_pages(RenderFixture *f, NphRenderTexture t, const Levels *l, KeepFn keep)
{
    NphRenderTexturePages pages;
    NTEST_ASSERT_EQ_INT(nph_render_texture_pages(f->renderer, t, &pages), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(pages.paged);
    NTEST_EXPECT_EQ_INT(pages.format, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    NTEST_EXPECT_EQ_UINT(pages.width, TEX);
    NTEST_EXPECT_EQ_UINT(pages.height, TEX);
    u32 n = 0U;
    for (u32 m = 0; m < pages.tail_first_mip; ++m) {
        const u32 w = nph_gpu_mip_extent(TEX, m);
        for (u32 y = 0; y < pages.pages_y[m]; ++y) {
            for (u32 x = 0; x < pages.pages_x[m]; ++x) {
                if (keep != NULL && !keep(m, x, y)) {
                    continue;
                }
                const u8 *at =
                    l->data[m] + ((u64)y * pages.page_height * w + (u64)x * pages.page_width) * 4U;
                NTEST_ASSERT_EQ_INT(
                    nph_render_texture_page_install(f->renderer, t, m, x, y, at, (u64)w * 4U),
                    NPH_RESULT_OK);
                ++n;
            }
        }
    }
    return n;
}

static NphRenderMesh facing_plane(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 4.0F, 1U, 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

typedef struct Scene {
    NphRenderMesh plane;
    NphRenderMaterial material;
    NphRenderInstance instance;
    NphRenderLight light;
} Scene;

// The plane (+Y up, through the origin) with `t` repeated on it, under a
// light from above.
static void scene_make(RenderFixture *f, Scene *s, NphRenderTexture t)
{
    s->plane = facing_plane(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.roughness = 0.6F;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = t;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &s->material), NPH_RESULT_OK);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = s->plane;
    id.materials = &s->material;
    id.material_count = 1U;
    id.transform = nph_render_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &s->instance), NPH_RESULT_OK);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 2.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.2F, -1.0F, -0.3F));
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &s->light), NPH_RESULT_OK);
}

// `masked`: the base colour's alpha tested against 0.5 (the raster passes
// sample it), so a texel read as zeros would leave a hole.
static void scene_set_material(RenderFixture *f, Scene *s, NphRenderTexture t, bool masked)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.roughness = 0.6F;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = t;
    if (masked) {
        md.alpha_mode = NPH_RENDER_ALPHA_MASK;
        md.alpha_cutoff = 0.5F;
    }
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, s->material, &md), NPH_RESULT_OK);
}

static void scene_set_texture(RenderFixture *f, Scene *s, NphRenderTexture t)
{
    scene_set_material(f, s, t, false);
}

static void scene_free(RenderFixture *f, Scene *s)
{
    nph_render_light_destroy(f->renderer, s->light);
    nph_render_instance_destroy(f->renderer, s->instance);
    nph_render_material_destroy(f->renderer, s->material);
    nph_render_mesh_destroy(f->renderer, s->plane);
}

// The views the tests draw: the plane from near (level 0 at the centre),
// from farther (levels 2 and 3), and at a grazing angle (anisotropic, the
// levels sweeping from 0 to the tail).
#define VIEWS 3U

static NphRenderView view_of(u32 which, NphRenderPipeline pipeline, bool lit)
{
    static const f64 eye[VIEWS][3] = {{0.0, 0.45, 0.0}, {0.3, 2.0, 0.4}, {0.0, 0.35, 1.6}};
    static const f64 at[VIEWS][3] = {{0.02, 0.0, -0.03}, {0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}};
    const NphVec3 up = which == 2U ? nph_vec3(0.0F, 1.0F, 0.0F) : nph_vec3(0.0F, 0.0F, -1.0F);
    const NphRenderCamera cam = nph_render_camera_look_at(
        nph_dvec3(eye[which][0], eye[which][1], eye[which][2]),
        nph_dvec3(at[which][0], at[which][1], at[which][2]), up, 0.9F, 0.02F);
    NphRenderView v;
    nph_render_view_default(&v, cam);
    v.pipeline = pipeline;
    v.ev100 = 0.0F;
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3_zero();
    v.disable_ibl = 1;
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    v.ao_radius = 0.0F;
    v.gi = 0;
    if (!lit) {
        v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
    }
    return v;
}

// Renders until every page installed is resident (bound in one frame,
// copied in the next; 32 moves a frame), then draws `v` into `out`.
static void settle_and_draw(RenderFixture *f, const NphRenderView *v, u8 *out)
{
    for (u32 i = 0; i < 8U; ++i) {
        (void)test_render_read(f, v);
    }
    (void)test_render_read(f, v);
    memcpy(out, f->pixels, PIXELS);
}

static u32 bytes_differing(const u8 *a, const u8 *b)
{
    u32 n = 0U;
    for (u64 i = 0; i < PIXELS; ++i) {
        n += a[i] != b[i] ? 1U : 0U;
    }
    return n;
}

static u32 lit_pixels(const u8 *img)
{
    u32 n = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        n += (img[i] | img[i + 1U] | img[i + 2U]) != 0U ? 1U : 0U;
    }
    return n;
}

// With every page resident, a paged texture draws as the same texture
// resident whole, byte for byte: in the production path and the reference,
// lit and unlit, near, far and at a grazing angle.
NTEST_F(Paging, a_texture_with_every_page_resident_draws_as_the_whole_texture)
{
    REQUIRE_PAGING(f);
    Levels l;
    levels_make(&l, false);
    const NphRenderTexture whole = make_texture(f, &l, false);
    const NphRenderTexture paged = make_texture(f, &l, true);
    const u32 installed = install_pages(f, paged, &l, NULL);
    NTEST_EXPECT_EQ_UINT(installed, 85U);
    Scene s;
    scene_make(f, &s, whole);
    static u8 a[PIXELS];
    static u8 b[PIXELS];
    for (u32 pl = 0; pl < 2U; ++pl) {
        const NphRenderPipeline pipeline =
            pl == 0U ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE;
        if (pipeline == NPH_RENDER_PIPELINE_PRODUCTION &&
            !nph_renderer_production_supported(f->renderer)) {
            continue;
        }
        for (u32 lit = 0; lit < 2U; ++lit) {
            for (u32 w = 0; w < VIEWS; ++w) {
                const NphRenderView v = view_of(w, pipeline, lit != 0U);
                scene_set_texture(f, &s, whole);
                settle_and_draw(f, &v, a);
                scene_set_texture(f, &s, paged);
                settle_and_draw(f, &v, b);
                const u32 diff = bytes_differing(a, b);
                NTEST_LOG("%s, %s, view %u: %u pixels drawn, %u bytes differ",
                          pl == 0U ? "production" : "reference", lit != 0U ? "lit" : "unlit", w,
                          lit_pixels(a), diff);
                NTEST_EXPECT_GT_UINT(lit_pixels(a), PIXELS / 8U);
                NTEST_EXPECT_EQ_UINT(diff, 0U);
            }
        }
    }
    for (u32 m = 0; m < 4U; ++m) {
        NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->renderer, paged, m, 0U, 0U),
                            NPH_RENDER_PAGE_RESIDENT);
    }
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, paged);
    nph_render_texture_destroy(f->renderer, whole);
    levels_free(&l);
}

// Half the pages of every paged level (a checkerboard of them), and none
// at all.
static bool keep_checker(u32 mip, u32 x, u32 y)
{
    return ((x + y + mip) & 1U) == 0U;
}

static bool keep_coarse(u32 mip, u32 x, u32 y)
{
    NPH_UNUSED(x);
    NPH_UNUSED(y);
    return mip >= 2U;
}

// A texture of one colour, every level alike: whatever level a pixel
// samples, it draws as the whole texture, unless a sample read a page that
// is not resident (zeros). With pages missing in a checkerboard, from every
// view and in both paths, it draws exactly as the whole texture.
NTEST_F(Paging, a_texture_with_pages_missing_never_samples_them)
{
    REQUIRE_PAGING(f);
    Levels l;
    levels_make(&l, true);
    const NphRenderTexture whole = make_texture(f, &l, false);
    const NphRenderTexture paged = make_texture(f, &l, true);
    Scene s;
    scene_make(f, &s, whole);
    static u8 a[PIXELS];
    static u8 b[PIXELS];
    const KeepFn keeps[3] = {keep_checker, keep_coarse, NULL};
    for (u32 k = 0; k < 3U; ++k) {
        // The last case installs none: the tail alone.
        if (keeps[k] != NULL) {
            (void)install_pages(f, paged, &l, keeps[k]);
        }
        for (u32 pl = 0; pl < 2U; ++pl) {
            const NphRenderPipeline pipeline =
                pl == 0U ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE;
            if (pipeline == NPH_RENDER_PIPELINE_PRODUCTION &&
                !nph_renderer_production_supported(f->renderer)) {
                continue;
            }
            for (u32 w = 0; w < VIEWS; ++w) {
                for (u32 masked = 0; masked < 2U; ++masked) {
                    const NphRenderView v = view_of(w, pipeline, false);
                    scene_set_material(f, &s, whole, masked != 0U);
                    settle_and_draw(f, &v, a);
                    scene_set_material(f, &s, paged, masked != 0U);
                    settle_and_draw(f, &v, b);
                    const u32 diff = bytes_differing(a, b);
                    NTEST_LOG("pages %s, %s, view %u%s: %u bytes differ",
                              k == 0U   ? "in a checkerboard"
                              : k == 1U ? "from level 2"
                                        : "none",
                              pl == 0U ? "production" : "reference", w,
                              masked != 0U ? ", alpha tested" : "", diff);
                    NTEST_EXPECT_GT_UINT(lit_pixels(a), PIXELS / 8U);
                    NTEST_EXPECT_EQ_UINT(diff, 0U);
                }
            }
        }
        // The next case starts from no page resident.
        NphRenderTexturePages pages;
        NTEST_ASSERT_EQ_INT(nph_render_texture_pages(f->renderer, paged, &pages), NPH_RESULT_OK);
        for (u32 m = 0; m < pages.tail_first_mip; ++m) {
            for (u32 y = 0; y < pages.pages_y[m]; ++y) {
                for (u32 x = 0; x < pages.pages_x[m]; ++x) {
                    nph_render_texture_page_evict(f->renderer, paged, m, x, y);
                }
            }
        }
    }
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, paged);
    nph_render_texture_destroy(f->renderer, whole);
    levels_free(&l);
}

// Pixels whose pages are missing draw a coarser level, not black: with no
// level-0 or level-1 page, the near view shows level 2's tint and no zeros.
NTEST_F(Paging, a_missing_page_draws_a_coarser_level)
{
    REQUIRE_PAGING(f);
    Levels l;
    levels_make(&l, false);
    const NphRenderTexture paged = make_texture(f, &l, true);
    (void)install_pages(f, paged, &l, keep_coarse);
    Scene s;
    scene_make(f, &s, paged);
    static u8 img[PIXELS];
    const NphRenderView v = view_of(0U, NPH_RENDER_PIPELINE_REFERENCE, false);
    settle_and_draw(f, &v, img);
    // The level's tint is its green: 40 + 20 m. A level-2 sample of the
    // checker blurs its red, never its green.
    u32 level0 = 0U;
    u32 coarser = 0U;
    u32 black = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        const u32 g = img[i + 1U];
        level0 += g >= 39U && g <= 41U ? 1U : 0U;
        coarser += g >= 79U ? 1U : 0U;
        black += (img[i] | img[i + 1U] | img[i + 2U]) == 0U ? 1U : 0U;
    }
    NTEST_LOG("level-0 tints %u, level 2 or coarser %u, black %u", level0, coarser, black);
    NTEST_EXPECT_EQ_UINT(level0, 0U);
    NTEST_EXPECT_GT_UINT(coarser, (u32)(PIXELS / 4U / 2U));
    NTEST_EXPECT_EQ_UINT(black, 0U);
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, paged);
    levels_free(&l);
}

// Every page but level 0's farthest, which the near view never shows.
static bool keep_all_but_corner(u32 mip, u32 x, u32 y)
{
    return !(mip == 0U && x == 7U && y == 7U);
}

static bool keep_from_level_1(u32 mip, u32 x, u32 y)
{
    NPH_UNUSED(x);
    NPH_UNUSED(y);
    return mip >= 1U;
}

// A view close enough to magnify level 0 (about 0.6 texels a pixel) around
// the plane's centre, lit.
static NphRenderView close_view(NphRenderPipeline pipeline)
{
    NphRenderView v = view_of(0U, pipeline, true);
    v.camera = nph_render_camera_look_at(nph_dvec3(0.0, 0.15, 0.0), nph_dvec3(0.01, 0.0, -0.01),
                                         nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.02F);
    return v;
}

// A level is sampled only once every page of it is resident: with one
// level-0 page missing, one the close view does not even show, the view
// draws as though level 0 were absent, byte for byte; with it installed,
// as the whole texture. In both paths.
NTEST_F(Paging, a_level_is_sampled_only_once_resident_whole)
{
    REQUIRE_PAGING(f);
    Levels l;
    levels_make(&l, false);
    const NphRenderTexture whole = make_texture(f, &l, false);
    const NphRenderTexture coarse = make_texture(f, &l, true);
    const NphRenderTexture corner = make_texture(f, &l, true);
    (void)install_pages(f, coarse, &l, keep_from_level_1);
    (void)install_pages(f, corner, &l, keep_all_but_corner);
    Scene s;
    scene_make(f, &s, whole);
    static u8 a[PIXELS];
    static u8 b[PIXELS];
    static u8 c[PIXELS];
    for (u32 round = 0; round < 2U; ++round) {
        if (round == 1U) {
            const u8 *at = l.data[0] + ((u64)7U * 128U * TEX + (u64)7U * 128U) * 4U;
            NTEST_ASSERT_EQ_INT(
                nph_render_texture_page_install(f->renderer, corner, 0U, 7U, 7U, at, (u64)TEX * 4U),
                NPH_RESULT_OK);
        }
        for (u32 pl = 0; pl < 2U; ++pl) {
            const NphRenderPipeline pipeline =
                pl == 0U ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE;
            if (pipeline == NPH_RENDER_PIPELINE_PRODUCTION &&
                !nph_renderer_production_supported(f->renderer)) {
                continue;
            }
            const NphRenderView v = close_view(pipeline);
            scene_set_texture(f, &s, whole);
            settle_and_draw(f, &v, a);
            scene_set_texture(f, &s, coarse);
            settle_and_draw(f, &v, b);
            scene_set_texture(f, &s, corner);
            settle_and_draw(f, &v, c);
            NTEST_LOG("%s, a level-0 page %s: %u bytes differ from level 0 absent, %u from the "
                      "whole texture",
                      pl == 0U ? "production" : "reference", round == 0U ? "missing" : "installed",
                      bytes_differing(b, c), bytes_differing(a, c));
            NTEST_EXPECT_GT_UINT(bytes_differing(a, b), PIXELS / 8U);
            if (round == 0U) {
                NTEST_EXPECT_EQ_UINT(bytes_differing(b, c), 0U);
            } else {
                NTEST_EXPECT_EQ_UINT(bytes_differing(a, c), 0U);
            }
        }
    }
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, corner);
    nph_render_texture_destroy(f->renderer, coarse);
    nph_render_texture_destroy(f->renderer, whole);
    levels_free(&l);
}

// Pixels ask for the pages they sample: from the near view, after enough
// frames for every pixel's turn (one in 64 a frame) and the readback, the
// level-0 pages under the view are asked for and the far ones are not.
NTEST_F(Paging, pixels_ask_for_the_pages_they_sample)
{
    REQUIRE_PAGING(f);
    Levels l;
    levels_make(&l, false);
    const NphRenderTexture paged = make_texture(f, &l, true);
    Scene s;
    scene_make(f, &s, paged);
    for (u32 pl = 0; pl < 2U; ++pl) {
        const NphRenderPipeline pipeline =
            pl == 0U ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE;
        if (pipeline == NPH_RENDER_PIPELINE_PRODUCTION &&
            !nph_renderer_production_supported(f->renderer)) {
            continue;
        }
        const NphRenderView v = view_of(0U, pipeline, false);
        for (u32 i = 0; i < 64U + NPH_GPU_FRAMES_IN_FLIGHT + 2U; ++i) {
            (void)test_render_read(f, &v);
        }
        // The near view looks at the plane's centre: uv (0.5, 0.5) on the
        // 4 m plane, level-0 pages 3 and 4 a side around it.
        u32 near = 0U;
        for (u32 y = 3U; y <= 4U; ++y) {
            for (u32 x = 3U; x <= 4U; ++x) {
                near +=
                    nph_render_texture_page_requested(f->renderer, paged, 0U, x, y) != NPH_U64_MAX
                        ? 1U
                        : 0U;
            }
        }
        const u64 far = nph_render_texture_page_requested(f->renderer, paged, 0U, 0U, 0U);
        NTEST_LOG("%s: %u of the 4 central level-0 pages asked for; corner page %s",
                  pl == 0U ? "production" : "reference", near,
                  far == NPH_U64_MAX ? "not asked for" : "asked for");
        NTEST_EXPECT_GE_UINT(near, 1U);
        NTEST_EXPECT_EQ_UINT(far, NPH_U64_MAX);
    }
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, paged);
    levels_free(&l);
}

// An evicted page stops being sampled at once, and its memory returns once
// the frames that could read it are done.
NTEST_F(Paging, an_evicted_page_returns_its_memory_after_the_frames_in_flight)
{
    REQUIRE_PAGING(f);
    Levels l;
    levels_make(&l, true);
    const NphRenderTexture paged = make_texture(f, &l, true);
    (void)install_pages(f, paged, &l, NULL);
    Scene s;
    scene_make(f, &s, paged);
    static u8 img[PIXELS];
    const NphRenderView v = view_of(0U, NPH_RENDER_PIPELINE_REFERENCE, false);
    settle_and_draw(f, &v, img);
    NphRenderFrameStats st = test_render_read(f, &v);
    const u32 before = st.pages_resident;
    NTEST_EXPECT_GE_UINT(before, 85U);
    NphRenderPageRoom room0;
    nph_render_texture_page_room(f->renderer, &room0);
    NTEST_EXPECT_GT_UINT(room0.pages, before);
    NTEST_EXPECT_LE_UINT(room0.pages_free + before, room0.pages);
    NTEST_EXPECT_EQ_UINT(room0.pages_returning, 0U);
    NTEST_EXPECT_GT_UINT(room0.staging_free, 0U);
    for (u32 y = 0; y < 8U; ++y) {
        for (u32 x = 0; x < 8U; ++x) {
            nph_render_texture_page_evict(f->renderer, paged, 0U, x, y);
        }
    }
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->renderer, paged, 0U, 3U, 3U),
                        NPH_RENDER_PAGE_EVICTING);
    // Returning, their memory not yet free.
    NphRenderPageRoom room;
    nph_render_texture_page_room(f->renderer, &room);
    NTEST_EXPECT_EQ_UINT(room.pages_returning, 64U);
    NTEST_EXPECT_EQ_UINT(room.pages_free, room0.pages_free);
    // Drawn at once from level 1 (the colour is the same), never from the
    // pages leaving.
    st = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(bytes_differing(img, f->pixels), 0U);
    u32 evicted = st.pages_evicted;
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT + 2U; ++i) {
        st = test_render_read(f, &v);
        evicted += st.pages_evicted;
    }
    NTEST_EXPECT_EQ_UINT(evicted, 64U);
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->renderer, paged, 0U, 3U, 3U),
                        NPH_RENDER_PAGE_ABSENT);
    // Freed with the frame: the pool counts them free a few frames on.
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT + 1U; ++i) {
        st = test_render_read(f, &v);
    }
    NTEST_LOG("pages resident: %u before, %u after", before, st.pages_resident);
    NTEST_EXPECT_EQ_UINT(st.pages_resident, before - 64U);
    NTEST_EXPECT_EQ_UINT(bytes_differing(img, f->pixels), 0U);
    nph_render_texture_page_room(f->renderer, &room);
    NTEST_EXPECT_EQ_UINT(room.pages_returning, 0U);
    NTEST_EXPECT_EQ_UINT(room.pages_free, room0.pages_free + 64U);
    // A page evicting is resident again at once when installed.
    nph_render_texture_page_evict(f->renderer, paged, 1U, 0U, 0U);
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->renderer, paged, 1U, 0U, 0U),
                        NPH_RENDER_PAGE_EVICTING);
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_install(f->renderer, paged, 1U, 0U, 0U, l.data[1],
                                                        (u64)(TEX / 2U) * 4U),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->renderer, paged, 1U, 0U, 0U),
                        NPH_RENDER_PAGE_RESIDENT);
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, paged);
    levels_free(&l);
}

// Paging switched off: a texture made PAGED is resident whole and draws as
// one.
NTEST(Paging_off, a_paged_texture_is_resident_whole_without_paging)
{
    RenderFixture fx;
    RenderFixture *f = &fx;
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.disable_texture_paging = 1;
    test_render_setup(f, &d);
    if (f->dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_EXPECT_FALSE(nph_renderer_texture_paging_supported(f->renderer));
    Levels l;
    levels_make(&l, false);
    const NphRenderTexture whole = make_texture(f, &l, false);
    const NphRenderTexture paged = make_texture(f, &l, true);
    NphRenderTexturePages pages;
    NTEST_ASSERT_EQ_INT(nph_render_texture_pages(f->renderer, paged, &pages), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(pages.paged);
    NTEST_EXPECT_EQ_INT(
        nph_render_texture_page_install(f->renderer, paged, 0U, 0U, 0U, l.data[0], 0U),
        NPH_RESULT_INVALID_ARGUMENT);
    // Resident whole: no levels to ask for, every level resident, no room.
    u64 asked[NPH_GPU_MAX_MIPS];
    NTEST_EXPECT_EQ_INT(nph_render_texture_levels_requested(f->renderer, paged, asked),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(asked[0], NPH_U64_MAX);
    NTEST_EXPECT_EQ_UINT(nph_render_texture_first_resident_mip(f->renderer, paged), 0U);
    NphRenderPageRoom room;
    nph_render_texture_page_room(f->renderer, &room);
    NTEST_EXPECT_EQ_UINT(room.pages + room.pages_free + room.pages_returning + room.staging_free,
                         0U);
    Scene s;
    scene_make(f, &s, whole);
    static u8 a[PIXELS];
    static u8 b[PIXELS];
    const NphRenderView v = view_of(2U, NPH_RENDER_PIPELINE_REFERENCE, true);
    settle_and_draw(f, &v, a);
    scene_set_texture(f, &s, paged);
    settle_and_draw(f, &v, b);
    NTEST_EXPECT_EQ_UINT(bytes_differing(a, b), 0U);
    scene_free(f, &s);
    nph_render_texture_destroy(f->renderer, paged);
    nph_render_texture_destroy(f->renderer, whole);
    // Stale handles.
    NTEST_EXPECT_EQ_INT(nph_render_texture_levels_requested(f->renderer, paged, asked),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(nph_render_texture_first_resident_mip(f->renderer, paged), 0U);
    levels_free(&l);
    test_render_teardown(f);
}

// Textures still alive when a renderer that pages none is destroyed go with
// it (it has no paged textures' state to look in: a lavapipe crash in CI,
// where a test left a texture alive).
NTEST(Paging_off, textures_alive_at_destruction_go_with_the_renderer)
{
    RenderFixture fx;
    RenderFixture *f = &fx;
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.disable_texture_paging = 1;
    test_render_setup(f, &d);
    if (f->dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    Levels l;
    levels_make(&l, false);
    (void)make_texture(f, &l, false);
    (void)make_texture(f, &l, true);
    levels_free(&l);
    test_render_teardown(f); // destroys them, logging that they were alive
}

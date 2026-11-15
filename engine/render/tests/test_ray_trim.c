// SPDX-License-Identifier: Apache-2.0
// Triangles for the rays alone (render/renderer.h NphRenderMeshDesc.ray_submeshes;
// docs/render.md "Rays and trimmed cards"; ADR-0096): a card given as parts of its triangles
// casts the shadow the card itself does, through an alpha test that reads each hit's place in
// its parent; parts that leave out what the test cuts away cast it too; parts that leave out
// more do not, which shows the rays take the parts; what is given is checked; an instance
// whose materials do not hold the parts' alpha key traces its mesh's own triangles (ADR-0098);
// and a paged card's rays take its parts only while the texture's finest level is resident.
// The traced tests are skipped where the device has no ray queries (and the paged one where
// it pages no textures); the fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include <nepheline/math/pack.h>

#include "test_shadow_support.h"

typedef RenderFixture RayTrim;

#define RT_FLAGS ((u32)NPH_RENDER_VIS_RT_SHADOWS)
#define CARD_KEY 77U

NTEST_SETUP(RayTrim)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(RayTrim)
{
    test_render_teardown(f);
}

// A point of a triangle as a ray triangle's corner: the weights of its second and third
// corners, in 65535ths.
#define AT(second, third) ((u32)(second) | ((u32)(third) << 16U))

// The checker's solid quarters (test_shadow_support.h checker_mask_desc: its first and last
// texels are clear).
static bool solid(f32 u, f32 v)
{
    return (u >= 0.5F) != (v >= 0.5F);
}

static bool everywhere(f32 u, f32 v)
{
    (void)u;
    (void)v;
    return true;
}

static bool one_solid_quarter(f32 u, f32 v)
{
    return u >= 0.5F && v < 0.5F;
}

// The card's two triangles each split in four at its edges' middles (the card is a unit
// square of the texture, so each part lies in one quarter of the checker), those kept whose
// middle `keep` takes; returns how many.
static u32 card_parts(const NphRenderMeshData *d, bool (*keep)(f32, f32), NphRenderRayTriangle *out)
{
    static const u32 k_parts[4][3] = {
        {AT(0U, 0U), AT(32768U, 0U), AT(0U, 32768U)},
        {AT(32768U, 0U), AT(65535U, 0U), AT(32768U, 32767U)},
        {AT(0U, 32768U), AT(32768U, 32767U), AT(0U, 65535U)},
        {AT(32768U, 0U), AT(32768U, 32767U), AT(0U, 32768U)},
    };
    u32 n = 0U;
    for (u32 t = 0; t < d->index_count / 3U; ++t) {
        NphVec2 uv[3];
        for (u32 c = 0; c < 3U; ++c) {
            uv[c] = nph_unpack_f16x2(d->attribs[d->indices[t * 3U + c]].uv0);
        }
        for (u32 p = 0; p < 4U; ++p) {
            f32 u = 0.0F;
            f32 v = 0.0F;
            for (u32 c = 0; c < 3U; ++c) {
                const f32 w1 = (f32)(k_parts[p][c] & 0xFFFFU) / 65535.0F;
                const f32 w2 = (f32)(k_parts[p][c] >> 16U) / 65535.0F;
                u += (uv[0].x * (1.0F - w1 - w2) + uv[1].x * w1 + uv[2].x * w2) / 3.0F;
                v += (uv[0].y * (1.0F - w1 - w2) + uv[1].y * w1 + uv[2].y * w2) / 3.0F;
            }
            if (!keep(u, v)) {
                continue;
            }
            out[n].parent = t * 3U;
            memcpy(out[n].corner, k_parts[p], sizeof out[n].corner);
            ++n;
        }
    }
    return n;
}

// The card's two triangles each as two halves, through its first corner and the middle of the
// edge across from it: each half lies over quarters of the checker that are solid and quarters
// that are cut away, so its shadow is the card's only where every hit reads its alpha at its
// own place in its parent.
static bool halves(f32 u, f32 v)
{
    (void)u;
    (void)v;
    return true;
}

static u32 card_halves(const NphRenderMeshData *d, NphRenderRayTriangle *out)
{
    static const u32 k_halves[2][3] = {
        {AT(0U, 0U), AT(65535U, 0U), AT(32768U, 32767U)},
        {AT(0U, 0U), AT(32768U, 32767U), AT(0U, 65535U)},
    };
    u32 n = 0U;
    for (u32 t = 0; t < d->index_count / 3U; ++t) {
        for (u32 p = 0; p < 2U; ++p) {
            out[n].parent = t * 3U;
            memcpy(out[n].corner, k_halves[p], sizeof out[n].corner);
            ++n;
        }
    }
    return n;
}

// The card: a square of two triangles, with the parts `keep` takes as its triangles for the
// rays (NULL: none, the rays take its own; `halves`: its triangles' halves).
static NphRenderMesh card_mesh(RayTrim *f, bool (*keep)(f32, f32))
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 1U, 1.0F, &data),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(data.index_count, 6U);
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, "card", &sub, &md);
    NphRenderRayTriangle parts[8];
    NphRenderRaySubmesh run = {0U, 0U, 0U};
    if (keep != NULL) {
        run.count = keep == halves ? card_halves(&data, parts) : card_parts(&data, keep, parts);
        run.alpha_key = CARD_KEY;
        md.ray_submeshes = &run;
        md.ray_triangles = parts;
        md.ray_triangle_count = run.count;
    }
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh), NPH_RESULT_OK);
    nph_render_mesh_data_free(&data);
    return mesh;
}

// The checker's material with the card's key.
static NphRenderMaterial card_material(RayTrim *f, NphRenderMaterialDesc *out_desc)
{
    *out_desc = checker_mask_desc(f);
    out_desc->alpha_key = CARD_KEY;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, out_desc, &m), NPH_RESULT_OK);
    return m;
}

// The quarters of the card's shadow on the floor (test_shadow_support.h CheckerScene) that
// are black in `on`, a bit each, a quarter of a metre inside each; none may be neither black
// nor as `off` has it.
static u32 dark_quarters(const u8 *on, const u8 *off)
{
    const f32 xs[3] = {-2.5F, -0.5F, 1.5F};
    const f32 zs[3] = {-2.0F, 0.0F, 2.0F};
    u32 dark = 0U;
    for (u32 i = 0; i < 2U; ++i) {
        for (u32 j = 0; j < 2U; ++j) {
            const Quarter q = quarter_of(on, off, xs[i] + 0.25F, xs[i + 1U] - 0.25F, zs[j] + 0.25F,
                                         zs[j + 1U] - 0.25F);
            NTEST_EXPECT_GT_UINT(q.dark + q.lit, 100U);
            NTEST_EXPECT_EQ_UINT(q.other, 0U);
            NTEST_EXPECT_TRUE(q.dark == 0U || q.lit == 0U);
            dark |= q.dark != 0U ? 1U << (i * 2U + j) : 0U;
        }
    }
    return dark;
}

static u32 bits_set(u32 v)
{
    u32 n = 0U;
    for (; v != 0U; v &= v - 1U) {
        ++n;
    }
    return n;
}

// The sun through a card of the checker, 3 m over the floor: the card's own triangles, then
// their halves, then every quarter part of them, then the parts in the solid quarters alone,
// then one solid quarter's.
NTEST_F(RayTrim, a_cards_parts_cast_the_cards_shadow)
{
    REQUIRE_RENDERER(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    const NphRenderMesh floor = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, floor, grey, at(0.0, 0.0, 0.0, 6.0F));
    NphRenderMaterialDesc md;
    const NphRenderMaterial mask = card_material(f, &md);
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    bool (*const keeps[5])(f32, f32) = {NULL, halves, everywhere, solid, one_solid_quarter};
    static const char *const names[5] = {"the card's own triangles", "their halves", "every part",
                                         "the solid quarters' parts", "one solid quarter's parts"};
    static u8 off[PIXELS];
    static u8 own[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        const bool production = k_pipelines[k] == NPH_RENDER_PIPELINE_PRODUCTION;
        NphRenderView v;
        shadow_view(&v, top_camera(), 13.0F, k_pipelines[k], RT_FLAGS);
        u32 own_dark = 0U;
        for (u32 variant = 0; variant < 5U; ++variant) {
            const NphRenderMesh card = card_mesh(f, keeps[variant]);
            const NphRenderInstance inst = instance(f, card, mask, at(-3.5, 3.0, 0.0, 4.0F));
            set_shadows(f, sun, &ld, false);
            render_into(f, &v, off);
            set_shadows(f, sun, &ld, true);
            const NphRenderFrameStats stats = render_into(f, &v, variant == 0U ? own : on);
            NTEST_EXPECT_TRUE(stats.shadow_rays);
            const u32 dark = dark_quarters(variant == 0U ? own : on, off);
            NTEST_LOG("%s, %s: quarters black %x", k_pipeline_names[k], names[variant], dark);
            if (variant == 0U) {
                own_dark = dark;
                // The production path's rays pass where the card is cut away, a diagonal's
                // quarters; the reference's take every triangle for opaque.
                NTEST_EXPECT_TRUE(production ? (dark == 0x6U || dark == 0x9U) : dark == 0xFU);
            } else if (variant <= 2U) {
                // The whole card in parts: the card's shadow, each hit's alpha read at its
                // place in its parent (a half holds solid and clear quarters alike).
                NTEST_EXPECT_EQ_UINT(dark, own_dark);
                NTEST_EXPECT_LE_UINT(image_max_diff(on, own), SAME_TOL);
            } else if (variant == 3U) {
                // Without what the test cuts away, the production path's shadow is the same.
                // The reference's rays, which test nothing, now meet only what is there.
                NTEST_EXPECT_EQ_UINT(bits_set(dark), 2U);
                if (production) {
                    NTEST_EXPECT_EQ_UINT(dark, own_dark);
                    NTEST_EXPECT_LE_UINT(image_max_diff(on, own), SAME_TOL);
                }
            } else {
                // A part that is not given casts nothing: the rays take the parts.
                NTEST_EXPECT_EQ_UINT(bits_set(dark), 1U);
            }
            nph_render_instance_destroy(f->renderer, inst);
            nph_render_mesh_destroy(f->renderer, card);
        }
    }
}

// What is given as a mesh's triangles for the rays is checked against the mesh.
NTEST_F(RayTrim, ray_triangles_that_are_not_as_described_are_refused)
{
    REQUIRE_RENDERER(f);
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 1U, 1.0F, &data),
                        NPH_RESULT_OK);
    // Two submeshes of a triangle each, the second trimmed to two parts.
    NphRenderSubmesh subs[2];
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, "card", &subs[0], &md);
    subs[0].index_count = 3U;
    subs[1] = subs[0];
    subs[1].index_offset = 3U;
    subs[1].material_slot = 1U;
    md.submeshes = subs;
    md.submesh_count = 2U;
    NphRenderRaySubmesh runs[2] = {{0U, 0U, 0U}, {0U, 2U, CARD_KEY}};
    NphRenderRayTriangle parts[2] = {
        {3U, {AT(0U, 0U), AT(65535U, 0U), AT(0U, 65535U)}},
        {3U, {AT(100U, 100U), AT(40000U, 100U), AT(100U, 25435U)}},
    };
    md.ray_submeshes = runs;
    md.ray_triangles = parts;
    md.ray_triangle_count = 2U;
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh), NPH_RESULT_OK);
    nph_render_mesh_destroy(f->renderer, mesh);
#define EXPECT_REFUSED(change, restore)                                                            \
    do {                                                                                           \
        change;                                                                                    \
        NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh),                       \
                            NPH_RESULT_INVALID_ARGUMENT);                                          \
        NTEST_EXPECT_EQ_UINT(mesh.value, 0U);                                                      \
        restore;                                                                                   \
    } while (0)
    // Runs without their triangles, or that do not fill them.
    EXPECT_REFUSED(md.ray_triangles = NULL, md.ray_triangles = parts);
    EXPECT_REFUSED(md.ray_triangle_count = 0U, md.ray_triangle_count = 2U);
    EXPECT_REFUSED(md.ray_triangle_count = 3U, md.ray_triangle_count = 2U);
    EXPECT_REFUSED(runs[1].count = 1U, runs[1].count = 2U);
    EXPECT_REFUSED(runs[1].count = 3U, runs[1].count = 2U);
    EXPECT_REFUSED(runs[1].first = 1U, runs[1].first = 0U);
    // A run without a key, and a run of none with one or with a start.
    EXPECT_REFUSED(runs[1].alpha_key = 0U, runs[1].alpha_key = CARD_KEY);
    EXPECT_REFUSED(runs[0].alpha_key = 5U, runs[0].alpha_key = 0U);
    EXPECT_REFUSED(runs[0].first = 1U, runs[0].first = 0U);
    // A parent in another submesh, past the indices, or no triangle's first index.
    EXPECT_REFUSED(parts[1].parent = 0U, parts[1].parent = 3U);
    EXPECT_REFUSED(parts[1].parent = 6U, parts[1].parent = 3U);
    EXPECT_REFUSED(parts[1].parent = 4U, parts[1].parent = 3U);
    // A corner outside its parent.
    EXPECT_REFUSED(parts[0].corner[2] = AT(1U, 65535U), parts[0].corner[2] = AT(0U, 65535U));
#undef EXPECT_REFUSED
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh), NPH_RESULT_OK);
    nph_render_mesh_destroy(f->renderer, mesh);
    nph_render_mesh_data_free(&data);
}

// The card's shadow under the sun: `v` drawn without shadows into `off` and with them into
// `on`, and the quarters black in `on` (dark_quarters).
static u32 card_shadow(RayTrim *f, const NphRenderView *v, NphRenderLight sun,
                       NphRenderLightDesc *ld, u8 *off, u8 *on)
{
    set_shadows(f, sun, ld, false);
    render_into(f, v, off);
    set_shadows(f, sun, ld, true);
    const NphRenderFrameStats stats = render_into(f, v, on);
    NTEST_EXPECT_TRUE(stats.shadow_rays);
    return dark_quarters(on, off);
}

// The most records of the production path's top level that named their mesh's own triangles
// (NphRenderFrameStats.vis_rt_own) over the frames after one that traced nothing, whose
// successor rebuilds the top level: the count comes back a few frames late.
static u32 own_records(RayTrim *f, const NphRenderView *v, NphRenderLight sun,
                       NphRenderLightDesc *ld)
{
    set_shadows(f, sun, ld, false);
    (void)test_render_read(f, v);
    set_shadows(f, sun, ld, true);
    u32 most = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        most = nph_max_u32(most, test_render_read(f, v).vis_rt_own);
    }
    return most;
}

static NphRenderInstance card_instance(RayTrim *f, NphRenderMesh card, const NphRenderMaterial *m)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = card;
    id.transform = at(-3.5, 3.0, 0.0, 4.0F);
    id.material_count = m != NULL ? 1U : 0U;
    id.materials = m;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

// An instance of a mesh whose triangles for the rays alone are one solid quarter's parts
// (which the checker's key alone holds, and which cast a shadow of their own: one quarter's)
// traces them with a material of their key, and its mesh's own triangles with any other
// (ADR-0098): another key, none, no material at all (the default, which tests no alpha). Its
// shadow is then the card's own, to the pixel. A material that changes its key turns it so
// from the next frame, and back; one destroyed under it leaves it the default's.
NTEST_F(RayTrim, an_instance_traces_its_meshs_own_triangles_where_its_materials_do_not_hold_them)
{
    REQUIRE_RENDERER(f);
    // A key is an alpha-tested material's alone.
    NphRenderMaterialDesc md = checker_mask_desc(f);
    md.alpha_key = CARD_KEY;
    NphRenderMaterialDesc opaque = md;
    opaque.alpha_mode = NPH_RENDER_ALPHA_OPAQUE;
    NphRenderMaterial refused = NPH_RENDER_MATERIAL_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_material_create(f->renderer, &opaque, &refused),
                        NPH_RESULT_INVALID_ARGUMENT);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    const NphRenderMesh floor = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, floor, grey, at(0.0, 0.0, 0.0, 6.0F));
    const NphRenderMaterial keyed = card_material(f, &md);
    NphRenderMaterialDesc other_desc = md;
    other_desc.alpha_key = CARD_KEY + 1U;
    NphRenderMaterial other = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &other_desc, &other),
                        NPH_RESULT_OK);
    NphRenderMaterialDesc unkeyed_desc = md;
    unkeyed_desc.alpha_key = 0U;
    NphRenderMaterial unkeyed = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &unkeyed_desc, &unkeyed),
                        NPH_RESULT_OK);
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderMesh plain = card_mesh(f, NULL);
    const NphRenderMesh card = card_mesh(f, one_solid_quarter);
    static u8 off[PIXELS];
    static u8 own[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        const bool production = k_pipelines[k] == NPH_RENDER_PIPELINE_PRODUCTION;
        NphRenderView v;
        shadow_view(&v, top_camera(), 13.0F, k_pipelines[k], RT_FLAGS);
        // The card's own triangles with the keyed material: the checker's diagonal, or on the
        // reference, whose rays test no alpha, the whole card.
        NphRenderInstance inst = card_instance(f, plain, &keyed);
        const u32 own_dark = card_shadow(f, &v, sun, &ld, off, own);
        NTEST_EXPECT_TRUE(production ? (own_dark == 0x6U || own_dark == 0x9U) : own_dark == 0xFU);
        nph_render_instance_destroy(f->renderer, inst);
        // Its parts with that key: one quarter.
        inst = card_instance(f, card, &keyed);
        NTEST_EXPECT_EQ_UINT(bits_set(card_shadow(f, &v, sun, &ld, off, on)), 1U);
        if (production) {
            NTEST_EXPECT_EQ_UINT(own_records(f, &v, sun, &ld), 0U);
        }
        nph_render_instance_destroy(f->renderer, inst);
        // Another key and none: the card's own shadow, to the pixel.
        const NphRenderMaterial others[2] = {other, unkeyed};
        for (u32 o = 0; o < 2U; ++o) {
            inst = card_instance(f, card, &others[o]);
            NTEST_EXPECT_EQ_UINT(card_shadow(f, &v, sun, &ld, off, on), own_dark);
            NTEST_EXPECT_LE_UINT(image_max_diff(on, own), SAME_TOL);
            if (production) {
                NTEST_EXPECT_EQ_UINT(own_records(f, &v, sun, &ld), 1U);
            }
            nph_render_instance_destroy(f->renderer, inst);
        }
        // No material: the default, opaque, whose shadow is the whole card's.
        inst = card_instance(f, card, NULL);
        NTEST_EXPECT_EQ_UINT(card_shadow(f, &v, sun, &ld, off, on), 0xFU);
        nph_render_instance_destroy(f->renderer, inst);
        // The keyed material given another key while the instance lives, then its own again.
        inst = card_instance(f, card, &keyed);
        NphRenderMaterialDesc changed = md;
        changed.alpha_key = CARD_KEY + 2U;
        NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, keyed, &changed),
                            NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(card_shadow(f, &v, sun, &ld, off, on), own_dark);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, own), SAME_TOL);
        NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, keyed, &md), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(bits_set(card_shadow(f, &v, sun, &ld, off, on)), 1U);
        nph_render_instance_destroy(f->renderer, inst);
        // A material of the key destroyed under its instance, which draws the default after.
        NphRenderMaterial doomed = NPH_RENDER_MATERIAL_NULL;
        NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &doomed), NPH_RESULT_OK);
        inst = card_instance(f, card, &doomed);
        NTEST_EXPECT_EQ_UINT(bits_set(card_shadow(f, &v, sun, &ld, off, on)), 1U);
        nph_render_material_destroy(f->renderer, doomed);
        NTEST_EXPECT_EQ_UINT(card_shadow(f, &v, sun, &ld, off, on), 0xFU);
        nph_render_instance_destroy(f->renderer, inst);
    }
    nph_render_mesh_destroy(f->renderer, card);
    nph_render_mesh_destroy(f->renderer, plain);
}

// The checker's quarters (checker_mask_desc) at PAGED_TEX texels a side, every level box
// filtered down from the finest: the quarters stay exact down to two texels, so a nearest
// lookup reads the same alpha at the same point of any level (sRGB colour: white).
#define PAGED_TEX  1024U // RGBA8: pages of 128 x 128, 8 x 8 at level 0, the tail from level 4
#define PAGED_MIPS 11U

typedef struct PagedChecker {
    u32 count;
    u8 *data[PAGED_MIPS];
    u64 size[PAGED_MIPS];
} PagedChecker;

static void paged_checker_make(PagedChecker *c)
{
    memset(c, 0, sizeof *c);
    c->count = nph_gpu_mip_count(PAGED_TEX, PAGED_TEX, 1U);
    for (u32 m = 0; m < c->count; ++m) {
        const u32 w = nph_gpu_mip_extent(PAGED_TEX, m);
        c->size[m] = (u64)w * w * 4U;
        c->data[m] = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, c->size[m]);
        NTEST_ASSERT_NOT_NULL(c->data[m]);
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8 *t = c->data[m] + ((u64)y * w + x) * 4U;
                t[0] = 255U;
                t[1] = 255U;
                t[2] = 255U;
                // 1 x 1, the average of the quarters (no lookup of the tests reads it).
                t[3] = w == 1U ? 128U : (((x >= w / 2U) != (y >= w / 2U)) ? 255U : 0U);
            }
        }
    }
}

static void paged_checker_free(PagedChecker *c)
{
    for (u32 m = 0; m < c->count; ++m) {
        NPH_FREE_ARRAY(nph_allocator_system(), u8, c->data[m], c->size[m]);
    }
}

// Draws frames of `v` until the texture's first resident level is `mip`; false if it is not
// after 32.
static bool settle_level(RayTrim *f, const NphRenderView *v, NphRenderTexture t, u32 mip)
{
    for (u32 i = 0; i < 32U; ++i) {
        if (nph_render_texture_first_resident_mip(f->renderer, t) == mip) {
            return true;
        }
        (void)test_render_read(f, v);
    }
    return nph_render_texture_first_resident_mip(f->renderer, t) == mip;
}

// A card whose texture is paged (ADR-0065): its rays read the texture's first resident level,
// and take its triangles for the rays alone (the solid quarter's parts) only while that is
// level 0, which their key was made of (ADR-0098). With the tail alone resident, its own
// triangles' shadow, to the pixel; with every level resident, its parts' (one quarter); with
// a page of level 0 evicted, its own triangles' again.
NTEST_F(RayTrim, a_paged_cards_rays_take_its_parts_only_while_its_finest_level_is_resident)
{
    REQUIRE_RENDERER(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer) ||
        !nph_renderer_texture_paging_supported(f->renderer)) {
        NTEST_SKIP("no ray queries or no texture paging on this device");
    }
    PagedChecker c;
    paged_checker_make(&c);
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.width = PAGED_TEX;
    td.height = PAGED_TEX;
    td.mip_count = c.count;
    td.mips = (const void *const *)c.data;
    td.mip_sizes = c.size;
    td.flags = (u32)NPH_RENDER_TEXTURE_FLAG_PAGED;
    td.name = "paged checker";
    NphRenderTexture tex = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderTexturePages pages;
    NTEST_ASSERT_EQ_INT(nph_render_texture_pages(f->renderer, tex, &pages), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(pages.paged);
    NTEST_ASSERT_GT_UINT(pages.tail_first_mip, 0U);

    const NphRenderMesh floor = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, floor, grey, at(0.0, 0.0, 0.0, 6.0F));
    // checker_mask_desc's material, on the paged checker.
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.alpha_mode = NPH_RENDER_ALPHA_MASK;
    md.double_sided = 1;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = tex;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].nearest = 1;
    md.alpha_key = CARD_KEY;
    NphRenderMaterial keyed = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &keyed), NPH_RESULT_OK);
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderMesh plain = card_mesh(f, NULL);
    const NphRenderMesh card = card_mesh(f, one_solid_quarter);
    static u8 off[PIXELS];
    static u8 own[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        const bool production = k_pipelines[k] == NPH_RENDER_PIPELINE_PRODUCTION;
        NphRenderView v;
        shadow_view(&v, top_camera(), 13.0F, k_pipelines[k], RT_FLAGS);
        // The tail alone (the first pipeline's run evicted what it installed).
        NTEST_ASSERT_TRUE(settle_level(f, &v, tex, pages.tail_first_mip));
        NphRenderInstance inst = card_instance(f, plain, &keyed);
        const u32 own_dark = card_shadow(f, &v, sun, &ld, off, own);
        nph_render_instance_destroy(f->renderer, inst);
        inst = card_instance(f, card, &keyed);
        NTEST_EXPECT_EQ_UINT(card_shadow(f, &v, sun, &ld, off, on), own_dark);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, own), SAME_TOL);
        if (production) {
            NTEST_EXPECT_EQ_UINT(own_records(f, &v, sun, &ld), 1U);
        }
        // Every level resident: its parts.
        for (u32 m = 0; m < pages.tail_first_mip; ++m) {
            const u32 w = nph_gpu_mip_extent(PAGED_TEX, m);
            for (u32 y = 0; y < pages.pages_y[m]; ++y) {
                for (u32 x = 0; x < pages.pages_x[m]; ++x) {
                    const u8 *texels =
                        c.data[m] +
                        ((u64)y * pages.page_height * w + (u64)x * pages.page_width) * 4U;
                    NTEST_ASSERT_EQ_INT(nph_render_texture_page_install(f->renderer, tex, m, x, y,
                                                                        texels, (u64)w * 4U),
                                        NPH_RESULT_OK);
                }
            }
        }
        NTEST_ASSERT_TRUE(settle_level(f, &v, tex, 0U));
        NTEST_EXPECT_EQ_UINT(bits_set(card_shadow(f, &v, sun, &ld, off, on)), 1U);
        if (production) {
            NTEST_EXPECT_EQ_UINT(own_records(f, &v, sun, &ld), 0U);
        }
        // A page of level 0 gone: level 1 is the finest the rays read, and its own triangles
        // are back.
        nph_render_texture_page_evict(f->renderer, tex, 0U, 0U, 0U);
        NTEST_ASSERT_TRUE(settle_level(f, &v, tex, 1U));
        NTEST_EXPECT_EQ_UINT(card_shadow(f, &v, sun, &ld, off, on), own_dark);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, own), SAME_TOL);
        nph_render_instance_destroy(f->renderer, inst);
        // Back to the tail for the next pipeline.
        for (u32 m = 0; m < pages.tail_first_mip; ++m) {
            for (u32 y = 0; y < pages.pages_y[m]; ++y) {
                for (u32 x = 0; x < pages.pages_x[m]; ++x) {
                    nph_render_texture_page_evict(f->renderer, tex, m, x, y);
                }
            }
        }
    }
    nph_render_mesh_destroy(f->renderer, card);
    nph_render_mesh_destroy(f->renderer, plain);
    nph_render_material_destroy(f->renderer, keyed);
    nph_render_texture_destroy(f->renderer, tex);
    paged_checker_free(&c);
}

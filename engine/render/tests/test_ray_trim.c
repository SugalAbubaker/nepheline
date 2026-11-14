// SPDX-License-Identifier: Apache-2.0
// Triangles for the rays alone (render/renderer.h NphRenderMeshDesc.ray_submeshes;
// docs/render.md "Rays and trimmed cards"; ADR-0096): a card given as parts of its triangles
// casts the shadow the card itself does, through an alpha test that reads each hit's place in
// its parent; parts that leave out what the test cuts away cast it too; parts that leave out
// more do not, which shows the rays take the parts; what is given is checked; and an instance
// is held to the alpha keys its mesh's parts were made for. The traced tests are skipped where
// the device has no ray queries; the fixture's teardown asserts zero validation and
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

// An instance of a mesh with triangles for the rays is made only with materials of the keys
// they were made for, and such a material keeps its key while the instance lives.
NTEST_F(RayTrim, an_instance_is_held_to_the_keys_its_meshs_ray_triangles_were_made_for)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh card = card_mesh(f, solid);
    const NphRenderMesh plain = card_mesh(f, NULL);
    NphRenderMaterialDesc md;
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
    // A key is an alpha-tested material's alone.
    NphRenderMaterialDesc opaque = md;
    opaque.alpha_mode = NPH_RENDER_ALPHA_OPAQUE;
    NphRenderMaterial refused = NPH_RENDER_MATERIAL_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_material_create(f->renderer, &opaque, &refused),
                        NPH_RESULT_INVALID_ARGUMENT);

    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = card;
    id.transform = at(0.0, 1.0, 0.0, 1.0F);
    id.material_count = 1U;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    // Another key, no key, and no material at all (the default material cuts nothing away).
    id.materials = &other;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst),
                        NPH_RESULT_INVALID_ARGUMENT);
    id.materials = &unkeyed;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst),
                        NPH_RESULT_INVALID_ARGUMENT);
    id.material_count = 0U;
    id.materials = NULL;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(inst.value, 0U);
    // The same card without triangles for the rays takes any of them.
    id.mesh = plain;
    id.material_count = 1U;
    id.materials = &other;
    NphRenderInstance free_inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &free_inst), NPH_RESULT_OK);
    // The key they were made for.
    id.mesh = card;
    id.materials = &keyed;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);

    // While the instance lives its material keeps its key: what else it is may change.
    NphRenderMaterialDesc changed = md;
    changed.base_color = nph_vec4(0.2F, 0.4F, 0.6F, 1.0F);
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, keyed, &changed), NPH_RESULT_OK);
    changed.alpha_key = CARD_KEY + 2U;
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, keyed, &changed),
                        NPH_RESULT_INVALID_ARGUMENT);
    changed.alpha_key = 0U;
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, keyed, &changed),
                        NPH_RESULT_INVALID_ARGUMENT);
    changed.alpha_mode = NPH_RENDER_ALPHA_OPAQUE; // no key, as an opaque material has none
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, keyed, &changed),
                        NPH_RESULT_INVALID_ARGUMENT);
    // A material no trimmed mesh's instance names changes as it will.
    NphRenderMaterialDesc other_changed = other_desc;
    other_changed.alpha_key = CARD_KEY + 3U;
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, other, &other_changed),
                        NPH_RESULT_OK);
    // With the instance gone, so does this one.
    nph_render_instance_destroy(f->renderer, inst);
    NTEST_EXPECT_EQ_INT(nph_render_material_update(f->renderer, keyed, &changed), NPH_RESULT_OK);
    // And the card's instances are refused it from then on.
    id.materials = &keyed;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_instance_destroy(f->renderer, free_inst);
}

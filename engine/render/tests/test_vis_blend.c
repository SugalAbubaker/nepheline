// SPDX-License-Identifier: Apache-2.0
// The production path's blended surfaces (docs/render.md "Blended surfaces";
// docs/adr/0097-gpu-driven-transparency.md): culled, ordered and drawn by the
// GPU from the renderer's table of the instances' blended submeshes. Checked
// against the reference renderer, which orders and draws its own on the CPU,
// and against what the GPU wrote, read back: the order of every draw, what
// the cull kept, and what the table holds as instances and materials come
// and go. The fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture Blend;
typedef RenderFixture BlendSmall;
typedef RenderFixture BlendTiny;

enum {
    BLEND_INSTANCES = 1024,
    BLEND_ITEMS = 512,
    BLEND_DRAWS = 320,
    BLEND_INDICES = 4096,
    SMALL_ITEMS = 128,
    SMALL_DRAWS = 64,
    PLANE_TRIANGLES = 8, // a plane of 2 x 2 quads
    PLANE_INDICES = PLANE_TRIANGLES * 3,
    // In the index pool a two-sided submesh's list holds each triangle and its turn
    // (NPH_VIS_BLEND_BOTH): a pane's, and one of a split pane's halves'.
    PANE_LIST = 2 * PLANE_INDICES,
    HALF_LIST = PLANE_INDICES,
    TINY_INDICES = PANE_LIST + HALF_LIST
};

static void blend_setup(RenderFixture *f, u32 items, u32 draws, u32 indices)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_instances = BLEND_INSTANCES;
    d.max_materials = 512U;
    d.max_draws = BLEND_INSTANCES;
    d.max_instance_updates = BLEND_INSTANCES;
    d.max_blend_items = items;
    d.max_blend_draws = draws;
    d.blend_index_pool = indices;
    test_render_setup(f, &d);
}

NTEST_SETUP(Blend)
{
    blend_setup(f, BLEND_ITEMS, BLEND_DRAWS, BLEND_INDICES);
}

NTEST_TEARDOWN(Blend)
{
    test_render_teardown(f);
}

NTEST_SETUP(BlendSmall)
{
    blend_setup(f, SMALL_ITEMS, SMALL_DRAWS, BLEND_INDICES);
}

NTEST_TEARDOWN(BlendSmall)
{
    test_render_teardown(f);
}

NTEST_SETUP(BlendTiny)
{
    blend_setup(f, BLEND_ITEMS, BLEND_DRAWS, TINY_INDICES);
}

NTEST_TEARDOWN(BlendTiny)
{
    test_render_teardown(f);
}

// The indices in the blended surfaces' pool.
static u32 pool_used(const RenderFixture *f)
{
    const NphRenderer *r = f->renderer;
    return r->desc.blend_index_pool - nph_range_alloc_free_total(&r->vis.blend_index_ranges);
}

#define REQUIRE_PRODUCTION(f)                                                                      \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_production_supported((f)->renderer)) {                                   \
            NTEST_SKIP("no production path on this device");                                       \
        }                                                                                          \
    } while (0)

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

// A square of edge 1 facing +Y, 2 x 2 quads.
static NphRenderMesh plane_mesh(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 2U, 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

// The square's indices, as plane_mesh's mesh has them.
static void plane_indices_of(u32 out[PLANE_INDICES])
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 2U, 1.0F, &data),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(data.index_count, (u32)PLANE_INDICES);
    memcpy(out, data.indices, sizeof(u32) * (u64)PLANE_INDICES);
    nph_render_mesh_data_free(&data);
}

// The same square as two submeshes, half its triangles each, of material
// slots 0 and 1.
static NphRenderMesh split_plane_mesh(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 2U, 1.0F, &data),
                        NPH_RESULT_OK);
    NphRenderSubmesh whole;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, "split plane", &whole, &md);
    NphRenderSubmesh halves[2];
    const u32 first = (u32)PLANE_TRIANGLES / 2U * 3U;
    halves[0].index_offset = whole.index_offset;
    halves[0].index_count = first;
    halves[0].material_slot = 0U;
    halves[1].index_offset = whole.index_offset + first;
    halves[1].index_count = whole.index_count - first;
    halves[1].material_slot = 1U;
    md.submeshes = halves;
    md.submesh_count = 2U;
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh), NPH_RESULT_OK);
    nph_render_mesh_data_free(&data);
    return mesh;
}

static NphRenderMaterial make_material(RenderFixture *f, NphVec4 color, NphRenderAlphaMode mode,
                                       bool unlit, bool double_sided)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = color;
    md.alpha_mode = mode;
    md.unlit = unlit ? 1 : 0;
    md.double_sided = double_sided ? 1 : 0;
    md.roughness = 0.8F;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

static NphRenderMaterial glass(RenderFixture *f, NphVec4 color)
{
    return make_material(f, color, NPH_RENDER_ALPHA_BLEND, true, true);
}

static NphRenderInstance instance_of(RenderFixture *f, NphRenderMesh mesh,
                                     const NphRenderMaterial *materials, u32 count,
                                     NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = materials;
    id.material_count = count;
    id.transform = t;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

// A pane facing the camera on +Z (the plane faces +Y: a quarter turn about
// X), `size` across, at a point.
static NphRenderTransform pane(f64 x, f64 y, f64 z, f32 size)
{
    return nph_render_transform_from_trs(
        nph_dvec3(x, y, z), nph_quat_from_rotation_x(0.5F * NPH_PI), nph_vec3(size, 1.0F, size));
}

// Colours as authored, over a grey background: easy to compare.
static void plain_view(NphRenderView *v, NphRenderPipeline pipeline)
{
    nph_render_view_default(v, test_camera());
    v->ev100 = 0.0F;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3(0.24F, 0.24F, 0.24F);
    v->disable_ibl = 1;
    v->pipeline = pipeline;
}

// Renders a frame and as many more as the GPU's statistics take to come back
// (NPH_GPU_FRAMES_IN_FLIGHT): the statistics of the first, the image of the last.
static NphRenderFrameStats settled(RenderFixture *f, const NphRenderView *v)
{
    NphRenderFrameStats s = test_render_read(f, v);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        s = test_render_read(f, v);
    }
    return s;
}

typedef struct Diff {
    f32 within; // the share of pixels whose channels all agree within the tolerance
    f32 mean;
    u32 max;
} Diff;

static Diff diff_of(const u8 *a, const u8 *b, u32 tol)
{
    Diff d = {0.0F, 0.0F, 0U};
    u64 sum = 0U;
    u32 ok = 0U;
    for (u32 i = 0; i < TEST_TARGET_SIZE * TEST_TARGET_SIZE; ++i) {
        u32 worst = 0U;
        for (u32 c = 0; c < 3U; ++c) {
            const u32 x = a[i * 4U + c];
            const u32 y = b[i * 4U + c];
            const u32 e = x > y ? x - y : y - x;
            worst = nph_max_u32(worst, e);
            sum += e;
        }
        ok += worst <= tol ? 1U : 0U;
        d.max = nph_max_u32(d.max, worst);
    }
    d.within = (f32)ok / (f32)(TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    d.mean = (f32)sum / (f32)(TEST_TARGET_SIZE * TEST_TARGET_SIZE * 3U);
    return d;
}

// What the last frame's cull and sort wrote, read back.
typedef struct BlendFrame {
    u32 count; // the draws, as the indirect draw takes them
    u32 listed;
    // VkDrawIndexedIndirectCommand: index count, instance count, first index (in the
    // pool), vertex offset, first instance (the draw's record).
    u32 commands[BLEND_DRAWS][5];
    u32 keys[BLEND_DRAWS][2];
    NphRenderDrawData draws[BLEND_DRAWS];
} BlendFrame;

static void read_blend(RenderFixture *f, BlendFrame *out)
{
    const NphRenderer *r = f->renderer;
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NphVisCounters c;
    test_read_buffer(f->dev, r->vis.counters, 0U, sizeof c, &c);
    const u32 cap = nph_min_u32(r->desc.max_blend_draws, (u32)BLEND_DRAWS);
    out->listed = c.blend_draws;
    out->count = nph_min_u32(c.blend_draws, cap);
    test_read_buffer(f->dev, r->vis.blend_commands, 0U, (u64)cap * sizeof out->commands[0],
                     out->commands);
    test_read_buffer(f->dev, r->vis.blend_keys, 0U, (u64)cap * sizeof out->keys[0], out->keys);
    test_read_buffer(f->dev, r->vis.blend_draws, 0U, (u64)cap * sizeof out->draws[0], out->draws);
}

// ---------------------------------------------------------------------------
// The order
// ---------------------------------------------------------------------------

// Three hundred panes at as many distances, made in no order, and one of two
// submeshes: the GPU's draws are every one of them once, the farthest first,
// an instance's submeshes in their mesh's order.
NTEST_F(Blend, the_sort_orders_every_draw_farthest_first)
{
    REQUIRE_PRODUCTION(f);
    enum {
        PANES = 300
    };
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMesh split = split_plane_mesh(f);
    NphRenderMaterial mats[8];
    for (u32 i = 0; i < 8U; ++i) {
        mats[i] =
            glass(f, nph_vec4((f32)(i & 1U), (f32)((i >> 1U) & 1U), (f32)((i >> 2U) & 1U), 0.05F));
    }
    // Depths a fortieth of a metre apart, dealt out in a shuffled order.
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0xB1E2D5ULL, 3U);
    static u32 order[PANES];
    for (u32 i = 0; i < (u32)PANES; ++i) {
        order[i] = i;
    }
    for (u32 i = (u32)PANES - 1U; i > 0U; --i) {
        const u32 j = nph_pcg32_next_u32(&rng) % (i + 1U);
        const u32 t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    static f64 depth_of_slot[BLEND_INSTANCES];
    static NphRenderInstance panes[PANES];
    u32 split_slot = NPH_U32_MAX;
    for (u32 i = 0; i < (u32)PANES; ++i) {
        const f64 z = -5.5 + 0.025 * (f64)order[i];
        const f64 x = (f64)nph_pcg32_next_range_f32(&rng, -0.4F, 0.4F);
        const f64 y = (f64)nph_pcg32_next_range_f32(&rng, -0.4F, 0.4F);
        if (i == 137U) {
            const NphRenderMaterial two[2] = {mats[1], mats[2]};
            panes[i] = instance_of(f, split, two, 2U, pane(x, y, z, 0.3F));
            split_slot = render_slot_of(&f->renderer->instance_pool, panes[i].value);
        } else {
            panes[i] = instance_of(f, plane, &mats[i % 8U], 1U, pane(x, y, z, 0.3F));
        }
        depth_of_slot[render_slot_of(&f->renderer->instance_pool, panes[i].value)] = 4.0 - z;
    }
    NTEST_ASSERT_NE_UINT(split_slot, NPH_U32_MAX);

    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    const NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, (u32)PANES + 1U);
    NTEST_EXPECT_EQ_UINT(s.draws, (u32)PANES + 1U);
    NTEST_EXPECT_EQ_UINT(s.dropped, 0U);
    NTEST_EXPECT_EQ_UINT(s.triangles, (u32)PANES * (u32)PLANE_TRIANGLES);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, 0U);
    NTEST_EXPECT_EQ_UINT(s.vis_overflow, 0U);

    static BlendFrame b;
    read_blend(f, &b);
    NTEST_ASSERT_EQ_UINT(b.count, (u32)PANES + 1U);
    static u8 seen[BLEND_DRAWS];
    memset(seen, 0, sizeof seen);
    u32 out_of_order = 0U;
    u32 repeated = 0U;
    u32 malformed = 0U;
    u32 split_first = NPH_U32_MAX;
    u32 split_indices = 0U;
    u32 plane_indices = NPH_U32_MAX;
    f64 last = 1.0e30;
    for (u32 i = 0; i < b.count; ++i) {
        const u32 record = b.commands[i][4];
        if (record >= b.count || b.commands[i][1] != 1U || b.commands[i][3] != 0U ||
            b.commands[i][2] + b.commands[i][0] > (u32)BLEND_INDICES) {
            malformed += 1U;
            continue;
        }
        repeated += seen[record] != 0U ? 1U : 0U;
        seen[record] = 1U;
        const NphRenderDrawData *d = &b.draws[record];
        if (d->instance_index >= (u32)BLEND_INSTANCES) {
            malformed += 1U;
            continue;
        }
        const f64 depth = depth_of_slot[d->instance_index];
        if (d->instance_index == split_slot) {
            // Its two halves share a depth, and come in the mesh's order (their items'),
            // each half's indices its own in the pool.
            malformed += b.commands[i][0] != (u32)HALF_LIST ? 1U : 0U;
            if (split_first == NPH_U32_MAX) {
                split_first = i;
                split_indices = b.commands[i][2];
            } else {
                malformed += i != split_first + 1U ? 1U : 0U;
                malformed += b.commands[i][2] == split_indices ? 1U : 0U;
                malformed += b.keys[record][1] <= b.keys[b.commands[split_first][4]][1] ? 1U : 0U;
            }
        } else {
            // Every pane's indices are the one mesh's, once in the pool.
            malformed += b.commands[i][0] != (u32)PANE_LIST ? 1U : 0U;
            if (plane_indices == NPH_U32_MAX) {
                plane_indices = b.commands[i][2];
            }
            malformed += b.commands[i][2] != plane_indices ? 1U : 0U;
        }
        out_of_order += depth > last ? 1U : 0U;
        last = depth;
        // The key the cull gave it is the CPU's for its distance, to a rounding of the
        // distance (panes are a fortieth of a metre apart: thousands of roundings).
        const u32 want = nph_render_ref_vis_blend_key((f32)-depth);
        const u32 got = b.keys[record][0];
        malformed += (got > want ? got - want : want - got) > 64U ? 1U : 0U;
    }
    NTEST_LOG("%u draws: %u out of order, %u repeated, %u malformed", b.count, out_of_order,
              repeated, malformed);
    NTEST_EXPECT_EQ_UINT(out_of_order, 0U);
    NTEST_EXPECT_EQ_UINT(repeated, 0U);
    NTEST_EXPECT_EQ_UINT(malformed, 0U);
    NTEST_EXPECT_NE_UINT(split_first, NPH_U32_MAX);
    // The pool holds the pane's list and the two halves', whatever their instances; and
    // the pane's is its mesh's own triangles (its submesh is its whole mesh), each followed
    // by the same turned: the panes have two sides.
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST + 2U * (u32)HALF_LIST);
    NTEST_ASSERT_NE_UINT(plane_indices, NPH_U32_MAX);
    u32 pooled[PANE_LIST];
    u32 own[PLANE_INDICES];
    test_read_buffer(f->dev, f->renderer->vis.blend_indices, (u64)plane_indices * sizeof(u32),
                     sizeof pooled, pooled);
    plane_indices_of(own);
    u32 unwound = 0U;
    for (u32 t = 0; t < (u32)PLANE_TRIANGLES; ++t) {
        const u32 *made = &own[(u64)t * 3U];
        const u32 *list = &pooled[(u64)t * 6U];
        unwound += list[0] != made[0] || list[1] != made[1] || list[2] != made[2] ? 1U : 0U;
        unwound += list[3] != made[0] || list[4] != made[2] || list[5] != made[1] ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(unwound, 0U);
}

// Panes that overlap in threes, each of its own colour and half clear: what
// shows depends on their order everywhere, and matches the reference
// renderer's, whose CPU sorts its draws.
NTEST_F(Blend, overlapping_panes_composite_as_the_reference_renderer_orders_them)
{
    REQUIRE_PRODUCTION(f);
    enum {
        PANES = 24
    };
    const NphRenderMesh plane = plane_mesh(f);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0xC0102ULL, 5U);
    static NphRenderInstance panes[PANES];
    static f64 depths[PANES];
    for (u32 i = 0; i < (u32)PANES; ++i) {
        const NphRenderMaterial m =
            glass(f, nph_vec4(nph_pcg32_next_f32(&rng), nph_pcg32_next_f32(&rng),
                              nph_pcg32_next_f32(&rng), 0.5F));
        // Columns a third of a pane apart, rows of six, depths in no order.
        const u32 row = i / 6U;
        const f64 x = -1.0 + 0.36 * (f64)(i % 6U) + 0.1 * (f64)nph_pcg32_next_f32(&rng);
        const f64 y = -0.75 + 0.5 * (f64)row;
        depths[i] = -2.0 + 3.0 * (f64)nph_pcg32_next_f32(&rng);
        panes[i] = instance_of(f, plane, &m, 1U, pane(x, y, depths[i], 1.0F));
    }
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_REFERENCE);
    static u8 ref[PIXELS];
    static u8 vis[PIXELS];
    static u8 flipped[PIXELS];
    test_render_read(f, &v);
    memcpy(ref, f->pixels, PIXELS);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    test_render_read(f, &v);
    memcpy(vis, f->pixels, PIXELS);
    const Diff d = diff_of(ref, vis, 3U);
    NTEST_LOG("production against the reference: %.2f %% within 3 levels, mean %.3f, max %u",
              (f64)d.within * 100.0, (f64)d.mean, d.max);
    // The production path's HDR target has ten and eleven bits a channel, and every pane
    // blended into it rounds: a few levels after six of them.
    NTEST_EXPECT_TRUE(d.within >= 0.97F);
    NTEST_EXPECT_TRUE(d.mean <= 1.5F);
    // The order shows: the same panes at mirrored depths are another picture.
    for (u32 i = 0; i < (u32)PANES; ++i) {
        NphRenderTransform t = pane(0.0, 0.0, 0.0, 1.0F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_transform(f->renderer, panes[i], &t),
                            NPH_RESULT_OK);
        t.translation.z = -1.0 - depths[i];
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, panes[i], &t),
                            NPH_RESULT_OK);
    }
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    test_render_read(f, &v);
    memcpy(flipped, f->pixels, PIXELS);
    const Diff other = diff_of(ref, flipped, 3U);
    NTEST_LOG("the reference with the depths mirrored: %.2f %% within 3 levels, mean %.3f",
              (f64)other.within * 100.0, (f64)other.mean);
    NTEST_EXPECT_TRUE(other.mean >= 4.0F);
}

// ---------------------------------------------------------------------------
// The cull
// ---------------------------------------------------------------------------

NTEST_F(Blend, only_what_is_shown_and_in_view_is_drawn)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMaterial clear = glass(f, nph_vec4(0.2F, 0.6F, 1.0F, 0.5F));
    const NphRenderMaterial solid =
        make_material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), NPH_RENDER_ALPHA_OPAQUE, true, true);
    NphRenderInstance in_view[5];
    for (u32 i = 0; i < 5U; ++i) {
        in_view[i] =
            instance_of(f, plane, &clear, 1U, pane(-0.8 + 0.4 * (f64)i, 0.0, -(f64)i, 0.3F));
    }
    // Left of the view, right of it, and behind the camera at z = 4.
    (void)instance_of(f, plane, &clear, 1U, pane(-30.0, 0.0, 0.0, 0.3F));
    (void)instance_of(f, plane, &clear, 1U, pane(30.0, 0.0, 0.0, 0.3F));
    (void)instance_of(f, plane, &clear, 1U, pane(0.0, 0.0, 9.0, 0.3F));
    const NphRenderInstance hidden = instance_of(f, plane, &clear, 1U, pane(0.0, 0.6, 0.0, 0.3F));
    nph_render_instance_set_hidden(f->renderer, hidden, true);
    (void)instance_of(f, plane, &solid, 1U, pane(0.0, -0.6, 0.0, 0.3F));

    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.blend_listed, 9U);
    NTEST_EXPECT_EQ_UINT(s.blended, 5U);
    NTEST_EXPECT_EQ_UINT(s.draws, 5U);
    NTEST_EXPECT_EQ_UINT(s.triangles, 5U * (u32)PLANE_TRIANGLES);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, 3U);
    NTEST_EXPECT_EQ_UINT(s.instances, 10U);
    NTEST_EXPECT_EQ_UINT(s.culled, 1U); // the hidden one

    // Shown again; another hidden; another moved out of view.
    nph_render_instance_set_hidden(f->renderer, hidden, false);
    nph_render_instance_set_hidden(f->renderer, in_view[0], true);
    const NphRenderTransform away = pane(0.0, 40.0, 0.0, 0.3F);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, in_view[1], &away),
                        NPH_RESULT_OK);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 4U);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, 4U);
    // A view the other way sees the pane behind the first one's camera alone.
    NphRenderView back;
    plain_view(&back, NPH_RENDER_PIPELINE_PRODUCTION);
    back.camera = nph_render_camera_look_at(nph_dvec3(0.0, 0.0, 4.0), nph_dvec3(0.0, 0.0, 9.0),
                                            nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
    s = settled(f, &back);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
}

// Glass behind a wall is not drawn, and no pixel says so: a blended fragment
// behind an opaque surface writes nothing, so the cull drops what the frame's
// depth pyramid hides whole, as it drops opaque instances. A pane past the
// wall's edge, half hidden, is drawn.
NTEST_F(Blend, glass_behind_opaque_surfaces_is_not_drawn_and_no_pixel_changes)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMaterial wall =
        make_material(f, nph_vec4(0.7F, 0.6F, 0.5F, 1.0F), NPH_RENDER_ALPHA_OPAQUE, true, true);
    const NphRenderMaterial clear = glass(f, nph_vec4(0.1F, 0.9F, 0.3F, 0.5F));
    (void)instance_of(f, plane, &wall, 1U, pane(0.0, 0.0, 0.0, 2.0F));
    (void)instance_of(f, plane, &clear, 1U, pane(-0.3, 0.2, 1.0, 0.5F)); // before the wall
    (void)instance_of(f, plane, &clear, 1U, pane(1.3, 0.0, -1.0, 0.4F)); // past its edge
    for (u32 i = 0; i < 9U; ++i) {                                       // behind its middle
        const u32 row = i / 3U;
        (void)instance_of(f, plane, &clear, 1U,
                          pane(-0.3 + 0.3 * (f64)(i % 3U), -0.3 + 0.3 * (f64)row, -1.0, 0.25F));
    }
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    static u8 culled[PIXELS];
    static u8 plain[PIXELS];
    NphRenderFrameStats s = settled(f, &v);
    memcpy(culled, f->pixels, PIXELS);
    NTEST_LOG("with the pyramid: %u drawn, %u hidden", s.blended, s.vis_blended_occluded);
    NTEST_EXPECT_EQ_UINT(s.blended, 2U);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_occluded, 9U);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, 0U);
    // Without the test every one is drawn, and the picture is the same to the bit.
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_OCCLUSION;
    s = settled(f, &v);
    memcpy(plain, f->pixels, PIXELS);
    NTEST_EXPECT_EQ_UINT(s.blended, 11U);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_occluded, 0U);
    NTEST_EXPECT_EQ_MEM(culled, plain, PIXELS);
    // The pane before the wall shows (green over it), and the one past its edge.
    const f32 span = 2.0F * nph_f32_tan(0.3F) * 4.0F;
    const u32 px = (u32)((-0.3F * 4.0F / 3.0F / span + 0.5F) * (f32)TEST_TARGET_SIZE);
    const u32 py = (u32)((0.5F - 0.2F * 4.0F / 3.0F / span) * (f32)TEST_TARGET_SIZE);
    NTEST_EXPECT_GT_UINT(test_pixel(f, px, py)[1], test_pixel(f, px, py)[0]);
}

// ---------------------------------------------------------------------------
// Sides
// ---------------------------------------------------------------------------

// One pipeline draws them all, culling no face (forward_frag.glsl
// k_indirect): a pane of one side seen from behind is not drawn, a mirrored
// one's facing is its geometry's, and a two-sided one's back is lit as a back.
NTEST_F(Blend, sides_and_mirrored_transforms_match_the_reference)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMaterial one_sided =
        make_material(f, nph_vec4(1.0F, 0.3F, 0.2F, 0.7F), NPH_RENDER_ALPHA_BLEND, false, false);
    const NphRenderMaterial two_sided =
        make_material(f, nph_vec4(0.2F, 0.5F, 1.0F, 0.7F), NPH_RENDER_ALPHA_BLEND, false, true);
    const NphQuat toward = nph_quat_from_rotation_x(0.5F * NPH_PI);
    const NphQuat away = nph_quat_from_rotation_x(-0.5F * NPH_PI);
    const NphMat3 mirror = nph_mat3_from_scale(nph_vec3(-1.0F, 1.0F, 1.0F));
    // Three columns, two rows: [0] one side, toward; [1] one side, away (not drawn);
    // [2] one side, toward, mirrored; [3] one side, away, mirrored (not drawn);
    // [4] two sides, away; [5] two sides, away, mirrored.
    const f64 xs[3] = {-0.75, 0.0, 0.75};
    const f64 ys[2] = {0.45, -0.45};
    for (u32 i = 0; i < 6U; ++i) {
        const bool faces_away = i == 1U || i >= 3U;
        const bool mirrored = i == 2U || i == 3U || i == 5U;
        NphRenderTransform t =
            nph_render_transform_from_trs(nph_dvec3(xs[i % 3U], ys[i / 3U], 0.0),
                                          faces_away ? away : toward, nph_vec3(0.6F, 1.0F, 0.6F));
        if (mirrored) {
            t.linear = nph_mat3_mul(mirror, t.linear);
        }
        (void)instance_of(f, plane, i < 4U ? &one_sided : &two_sided, 1U, t);
    }
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3_one();
    ld.intensity = 60.0F;
    ld.position = nph_dvec3(0.3, 0.4, 2.5);
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);

    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.ev100 = 4.0F;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3(30.0F, 30.0F, 30.0F);
    v.disable_ibl = 1;
    static u8 ref[PIXELS];
    static u8 vis[PIXELS];
    test_render_read(f, &v);
    memcpy(ref, f->pixels, PIXELS);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    test_render_read(f, &v);
    memcpy(vis, f->pixels, PIXELS);
    // The cull keeps all six: the faces are the fragment stage's to end.
    const NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 6U);
    const Diff d = diff_of(ref, vis, 3U);
    NTEST_LOG("sides: %.2f %% within 3 levels, mean %.3f, max %u", (f64)d.within * 100.0,
              (f64)d.mean, d.max);
    NTEST_EXPECT_TRUE(d.within >= 0.98F);
    NTEST_EXPECT_TRUE(d.mean <= 1.0F);
    // Each pane's middle, in pixels (the view spans 2 tan(0.3) * 4 m).
    const f32 span = 2.0F * nph_f32_tan(0.3F) * 4.0F;
    const u8 *corner = vis; // the background
    for (u32 i = 0; i < 6U; ++i) {
        const u32 px = (u32)(((f32)xs[i % 3U] / span + 0.5F) * (f32)TEST_TARGET_SIZE);
        const u32 py = (u32)((0.5F - (f32)ys[i / 3U] / span) * (f32)TEST_TARGET_SIZE);
        const u8 *p = vis + ((u64)py * TEST_TARGET_SIZE + px) * 4U;
        const bool shows = p[0] != corner[0] || p[1] != corner[1] || p[2] != corner[2];
        const bool drawn = i != 1U && i != 3U;
        if (shows != drawn) {
            NTEST_LOG("pane %u at (%u, %u): %u %u %u, the background %u %u %u", i, px, py, p[0],
                      p[1], p[2], corner[0], corner[1], corner[2]);
        }
        NTEST_EXPECT_TRUE(shows == drawn);
    }
}

// The lists the pool holds are wound for the one pipeline's culling (back
// faces, counter-clockwise the front): a one-sided submesh's as its mesh winds
// it, or turned for an instance that mirrors; a two-sided one's each triangle
// and its turn. An instance that begins to mirror, and a material that gains a
// side, have their lists wound afresh, and what is drawn stays the reference
// renderer's picture.
NTEST_F(Blend, index_lists_are_wound_for_sides_and_mirroring)
{
    REQUIRE_PRODUCTION(f);
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.unlit = 1;
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    md.base_color = nph_vec4(0.9F, 0.2F, 0.1F, 0.6F);
    NphRenderMaterial one_sided = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(r, &md, &one_sided), NPH_RESULT_OK);
    const NphRenderMaterial two_sided = glass(f, nph_vec4(0.1F, 0.3F, 0.9F, 0.6F));
    const NphRenderInstance plain =
        instance_of(f, plane, &one_sided, 1U, pane(-0.7, 0.0, 0.0, 0.5F));
    NphRenderTransform mirror = pane(0.0, 0.0, 0.0, 0.5F);
    mirror.linear = nph_mat3_mul(nph_mat3_from_scale(nph_vec3(-1.0F, 1.0F, 1.0F)), mirror.linear);
    const NphRenderInstance mirrored = instance_of(f, plane, &one_sided, 1U, mirror);
    const NphRenderInstance both = instance_of(f, plane, &two_sided, 1U, pane(0.7, 0.0, 0.0, 0.5F));
    // As made, turned, and both: three lists of the one submesh.
    NTEST_EXPECT_EQ_UINT(pool_used(f), 2U * (u32)PLANE_INDICES + (u32)PANE_LIST);

    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_REFERENCE);
    static u8 ref[PIXELS];
    static u8 vis[PIXELS];
    test_render_read(f, &v);
    memcpy(ref, f->pixels, PIXELS);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    NphRenderFrameStats s = settled(f, &v);
    memcpy(vis, f->pixels, PIXELS);
    NTEST_EXPECT_EQ_UINT(s.blended, 3U);
    NTEST_EXPECT_EQ_UINT(s.triangles, 3U * (u32)PLANE_TRIANGLES);
    Diff d = diff_of(ref, vis, 3U);
    NTEST_LOG("three windings: %.2f %% within 3 levels, mean %.3f, max %u", (f64)d.within * 100.0,
              (f64)d.mean, d.max);
    NTEST_EXPECT_TRUE(d.within >= 0.98F);

    // Each instance's list, read back, against the mesh's own indices.
    u32 own[PLANE_INDICES];
    plane_indices_of(own);
    const NphRenderInstance of[3] = {plain, mirrored, both};
    const u32 want[3] = {NPH_VIS_BLEND_AS_MADE, NPH_VIS_BLEND_TURNED, NPH_VIS_BLEND_BOTH};
    for (u32 k = 0; k < 3U; ++k) {
        const RenderInstance *inst = &r->instances[render_slot_of(&r->instance_pool, of[k].value)];
        NTEST_ASSERT_EQ_UINT(inst->blend_count, 1U);
        const NphVisBlendItem item = r->vis.blend_mirror[inst->blend_range.offset];
        NTEST_EXPECT_EQ_UINT(item.flags >> (u32)NPH_VIS_BLEND_WINDING_SHIFT, want[k]);
        NTEST_EXPECT_EQ_UINT(item.index_count, k == 2U ? (u32)PANE_LIST : (u32)PLANE_INDICES);
        u32 list[PANE_LIST];
        test_read_buffer(f->dev, r->vis.blend_indices, (u64)item.first_index * sizeof(u32),
                         (u64)item.index_count * sizeof(u32), list);
        u32 unwound = 0U;
        for (u32 t = 0; t < (u32)PLANE_TRIANGLES; ++t) {
            const u32 *made = &own[(u64)t * 3U];
            const u32 *l = &list[(u64)t * (k == 2U ? 6U : 3U)];
            const bool turned = k == 1U;
            unwound +=
                l[0] != made[0] || l[1] != made[turned ? 2U : 1U] || l[2] != made[turned ? 1U : 2U]
                    ? 1U
                    : 0U;
            if (k == 2U) {
                unwound += l[3] != made[0] || l[4] != made[2] || l[5] != made[1] ? 1U : 0U;
            }
        }
        NTEST_EXPECT_EQ_UINT(unwound, 0U);
    }

    // The plain one begins to mirror: its list is the turned one, which the mirrored
    // instance already holds; the list as made has no user left.
    NphRenderTransform flipped = pane(-0.7, 0.0, 0.0, 0.5F);
    flipped.linear = nph_mat3_mul(nph_mat3_from_scale(nph_vec3(-1.0F, 1.0F, 1.0F)), flipped.linear);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(r, plain, &flipped), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PLANE_INDICES + (u32)PANE_LIST);
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    test_render_read(f, &v);
    memcpy(ref, f->pixels, PIXELS);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    s = settled(f, &v);
    memcpy(vis, f->pixels, PIXELS);
    NTEST_EXPECT_EQ_UINT(s.blended, 3U);
    d = diff_of(ref, vis, 3U);
    NTEST_LOG("one mirrored since: %.2f %% within 3 levels, mean %.3f, max %u",
              (f64)d.within * 100.0, (f64)d.mean, d.max);
    NTEST_EXPECT_TRUE(d.within >= 0.98F);

    // The one-sided material gains a side: every list is of both, one for the mesh.
    md.double_sided = 1;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, one_sided, &md), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(r->vis.blend_stale);
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    test_render_read(f, &v);
    memcpy(ref, f->pixels, PIXELS);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    s = settled(f, &v);
    memcpy(vis, f->pixels, PIXELS);
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST);
    NTEST_EXPECT_EQ_UINT(s.blended, 3U);
    d = diff_of(ref, vis, 3U);
    NTEST_EXPECT_TRUE(d.within >= 0.98F);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

NTEST_F(Blend, the_table_follows_instances_and_materials)
{
    REQUIRE_PRODUCTION(f);
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.unlit = 1;
    md.double_sided = 1;
    md.base_color = nph_vec4(1.0F, 0.0F, 0.0F, 0.5F);
    NphRenderMaterial first = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(r, &md, &first), NPH_RESULT_OK);
    const NphRenderInstance a = instance_of(f, plane, &first, 1U, pane(-0.4, 0.0, 0.0, 0.5F));
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    const u32 c = TEST_TARGET_SIZE / 2U;
    const u32 left = c - 10U; // on pane `a`

    // Opaque: none listed, and the frame has none of their passes.
    NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 0U);
    NTEST_EXPECT_EQ_UINT(s.blended, 0U);
    NTEST_EXPECT_FALSE(r->vis.frame.blend);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, left, c)[0], 255U);

    // Its material begins to blend: listed with the next frame.
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, first, &md), NPH_RESULT_OK);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 1U);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    NTEST_EXPECT_TRUE(r->vis.frame.blend);
    NTEST_EXPECT_LT_UINT(test_pixel(f, left, c)[0], 240U);
    // A change that is not of its alpha mode lists nothing afresh.
    md.base_color = nph_vec4(1.0F, 0.0F, 0.0F, 0.25F);
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, first, &md), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(r->vis.blend_stale);

    // Another instance, of a material that blends from the start.
    const NphRenderMaterial second = glass(f, nph_vec4(0.0F, 1.0F, 0.0F, 0.5F));
    const NphRenderInstance b = instance_of(f, plane, &second, 1U, pane(0.4, 0.0, 0.0, 0.5F));
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 2U);
    NTEST_EXPECT_EQ_UINT(s.blended, 2U);

    // The first goes, and an opaque instance takes its place before a frame is drawn: its
    // item is gone with it.
    nph_render_instance_destroy(r, a);
    const NphRenderMaterial solid =
        make_material(f, nph_vec4(0.0F, 0.0F, 1.0F, 1.0F), NPH_RENDER_ALPHA_OPAQUE, true, true);
    const NphRenderInstance o = instance_of(f, plane, &solid, 1U, pane(-0.4, 0.0, 0.0, 0.5F));
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 1U);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    NTEST_EXPECT_EQ_UINT(test_pixel(f, left, c)[2], 255U); // blue, whole
    NTEST_EXPECT_EQ_UINT(test_pixel(f, left, c)[0], 0U);

    // The second's material ends to blend, begins again, and is destroyed: its instance
    // is then drawn with the default material, which does not blend.
    NphRenderMaterialDesc sd;
    nph_render_material_desc_default(&sd);
    sd.unlit = 1;
    sd.double_sided = 1;
    sd.base_color = nph_vec4(0.0F, 1.0F, 0.0F, 0.5F);
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, second, &sd), NPH_RESULT_OK);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 0U);
    NTEST_EXPECT_EQ_UINT(s.blended, 0U);
    sd.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, second, &sd), NPH_RESULT_OK);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    nph_render_material_destroy(r, second);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 0U);
    NTEST_EXPECT_EQ_UINT(s.blended, 0U);
    nph_render_instance_destroy(r, b);
    nph_render_instance_destroy(r, o);
}

// An item is drawn only for the record it was listed for (vis_blend.comp): an
// instance's record reaches the GPU a frame late where a frame's updates are
// too many, and its slot may have changed hands by then. Items written here as
// the table would hold them across such a frame: one of an instance whose
// material does not blend, one of another mesh than its instance's, one of
// an instance that is gone, and beside them a hidden instance's and one as
// it should be.
NTEST_F(Blend, an_item_is_drawn_only_for_the_record_it_was_listed_for)
{
    REQUIRE_PRODUCTION(f);
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMesh other = split_plane_mesh(f);
    const NphRenderMaterial clear = glass(f, nph_vec4(0.2F, 0.6F, 1.0F, 0.5F));
    const NphRenderMaterial solid =
        make_material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), NPH_RENDER_ALPHA_OPAQUE, true, true);
    const NphRenderInstance opaque = instance_of(f, plane, &solid, 1U, pane(-0.6, 0.0, 0.0, 0.3F));
    const NphRenderInstance good = instance_of(f, plane, &clear, 1U, pane(0.0, 0.0, 0.0, 0.3F));
    const NphRenderInstance hidden = instance_of(f, plane, &clear, 1U, pane(0.6, 0.0, 0.0, 0.3F));
    const NphRenderInstance gone = instance_of(f, plane, &clear, 1U, pane(0.0, 0.6, 0.0, 0.3F));
    const u32 gone_slot = render_slot_of(&r->instance_pool, gone.value);
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    NphRenderFrameStats s = settled(f, &v);
    NTEST_ASSERT_EQ_UINT(r->vis.blend_listed, 3U);
    NTEST_EXPECT_EQ_UINT(s.blended, 3U);
    nph_render_instance_set_hidden(r, hidden, true);
    nph_render_instance_destroy(r, gone);

    // The table's next slots, by hand.
    const u32 at = r->vis.blend_end;
    NTEST_ASSERT_LE_UINT(at + 3U, 64U); // the first chunk
    const NphVisBlendItem model =
        r->vis.blend_mirror[r->instances[render_slot_of(&r->instance_pool, good.value)]
                                .blend_range.offset];
    NphVisBlendItem stale = model;
    stale.instance = render_slot_of(&r->instance_pool, opaque.value);
    r->vis.blend_mirror[at] = stale; // its instance's material does not blend
    stale = model;
    stale.mesh = render_slot_of(&r->mesh_pool, other.value);
    r->vis.blend_mirror[at + 1U] = stale; // listed for another mesh
    stale = model;
    stale.instance = gone_slot;
    r->vis.blend_mirror[at + 2U] = stale; // its instance is gone: the slot's record is not shown
    r->vis.blend_dirty[0] |= 1U;
    r->vis.blend_end = at + 3U;
    s = settled(f, &v);
    // The good one alone: the hidden one's record says so, and the three by hand are
    // passed by, neither drawn nor counted as out of view.
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, 0U);
    // Taken out again, for the table's own bookkeeping.
    memset(&r->vis.blend_mirror[at], 0xFF, 3U * sizeof(NphVisBlendItem));
    r->vis.blend_dirty[0] |= 1U;
    r->vis.blend_end = at;
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
}

// A whole population of panes replaced between two frames, twice: the table
// holds the new ones alone, in the room the old ones left.
NTEST_F(Blend, a_population_of_panes_is_replaced_between_frames)
{
    REQUIRE_PRODUCTION(f);
    enum {
        PANES = 200
    };
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMesh split = split_plane_mesh(f);
    const NphRenderMaterial two[2] = {glass(f, nph_vec4(1.0F, 0.5F, 0.0F, 0.1F)),
                                      glass(f, nph_vec4(0.0F, 0.5F, 1.0F, 0.1F))};
    static NphRenderInstance panes[PANES];
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    for (u32 round = 0; round < 3U; ++round) {
        u32 items = 0U;
        for (u32 i = 0; i < (u32)PANES; ++i) {
            // Every third has two submeshes; a quarter of them are out of view.
            const bool halves = (i + round) % 3U == 0U;
            const f64 x = i % 4U == 0U ? 25.0 : -0.5 + 0.005 * (f64)i;
            panes[i] = instance_of(f, halves ? split : plane, two, halves ? 2U : 1U,
                                   pane(x, 0.0, -0.01 * (f64)i, 0.2F));
            items += halves ? 2U : 1U;
        }
        const NphRenderFrameStats s = settled(f, &v);
        u32 in_view = 0U;
        for (u32 i = 0; i < (u32)PANES; ++i) {
            in_view += i % 4U == 0U ? 0U : ((i + round) % 3U == 0U ? 2U : 1U);
        }
        NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, items);
        NTEST_EXPECT_LE_UINT(r->vis.blend_end, (u32)BLEND_ITEMS);
        NTEST_EXPECT_EQ_UINT(s.blended, in_view);
        NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, items - in_view);
        // Two meshes' lists in the pool, however many instances draw them.
        NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST + 2U * (u32)HALF_LIST);
        for (u32 i = 0; i < (u32)PANES; ++i) {
            nph_render_instance_destroy(r, panes[i]);
        }
        NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 0U);
        NTEST_EXPECT_EQ_UINT(r->vis.blend_end, 0U);
        NTEST_EXPECT_EQ_UINT(pool_used(f), 0U);
    }
    // With none left a frame has none of their passes, and draws none.
    const NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_FALSE(r->vis.frame.blend);
    NTEST_EXPECT_EQ_UINT(s.blended, 0U);
}

// ---------------------------------------------------------------------------
// Capacities
// ---------------------------------------------------------------------------

NTEST_F(BlendSmall, draws_in_view_past_max_blend_draws_are_dropped_and_said)
{
    REQUIRE_PRODUCTION(f);
    enum {
        PANES = 100
    };
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMaterial clear = glass(f, nph_vec4(0.3F, 0.7F, 1.0F, 0.02F));
    for (u32 i = 0; i < (u32)PANES; ++i) {
        (void)instance_of(f, plane, &clear, 1U, pane(0.0, 0.0, -0.02 * (f64)i, 0.5F));
    }
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    const NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.blend_listed, (u32)PANES);
    NTEST_EXPECT_EQ_UINT(s.blended, (u32)SMALL_DRAWS);
    NTEST_EXPECT_EQ_UINT(s.dropped, (u32)PANES - (u32)SMALL_DRAWS);
    NTEST_EXPECT_NE_UINT(s.vis_overflow & (u32)NPH_RENDER_VIS_OVERFLOW_BLEND, 0U);
    // What it drew is in order, each draw once.
    static BlendFrame b;
    read_blend(f, &b);
    NTEST_EXPECT_EQ_UINT(b.listed, (u32)PANES);
    NTEST_ASSERT_EQ_UINT(b.count, (u32)SMALL_DRAWS);
    u8 seen[SMALL_DRAWS];
    memset(seen, 0, sizeof seen);
    u32 wrong = 0U;
    u32 previous = 0U;
    for (u32 i = 0; i < b.count; ++i) {
        const u32 record = b.commands[i][4];
        if (record >= b.count || seen[record] != 0U) {
            wrong += 1U;
            continue;
        }
        seen[record] = 1U;
        wrong += i != 0U && b.keys[record][0] < b.keys[previous][0] ? 1U : 0U;
        previous = record;
    }
    NTEST_EXPECT_EQ_UINT(wrong, 0U);
}

NTEST_F(BlendSmall, instances_past_max_blend_items_are_drawn_without_their_blended_submeshes)
{
    REQUIRE_PRODUCTION(f);
    enum {
        PANES = 150
    };
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMaterial clear = glass(f, nph_vec4(0.3F, 0.7F, 1.0F, 0.02F));
    static NphRenderInstance panes[PANES];
    // Out of view but for the last, which finds the table full.
    for (u32 i = 0; i < (u32)PANES; ++i) {
        const f64 x = i + 1U == (u32)PANES ? 0.0 : 20.0 + (f64)i;
        panes[i] = instance_of(f, plane, &clear, 1U, pane(x, 0.0, 0.0, 0.5F));
    }
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, (u32)SMALL_ITEMS);
    NTEST_EXPECT_TRUE(r->vis.blend_full_logged);
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 0U);
    NTEST_EXPECT_EQ_UINT(s.vis_blended_culled, (u32)SMALL_ITEMS);
    // Room made: an instance made now is listed, and drawn.
    for (u32 i = 0; i < 40U; ++i) {
        nph_render_instance_destroy(r, panes[i]);
    }
    (void)instance_of(f, plane, &clear, 1U, pane(0.0, 0.5, 0.0, 0.5F));
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, (u32)SMALL_ITEMS - 40U + 1U);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
}

// The index pool holds a submesh's indices while an instance draws it blended:
// they come with the first, go with the last, and come again; a material that
// ends to blend takes them out as its instances' going does.
NTEST_F(Blend, the_index_pool_holds_what_is_drawn_blended)
{
    REQUIRE_PRODUCTION(f);
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMesh split = split_plane_mesh(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.unlit = 1;
    md.double_sided = 1;
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    md.base_color = nph_vec4(0.2F, 0.8F, 0.4F, 0.5F);
    NphRenderMaterial clear = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(r, &md, &clear), NPH_RESULT_OK);
    const NphRenderMaterial solid =
        make_material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), NPH_RENDER_ALPHA_OPAQUE, true, true);
    NTEST_EXPECT_EQ_UINT(pool_used(f), 0U);
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);

    // A split plane whose second half alone blends: that half's indices alone.
    const NphRenderMaterial half[2] = {solid, clear};
    const NphRenderInstance a = instance_of(f, split, half, 2U, pane(-0.5, 0.0, 0.0, 0.5F));
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)HALF_LIST);
    NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    NTEST_EXPECT_EQ_UINT(s.triangles, (u32)PLANE_TRIANGLES / 2U);
    // Another of the same, and whole planes: the half is there once, the plane's once.
    const NphRenderInstance b = instance_of(f, split, half, 2U, pane(0.5, 0.0, 0.0, 0.5F));
    const NphRenderInstance c = instance_of(f, plane, &clear, 1U, pane(0.0, 0.6, 0.0, 0.5F));
    const NphRenderInstance d = instance_of(f, plane, &clear, 1U, pane(0.0, -0.6, 0.0, 0.5F));
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)HALF_LIST + (u32)PANE_LIST);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 4U);
    // The first split goes: its half stays for the other. Both gone: the half goes.
    nph_render_instance_destroy(r, a);
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)HALF_LIST + (u32)PANE_LIST);
    nph_render_instance_destroy(r, b);
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 2U);
    // The material ends to blend: nothing of it is in the pool from the next frame; and it
    // begins again: the plane's indices are copied in again, and drawn.
    md.alpha_mode = NPH_RENDER_ALPHA_OPAQUE;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, clear, &md), NPH_RESULT_OK);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(pool_used(f), 0U);
    NTEST_EXPECT_EQ_UINT(s.blended, 0U);
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(r, clear, &md), NPH_RESULT_OK);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST);
    NTEST_EXPECT_EQ_UINT(s.blended, 2U);
    NTEST_EXPECT_EQ_UINT(s.triangles, 2U * (u32)PLANE_TRIANGLES);
    // What they draw is the pane: green over the grey where the upper one lies.
    const u32 px = TEST_TARGET_SIZE / 2U;
    const u32 py = TEST_TARGET_SIZE / 2U - 15U;
    NTEST_EXPECT_GT_UINT(test_pixel(f, px, py)[1], test_pixel(f, px, py)[0] + 20U);
    nph_render_instance_destroy(r, c);
    nph_render_instance_destroy(r, d);
    NTEST_EXPECT_EQ_UINT(pool_used(f), 0U);
}

// A pool with room for a pane's list and half a pane's: an instance whose
// blended submeshes' do not all fit is drawn without them, and said; with
// room again, the next is drawn.
NTEST_F(BlendTiny, an_instance_whose_indices_do_not_fit_the_pool_is_drawn_without_them)
{
    REQUIRE_PRODUCTION(f);
    NphRenderer *r = f->renderer;
    const NphRenderMesh plane = plane_mesh(f);
    const NphRenderMesh split = split_plane_mesh(f);
    const NphRenderMaterial two[2] = {glass(f, nph_vec4(1.0F, 0.5F, 0.0F, 0.5F)),
                                      glass(f, nph_vec4(0.0F, 0.5F, 1.0F, 0.5F))};
    const NphRenderInstance whole = instance_of(f, plane, two, 1U, pane(-0.5, 0.0, 0.0, 0.5F));
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST);
    NTEST_EXPECT_FALSE(r->vis.blend_full_logged);
    // Both halves of the split plane: the first fits, the second does not, and neither stays.
    const NphRenderInstance halves = instance_of(f, split, two, 2U, pane(0.5, 0.0, 0.0, 0.5F));
    NTEST_EXPECT_EQ_UINT(pool_used(f), (u32)PANE_LIST);
    NTEST_EXPECT_EQ_UINT(r->vis.blend_listed, 1U);
    NTEST_EXPECT_TRUE(r->vis.blend_full_logged);
    NphRenderView v;
    plain_view(&v, NPH_RENDER_PIPELINE_PRODUCTION);
    NphRenderFrameStats s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 1U);
    // The whole plane gone, the two halves fit.
    nph_render_instance_destroy(r, whole);
    nph_render_instance_destroy(r, halves);
    NTEST_EXPECT_EQ_UINT(pool_used(f), 0U);
    (void)instance_of(f, split, two, 2U, pane(0.5, 0.0, 0.0, 0.5F));
    NTEST_EXPECT_EQ_UINT(pool_used(f), 2U * (u32)HALF_LIST);
    s = settled(f, &v);
    NTEST_EXPECT_EQ_UINT(s.blended, 2U);
}

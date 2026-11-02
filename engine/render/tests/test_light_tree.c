// SPDX-License-Identifier: Apache-2.0
// The tree of many lights (render/light_tree.h): every light reachable from
// anywhere, the probabilities a distribution, the walk taking them, a near or bright
// light worth more, and the same tree every time.
#include <nepheline/core/alloc.h>
#include <nepheline/math/random.h>
#include <nepheline/render/light_tree.h>

#include <ntest/ntest.h>

#include <string.h>

#define MANY 2048U

static NphDVec3 g_positions[MANY];
static f32 g_powers[MANY];

static f32 next_f32(NphPcg32 *r, f32 lo, f32 hi)
{
    return lo + (hi - lo) * nph_pcg32_next_f32(r);
}

// `count` lights in a box 26 by 10 by 11 m at `at`, each of power 1 to 3.
static void scatter(u32 count, NphDVec3 at)
{
    NphPcg32 r;
    nph_pcg32_seed(&r, 42U, 7U);
    for (u32 i = 0; i < count; ++i) {
        const f32 x = next_f32(&r, -13.0F, 13.0F);
        const f32 y = next_f32(&r, 0.0F, 10.0F);
        const f32 z = next_f32(&r, -5.5F, 5.5F);
        g_positions[i] = nph_dvec3(at.x + (f64)x, at.y + (f64)y, at.z + (f64)z);
        g_powers[i] = next_f32(&r, 1.0F, 3.0F);
    }
}

static void build(u32 count, NphRenderLightTree *out)
{
    NphRenderLightTreeInput in;
    memset(&in, 0, sizeof in);
    in.positions = g_positions;
    in.powers = g_powers;
    in.count = count;
    NTEST_ASSERT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), out),
                        NPH_RESULT_OK);
}

NTEST(light_tree, every_light_is_reachable_and_the_probabilities_sum_to_one)
{
    scatter(MANY, nph_dvec3(0.0, 0.0, 0.0));
    NphRenderLightTree tree;
    build(MANY, &tree);
    NTEST_EXPECT_EQ_UINT(tree.light_count, MANY);
    NTEST_EXPECT_LE_UINT(tree.node_count, MANY);
    const NphVec3 points[4] = {nph_vec3(0.0F, 2.0F, 0.0F), nph_vec3(13.0F, 0.0F, 5.5F),
                               nph_vec3(-40.0F, 30.0F, 9.0F), nph_vec3(0.1F, 0.1F, 0.1F)};
    for (u32 p = 0; p < 4U; ++p) {
        f64 sum = 0.0;
        f32 lowest = 1.0F;
        for (u32 i = 0; i < MANY; ++i) {
            const f32 pdf = nph_render_light_tree_pdf(&tree, points[p], i);
            NTEST_EXPECT_TRUE(pdf > 0.0F);
            lowest = nph_f32_min(lowest, pdf);
            sum += (f64)pdf;
        }
        NTEST_LOG("from point %u: the probabilities sum to %.6f, the lowest %.3g", p, sum,
                  (f64)lowest);
        NTEST_EXPECT_NEAR_F64(sum, 1.0, 1e-4);
    }
    nph_render_light_tree_free(&tree);
}

NTEST(light_tree, the_walk_takes_each_light_at_its_probability)
{
    scatter(256U, nph_dvec3(0.0, 0.0, 0.0));
    NphRenderLightTree tree;
    build(256U, &tree);
    enum {
        SAMPLES = 2000000
    };
    static u32 counts[256];
    memset(counts, 0, sizeof counts);
    const NphVec3 x = nph_vec3(3.0F, 1.0F, -2.0F);
    NphPcg32 r;
    nph_pcg32_seed(&r, 9U, 1U);
    for (u32 s = 0; s < (u32)SAMPLES; ++s) {
        f32 pdf = 0.0F;
        const u32 light = nph_render_light_tree_sample(&tree, x, nph_pcg32_next_f32(&r), &pdf);
        NTEST_ASSERT_LT_UINT(light, 256U);
        // What the walk says is what enumeration says.
        if (s < 1024U) {
            NTEST_EXPECT_NEAR_F32(pdf, nph_render_light_tree_pdf(&tree, x, light), pdf * 1e-3F);
        }
        counts[light] += 1U;
    }
    f64 chi2 = 0.0;
    for (u32 i = 0; i < 256U; ++i) {
        const f64 expected = (f64)nph_render_light_tree_pdf(&tree, x, i) * (f64)SAMPLES;
        const f64 d = (f64)counts[i] - expected;
        chi2 += d * d / expected;
    }
    // 255 degrees of freedom: mean 255, standard deviation about 22.6; 5 of them is
    // about 1 in 3.5 million. The float walk's own error adds little at these counts.
    NTEST_LOG("chi-squared %.1f over 255 degrees of freedom", chi2);
    NTEST_EXPECT_TRUE(chi2 < 255.0 + 5.0 * 22.6);
    nph_render_light_tree_free(&tree);
}

NTEST(light_tree, a_near_or_bright_light_is_worth_more)
{
    // Three lights: from a metre off the first, a hundred times the power at ten times
    // the distance is worth the same, and the same power a hundred times as far off,
    // next to nothing.
    g_positions[0] = nph_dvec3(0.0, 0.0, 0.0);
    g_positions[1] = nph_dvec3(11.0, 0.0, 0.0);
    g_positions[2] = nph_dvec3(-100.0, 0.0, 0.0);
    g_powers[0] = 1.0F;
    g_powers[1] = 100.0F;
    g_powers[2] = 1.0F;
    NphRenderLightTree tree;
    build(3U, &tree);
    NphVec3 x = nph_vec3((f32)(1.0 - tree.origin.x), 0.0F, 0.0F);
    NTEST_EXPECT_TRUE(nph_render_light_tree_pdf(&tree, x, 0U) > 0.45F);
    NTEST_EXPECT_TRUE(nph_render_light_tree_pdf(&tree, x, 1U) > 0.45F);
    NTEST_EXPECT_TRUE(nph_render_light_tree_pdf(&tree, x, 2U) < 0.01F);
    nph_render_light_tree_free(&tree);
}

NTEST(light_tree, no_light_counts_as_nearer_than_the_trees_nearest)
{
    // One light beside the point and one a metre off, of the same power: alone they
    // weigh 100 to 1, and with a nearest of a metre the same.
    g_positions[0] = nph_dvec3(0.0, 0.0, 0.0);
    g_positions[1] = nph_dvec3(1.0, 0.0, 0.0);
    g_powers[0] = 1.0F;
    g_powers[1] = 1.0F;
    NphRenderLightTree tree;
    build(2U, &tree);
    const NphVec3 x = nph_vec3((f32)(0.1 - tree.origin.x), 0.0F, 0.0F);
    NTEST_EXPECT_TRUE(nph_render_light_tree_pdf(&tree, x, 0U) > 0.95F);
    nph_render_light_tree_free(&tree);
    NphRenderLightTreeInput in;
    memset(&in, 0, sizeof in);
    in.positions = g_positions;
    in.powers = g_powers;
    in.count = 2U;
    in.nearest = 1.0F;
    NTEST_ASSERT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), &tree),
                        NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_render_light_tree_pdf(&tree, x, 0U), 0.5F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(nph_render_light_tree_pdf(&tree, x, 1U), 0.5F, 1e-5F);
    nph_render_light_tree_free(&tree);
    in.nearest = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), &tree),
                        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(light_tree, the_tree_is_the_same_every_time_and_far_from_the_origin)
{
    scatter(MANY, nph_dvec3(1000000.0, -500000.0, 250000.0));
    NphRenderLightTree a;
    NphRenderLightTree b;
    build(MANY, &a);
    build(MANY, &b);
    NTEST_ASSERT_EQ_UINT(a.node_count, b.node_count);
    NTEST_EXPECT_EQ_MEM(a.nodes, b.nodes, (u64)a.node_count * sizeof(NphRenderLightTreeNode));
    // The nodes are about the origin, which is the lights' middle: no precision lost.
    NTEST_EXPECT_NEAR_F64(a.origin.x, 1000000.0, 14.0);
    NTEST_EXPECT_TRUE(a.nodes[0].sphere[0].w < 40.0F);
    nph_render_light_tree_free(&a);
    nph_render_light_tree_free(&b);
}

NTEST(light_tree, lights_without_power_are_left_out_and_small_trees_work)
{
    NphRenderLightTree tree;
    NphRenderLightTreeInput in;
    memset(&in, 0, sizeof in);
    NTEST_ASSERT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), &tree),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(tree.node_count, 0U);
    f32 pdf = 1.0F;
    NTEST_EXPECT_EQ_UINT(nph_render_light_tree_sample(&tree, nph_vec3_zero(), 0.5F, &pdf),
                         NPH_RENDER_LIGHT_TREE_NONE);
    NTEST_EXPECT_NEAR_F32(pdf, 0.0F, 0.0F);
    nph_render_light_tree_free(&tree);

    g_positions[0] = nph_dvec3(1.0, 2.0, 3.0);
    g_positions[1] = nph_dvec3(4.0, 5.0, 6.0);
    g_powers[0] = 0.0F;
    g_powers[1] = 2.0F;
    build(2U, &tree);
    NTEST_EXPECT_EQ_UINT(tree.light_count, 1U);
    NTEST_EXPECT_EQ_UINT(tree.node_count, 1U);
    const u32 only = nph_render_light_tree_sample(&tree, nph_vec3(9.0F, 9.0F, 9.0F), 0.999F, &pdf);
    NTEST_EXPECT_EQ_UINT(only, 1U);
    NTEST_EXPECT_NEAR_F32(pdf, 1.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(nph_render_light_tree_pdf(&tree, nph_vec3_zero(), 0U), 0.0F, 0.0F);
    nph_render_light_tree_free(&tree);

    // Bad input.
    g_powers[1] = -1.0F;
    in.positions = g_positions;
    in.powers = g_powers;
    in.count = 2U;
    NTEST_EXPECT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), &tree),
                        NPH_RESULT_INVALID_ARGUMENT);
    g_powers[1] = 1.0F;
    in.powers = NULL;
    NTEST_EXPECT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), &tree),
                        NPH_RESULT_INVALID_ARGUMENT);
}

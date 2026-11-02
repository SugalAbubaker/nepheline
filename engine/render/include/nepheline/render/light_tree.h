// SPDX-License-Identifier: Apache-2.0
#pragma once

// A tree of many lights, for sampling one in proportion to what it may deliver
// (docs/render.md "Many lights"; Conty Estevez & Kulla, "Importance Sampling of
// Many Lights with Adaptive Tree Splitting", HPG 2018; Yuksel, "Stochastic
// Lightcuts", HPG 2019). The radiance cache's update walks it on the GPU once a
// sample (gi_update.comp), to gather the lights' far fields.
//
// A wide tree: each node holds four children's bounding spheres and powers, so a
// step of the walk reads one record, and 10,000 lights take seven. The walk goes
// from the root down, choosing a child in proportion to its power over its
// squared distance from the point (no nearer than its radius, or than the tree's
// `nearest`, within which the many lights have no far field to gather), and the
// probability of the leaf reached is the product of the choices'. Every light
// with power has a positive probability from anywhere, so the estimate is
// unbiased however poorly a node's sphere stands for its lights.
//
// The build sorts the lights along a Morton curve and halves the run twice per
// level, which is O(n log n), deterministic, and leaves every leaf within a level
// of the others. Lights with no power are left out.
//
// Thread-safety: pure, NPH_THREADSAFE (the tree is the caller's).

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_RENDER_LIGHT_TREE_WIDTH 4U
// A child that is a light rather than a node, its index the input's.
#define NPH_RENDER_LIGHT_TREE_LEAF 0x80000000U
// No child there (a node of fewer than four, a light with no power).
#define NPH_RENDER_LIGHT_TREE_NONE 0xFFFFFFFFU

typedef struct NphRenderLightTreeNode {
    // A child's bounding sphere, xyz about the tree's origin, w its radius.
    NphVec4 sphere[NPH_RENDER_LIGHT_TREE_WIDTH];
    f32 power[NPH_RENDER_LIGHT_TREE_WIDTH];
    // A node's index, NPH_RENDER_LIGHT_TREE_LEAF | a light's, or none.
    u32 child[NPH_RENDER_LIGHT_TREE_WIDTH];
} NphRenderLightTreeNode;

NPH_STATIC_ASSERT(sizeof(NphRenderLightTreeNode) == 96U, "as its shaders read it");

typedef struct NphRenderLightTreeInput {
    const NphDVec3 *positions; // world space
    const f32 *powers;         // what each may deliver at a metre: its candela, by its luminance
    u32 count;
    f32 nearest; // no child counts as nearer than this to a point (metres; 0: only its radius)
} NphRenderLightTreeInput;

typedef struct NphRenderLightTree {
    NphAllocator *alloc;
    NphRenderLightTreeNode *nodes; // the root first
    u32 node_count;
    u32 node_cap;
    u32 light_count; // with power
    NphDVec3 origin; // what the nodes' spheres are about: the lights' middle
    f32 nearest2;    // NphRenderLightTreeInput.nearest, squared
} NphRenderLightTree;

// Builds the tree. NPH_RESULT_INVALID_ARGUMENT for a missing array, a position or
// power that is not finite, or a negative power; NPH_RESULT_OUT_OF_MEMORY. No light
// with power makes an empty tree (node_count 0), which is no error.
NPH_MUST_USE NphResult nph_render_light_tree_build(const NphRenderLightTreeInput *input,
                                                   NphAllocator *alloc, NphRenderLightTree *out);

void nph_render_light_tree_free(NphRenderLightTree *tree);

// What the walk does at a point `x`, about the tree's origin (the shaders' twin:
// gi_update.comp, the numbers alike): a light (the input's index, or
// NPH_RENDER_LIGHT_TREE_NONE where the tree is empty) for `u` uniform in [0, 1),
// and its probability.
u32 nph_render_light_tree_sample(const NphRenderLightTree *tree, NphVec3 x, f32 u, f32 *out_pdf);

// The probability of light `light` from `x`, by walking every path to it: for tests
// (O(nodes)). 0 for a light not in the tree.
f32 nph_render_light_tree_pdf(const NphRenderLightTree *tree, NphVec3 x, u32 light);

#ifdef __cplusplus
}
#endif

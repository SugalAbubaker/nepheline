// SPDX-License-Identifier: Apache-2.0
// The tree of many lights (render/light_tree.h).
#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/light_tree.h>

#include <string.h>

typedef struct TreeLight {
    u64 morton;
    u32 index;
    f32 power;
    NphVec3 position; // about the tree's origin
} TreeLight;

typedef struct TreeBuild {
    TreeLight *lights;
    NphRenderLightTreeNode *nodes;
    u32 node_count;
    u32 node_cap;
} TreeBuild;

// The bits of `v` spread three apart (the 21 low ones).
static u64 spread_bits(u64 v)
{
    v &= 0x1FFFFFULL;
    v = (v | (v << 32U)) & 0x1F00000000FFFFULL;
    v = (v | (v << 16U)) & 0x1F0000FF0000FFULL;
    v = (v | (v << 8U)) & 0x100F00F00F00F00FULL;
    v = (v | (v << 4U)) & 0x10C30C30C30C30C3ULL;
    v = (v | (v << 2U)) & 0x1249249249249249ULL;
    return v;
}

static u64 morton_of(f64 x, f64 y, f64 z)
{
    return spread_bits((u64)x) | (spread_bits((u64)y) << 1U) | (spread_bits((u64)z) << 2U);
}

// Ties by the input's order, so that the tree is the same whatever the sort does.
static bool light_before(const TreeLight *a, const TreeLight *b)
{
    return a->morton != b->morton ? a->morton < b->morton : a->index < b->index;
}

// A bottom-up merge sort of `n` lights, `scratch` as large.
static void sort_lights(TreeLight *lights, TreeLight *scratch, u32 n)
{
    TreeLight *from = lights;
    TreeLight *to = scratch;
    for (u32 width = 1U; width < n; width *= 2U) {
        for (u32 start = 0U; start < n; start += 2U * width) {
            const u32 mid = nph_min_u32(start + width, n);
            const u32 end = nph_min_u32(mid + width, n);
            u32 a = start;
            u32 b = mid;
            for (u32 k = start; k < end; ++k) {
                to[k] = a < mid && (b >= end || !light_before(&from[b], &from[a])) ? from[a++]
                                                                                   : from[b++];
            }
        }
        TreeLight *t = from;
        from = to;
        to = t;
    }
    if (from != lights) {
        memcpy(lights, from, (u64)n * sizeof *lights);
    }
}

// The run's bounding sphere (about its box's middle, each light a point) and power.
static void bound_run(const TreeLight *lights, u32 count, NphVec4 *out_sphere, f32 *out_power)
{
    NphVec3 lo = lights[0].position;
    NphVec3 hi = lights[0].position;
    f64 power = 0.0;
    for (u32 i = 0; i < count; ++i) {
        lo = nph_vec3_min(lo, lights[i].position);
        hi = nph_vec3_max(hi, lights[i].position);
        power += (f64)lights[i].power;
    }
    const NphVec3 c = nph_vec3_scale(nph_vec3_add(lo, hi), 0.5F);
    f32 r2 = 0.0F;
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 d = nph_vec3_sub(lights[i].position, c);
        r2 = nph_f32_max(r2, nph_vec3_dot(d, d));
    }
    *out_sphere = nph_vec4(c.x, c.y, c.z, nph_f32_sqrt(r2));
    *out_power = (f32)power;
}

// Node `at` over the run of `count` lights, split into four runs along the curve.
static NphResult build_node(TreeBuild *b, u32 at, u32 first, u32 count)
{
    NphRenderLightTreeNode *n = &b->nodes[at];
    u32 begin = first;
    for (u32 c = 0; c < NPH_RENDER_LIGHT_TREE_WIDTH; ++c) {
        // Four runs within one light of each other; fewer lights than children
        // leave the last ones empty.
        const u32 take =
            (count + NPH_RENDER_LIGHT_TREE_WIDTH - 1U - c) / NPH_RENDER_LIGHT_TREE_WIDTH;
        n->child[c] = NPH_RENDER_LIGHT_TREE_NONE;
        n->sphere[c] = nph_vec4(0.0F, 0.0F, 0.0F, 0.0F);
        n->power[c] = 0.0F;
        if (take == 0U) {
            continue;
        }
        bound_run(&b->lights[begin], take, &n->sphere[c], &n->power[c]);
        if (take == 1U) {
            n->child[c] = NPH_RENDER_LIGHT_TREE_LEAF | b->lights[begin].index;
        } else {
            if (b->node_count == b->node_cap) {
                return NPH_RESULT_FULL; // sized for every internal node: not reachable
            }
            const u32 child = b->node_count++;
            n->child[c] = child;
            NPH_TRY(build_node(b, child, begin, take));
            n = &b->nodes[at];
        }
        begin += take;
    }
    return NPH_RESULT_OK;
}

NphResult nph_render_light_tree_build(const NphRenderLightTreeInput *input, NphAllocator *alloc,
                                      NphRenderLightTree *out)
{
    NPH_ASSERT(input != NULL && alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    if ((input->count != 0U && (input->positions == NULL || input->powers == NULL)) ||
        !nph_f32_is_finite(input->nearest) || input->nearest < 0.0F) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    out->nearest2 = input->nearest * input->nearest;
    // The lights with power, their box.
    u32 count = 0U;
    NphDVec3 lo = nph_dvec3(0.0, 0.0, 0.0);
    NphDVec3 hi = nph_dvec3(0.0, 0.0, 0.0);
    for (u32 i = 0; i < input->count; ++i) {
        const NphDVec3 p = input->positions[i];
        const f32 w = input->powers[i];
        if (!nph_f64_is_finite(p.x) || !nph_f64_is_finite(p.y) || !nph_f64_is_finite(p.z) ||
            !nph_f32_is_finite(w) || w < 0.0F) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (w == 0.0F) {
            continue;
        }
        lo = count == 0U ? p
                         : nph_dvec3(nph_f64_min(lo.x, p.x), nph_f64_min(lo.y, p.y),
                                     nph_f64_min(lo.z, p.z));
        hi = count == 0U ? p
                         : nph_dvec3(nph_f64_max(hi.x, p.x), nph_f64_max(hi.y, p.y),
                                     nph_f64_max(hi.z, p.z));
        ++count;
    }
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    out->origin = nph_dvec3((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5);
    TreeLight *lights = NPH_ALLOC_ARRAY(alloc, TreeLight, count);
    TreeLight *scratch = NPH_ALLOC_ARRAY(alloc, TreeLight, count);
    // Every internal node has at least two children but for a root over one light:
    // count of them at most.
    const u32 cap = nph_max_u32(count, 1U);
    NphRenderLightTreeNode *nodes = NPH_ALLOC_ARRAY(alloc, NphRenderLightTreeNode, cap);
    if (lights == NULL || scratch == NULL || nodes == NULL) {
        if (lights != NULL) {
            NPH_FREE_ARRAY(alloc, TreeLight, lights, count);
        }
        if (scratch != NULL) {
            NPH_FREE_ARRAY(alloc, TreeLight, scratch, count);
        }
        if (nodes != NULL) {
            NPH_FREE_ARRAY(alloc, NphRenderLightTreeNode, nodes, cap);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const f64 extent =
        nph_f64_max(nph_f64_max(hi.x - lo.x, hi.y - lo.y), nph_f64_max(hi.z - lo.z, 1e-6));
    const f64 grid = 2097151.0 / extent;
    u32 k = 0U;
    for (u32 i = 0; i < input->count; ++i) {
        if (input->powers[i] == 0.0F) {
            continue;
        }
        const NphDVec3 p = input->positions[i];
        lights[k].index = i;
        lights[k].power = input->powers[i];
        lights[k].position = nph_vec3((f32)(p.x - out->origin.x), (f32)(p.y - out->origin.y),
                                      (f32)(p.z - out->origin.z));
        lights[k].morton = morton_of((p.x - lo.x) * grid, (p.y - lo.y) * grid, (p.z - lo.z) * grid);
        ++k;
    }
    sort_lights(lights, scratch, count);
    TreeBuild b;
    b.lights = lights;
    b.nodes = nodes;
    b.node_count = 1U;
    b.node_cap = cap;
    const NphResult r = build_node(&b, 0U, 0U, count);
    NPH_FREE_ARRAY(alloc, TreeLight, lights, count);
    NPH_FREE_ARRAY(alloc, TreeLight, scratch, count);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(alloc, NphRenderLightTreeNode, nodes, cap);
        return r;
    }
    out->nodes = nodes;
    out->node_count = b.node_count;
    out->node_cap = cap;
    out->light_count = count;
    return NPH_RESULT_OK;
}

void nph_render_light_tree_free(NphRenderLightTree *tree)
{
    if (tree != NULL && tree->nodes != NULL) {
        NPH_FREE_ARRAY(tree->alloc, NphRenderLightTreeNode, tree->nodes, tree->node_cap);
        tree->nodes = NULL;
        tree->node_count = 0U;
    }
}

// What a child is worth from `x`: its power over its squared distance, no nearer
// than its radius (a light in a cluster around x could be anywhere in it) or than
// `nearest2` (many_lights.glsl nph_many_sample does the same).
static f32 child_weight(const NphRenderLightTreeNode *n, u32 c, NphVec3 x, f32 nearest2)
{
    if (n->child[c] == NPH_RENDER_LIGHT_TREE_NONE) {
        return 0.0F;
    }
    const NphVec3 d = nph_vec3_sub(nph_vec3(n->sphere[c].x, n->sphere[c].y, n->sphere[c].z), x);
    const f32 r2 = n->sphere[c].w * n->sphere[c].w;
    return n->power[c] /
           nph_f32_max(nph_f32_max(nph_f32_max(nph_vec3_dot(d, d), r2), nearest2), 1e-8F);
}

u32 nph_render_light_tree_sample(const NphRenderLightTree *tree, NphVec3 x, f32 u, f32 *out_pdf)
{
    NPH_ASSERT(tree != NULL && out_pdf != NULL);
    *out_pdf = 0.0F;
    if (tree->node_count == 0U) {
        return NPH_RENDER_LIGHT_TREE_NONE;
    }
    f32 pdf = 1.0F;
    u32 at = 0U;
    for (;;) {
        const NphRenderLightTreeNode *n = &tree->nodes[at];
        f32 w[NPH_RENDER_LIGHT_TREE_WIDTH];
        f32 sum = 0.0F;
        for (u32 c = 0; c < NPH_RENDER_LIGHT_TREE_WIDTH; ++c) {
            w[c] = child_weight(n, c, x, tree->nearest2);
            sum += w[c];
        }
        // The choice, and `u` made uniform again within it.
        f32 pick = u * sum;
        u32 c = 0U;
        for (; c + 1U < NPH_RENDER_LIGHT_TREE_WIDTH; ++c) {
            if (pick < w[c] || (n->child[c + 1U] == NPH_RENDER_LIGHT_TREE_NONE)) {
                break;
            }
            pick -= w[c];
        }
        pdf *= w[c] / sum;
        u = nph_f32_min(pick / w[c], 0.99999994F);
        const u32 next = n->child[c];
        if ((next & NPH_RENDER_LIGHT_TREE_LEAF) != 0U) {
            *out_pdf = pdf;
            return next & ~(u32)NPH_RENDER_LIGHT_TREE_LEAF;
        }
        at = next;
    }
}

static f32 pdf_below(const NphRenderLightTree *tree, u32 at, NphVec3 x, u32 light)
{
    const NphRenderLightTreeNode *n = &tree->nodes[at];
    f32 w[NPH_RENDER_LIGHT_TREE_WIDTH];
    f32 sum = 0.0F;
    for (u32 c = 0; c < NPH_RENDER_LIGHT_TREE_WIDTH; ++c) {
        w[c] = child_weight(n, c, x, tree->nearest2);
        sum += w[c];
    }
    for (u32 c = 0; c < NPH_RENDER_LIGHT_TREE_WIDTH; ++c) {
        const u32 next = n->child[c];
        if (next == NPH_RENDER_LIGHT_TREE_NONE) {
            continue;
        }
        if ((next & NPH_RENDER_LIGHT_TREE_LEAF) != 0U) {
            if ((next & ~(u32)NPH_RENDER_LIGHT_TREE_LEAF) == light) {
                return w[c] / sum;
            }
            continue;
        }
        const f32 below = pdf_below(tree, next, x, light);
        if (below > 0.0F) {
            return w[c] / sum * below;
        }
    }
    return 0.0F;
}

f32 nph_render_light_tree_pdf(const NphRenderLightTree *tree, NphVec3 x, u32 light)
{
    NPH_ASSERT(tree != NULL);
    return tree->node_count == 0U ? 0.0F : pdf_below(tree, 0U, x, light);
}

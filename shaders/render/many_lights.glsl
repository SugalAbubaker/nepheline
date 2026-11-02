// SPDX-License-Identifier: Apache-2.0
// The many lights in the shaders (docs/render.md "Many lights"; many_lights.c;
// docs/adr/0073-many-lights.md): the walk down their tree, for the radiance cache's
// update; the resolve's near fields are in surface.glsl (nph_lit_many).
//
// The includer has the shared structs and the bindless arithmetic types.
#ifndef NPH_MANY_LIGHTS_GLSL
#define NPH_MANY_LIGHTS_GLSL

NPH_BUFFER_REF(ManyViewRef) { NphRenderManyView m; };
NPH_BUFFER_REF(ManyLightsRef) { NphRenderManyLightData l[]; };
// One light's record: ManyLightRef(records)[i] (as LightRef, render_common.glsl).
NPH_BUFFER_REF(ManyLightRef) { NphRenderManyLightData l; };
// A tree's nodes, six vectors each: four children's spheres, their powers, and the
// children themselves.
NPH_BUFFER_REF(ManyNodesRef) { vec4 v[]; };
NPH_BUFFER_REF(ManyChildrenRef) { uvec4 v[]; };

#define NPH_MANY_NODE_SIZE 6u
#define NPH_MANY_LEAF      0x80000000u
#define NPH_MANY_NONE      0xFFFFFFFFu

// What nph_render_light_tree_sample does (light_tree.c, the numbers alike): a light
// for `u` uniform in [0, 1) at `x`, about the set's origin, and its probability.
uint nph_many_sample(NphRenderManyView m, vec3 x, float u, out float pdf)
{
    ManyNodesRef nodes = ManyNodesRef(m.nodes);
    ManyChildrenRef children = ManyChildrenRef(m.nodes);
    pdf = 1.0;
    uint at = 0u;
    // Four children a node: 16 levels hold more lights than an index can name.
    for (uint level = 0u; level < 16u; ++level) {
        const uint base = at * NPH_MANY_NODE_SIZE;
        const vec4 s0 = nodes.v[base];
        const vec4 s1 = nodes.v[base + 1u];
        const vec4 s2 = nodes.v[base + 2u];
        const vec4 s3 = nodes.v[base + 3u];
        const vec4 power = nodes.v[base + 4u];
        const uvec4 child = children.v[base + 5u];
        const vec3 d0 = s0.xyz - x;
        const vec3 d1 = s1.xyz - x;
        const vec3 d2 = s2.xyz - x;
        const vec3 d3 = s3.xyz - x;
        const vec4 distance2 = vec4(dot(d0, d0), dot(d1, d1), dot(d2, d2), dot(d3, d3));
        const vec4 radius2 = vec4(s0.w * s0.w, s1.w * s1.w, s2.w * s2.w, s3.w * s3.w);
        // A child that is not there has no power, so no weight.
        const vec4 w = power / max(max(max(distance2, radius2), vec4(m.near2)), vec4(1e-8));
        const float sum = w.x + w.y + w.z + w.w;
        float pick = u * sum;
        uint c = 0u;
        for (; c < 3u; ++c) {
            if (pick < w[c] || child[c + 1u] == NPH_MANY_NONE) {
                break;
            }
            pick -= w[c];
        }
        pdf *= w[c] / sum;
        u = min(pick / w[c], 0.99999994);
        const uint next = child[c];
        if ((next & NPH_MANY_LEAF) != 0u) {
            return next & ~NPH_MANY_LEAF;
        }
        at = next;
    }
    pdf = 0.0;
    return 0u;
}

// The share of a light's light beyond its near range at squared distance `d2`,
// the near field's complement within it, and windowed at its range (color_w: 1 /
// range^2): the near field takes (1 - (d / near)^4)^2 of it, as a range's window does
// (nph_light_attenuation).
float nph_many_far_share(float d2, float position_w, float color_w)
{
    const float x = d2 * position_w;
    const float near_window = clamp(1.0 - x * x, 0.0, 1.0);
    const float y = d2 * color_w;
    const float range_window = clamp(1.0 - y * y, 0.0, 1.0);
    return (1.0 - near_window * near_window) * (range_window * range_window);
}

#endif

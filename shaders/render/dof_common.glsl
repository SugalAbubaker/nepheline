// SPDX-License-Identifier: Apache-2.0
// What the depth of field's kernels share (docs/render.md "Depth of field";
// ADR-0037): their push constants, a depth's circle of confusion, the depth
// test that says what lies in front of what, and the tiles' classes.
#ifndef NPH_DOF_COMMON_GLSL
#define NPH_DOF_COMMON_GLSL

#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderDofPush pc; };

// A sample is wholly in front of another once nearer by this fraction of
// the other's distance, and partly within it: a relative test, whatever the
// scene's scale (as the motion blur's, motion_blur.frag).
#define DOF_DEPTH_SOFTNESS 0.05

// The radius in full-resolution pixels, negative in front of the focus, of
// the circle of confusion at reverse-Z depth `d`: linear in d for a thin
// lens (dof.c), within NPH_RENDER_DOF_MAX_RADIUS.
float dof_circle(float d)
{
    return clamp(pc.coc_bias + pc.coc_scale * d, -NPH_RENDER_DOF_MAX_RADIUS,
                 NPH_RENDER_DOF_MAX_RADIUS);
}

// How much the surface at reverse-Z depth `a` lies in front of the one at
// `b`: 0 level or behind, 1 once nearer by DOF_DEPTH_SOFTNESS of b's
// distance (with d = znear / z, 1 - z_a / z_b = (a - b) / a).
float dof_in_front(float a, float b)
{
    return clamp((a - b) / (DOF_DEPTH_SOFTNESS * max(a, 1e-30)), 0.0, 1.0);
}

// A tile's class, by the largest circle that can reach its pixels (its
// neighbourhood's, dof_tiles.comp: half-resolution pixels): in focus, left
// as drawn; slightly out of focus, gathered at full resolution
// (dof_resolve.comp); or further, at half (dof_gather.comp).
#define DOF_TILE_SHARP  0
#define DOF_TILE_SLIGHT 1
#define DOF_TILE_LARGE  2

int dof_tile_class(vec4 neighbourhood)
{
    const float largest = max(-neighbourhood.x, neighbourhood.y);
    if (largest < 0.5 * NPH_RENDER_DOF_SHARP) {
        return DOF_TILE_SHARP;
    }
    return largest <= 0.5 * NPH_RENDER_DOF_SLIGHT ? DOF_TILE_SLIGHT : DOF_TILE_LARGE;
}

#endif

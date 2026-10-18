// SPDX-License-Identifier: Apache-2.0
// Requests the virtual shadow map's pages for the production path's blended
// surfaces (docs/render.md "Shadows"), which the visibility buffer does not
// hold and the depth pyramid's requests therefore miss (vis_hzb.comp): the
// blended draws with the forward vertex stage (forward.vert), against the
// opaque surfaces' depth, each fragment in front of them setting the pages
// its shading may read (vsm_request). No colour, no depth written; the
// depth test comes first, so hidden fragments ask for nothing.
#version 460
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_vote : require
#include "render_common.glsl"
#define NPH_VSM_REQUESTS
#include "vsm_common.glsl"

layout(early_fragment_tests) in;
layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) in vec3 in_position; // camera-relative world space

void main()
{
    // Helper invocations' atomics do nothing: out of the subgroup's vote.
    if (gl_HelperInvocation) {
        return;
    }
    VsmViewRef r = VsmViewRef(ViewRef(pc.view_address).v.vsm);
    const uint request = vsm_request(r, vsm_light_space(r, in_position));
    for (uint k = 0u; k < 4u; ++k) {
        vsm_set_bit(r.v.requests, vsm_request_bit(request, k));
    }
}

// SPDX-License-Identifier: Apache-2.0
// Clears the pages the virtual shadow map draws this frame (docs/render.md
// "Shadows"): an instance per page drawn, two triangles over its texels in
// the pool at depth 1 (nothing), drawn with the depth test set to always
// before the casters.
#version 460
#include "vsm_raster.glsl"

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    VsmViewRef r = VsmViewRef(ViewRef(view.v.render_view).v.vsm);
    const uint entry = VisUintRef(r.v.rendered).v[gl_InstanceIndex];
    const VsmTarget target = vsm_target(r, entry);
    const vec2 corners[6] =
        vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                vec2(0.0, 1.0));
    const vec2 texel = target.base + corners[gl_VertexIndex] * float(NPH_VSM_PAGE);
    gl_Position = vec4(texel / vsm_pool_extent(r) * 2.0 - 1.0, 1.0, 1.0);
}

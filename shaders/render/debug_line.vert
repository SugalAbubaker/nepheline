// SPDX-License-Identifier: Apache-2.0
// Debug lines (render/debug_draw.h): end points relative to the debug
// origin, moved to camera-relative space by the view's debug offset, and
// projected without the frame's jitter. The draw's first vertex selects the
// depth-tested or the overlay list.
#version 460
#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) out vec4 out_color;

void main()
{
    ViewRef view = ViewRef(pc.view_address);
    const NphRenderDebugVertex dv = DebugRef(view.v.debug_address).v[gl_VertexIndex];
    const vec3 p = vec3(dv.x, dv.y, dv.z) + view.v.debug_offset.xyz;
    const vec4 c = unpackUnorm4x8(dv.color);
    // Authored colours are sRGB encoded; an sRGB target wants linear.
    out_color = (pc.flags & NPH_RENDER_PUSH_LINEAR_OUTPUT) != 0u
                    ? vec4(nph_srgb_to_linear(c.rgb), c.a)
                    : c;
    gl_Position = view.v.view_proj * vec4(p, 1.0);
    // The lines are drawn over the temporal resolve's output, which has no
    // jitter: take this frame's off the projection.
    gl_Position.xy -= view.v.jitter.xy * gl_Position.w;
}

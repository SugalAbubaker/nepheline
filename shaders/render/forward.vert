// SPDX-License-Identifier: Apache-2.0
// The forward reference renderer's vertex stage (docs/render.md "Forward
// pass"): pulls the quantised streams of the draw's mesh (16-bit unorm
// positions across the mesh bounds, octahedral normals, snorm8 tangents,
// half-float UVs, RGBA8 colours) and moves them to camera-relative world
// space with the draw's transform.
#version 460
#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) out vec3 out_position; // camera-relative world space
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec4 out_tangent; // xyz; w: bitangent sign
layout(location = 3) out vec2 out_uv;
layout(location = 4) out vec4 out_color;

void main()
{
    ViewRef view = ViewRef(pc.view_address);
    const NphRenderDrawData draw = DrawsRef(view.v.draws_address).d[pc.draw_index];
    const uvec2 q = PositionsRef(draw.positions_address).p[gl_VertexIndex];
    const vec3 unorm = vec3(q.x & 0xFFFFu, q.x >> 16u, q.y & 0xFFFFu) / 65535.0;
    const vec4 local = vec4(draw.position_offset.xyz + unorm * draw.position_scale.xyz, 1.0);
    const vec3 world = vec3(dot(draw.model0, local), dot(draw.model1, local), dot(draw.model2, local));

    const uvec4 attr = AttribsRef(draw.attribs_address).a[gl_VertexIndex];
    const vec3 n = nph_oct_decode(unpackSnorm2x16(attr.x));
    const vec4 t = unpackSnorm4x8(attr.y);
    // Normals take the inverse transpose; tangents follow the surface, so
    // the linear part. A mirroring transform flips the frame's handedness.
    out_normal = vec3(dot(draw.normal0.xyz, n), dot(draw.normal1.xyz, n), dot(draw.normal2.xyz, n));
    out_tangent = vec4(dot(draw.model0.xyz, t.xyz), dot(draw.model1.xyz, t.xyz),
                       dot(draw.model2.xyz, t.xyz),
                       (t.w < 0.0 ? -1.0 : 1.0) * draw.position_scale.w);
    out_uv = unpackHalf2x16(attr.z);
    out_color = unpackUnorm4x8(attr.w);
    out_position = world;
    gl_Position = view.v.view_proj * vec4(world, 1.0);
}

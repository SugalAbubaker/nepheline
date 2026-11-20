// SPDX-License-Identifier: Apache-2.0
// The forward reference renderer's vertex stage (docs/render.md "Forward
// pass"): pulls the quantised streams of the draw's mesh (16-bit unorm
// positions across the mesh bounds, octahedral normals, snorm8 tangents,
// half-float UVs, RGBA8 colours) and moves them to camera-relative world
// space with the draw's transform.
//
// The production path's blended surfaces are drawn with it too, by one
// indirect draw of draws the GPU listed (vis_blend.comp; ADR-0097): their
// pipelines are specialised on k_indirect, with which the draw's record is
// the one its instance index names (an indirect draw's first instance),
// where the reference renderer's CPU pushes each draw's.
#version 460
#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(constant_id = 4) const bool k_indirect = false;
// Whether a material reads the second texture coordinate set (ADR-0107; renderer.c
// render_forward_pipeline's `uv1`): only then is it passed on (the fragment stage's k_uv1).
layout(constant_id = 5) const bool k_uv1 = false;

layout(location = 0) out vec3 out_position; // camera-relative world space
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec4 out_tangent; // xyz; w: bitangent sign
layout(location = 3) out vec2 out_uv;
layout(location = 4) out vec4 out_color;
// What an indirect draw's fragments need of its record (forward_frag.glsl
// k_indirect): x its material; y bit 0: its transform mirrors, bit 1: it has
// one side (NphRenderDrawData.position_offset.w).
layout(location = 5) flat out uvec2 out_draw;
// The second texture coordinate set (ADR-0107): the mesh's, else the first; through the
// material's transform as the first is. Zero without k_uv1, and not read.
layout(location = 6) out vec2 out_uv1;

void main()
{
    ViewRef view = ViewRef(pc.view_address);
    uint draw_index = pc.draw_index;
    if (k_indirect) {
        draw_index = uint(gl_InstanceIndex);
    }
    const NphRenderDrawData draw = DrawsRef(view.v.draws_address).d[draw_index];
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
    // The material's coordinates: affine, so exact per vertex, and the fragment stage's
    // derivatives follow (ADR-0105).
    MaterialsRef materials = MaterialsRef(view.v.materials_address);
    out_uv = nph_material_uv(materials.m[draw.material_index].uv_u,
                             materials.m[draw.material_index].uv_v, unpackHalf2x16(attr.z));
    out_uv1 = vec2(0.0);
    if (k_uv1) {
        out_uv1 = nph_material_uv(
            materials.m[draw.material_index].uv_u, materials.m[draw.material_index].uv_v,
            nph_uv1(draw.uv1_address, gl_VertexIndex, unpackHalf2x16(attr.z)));
    }
    out_color = unpackUnorm4x8(attr.w);
    out_position = world;
    out_draw = uvec2(draw.material_index, (draw.position_scale.w < 0.0 ? 1u : 0u) |
                                              (draw.position_offset.w != 0.0 ? 2u : 0u));
    gl_Position = view.v.view_proj * vec4(world, 1.0);
}

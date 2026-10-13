// SPDX-License-Identifier: Apache-2.0
// gpu_mesh: vertex pulling from a buffer address and per-instance
// transforms computed by the async compute pass (mesh_anim.comp).
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

NPH_BUFFER_REF(Vertices) { NphMeshVertex v[]; };
NPH_BUFFER_REF(Instances) { NphMat4 m[]; };

layout(push_constant, scalar) uniform Push { NphMeshPush pc; };

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

void main()
{
    Vertices verts = Vertices(pc.vertex_address);
    Instances inst = Instances(pc.instance_address);
    const NphMeshVertex vtx = verts.v[gl_VertexIndex];
    const mat4 model = inst.m[gl_InstanceIndex];
    const vec4 world = model * vec4(vtx.position.xyz, 1.0);
    gl_Position = pc.view_proj * world;
    out_normal = normalize(mat3(model) * vtx.normal_uv.xyz);
    out_uv = vec2(vtx.normal_uv.w, vtx.extra.x);
}
